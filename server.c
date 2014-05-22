#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "server.h"

#define ASSERT(expr)                                      \
 do {                                                     \
  if (!(expr)) {                                          \
    fprintf(stderr,                                       \
            "Assertion failed in %s on line %d: %s\n",    \
            __FILE__,                                     \
            __LINE__,                                     \
            #expr);                                       \
    fflush(stderr);                                       \
    abort();                                              \
  }                                                       \
 } while (0)

#define FATAL(msg)                                        \
  do {                                                    \
    fprintf(stderr,                                       \
            "Fatal error in %s on line %d: %s\n",         \
            __FILE__,                                     \
            __LINE__,                                     \
            msg);                                         \
    fflush(stderr);                                       \
    abort();                                              \
  } while (0)

static uv_loop_t* loop;
static http_parser_settings parser_settings;

static void on_write(uv_write_t* req, int status);
static void on_read(uv_stream_t*, ssize_t nread, const uv_buf_t* buf);
static void on_close(uv_handle_t* peer);
static void on_server_close(uv_handle_t* handle);
static void on_connection(uv_stream_t*, int status);

int on_message_begin(http_parser* _);
int on_headers_complete(http_parser* _);
int on_message_complete(http_parser* _);
int on_url(http_parser* _, const char* at, size_t length);
int on_header_field(http_parser* _, const char* at, size_t length);
int on_header_value(http_parser* _, const char* at, size_t length);
int on_body(http_parser* _, const char* at, size_t length);

static void
destroy_request(http_request* request, int close_handle) {
  kl_destroy(header, request->headers);
  free(request->url);
  free(request->path);
  if (close_handle)
    uv_close((uv_handle_t*) request->handle, NULL);
  free(request);
}

static void
destroy_response(http_response* response) {
  free(response->pbuf);
  destroy_request(response->request, TRUE);
  uv_fs_t close_req;
  uv_fs_close(loop, &close_req, response->open_req->result, NULL);
  free(response);
}

static void
on_write(uv_write_t* req, int status) {
  http_response* response = (http_response*) req->data;
  if (response->request->offset == response->request->size) {
    destroy_response(response);
    free(req);
  }
}

static void
on_shutdown(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, on_close);
  free(req);
}

static void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  if (nread < 0) {
    if (buf->base) {
      free(buf->base);
    }
    uv_shutdown_t* shutdown_req = (uv_shutdown_t*) malloc(sizeof(uv_shutdown_t));
	if (shutdown_req == NULL) {
      uv_close((uv_handle_t*) stream, on_close);
      fprintf(stderr, "Allocate error\n");
      return;
	}
    uv_shutdown(shutdown_req, stream, on_shutdown);
    return;
  }

  if (nread == 0) {
    free(buf->base);
    return;
  }

  http_parser* parser = (http_parser*) stream->data;
  size_t nparsed = http_parser_execute(parser, &parser_settings, buf->base, nread);
  free(buf->base);
}

static void on_close(uv_handle_t* peer) {
  free(peer);
}

static void
on_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void
response_404(http_request* request) {
  char bufline[1024];
  sprintf(bufline,
	  "HTTP/1.0 404 Not Found\r\n"
	  "Content-Length: 10\r\n"
	  "Content-Type: text/plain; charset=UTF-8;\r\n"
	  "\r\n"
	  "Not Found\n");
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  int r = uv_write(write_req, (uv_stream_t*) request->handle, &buf, 1, NULL);
  if (r) {
	free(write_req);
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
  }
}

static void
response_500(http_request* request) {
  char bufline[1024];
  sprintf(bufline,
	  "HTTP/1.0 500 Internal Server Error\r\n"
	  "Content-Length: 22\r\n"
	  "Content-Type: text/plain; charset=UTF-8;\r\n"
	  "\r\n"
	  "Internal Server Error\n");
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  int r = uv_write(write_req, (uv_stream_t*) request->handle, &buf, 1, NULL);
  if (r) {
	free(write_req);
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
  }
}

static void
on_fs_read(uv_fs_t *req) {
  http_response* response = (http_response*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "File read error %s\n", uv_err_name(result));
	response_500(response->request);
    destroy_response(response);
	return;
  } else if (result == 0) {
    destroy_response(response);
	free(req);
	return;
  } else {
    uv_write_t *write_req = (uv_write_t *) malloc(sizeof(uv_write_t));
	write_req->data = response;
	uv_buf_t buf = uv_buf_init(response->pbuf, result);
    uv_write(write_req, (uv_stream_t*) response->handle, &buf, 1, on_write);
  }

  response->request->offset += result;
  if (response->request->offset >= response->request->size) {
    free(req);
	return;
  }
  int r = uv_fs_read(loop, response->read_req, response->open_req->result, &response->buf, 1, response->request->offset, on_fs_read);
  if (r) {
	response_500(response->request);
    destroy_request(response->request, TRUE);
  }
}

static void
on_fs_open_cb(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "Open error %s\n", uv_err_name(result));
	response_404(request);
    destroy_request(request, TRUE);
    return;
  }

  http_response* response = malloc(sizeof(http_response));
  memset(response, 0, sizeof(http_response));
  response->open_req = req;
  response->request = request;
  response->handle = request->handle;
  response->pbuf = malloc(8192);
  response->buf = uv_buf_init(response->pbuf, 8192);
  response->keep_alive = request->keep_alive;
  int offset = -1;
  uv_fs_t* read_req = malloc(sizeof(uv_fs_t));
  read_req->data = response;
  response->read_req = read_req;
  int r = uv_fs_read(loop, read_req, result, &response->buf, 1, offset, on_fs_read);
  if (r) {
	response_500(request);
    destroy_request(request, TRUE);
  }
}

static void
on_fs_stat_cb(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(result));
	response_404(request);
    destroy_request(request, TRUE);
    return;
  }
  request->size = req->statbuf.st_size;

  char bufline[1024];
  sprintf(bufline,
	  "HTTP/1.1 200 OK\r\n"
	  "Content-Length: %ld\r\n"
	  "Content-Type: text/html; charset=UTF-8;\r\n"
	  "\r\n", request->size);
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  int r = uv_write(write_req, (uv_stream_t*) request->handle, &buf, 1, NULL);
  free(req);
  if (r) {
    free(write_req);
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
	return;
  }

  uv_fs_t* open_req = malloc(sizeof(uv_fs_t));
  open_req->data = request;
  r = uv_fs_open(loop, open_req, request->path, O_RDONLY | O_BINARY, S_IREAD, on_fs_open_cb);
  if (r) {
    fprintf(stderr, "Open error %s\n", uv_err_name(r));
	response_404(request);
	destroy_request(request, TRUE);
	free(open_req);
  }
}

static void
on_request_complete(http_request* request) {
  kliter_t(header)* it;
  for (it = kl_begin(request->headers); it != kl_end(request->headers); it = kl_next(it)) {
	header_elem elem = kl_val(it);
	if (!strcasecmp(elem.key, "connection")) {
	  if (!strcasecmp(elem.value, "keep-alive")) {
        request->keep_alive = 1;
	  }
	}
  }

  if (request->url_handle.field_set & (1<<UF_PATH)) {
    char path[MAX_PATH];
	char* ptr = request->url + request->url_handle.field_data[UF_PATH].off;
	int len = request->url_handle.field_data[UF_PATH].len;
	snprintf(path, sizeof(path), "./public%.*s", len, ptr);
	if (*(ptr + len - 1) == '/') {
	  strcat(path, "index.html");
	}
    request->path = strdup(path);
  }

  uv_fs_t* stat_req = malloc(sizeof(uv_fs_t));
  stat_req->data = request;
  int r = uv_fs_stat(loop, stat_req, request->path, on_fs_stat_cb);
  if (r) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(r));
	uv_close((uv_handle_t*) request->handle, NULL);
	response_404(request);
	destroy_request(request, TRUE);
	free(stat_req);
  }
}

static void
on_connection(uv_stream_t* server, int status) {
  uv_stream_t* stream;
  int r;

  if (status != 0) {
    fprintf(stderr, "Connect error %s\n", uv_err_name(status));
	return;
  }

  stream = malloc(sizeof(uv_tcp_t));
  if (stream == NULL) {
    fprintf(stderr, "Allocate error\n");
	return;
  }

  r = uv_tcp_init(loop, (uv_tcp_t*) stream);
  if (r) {
    fprintf(stderr, "Socket creation error %s\n", uv_err_name(r));
	return;
  }

  r = uv_accept(server, stream);
  if (r) {
    fprintf(stderr, "Accept error %s\n", uv_err_name(r));
	return;
  }

  http_parser* parser = malloc(sizeof(http_parser));
  if (parser == NULL) {
    fprintf(stderr, "Allocate error\n");
	uv_close((uv_handle_t*) stream, NULL);
	return;
  }
  http_parser_init(parser, HTTP_REQUEST);

  http_request* request = malloc(sizeof(http_request));
  if (request == NULL) {
    fprintf(stderr, "Allocate error\n");
	uv_close((uv_handle_t*) stream, NULL);
	return;
  }
  parser->data = request;

  memset(request, 0, sizeof(http_request));
  request->handle = (uv_handle_t*) stream;
  request->on_request_complete = on_request_complete;
  stream->data = parser;

  r = uv_read_start(stream, on_alloc_cb, on_read);
  if (r) {
    fprintf(stderr, "Read error %s\n", uv_err_name(r));
	uv_close((uv_handle_t*) stream, NULL);
  }
}

int
main() {
  loop = uv_default_loop();

  struct sockaddr_in addr;
  int r;

  memset(&parser_settings, 0, sizeof(parser_settings));
  parser_settings.on_message_begin = on_message_begin;
  parser_settings.on_url = on_url;
  parser_settings.on_header_field = on_header_field;
  parser_settings.on_header_value = on_header_value;
  parser_settings.on_headers_complete = on_headers_complete;
  parser_settings.on_body = on_body;
  parser_settings.on_message_complete = on_message_complete;

  r = uv_ip4_addr("0.0.0.0", 7000, &addr);
  if (r) {
    fprintf(stderr, "Address error %s\n", uv_err_name(r));
    return 1;
  }

  uv_tcp_t server;
  r = uv_tcp_init(loop, &server);
  if (r) {
    fprintf(stderr, "Socket creation error %s\n", uv_err_name(r));
    return 1;
  }

  r = uv_tcp_bind(&server, (const struct sockaddr*) &addr, 0);
  if (r) {
    fprintf(stderr, "Bind error %s\n", uv_err_name(r));
    return 1;
  }

  r = uv_listen((uv_stream_t*)&server, SOMAXCONN, on_connection);
  if (r) {
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 1;
  }

  return uv_run(loop, UV_RUN_DEFAULT);
}

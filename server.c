/* Copyright 2014 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include "server.h"

#define ASSERT(expr)                                      \
 do {                                                     \
  if (!(expr)) {                                          \
    fprintf(stderr,                                       \
      "Assertion failed in %s on line %d: %s\n",          \
      __FILE__,                                           \
      __LINE__,                                           \
      #expr);                                             \
    fflush(stderr);                                       \
    abort();                                              \
  }                                                       \
 } while (0)

#define FATAL(msg)                                        \
  do {                                                    \
    fprintf(stderr,                                       \
      "Fatal error in %s on line %d: %s\n",               \
      __FILE__,                                           \
      __LINE__,                                           \
      msg);                                               \
    fflush(stderr);                                       \
    abort();                                              \
  } while (0)

static uv_loop_t* loop;
static http_parser_settings parser_settings;
static char* static_dir = "./public";

KHASH_MAP_INIT_STR(mime_type, const char*)
static khash_t(mime_type)* mime_type;

#if 0
#include <sys/time.h>
void
btime(int f) {
  static struct timeval s;
  struct timeval e;
  if (f == 0) {
    puts("start");
    gettimeofday(&s, NULL);
  } else {
    puts("end");
    gettimeofday(&e, NULL);
    double startTime = s.tv_sec + (double)(s.tv_usec * 1e-6);
    double endTime = e.tv_sec + (double)(e.tv_usec * 1e-6);
    printf("%f\n", endTime - startTime);
  }
}
#endif

static void on_write(uv_write_t*, int);
static void on_write_error(uv_write_t*, int);
static void on_header_write(uv_write_t*, int);
static void on_read(uv_stream_t*, ssize_t, const uv_buf_t*);
static void on_close(uv_handle_t*);
static void on_server_close(uv_handle_t*);
static void on_connection(uv_stream_t*, int);
static void on_alloc(uv_handle_t*, size_t, uv_buf_t*);
static void on_request_complete(http_parser*, http_request*);
static void on_fs_read(uv_fs_t*);
static void response_error(uv_handle_t*, int, const char*, const char*);

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
  if (close_handle && request->handle) {
    uv_close((uv_handle_t*) request->handle, on_close);
  }
  free(request);
}

static void
destroy_response(http_response* response, int close_handle) {
  if (response->pbuf) free(response->pbuf);
  //if (response->handle) free(response->handle);
  if (response->request) destroy_request(response->request, close_handle);
  if (response->open_req) {
    uv_fs_t close_req;
    uv_fs_close(loop, &close_req, response->open_req->result, NULL);
    free(response->open_req);
  }
  if (response->read_req) {
    free(response->read_req);
  }
  free(response);
}

static void
on_write(uv_write_t* req, int status) {
  http_response* response = (http_response*) req->data;
  free(req);

  if (response->request->offset < response->request->size) {
    int r = uv_fs_read(loop, response->read_req, response->open_req->result, &response->buf, 1, response->request->offset, on_fs_read);
    if (r) {
      response_error(response->handle, 500, "Internal Server Error", NULL);
      destroy_request(response->request, 1);
    }
    return;
  }
  if (!response->keep_alive) {
    destroy_response(response, 1);
    return;
  }

  destroy_response(response, 0);
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
    /* FIXME
    uv_shutdown_t* shutdown_req = (uv_shutdown_t*) malloc(sizeof(uv_shutdown_t));
    if (shutdown_req == NULL) {
      fprintf(stderr, "Allocate error\n");
      uv_close((uv_handle_t*) stream, on_close);
      return;
    }
    uv_shutdown(shutdown_req, stream, on_shutdown);
    */
    uv_close((uv_handle_t*) stream, on_close);
    return;
  }

  if (nread == 0) {
    free(buf->base);
    return;
  }

#define END_WITH(p,l) (l>=4 && (*(p+l-4)=='\r' && *(p+l-3)=='\n' && *(p+l-2)=='\r' && *(p+l-1)=='\n')||(l>=2 && *(p+l-2)=='\n' && *(p+l-1)=='\n'))
  if (END_WITH(buf->base, nread)) {
    http_request* request = calloc(1, sizeof(http_request));
    if (request == NULL) {
      fprintf(stderr, "Allocate error\n");
      uv_close((uv_handle_t*) stream, on_close);
      return;
    }
    request->handle = (uv_handle_t*) stream;
    request->on_request_complete = on_request_complete;
    http_parser_init(&request->parser, HTTP_REQUEST);
    request->parser.data = request;
    stream->data = request;

    size_t nparsed = http_parser_execute(&request->parser, &parser_settings, buf->base, nread);
  }
  free(buf->base);
}

static void on_close(uv_handle_t* peer) {
  free(peer);
}

static void
on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void
on_write_error(uv_write_t* req, int status) {
  free(req);
}

static void
response_error(uv_handle_t* handle, int status_code, const char* status, const char* message) {
  char bufline[1024];
  const char* ptr = message ? message : status;
  sprintf(bufline,
      "HTTP/1.0 %d %s\r\n"
      "Content-Length: %d\r\n"
      "Content-Type: text/plain; charset=UTF-8;\r\n"
      "\r\n"
      "%s", status_code, status, (int) strlen(ptr), ptr);
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  int r = uv_write(write_req, (uv_stream_t*) handle, &buf, 1, on_write_error);
  if (r) {
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
    response_error(response->handle, 500, "Internal Server Error", NULL);
    destroy_response(response, 1);
    return;
  } else if (result == 0) {
    destroy_response(response, 1);
    return;
  }

  uv_write_t *write_req = (uv_write_t *) malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  write_req->data = response;
  uv_buf_t buf = uv_buf_init(response->pbuf, result);
  int r = uv_write(write_req, (uv_stream_t*) response->handle, &buf, 1, on_write);
  if (r) {
    destroy_response(response, 1);
    return;
  }
  response->request->offset += result;
}

static void
on_fs_open(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "Open error %s\n", uv_err_name(result));
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_request(request, 1);
    return;
  }

  http_response* response = calloc(1, sizeof(http_response));
  if (response == NULL) {
    fprintf(stderr, "Allocate error\n");
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_request(request, 1);
    return;
  }
  response->open_req = req;
  response->request = request;
  response->handle = request->handle;
  response->pbuf = malloc(8192);
  if (response->pbuf == NULL) {
    fprintf(stderr, "Allocate error\n");
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_response(response, 1);
    return;
  }
  response->buf = uv_buf_init(response->pbuf, 8192);
  response->keep_alive = request->keep_alive;
  int offset = -1;
  uv_fs_t* read_req = malloc(sizeof(uv_fs_t));
  if (read_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_response(response, 1);
    return;
  }
  read_req->data = response;
  response->read_req = read_req;
  int r = uv_fs_read(loop, read_req, result, &response->buf, 1, offset, on_fs_read);
  if (r) {
    response_error(request->handle, 500, "Internal Server Error\n", NULL);
    destroy_response(response, 1);
  }
}

static void
on_header_write(uv_write_t* req, int status) {
  http_request* request = (http_request*) req->data;
  free(req);

  uv_fs_t* open_req = malloc(sizeof(uv_fs_t));
  if (open_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  open_req->data = request;
  int r = uv_fs_open(loop, open_req, request->path, O_RDONLY, S_IREAD, on_fs_open);
  if (r) {
    fprintf(stderr, "Open error %s\n", uv_err_name(r));
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_request(request, 1);
    free(open_req);
  }
}

static void
on_fs_stat(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  if (result < 0) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(result));
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_request(request, 1);
    return;
  }
  uv_fs_req_cleanup(req);
  request->size = req->statbuf.st_size;
  free(req);

  const char* ctype = "application/octet-stream";
  char* dot = request->path;
  char* ptr = dot;
  while (dot) {
    ptr = dot;
    dot = strchr(dot + 1, '.');
  }
  khint_t k = kh_get(mime_type, mime_type, ptr);
  if (k != kh_end(mime_type)) {
    ctype = kh_value(mime_type, k);
  }

  char bufline[1024];
  snprintf(bufline,
      sizeof(bufline),
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %" PRId64 "\r\n"
      "Content-Type: %s\r\n"
      "Connection: %s\r\n"
      "\r\n",
      request->size,
      ctype,
      (request->keep_alive ? "keep-alive" : "close"));
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  write_req->data = request;
  int r = uv_write(write_req, (uv_stream_t*) request->handle, &buf, 1, on_header_write);
  if (r) {
    free(write_req);
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
    return;
  }
}

static void
on_request_complete(http_parser* parser, http_request* request) {
  if (!(request->url_handle.field_set & (1<<UF_PATH))) {
    snprintf(request->path, sizeof(request->path), "%s/index.html", static_dir);
  } else {
    const char* ptr = request->url + request->url_handle.field_data[UF_PATH].off;
    int len = request->url_handle.field_data[UF_PATH].len;
    snprintf(request->path, sizeof(request->path), "%s%.*s", static_dir, len, ptr);
    if (*(ptr + len - 1) == '/') {
      strcat(request->path, "index.html");
    }
    /*
    if (strstr(path, "quit")) {
      destroy_request(request, 1);
      uv_stop(loop);
      return;
    }
    */
  }
  request->keep_alive = http_should_keep_alive(parser);

  uv_fs_t* stat_req = malloc(sizeof(uv_fs_t));
  if (stat_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  stat_req->data = request;
  int r = uv_fs_stat(loop, stat_req, request->path, on_fs_stat);
  if (r) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(r));
    free(stat_req);
    uv_close((uv_handle_t*) request->handle, on_close);
    response_error(request->handle, 404, "Not Found\n", NULL);
    destroy_request(request, 1);
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

  r = uv_tcp_nodelay((uv_tcp_t*) stream, 1);
  if (r) {
    fprintf(stderr, "Flag error %s\n", uv_err_name(r));
    return;
  }

  r = uv_read_start(stream, on_alloc, on_read);
  if (r) {
    fprintf(stderr, "Read error %s\n", uv_err_name(r));
    uv_close((uv_handle_t*) stream, on_close);
  }
}

static void
usage(const char* app) {
  fprintf(stderr, "usage: %s [OPTIONS]\n", app);
  fprintf(stderr, "    -a ADDR: specify address\n");
  fprintf(stderr, "    -p PORT: specify port number\n");
  fprintf(stderr, "    -d DIR:  specify root directory\n");
  exit(1);
}

static void
add_mime_type(const char* ext, const char* value) {
  int hr;
  khint_t k = kh_put(mime_type, mime_type, ext, &hr);
  kh_value(mime_type, k) = value;
}

int
main(int argc, char* argv[]) {
  char* ipaddr = "0.0.0.0";
  int port = 7000;
  int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) {
      if (i == argc-1) usage(argv[0]);
      ipaddr = argv[++i];
    } else
    if (!strcmp(argv[i], "-p")) {
      if (i == argc-1) usage(argv[0]);
      char* e = NULL;
      port = strtol(argv[++i], &e, 10);
      if (e && *e) usage(argv[0]);
    } else
    if (!strcmp(argv[i], "-d")) {
      if (i == argc-1) usage(argv[0]);
      static_dir = argv[++i];
    } else
      usage(argv[0]);
  }

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

  mime_type = kh_init(mime_type);
  add_mime_type(".jpg", "image/jpeg");
  add_mime_type(".png", "image/png");
  add_mime_type(".gif", "image/gif");
  add_mime_type(".html", "text/html");
  add_mime_type(".txt", "text/plain");

  r = uv_ip4_addr(ipaddr, port, &addr);
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

  fprintf(stderr, "Listening %s:%d\n", ipaddr, port);
  r = uv_listen((uv_stream_t*)&server, SOMAXCONN, on_connection);
  if (r) {
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 1;
  }

  return uv_run(loop, UV_RUN_DEFAULT);
}

/* vim:set et: */

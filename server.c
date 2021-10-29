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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <inttypes.h>
#include "server.h"
#include "khash.h"

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

#define WRITE_BUF_SIZE (8192/4)

#ifndef S_IREAD
#define S_IREAD _S_IREAD
#endif

#ifdef _WIN32
# define INVALID_FD (INVALID_HANDLE_VALUE)
#else
# define INVALID_FD (-1)
#endif

static uv_loop_t* loop;
static char* static_dir = "./public";
static int static_dir_len = -1;

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
static void on_read(uv_stream_t*, ssize_t, const uv_buf_t*);
static void on_close(uv_handle_t*);
static void on_connection(uv_stream_t*, int);
static void on_alloc(uv_handle_t*, size_t, uv_buf_t*);
static void on_fs_read(uv_fs_t*);
static void response_error(uv_handle_t*, int, const char*, const char*);

static void
destroy_request(http_request* request, int close_handle) {
  if (close_handle && request->handle) {
    uv_close((uv_handle_t*) request->handle, on_close);
  }
  free(request);
}

static void
destroy_response(http_response* response, int close_handle) {
  if (response->pbuf) free(response->pbuf);
  if (response->request) destroy_request(response->request, close_handle);
  if (response->fd != INVALID_FD) {
    uv_fs_t close_req;
    uv_fs_close(loop, &close_req, response->fd, NULL);
  }
  free(response);
}

static void
on_write(uv_write_t* req, int status) {
  http_response* response = (http_response*) req->data;

  if (status != 0) {
    fprintf(stderr, "Write error: %s: %s\n", uv_err_name(status), uv_strerror(status));
    destroy_response(response, 1);
    return;
  }

  if (response->response_offset >= response->response_size) {
    destroy_response(response, !response->request->keep_alive);
    return;
  }

  int r = uv_fs_read(loop, &response->read_req, response->fd, &response->buf, 1, response->response_offset, on_fs_read);
  if (r) {
    fprintf(stderr, "File read error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    response_error(response->handle, 500, "Internal Server Error", NULL);
    destroy_request(response->request, 1);
  }
}

static void
on_fs_open(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  ssize_t result = req->result;

  uv_fs_req_cleanup(req);
  free(req);
  if (result < 0) {
    fprintf(stderr, "Open error: %s: %s: %s\n", request->file_path, uv_err_name(result), uv_strerror(result));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }

  uv_fs_t stat_req;
  int r = uv_fs_fstat(loop, &stat_req, (uv_os_fd_t) result, NULL);
  if (r < 0) {
    fprintf(stderr, "Stat error: %s: %s: %s\n", request->file_path, uv_err_name(r), uv_strerror(r));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }

  uint64_t response_size = stat_req.statbuf.st_size;
  uv_fs_req_cleanup(&stat_req);

  const static char* ctype = "application/octet-stream";
  const char* dot = request->file_path;
  const char* ptr = dot;
  while (dot) {
    ptr = dot;
    dot = strchr(dot + 1, '.');
  }
  khint_t k = kh_get(mime_type, mime_type, ptr);
  if (k != kh_end(mime_type)) {
    ctype = kh_value(mime_type, k);
  }

  http_response* response = calloc(1, sizeof(http_response));
  if (response == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(r));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }
  response->response_size = response_size;
  response->fd = (uv_os_fd_t) result;
  response->request = request;
  response->handle = request->handle;
  response->pbuf = malloc(WRITE_BUF_SIZE);
  if (response->pbuf == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(r));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_response(response, 1);
    return;
  }
  response->buf = uv_buf_init(response->pbuf, WRITE_BUF_SIZE);
  response->read_req.data = response;
  response->write_req.data = response;

  char bufline[1024];
  int nbuf = snprintf(bufline,
      sizeof(bufline),
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %" PRId64 "\r\n"
      "Content-Type: %s\r\n"
      "Connection: %s\r\n"
      "\r\n",
      response_size,
      ctype,
      (request->keep_alive ? "keep-alive" : "close"));
  uv_buf_t buf = uv_buf_init(bufline, nbuf);

#ifndef _WIN32
  r = uv_try_write((uv_stream_t*) request->handle, &buf, 1);
  if (r == 0) {
    fprintf(stderr, "Write error\n");
    destroy_response(response, 1);
    return;
  }
#else
  r = uv_write(&response->write_req, (uv_stream_t*) request->handle, &buf, 1, NULL);
  if (r) {
    fprintf(stderr, "Write error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    destroy_response(response, 1);
    return;
  }
#endif

  r = uv_fs_read(loop, &response->read_req, (uv_os_fd_t) result, &response->buf, 1, -1, on_fs_read);
  if (r) {
    response_error(request->handle, 500, "Internal Server Error\n", NULL);
    destroy_response(response, 1);
  }
}

/*
static void
on_shutdown(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, on_close);
  free(req);
}
*/

static int
content_length(http_request* request) {
  int i;
  char buf[16];
  for (i = 0; i < request->num_headers; i++)
    if (!strncasecmp(request->headers[i].name, "content-length", request->headers[i].name_len)) {
      strncpy(buf, request->headers[i].value, request->headers[i].value_len);
      return atol(buf);
    }
  return -1;
}

static int
find_header_value(http_request* request, const char* name, const char* value) {
  int i;
  for (i = 0; i < request->num_headers; i++) {
#ifdef DEBUG
    printf("%.*s: %.*s\n",
      (int) request->headers[i].name_len, request->headers[i].name,
      (int) request->headers[i].value_len, request->headers[i].value);
#endif
    if (!strncasecmp(request->headers[i].name, name, request->headers[i].name_len) &&
        !strncasecmp(request->headers[i].value, value, request->headers[i].value_len))
      return 1;
  }
  return 0;
}

static void
request_complete(http_request* request) {
  memcpy(request->file_path, static_dir, static_dir_len);
  memcpy(request->file_path + static_dir_len, request->path, request->path_len);
  if (*(request->path + request->path_len - 1) == '/') {
    memcpy(request->file_path + static_dir_len + request->path_len, "index.html", 11);
  } else
    request->file_path[static_dir_len + request->path_len] = 0;
  request->keep_alive = find_header_value(request, "Connection", "keep-alive");

  uv_fs_t* open_req = malloc(sizeof(uv_fs_t));
  if (open_req == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return;
  }
  open_req->data = request;
  int r = uv_fs_open(loop, open_req, request->file_path, O_RDONLY, S_IREAD, on_fs_open);
  if (r) {
    fprintf(stderr, "Open error: %s: %s: %s\n", request->file_path, uv_err_name(r), uv_strerror(r));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    free(open_req);
  }
}

static void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  if (nread < 0) {
    if (buf->base)
      free(buf->base);
    /*
    uv_shutdown_t* shutdown_req = (uv_shutdown_t*) malloc(sizeof(uv_shutdown_t));
    if (shutdown_req == NULL) {
      fprintf(stderr, "Allocate error\n");
      return;
    }
    uv_shutdown(shutdown_req, stream, on_shutdown);
    */
    return;
  }

  if (nread == 0) {
    free(buf->base);
    return;
  }

  http_request* request = calloc(1, sizeof(http_request));
  if (request == NULL) {
    free(buf->base);
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    uv_close((uv_handle_t*) stream, on_close);
    return;
  }
  stream->data = request;

  request->handle = (uv_handle_t*) stream;

  request->num_headers = sizeof(request->headers) / sizeof(request->headers[0]);
  int nparsed = phr_parse_request(
          buf->base,
          buf->len,
          &request->method,
          &request->method_len,
          &request->path,
          &request->path_len,
          &request->minor_version,
          request->headers,
          &request->num_headers,
          0);
  if (nparsed < 0) {
    free(request);
    free(buf->base);
    fprintf(stderr, "Invalid request\n");
    uv_close((uv_handle_t*) stream, on_close);
    return;
  }
  /*
  int cl = content_length(request);
  if (cl >= 0 && cl < buf->len - nparsed) {
    free(request);
    free(buf->base);
    return;
  }
  */
  /* TODO: handle reading whole payload */
  request->payload = buf->base + nparsed;
  request->payload_len = buf->len - nparsed;
  request_complete(request);
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
  int nbuf = sprintf(bufline,
      "HTTP/1.0 %d %s\r\n"
      "Content-Length: %d\r\n"
      "Content-Type: text/plain; charset=UTF-8;\r\n"
      "\r\n"
      "%s", status_code, status, (int) strlen(ptr), ptr);
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return;
  }
  uv_buf_t buf = uv_buf_init(bufline, nbuf);
  int r = uv_write(write_req, (uv_stream_t*) handle, &buf, 1, on_write_error);
  if (r) {
    fprintf(stderr, "Write error %s: %s\n", uv_err_name(r), uv_strerror(r));
  }
}

static void
on_fs_read(uv_fs_t *req) {
  http_response* response = (http_response*) req->data;
  ssize_t result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "File read error: %s: %s\n", uv_err_name(result), uv_strerror(result));
    response_error(response->handle, 500, "Internal Server Error", NULL);
    destroy_response(response, 1);
    return;
  }

  uv_buf_t buf = uv_buf_init(response->pbuf, result);
  int r = uv_write(&response->write_req, (uv_stream_t*) response->handle, &buf, 1, on_write);
  if (r) {
    destroy_response(response, 1);
    return;
  }
  response->response_offset += result;
}

static void
on_connection(uv_stream_t* server, int status) {
  uv_stream_t* stream;
  int r;

  if (status != 0) {
    fprintf(stderr, "Connect error: %s: %s\n", uv_err_name(status), uv_strerror(status));
    return;
  }

  stream = malloc(sizeof(uv_tcp_t));
  if (stream == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return;
  }

  r = uv_tcp_init(loop, (uv_tcp_t*) stream);
  if (r) {
    fprintf(stderr, "Socket creation error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return;
  }

  r = uv_tcp_simultaneous_accepts((uv_tcp_t*) stream, 1);
  if (r) {
    fprintf(stderr, "Flag error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return;
  }

  r = uv_accept(server, stream);
  if (r) {
    fprintf(stderr, "Accept error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return;
  }

  r = uv_tcp_nodelay((uv_tcp_t*) stream, 1);
  if (r) {
    fprintf(stderr, "Flag error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return;
  }

  r = uv_read_start(stream, on_alloc, on_read);
  if (r) {
    fprintf(stderr, "Read error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    uv_close((uv_handle_t*) stream, on_close);
  }
}

static void
usage(const char* app) {
  fprintf(stderr, "usage: %s [OPTIONS]\n", app);
  fprintf(stderr, "    -a ADDR: address (default: 0.0.0.0)\n");
  fprintf(stderr, "    -p PORT: port number (default: 7000)\n");
  fprintf(stderr, "    -d DIR:  root directory (default: public)\n");
  exit(1);
}

static void
add_mime_type(const char* ext, const char* value) {
  int hr;
  khint_t k = kh_put(mime_type, mime_type, ext, &hr);
  kh_value(mime_type, k) = value;
}

static void
on_signal(uv_signal_t* handle, int signum) {
  uv_stop((uv_loop_t*) handle->data);
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
      if ((e && *e) || port < 0 || port > 65535) usage(argv[0]);
    } else
    if (!strcmp(argv[i], "-d")) {
      if (i == argc-1) usage(argv[0]);
      static_dir = argv[++i];
    } else
      usage(argv[0]);
  }
  static_dir_len = strlen(static_dir);

  struct sockaddr_in addr;
  int r;

  mime_type = kh_init(mime_type);
  add_mime_type(".jpg", "image/jpeg");
  add_mime_type(".png", "image/png");
  add_mime_type(".gif", "image/gif");
  add_mime_type(".html", "text/html");
  add_mime_type(".css", "text/css");
  add_mime_type(".txt", "text/plain");
  add_mime_type(".js", "text/javascript");

  r = uv_ip4_addr(ipaddr, port, &addr);
  if (r) {
    fprintf(stderr, "Address error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

  loop = uv_default_loop();

  uv_tcp_t server;
  r = uv_tcp_init(loop, &server);
  if (r) {
    fprintf(stderr, "Socket creation error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

  r = uv_tcp_bind(&server, (const struct sockaddr*) &addr, 0);
  if (r) {
    fprintf(stderr, "Bind error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

  r = uv_tcp_simultaneous_accepts((uv_tcp_t*) &server, 1);
  if (r) {
    fprintf(stderr, "Accept error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

  fprintf(stderr, "Listening %s:%d\n", ipaddr, port);

  r = uv_listen((uv_stream_t*)&server, SOMAXCONN, on_connection);
  if (r) {
    fprintf(stderr, "Listen error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

  uv_signal_t sig;
  r = uv_signal_init(loop, &sig);
  if (r) {
    fprintf(stderr, "Signal error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }
  sig.data = loop;
  r = uv_signal_start(&sig, on_signal, SIGINT);
  if (r) {
    fprintf(stderr, "Signal error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    return 1;
  }

#ifdef SIGPIPE
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  act.sa_flags = SA_RESTART;
  if (sigaction(SIGPIPE, &act, NULL)) {
    fprintf(stderr, "cannot ignore SIGPIPE\n");
    return 1;
  }
#endif

  return uv_run(loop, UV_RUN_DEFAULT);
}

/* vim:set et ts=2 sw=2 cino=>2: */

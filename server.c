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
#include <errno.h>
#include <string.h>
#include <strings.h>
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

/* A request head can arrive in pieces, so it is buffered until complete. This
 * caps how much a client can make us hold before it has sent a whole one. */
#define MAX_REQUEST_HEAD (64 * 1024)

/* Appended when the request target names a directory. */
#define INDEX_FILE "index.html"

/* Windows accepts both characters as path separators, so a request target
 * carrying backslashes has to be split on them there as well. */
#ifdef _WIN32
# define IS_PATH_SEP(c) ((c) == '/' || (c) == '\\')
#else
# define IS_PATH_SEP(c) ((c) == '/')
#endif

#ifndef S_IREAD
#define S_IREAD _S_IREAD
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
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

/* Every served file is kept in memory with both variants of its response
 * header rendered up front, so a hit costs one hash lookup and one write. */
typedef struct {
  char* path;
  char* body;
  size_t body_len;
  const char* ctype;
  char* header_keep_alive;
  size_t header_keep_alive_len;
  char* header_close;
  size_t header_close_len;
} file_cache_entry;
KHASH_MAP_INIT_STR(file_cache, file_cache_entry*)
static khash_t(file_cache)* file_cache;

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
static void on_write_error_free_buf(uv_write_t*, int);
static void on_read(uv_stream_t*, ssize_t, const uv_buf_t*);
static void on_close(uv_handle_t*);
static void on_connection(uv_stream_t*, int);
static void on_alloc(uv_handle_t*, size_t, uv_buf_t*);
static void response_error(uv_handle_t*, int, const char*, const char*);
static void respond_with_cache_entry(http_request*, file_cache_entry*);

/* Closing a handle twice aborts inside libuv, and with asserts off links it
 * into the closing queue twice so on_close() frees it twice, so every close
 * goes through here. */
static void
close_connection(uv_handle_t* handle) {
  if (handle && !uv_is_closing(handle))
    uv_close(handle, on_close);
}

static void
destroy_request(http_request* request, int close_handle) {
  if (request->handle) {
    http_connection* conn = (http_connection*) request->handle->data;
    if (conn)
      conn->request = NULL;
    if (close_handle)
      close_connection(request->handle);
  }
  free(request);
}

static void
destroy_response(http_response* response, int close_handle) {
  if (response->request) destroy_request(response->request, close_handle);
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
  destroy_response(response, !response->request->keep_alive);
}

static const char*
find_content_type(const char* path) {
  const char* ctype = "application/octet-stream";
  const char* dot = path;
  const char* ptr = dot;
  while (dot) {
    ptr = dot;
    dot = strchr(dot + 1, '.');
  }
  khint_t k = kh_get(mime_type, mime_type, ptr);
  if (k != kh_end(mime_type)) {
    ctype = kh_value(mime_type, k);
  }
  return ctype;
}

static void
destroy_file_cache_entry(file_cache_entry* entry) {
  if (entry == NULL) {
    return;
  }
  free(entry->path);
  free(entry->body);
  free(entry->header_keep_alive);
  free(entry->header_close);
  free(entry);
}

static file_cache_entry*
create_file_cache_entry(const char* path, const char* ctype, char* body, size_t body_len) {
  file_cache_entry* entry = calloc(1, sizeof(file_cache_entry));
  if (entry == NULL) {
    free(body);
    return NULL;
  }

  entry->path = strdup(path);
  if (entry->path == NULL) {
    free(body);
    free(entry);
    return NULL;
  }
  entry->body = body;
  entry->body_len = body_len;
  entry->ctype = ctype;

  size_t header_capacity = strlen(ctype) + 128;
  entry->header_keep_alive = malloc(header_capacity);
  entry->header_close = malloc(header_capacity);
  if (entry->header_keep_alive == NULL || entry->header_close == NULL) {
    destroy_file_cache_entry(entry);
    return NULL;
  }

  entry->header_keep_alive_len = snprintf(
      entry->header_keep_alive,
      header_capacity,
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %zu\r\n"
      "Content-Type: %s\r\n"
      "Connection: keep-alive\r\n"
      "\r\n",
      body_len,
      ctype);
  entry->header_close_len = snprintf(
      entry->header_close,
      header_capacity,
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %zu\r\n"
      "Content-Type: %s\r\n"
      "Connection: close\r\n"
      "\r\n",
      body_len,
      ctype);
  return entry;
}

static file_cache_entry*
load_file_cache_entry(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return NULL;
  }

  struct stat st;
  /* Opening a directory succeeds on Linux, but reading it fails afterwards,
   * by which point a 200 and a Content-Length would already have gone out. */
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return NULL;
  }

  size_t body_len = (size_t) st.st_size;
  char* body = NULL;
  if (body_len > 0) {
    body = malloc(body_len);
    if (body == NULL) {
      close(fd);
      return NULL;
    }

    size_t offset = 0;
    while (offset < body_len) {
      ssize_t nread = read(fd, body + offset, body_len - offset);
      if (nread <= 0) {
        free(body);
        close(fd);
        return NULL;
      }
      offset += (size_t) nread;
    }
  }

  close(fd);

  return create_file_cache_entry(path, find_content_type(path), body, body_len);
}

static file_cache_entry*
get_or_load_file_cache_entry(const char* path) {
  khint_t k = kh_get(file_cache, file_cache, path);
  if (k != kh_end(file_cache)) {
    return kh_value(file_cache, k);
  }

  file_cache_entry* entry = load_file_cache_entry(path);
  if (entry == NULL) {
    return NULL;
  }

  int absent = 0;
  k = kh_put(file_cache, file_cache, entry->path, &absent);
  if (absent < 0) {
    destroy_file_cache_entry(entry);
    return NULL;
  }
  kh_value(file_cache, k) = entry;
  return entry;
}

static void
respond_with_cache_entry(http_request* request, file_cache_entry* entry) {
  uv_buf_t bufs[2];
  size_t nbufs = 0;
  size_t total_len = 0;

  if (request->keep_alive) {
    bufs[nbufs++] = uv_buf_init(entry->header_keep_alive, (unsigned int) entry->header_keep_alive_len);
    total_len += entry->header_keep_alive_len;
  } else {
    bufs[nbufs++] = uv_buf_init(entry->header_close, (unsigned int) entry->header_close_len);
    total_len += entry->header_close_len;
  }
  /* A HEAD response is the header and nothing else. */
  if (!request->head_only && entry->body_len > 0) {
    bufs[nbufs++] = uv_buf_init(entry->body, (unsigned int) entry->body_len);
    total_len += entry->body_len;
  }

#ifndef _WIN32
  /* Header and body usually leave in one synchronous writev, which saves an
   * allocation and a trip round the loop per response.  The buffers point into
   * the cache entry, which outlives any partial write that has to be queued. */
  int written = uv_try_write((uv_stream_t*) request->handle, bufs, (unsigned int) nbufs);
  if (written == (int) total_len) {
    destroy_request(request, !request->keep_alive);
    return;
  }
  if (written < 0 && written != UV_EAGAIN) {
    fprintf(stderr, "Write error: %s: %s\n", uv_err_name(written), uv_strerror(written));
    destroy_request(request, 1);
    return;
  }
  if (written > 0) {
    size_t remaining = (size_t) written;
    size_t i;
    for (i = 0; i < nbufs && remaining > 0; i++) {
      if (remaining >= bufs[i].len) {
        remaining -= bufs[i].len;
        bufs[i].base += bufs[i].len;
        bufs[i].len = 0;
      } else {
        bufs[i].base += remaining;
        bufs[i].len -= (unsigned int) remaining;
        remaining = 0;
      }
    }
    while (nbufs > 0 && bufs[0].len == 0) {
      memmove(bufs, bufs + 1, sizeof(bufs[0]) * (nbufs - 1));
      nbufs--;
    }
  }
#endif

  http_response* response = malloc(sizeof(http_response));
  if (response == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    response_error(request->handle, 500, "Internal Server Error", NULL);
    destroy_request(request, 1);
    return;
  }

  response->request = request;
  response->handle = request->handle;
  response->write_req.data = response;

  int r = uv_write(&response->write_req, (uv_stream_t*) request->handle, bufs, (unsigned int) nbufs, on_write);
  if (r) {
    fprintf(stderr, "Write error: %s: %s\n", uv_err_name(r), uv_strerror(r));
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

/* The received name has to match in full.  Comparing only as many bytes as
 * arrived would make every prefix of the name match, so a header called "C"
 * would answer for "Connection". */
static int
header_name_is(const struct phr_header* header, const char* name) {
  size_t len = strlen(name);
  return header->name_len == len && !strncasecmp(header->name, name, len);
}

/* Connection is a comma separated list of tokens, so a value has to be matched
 * one token at a time rather than against the whole field. */
static int
header_has_token(const struct phr_header* header, const char* token) {
  size_t token_len = strlen(token);
  const char* p = header->value;
  const char* end = p + header->value_len;

  while (p < end) {
    const char* start;
    size_t len;

    while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
      p++;
    start = p;
    while (p < end && *p != ',')
      p++;
    len = p - start;
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
      len--;
    if (len == token_len && !strncasecmp(start, token, token_len))
      return 1;
  }
  return 0;
}

static int
content_length(http_request* request) {
  size_t i;
  char buf[16];
  for (i = 0; i < request->num_headers; i++)
    if (header_name_is(&request->headers[i], "content-length")) {
      size_t len = request->headers[i].value_len;
      if (len >= sizeof(buf)) len = sizeof(buf) - 1;
      memcpy(buf, request->headers[i].value, len);
      buf[len] = '\0';
      return atol(buf);
    }
  return -1;
}

static int
find_header_value(http_request* request, const char* name, const char* value) {
  size_t i;
  for (i = 0; i < request->num_headers; i++) {
#ifdef DEBUG
    printf("%.*s: %.*s\n",
      (int) request->headers[i].name_len, request->headers[i].name,
      (int) request->headers[i].value_len, request->headers[i].value);
#endif
    if (header_name_is(&request->headers[i], name) &&
        header_has_token(&request->headers[i], value))
      return 1;
  }
  return 0;
}

static int
hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Resolves the request target into request->file_path as a path below
 * static_dir.  Percent escapes are decoded, "." and ".." are resolved
 * lexically, and a target that would escape the document root or that does not
 * fit in file_path is rejected instead of truncated.  Returns 0 on success,
 * otherwise the HTTP status code to reject the request with.
 *
 * Decoding happens per segment and before the "."/".." checks, so an escaped
 * "%2e%2e" is resolved as a parent reference rather than reaching the file
 * system.  A separator that arrives escaped is refused outright: decoding it
 * in place would hand the kernel a separator the segment loop never saw. */
static int
build_file_path(http_request* request) {
  const char* p = request->path;
  const char* end = p + request->path_len;
  char* out;
  char* root_end;
  char* limit;

  if (p == end || !IS_PATH_SEP(*p))
    return 400;

  /* Reserve room for the longest suffix appended below: a separator, plus
   * INDEX_FILE and its terminating NUL. */
  limit = request->file_path + sizeof(request->file_path) - sizeof(INDEX_FILE) - 1;
  if (request->file_path + static_dir_len > limit)
    return 500;

  memcpy(request->file_path, static_dir, static_dir_len);
  out = root_end = request->file_path + static_dir_len;

  while (p < end) {
    const char* seg;
    char* seg_start;
    char* w;
    size_t seg_len;

    while (p < end && IS_PATH_SEP(*p)) p++;
    seg = p;
    while (p < end && !IS_PATH_SEP(*p)) p++;
    if (seg == p)
      break;

    /* Decode into the position the segment would occupy, leaving room for the
     * separator, and check the bound against what is actually written.  The
     * segment is only committed once it is known not to be "." or "..". */
    seg_start = out + 1;
    w = seg_start;
    while (seg < p) {
      int c = (unsigned char) *seg++;
      if (c == '%') {
        int hi, lo;
        if (p - seg < 2)
          return 400;
        hi = hex_value((unsigned char) seg[0]);
        lo = hex_value((unsigned char) seg[1]);
        if (hi < 0 || lo < 0)
          return 400;
        seg += 2;
        c = (hi << 4) | lo;
        if (c == 0 || IS_PATH_SEP(c))
          return 400;
      }
      if (w >= limit)
        return 414;
      *w++ = (char) c;
    }
    seg_len = w - seg_start;

    if (seg_len == 1 && seg_start[0] == '.')
      continue;
    if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
      if (out == root_end)
        return 404;
      /* Drop the segment appended last, along with its leading separator. */
      while (out > root_end && *--out != '/')
        ;
      continue;
    }
    *out = '/';
    out = w;
  }

  if (out == root_end || IS_PATH_SEP(end[-1])) {
    *out++ = '/';
    memcpy(out, INDEX_FILE, sizeof(INDEX_FILE));
  } else
    *out = 0;

  return 0;
}

static void
respond_status(http_request* request, int status_code) {
  const char* status;
  switch (status_code) {
  case 400: status = "Bad Request"; break;
  case 414: status = "URI Too Long"; break;
  case 500: status = "Internal Server Error"; break;
  case 501: status = "Not Implemented"; break;
  default:
    status_code = 404;
    status = "Not Found";
    break;
  }
  response_error(request->handle, status_code, status, NULL);
  destroy_request(request, 1);
}

static void
request_complete(http_request* request) {
  int status;

  if (request->method_len == 3 && !memcmp(request->method, "GET", 3))
    request->head_only = 0;
  else if (request->method_len == 4 && !memcmp(request->method, "HEAD", 4))
    request->head_only = 1;
  else {
    respond_status(request, 501);
    return;
  }

  status = build_file_path(request);
  if (status) {
    respond_status(request, status);
    return;
  }
  /* "close" overrides the version default either way, so it is checked first;
   * otherwise HTTP/1.1 is persistent and HTTP/1.0 has to opt in. */
  if (find_header_value(request, "Connection", "close"))
    request->keep_alive = 0;
  else if (request->minor_version >= 1)
    request->keep_alive = 1;
  else
    request->keep_alive = find_header_value(request, "Connection", "keep-alive");

  file_cache_entry* entry = get_or_load_file_cache_entry(request->file_path);
  if (entry == NULL) {
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }
  respond_with_cache_entry(request, entry);
}

static void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  http_connection* conn = (http_connection*) stream->data;

  if (nread < 0) {
    /* A connection with no request in flight has no other owner, so nothing
     * else would ever close it.  One that does is closed by the request or
     * response that owns it, once its write fails. */
    if (conn == NULL || conn->request == NULL)
      close_connection((uv_handle_t*) stream);
    return;
  }

  if (nread == 0) {
    return;
  }

  /* One request per connection at a time.  Anything arriving while one is in
   * flight -- a pipelined request, or garbage -- is ignored: serving it would
   * give the connection a second owner, and whichever finished first would
   * close the handle out from under the other. */
  if (conn == NULL || conn->request != NULL) {
    return;
  }

  if (conn->len + (size_t) nread > MAX_REQUEST_HEAD) {
    fprintf(stderr, "Request head too large\n");
    response_error((uv_handle_t*) stream, 431, "Request Header Fields Too Large", NULL);
    close_connection((uv_handle_t*) stream);
    return;
  }

  if (conn->len + (size_t) nread > conn->cap) {
    size_t cap = conn->cap ? conn->cap : 8192;
    char* grown;
    while (cap < conn->len + (size_t) nread)
      cap *= 2;
    grown = realloc(conn->buf, cap);
    if (grown == NULL) {
      fprintf(stderr, "Allocate error: %s\n", strerror(errno));
      close_connection((uv_handle_t*) stream);
      return;
    }
    conn->buf = grown;
    conn->cap = cap;
  }
  memcpy(conn->buf + conn->len, buf->base, (size_t) nread);
  conn->len += (size_t) nread;

  /* Parsed into locals first: an incomplete head has to be able to return
   * without having allocated a request. */
  const char* method;
  size_t method_len;
  const char* path;
  size_t path_len;
  int minor_version;
  struct phr_header headers[32];
  size_t num_headers = sizeof(headers) / sizeof(headers[0]);
  int nparsed = phr_parse_request(
          conn->buf,
          conn->len,
          &method,
          &method_len,
          &path,
          &path_len,
          &minor_version,
          headers,
          &num_headers,
          conn->last_len);
  if (nparsed == -2) {
    /* Not a whole head yet; keep it and wait for the rest. */
    conn->last_len = conn->len;
    return;
  }
  if (nparsed < 0) {
    conn->len = conn->last_len = 0;
    fprintf(stderr, "Invalid request\n");
    response_error((uv_handle_t*) stream, 400, "Bad Request", NULL);
    close_connection((uv_handle_t*) stream);
    return;
  }

  http_request* request = calloc(1, sizeof(http_request));
  if (request == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    close_connection((uv_handle_t*) stream);
    return;
  }
  request->handle = (uv_handle_t*) stream;
  request->method = method;
  request->method_len = method_len;
  request->path = path;
  request->path_len = path_len;
  request->minor_version = minor_version;
  memcpy(request->headers, headers, sizeof(headers));
  request->num_headers = num_headers;
  /* TODO: handle reading whole payload */
  request->payload = conn->buf + nparsed;
  request->payload_len = conn->len - (size_t) nparsed;

  /* From here on this request owns the connection.  Its path and headers point
   * into conn->buf, which stays allocated; the buffer is only rewritten by a
   * later read, and reads are ignored while a request is in flight. */
  conn->request = request;
  conn->len = conn->last_len = 0;
  request_complete(request);
}

static void on_close(uv_handle_t* peer) {
  http_connection* conn = (http_connection*) peer->data;
  if (conn) {
    free(conn->buf);
    free(conn);
  }
  free(peer);
}

static void
on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  http_connection* conn = (http_connection*) handle->data;
  (void) suggested_size;
  buf->base = conn->read_buf;
  buf->len = sizeof(conn->read_buf);
}

static void
on_write_error_free_buf(uv_write_t* req, int status) {
  (void) status;
  free(req->data);
  free(req);
}

static void
response_error(uv_handle_t* handle, int status_code, const char* status, const char* message) {
  const char* ptr = message ? message : status;
  char* bufline = malloc(1024);
  if (bufline == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return;
  }
  int nbuf = snprintf(bufline, 1024,
      "HTTP/1.0 %d %s\r\n"
      "Content-Length: %d\r\n"
      "Content-Type: text/plain; charset=UTF-8;\r\n"
      "\r\n"
      "%s", status_code, status, (int) strlen(ptr), ptr);
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    free(bufline);
    return;
  }
  write_req->data = bufline;
  uv_buf_t buf = uv_buf_init(bufline, nbuf);
  int r = uv_write(write_req, (uv_stream_t*) handle, &buf, 1, on_write_error_free_buf);
  if (r) {
    fprintf(stderr, "Write error %s: %s\n", uv_err_name(r), uv_strerror(r));
    free(bufline);
    free(write_req);
  }
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
  stream->data = calloc(1, sizeof(http_connection));
  if (stream->data == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    free(stream);
    return;
  }

  r = uv_tcp_init(loop, (uv_tcp_t*) stream);
  if (r) {
    fprintf(stderr, "Socket creation error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    free(stream->data);
    free(stream);
    return;
  }

  /* Accept before anything else can fail: returning from this callback without
   * having accepted makes libuv stop watching the listening socket, and only
   * uv_accept() ever starts it again, so the server would take no further
   * connections at all. */
  r = uv_accept(server, stream);
  if (r) {
    fprintf(stderr, "Accept error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    close_connection((uv_handle_t*) stream);
    return;
  }

  /* Neither flag is worth dropping a connection over. */
  r = uv_tcp_simultaneous_accepts((uv_tcp_t*) stream, 1);
  if (r)
    fprintf(stderr, "Flag error: %s: %s\n", uv_err_name(r), uv_strerror(r));

  r = uv_tcp_nodelay((uv_tcp_t*) stream, 1);
  if (r)
    fprintf(stderr, "Flag error: %s: %s\n", uv_err_name(r), uv_strerror(r));

  r = uv_read_start(stream, on_alloc, on_read);
  if (r) {
    fprintf(stderr, "Read error: %s: %s\n", uv_err_name(r), uv_strerror(r));
    close_connection((uv_handle_t*) stream);
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
  /* On failure kh_put returns kh_end(), which is not a slot to write to. */
  if (hr < 0) {
    fprintf(stderr, "Allocate error: %s\n", ext);
    return;
  }
  kh_value(mime_type, k) = value;
}

static void
on_signal(uv_signal_t* handle, int signum) {
  (void) signum;
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
      /* Range checked as a long, before narrowing: assigning to an int first
       * lets a value like 4294967296 truncate to something that passes. */
      const char* arg = argv[++i];
      char* e = NULL;
      long value;
      errno = 0;
      value = strtol(arg, &e, 10);
      if (e == arg || *e || errno != 0 || value < 0 || value > 65535)
        usage(argv[0]);
      port = (int) value;
    } else
    if (!strcmp(argv[i], "-d")) {
      if (i == argc-1) usage(argv[0]);
      static_dir = argv[++i];
    } else
      usage(argv[0]);
  }
  static_dir_len = strlen(static_dir);
  if (static_dir_len > (int) (PATH_MAX - sizeof(INDEX_FILE) - 1)) {
    fprintf(stderr, "Root directory too long: %s\n", static_dir);
    return 1;
  }

  struct sockaddr_in addr;
  int r;

  mime_type = kh_init(mime_type);
  if (mime_type == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return 1;
  }
  file_cache = kh_init(file_cache);
  if (file_cache == NULL) {
    fprintf(stderr, "Allocate error: %s\n", strerror(errno));
    return 1;
  }
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

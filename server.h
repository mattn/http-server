#ifndef _SERVER_H_
#define _SERVER_H_

#include <uv.h>
#include "http_parser.h"
#include "klist.h"
#include "khash.h"

/* header element */
typedef struct {
  char* key;
  char* value;
} header_elem;

/* element free for klist */
inline static header_elem_free(void* p) {
  header_elem* elem = (header_elem*) p;
  free(elem->key);
  free(elem->value);
}
KLIST_INIT(header, header_elem, header_elem_free);

typedef struct _http_request {
  struct http_parser_url url_handle;
  char* url;
  char* payload;
  char* path;
  uint64_t size;
  uint64_t offset;
  char* last_field;
  klist_t(header)* headers;
  void (*on_request_complete)(struct _http_request*);
  uv_handle_t* handle;
  int keep_alive;
} http_request;

typedef struct {
  uv_fs_t* open_req;
  uv_fs_t* read_req;
  char* pbuf;
  uv_buf_t buf;
  uv_handle_t* handle;
  int keep_alive;
  http_request* request;
} http_response;

#endif

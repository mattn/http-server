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
#ifndef _SERVER_H_
#define _SERVER_H_

#include <uv.h>
#include "http_parser.h"
#include "klist.h"
#include "khash.h"

/* header element */
typedef struct {
  const char* key_ptr;
  size_t key_len;
  const char* value_ptr;
  size_t value_len;
} header_elem;

inline static header_elem_free(void* p) {}
KLIST_INIT(header, header_elem, header_elem_free);

typedef struct _http_request {
  struct http_parser_url url_handle;
  const char* url_ptr;
  size_t url_len;
  char path[PATH_MAX];
  const char* payload_ptr;
  size_t payload_len;
  uint64_t size;
  uint64_t offset;
  const char* last_field_ptr;
  size_t last_field_len;
  klist_t(header)* headers;
  uv_handle_t* handle;
  int keep_alive;
  http_parser parser;
  void (*on_request_complete)(http_parser*, struct _http_request*);
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

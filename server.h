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
#include "picohttpparser.h"
#include "khash.h"

#define REQUEST_BUF(x) \
  const char* x ## _ptr; \
  size_t x ## _len;

typedef struct _http_request {
  uv_handle_t* handle;

  const char* method;
  size_t method_len;
  int minor_version;
  const char* path;
  size_t path_len;
  struct phr_header headers[32];
  size_t num_headers;
  size_t last_len;
  const char* payload;
  size_t payload_len;

  char file_path[PATH_MAX];

  int keep_alive;
} http_request;

#undef REQUEST_BUF

typedef struct {
  int fd;
  uv_write_t write_req;
  uv_fs_t read_req;
  char* pbuf;
  uv_buf_t buf;
  uv_handle_t* handle;
  int keep_alive;

  uint64_t response_size;
  uint64_t response_offset;

  http_request* request;
} http_response;

#endif

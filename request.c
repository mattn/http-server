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
#include "http_parser.h"
#include "server.h"
#include <stdio.h>
#include <memory.h>

int
on_message_begin(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->path[0] = 0;
  request->headers = kl_init(header);
  return 0;
}

int
on_headers_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  if (request->last_field_ptr != NULL) {
    request->last_field_ptr = NULL;
    request->last_field_len = 0;
  }
  return 0;
}

int
on_message_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->on_request_complete(parser, request);
  return 0;
}

int
on_url(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  if (http_parser_parse_url(at, length, 0, &request->url_handle) != 0) {
    fprintf(stderr, "Parse error %s\n", http_errno_description(parser->http_errno));
    return 1;
  }
  request->url_ptr = at;
  request->url_len = length;
  return 0;
}

int
on_header_field(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  request->last_field_ptr = at;
  request->last_field_len = length;
  return 0;
}

int
on_header_value(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  header_elem elem;
  elem.key_ptr = request->last_field_ptr;
  elem.key_len = request->last_field_len;
  request->last_field_ptr = NULL;
  request->last_field_len = 0;
  elem.value_ptr = at;
  elem.value_len = length;
  *kl_pushp(header, request->headers) = elem;
  return 0;
}

int
on_body(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  request->payload_ptr = at;
  request->payload_len = length;
  return 0;
}

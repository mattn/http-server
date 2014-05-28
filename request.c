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

int on_message_begin(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->size = 0;
  request->offset = 0;
  request->url = NULL;
  request->payload = NULL;
  request->last_field = NULL;
  request->headers = kl_init(header);
  return 0;
}

int on_headers_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  if (request->last_field != NULL) {
    free(request->last_field);
    request->last_field = NULL;
  }
  return 0;
}

int on_message_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->on_request_complete(parser, request);
  return 0;
}

int on_url(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  if (http_parser_parse_url(at, length, 0, &request->url_handle) != 0) {
    fprintf(stderr, "Parse error %s\n", http_errno_description(parser->http_errno));
    return 1;
  }
  request->url = malloc(length + 1);
  if (request->url == NULL) {
    fprintf(stderr, "Allocate error\n");
    return 1;
  }
  memcpy(request->url, at, length);
  *(request->url + length) = 0;
  return 0;
}

int on_header_field(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  request->last_field = malloc(length + 1);
  if (request->last_field == NULL) {
    fprintf(stderr, "Allocate error\n");
    return 1;
  }
  memset(request->last_field, 0, length + 1);
  memcpy(request->last_field, at, length);
  return 0;
}

int on_header_value(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  char* value = malloc(length + 1);
  if (value == NULL) {
    fprintf(stderr, "Allocate error\n");
    return 1;
  }
  memset(value, 0, length + 1);
  memcpy(value, at, length);
  header_elem elem;
  elem.key = request->last_field;
  request->last_field = NULL;
  elem.value = value;
  *kl_pushp(header, request->headers) = elem;
  return 0;
}

int on_body(http_parser* parser, const char* at, size_t length) {
  http_request* request = (http_request*) parser->data;
  char* payload = malloc(length + 1);
  if (payload == NULL) {
    fprintf(stderr, "Allocate error\n");
    return 1;
  }
  memset(payload, 0, length + 1);
  memcpy(payload, at, length);
  request->payload = payload;
  return 0;
}

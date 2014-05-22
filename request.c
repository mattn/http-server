#include "http_parser.h"
#include "server.h"
#include <stdio.h>
#include <memory.h>

int on_message_begin(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->size = 0;
  request->offset = 0;
  request->url = NULL;
  request->last_field = NULL;
  request->headers = kl_init(header);
  return 0;
}

int on_headers_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  if (request->last_field != NULL) {
	free(request->last_field);
	request->last_field;
  }
  return 0;
}

int on_message_complete(http_parser* parser) {
  http_request* request = (http_request*) parser->data;
  request->on_request_complete(request);
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
  elem.key = strdup(request->last_field);
  if (elem.key == NULL) {
    fprintf(stderr, "Allocate error\n");
    return 1;
  }
  elem.value = value;
  *kl_pushp(header, request->headers) = elem;
  free(request->last_field);
  request->last_field;
  return 0;
}

int on_body(http_parser* parser, const char* at, size_t length) {
  (void)parser;
  printf("Body: %.*s\n", (int)length, at);
  return 0;
}

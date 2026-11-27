#ifndef ASCIIRC_HTTP_H
#define ASCIIRC_HTTP_H

#include "net.h"
#include "util.h"

typedef struct {
	int status;
	Buf body;
	char *location;
} HttpResponse;

int http_request(const Url *u, const char *method, const char *path, const char *extra_headers,
		 const char *ctype, const char *body, size_t body_len, HttpResponse *resp, char *err, size_t errlen);
void http_response_free(HttpResponse *resp);

void url_encode(Buf *b, const char *s);

#endif

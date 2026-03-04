#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdio.h>

int http_client_get(const char *host, int port, const char *path, FILE *out);

#endif

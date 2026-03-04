#ifndef URL_PARSER_H
#define URL_PARSER_H

#include <stdint.h>
#include <stddef.h>

int url_parse(const char *url, char *host, size_t host_len, 
              char *path, size_t path_len, int *port);

#endif

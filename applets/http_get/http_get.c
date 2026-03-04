#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "url_parser.h"
#include "http_client.h"

static void usage(const char *prog)
{
  fprintf(stderr, "usage: %s <url> [-o file]\n", prog);
  fprintf(stderr, "  simple HTTP GET client\n");
}

int main(int argc, char **argv)
{
  char host[256];
  char path[1024];
  int port;
  int opt;
  const char *output_file = NULL;
  FILE *out = NULL;
  int rc;

  while((opt = getopt(argc, argv, "ho:")) != -1){
    switch(opt){
    case 'o':
      output_file = optarg;
      break;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  if(argc - optind < 1){
    usage(argv[0]);
    return 1;
  }

  const char *url = argv[optind];

  if(url_parse(url, host, sizeof(host), path, sizeof(path), &port) != 0){
    fprintf(stderr, "failed to parse URL\n");
    return 1;
  }

  printf("Fetching %s:%d%s ...\n", host, port, path);

  if(output_file != NULL){
    out = fopen(output_file, "wb");
    if(out == NULL){
      perror("fopen");
      return 1;
    }
  }

  rc = http_client_get(host, port, path, out);

  if(out != NULL)
    fclose(out);

  return rc;
}

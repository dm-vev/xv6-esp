xv6_applet(
  NAME http_get
  ENABLED ON
  SOURCES http_get.c url_parser.c http_client.c
  INSTALL_PATH /bin/http_get
)

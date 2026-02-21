xv6_applet(
  NAME wc
  ENABLED ON
  SOURCES wc.c
  BUILD_DEPS wc.c
  INSTALL_PATH /bin/wc
)

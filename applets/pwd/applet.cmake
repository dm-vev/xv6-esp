xv6_applet(
  NAME pwd
  ENABLED ON
  SOURCES pwd.c
  BUILD_DEPS pwd.c
  INSTALL_PATH /bin/pwd
)

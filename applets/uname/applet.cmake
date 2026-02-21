xv6_applet(
  NAME uname
  ENABLED ON
  SOURCES uname.c
  BUILD_DEPS uname.c
  INSTALL_PATH /bin/uname
)

xv6_applet(
  NAME fd_test
  ENABLED ON
  SOURCES fd_test.c
  BUILD_DEPS fd_test.c
  INSTALL_PATH /bin/fd_test
)

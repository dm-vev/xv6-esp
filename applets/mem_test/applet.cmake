xv6_applet(
  NAME mem_test
  ENABLED ON
  SOURCES mem_test.c
  BUILD_DEPS mem_test.c
  INSTALL_PATH /bin/mem_test
)

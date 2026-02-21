xv6_applet(
  NAME printenv
  ENABLED ON
  SOURCES printenv.c
  BUILD_DEPS printenv.c
  INSTALL_PATH /bin/printenv
)

xv6_applet(
  NAME sleep
  ENABLED ON
  SOURCES sleep.c
  BUILD_DEPS sleep.c
  INSTALL_PATH /bin/sleep
)

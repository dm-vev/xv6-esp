xv6_applet(
  NAME tee
  ENABLED ON
  SOURCES tee.c
  BUILD_DEPS tee.c
  INSTALL_PATH /bin/tee
)

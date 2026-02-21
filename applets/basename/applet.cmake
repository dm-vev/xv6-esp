xv6_applet(
  NAME basename
  ENABLED ON
  SOURCES basename.c
  BUILD_DEPS basename.c
  INSTALL_PATH /bin/basename
)

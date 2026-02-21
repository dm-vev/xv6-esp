xv6_applet(
  NAME cat
  ENABLED ON
  SOURCES cat.c
  BUILD_DEPS cat.c
  INSTALL_PATH /bin/cat
)

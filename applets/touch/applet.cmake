xv6_applet(
  NAME touch
  ENABLED ON
  SOURCES touch.c
  BUILD_DEPS touch.c
  INSTALL_PATH /bin/touch
)

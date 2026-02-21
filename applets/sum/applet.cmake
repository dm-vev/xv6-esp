xv6_applet(
  NAME sum
  ENABLED ON
  SOURCES sum.c
  BUILD_DEPS sum.c
  INSTALL_PATH /bin/sum
)

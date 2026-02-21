xv6_applet(
  NAME echo
  ENABLED ON
  SOURCES echo.c
  BUILD_DEPS echo.c
  INSTALL_PATH /bin/echo
)

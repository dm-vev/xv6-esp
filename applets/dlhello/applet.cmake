xv6_applet(
  NAME dlhello
  INSTALL_PATH /bin/dlhello
  SOURCES dlhello.c
  BUILD_DEPS xv6_libdemo_so
  RESOURCES ${XV6_LIBDEMO_SO}:/lib/libdemo.so
)

xv6_applet(
  NAME xargs
  ENABLED ON
  SOURCES xargs.c
  CFLAGS -Oz -fno-unwind-tables -fno-asynchronous-unwind-tables
  INSTALL_PATH /bin/xargs
)

xv6_applet(
  NAME fs_stress
  ENABLED ON
  SOURCES fs_stress.c
  BUILD_DEPS fs_stress.c
  INSTALL_PATH /bin/fs_stress
)

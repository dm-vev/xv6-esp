xv6_applet(
  NAME fs_stress_test
  ENABLED ON
  SOURCES fs_stress_test.c
  BUILD_DEPS fs_stress_test.c
  INSTALL_PATH /bin/fs_stress_test
)

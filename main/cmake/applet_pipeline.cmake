if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  include(${CMAKE_CURRENT_LIST_DIR}/applets.cmake)

  file(GLOB APPLET_MANIFESTS CONFIGURE_DEPENDS "${APPLET_SRC_DIR}/*/applet.cmake")
  if(NOT APPLET_MANIFESTS)
    message(FATAL_ERROR "No applet manifests found in ${APPLET_SRC_DIR}")
  endif()
  foreach(APPLET_MANIFEST IN LISTS APPLET_MANIFESTS)
    include(${APPLET_MANIFEST})
  endforeach()

  add_custom_command(
    OUTPUT ${MKFS_TOOL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
    COMMAND cc -Wno-unknown-attributes -I${PROJECT_DIR} -o ${MKFS_TOOL} ${PROJECT_DIR}/mkfs/mkfs.c
    DEPENDS ${PROJECT_DIR}/mkfs/mkfs.c ${PROJECT_DIR}/kernel/fs/fs.h ${PROJECT_DIR}/kernel/core/param.h
    VERBATIM
  )

  xv6_emit_applet_build_graph(
    APPLET_OUT_DIR "${APPLET_BUILD_DIR}"
    FSROOT_ROOT_DIR "${XV6FS_ROOT_DIR}"
    FSROOT_STAGE "${FSROOT_STAGE}"
    FSROOT_STAMP "${FSROOT_STAMP}"
    STAMP_DIR "${CMAKE_BINARY_DIR}/applet_stamps"
    DEFAULT_CFLAGS ${ESP_USER_FLAGS}
    FSROOT_SOURCE_FILES ${FSROOT_SOURCE_FILES}
    EXTRA_RESOURCES "${XV6_LIBC_SO}:/lib/libc.so"
  )
endif()

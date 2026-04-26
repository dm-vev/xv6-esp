if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  include(${CMAKE_CURRENT_LIST_DIR}/applets.cmake)

  file(GLOB APPLET_MANIFESTS CONFIGURE_DEPENDS "${APPLET_SRC_DIR}/*/applet.cmake")
  if(NOT APPLET_MANIFESTS)
    message(FATAL_ERROR "No applet manifests found in ${APPLET_SRC_DIR}")
  endif()
  file(GLOB_RECURSE APPLET_INCLUDE_FILES CONFIGURE_DEPENDS "${APPLET_INCLUDE_DIR}/*")
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

  set(_xv6_extra_resources
    "${XV6_LIBC_SO}:/lib/libc.so"
    "${XV6_NETKMOD_SO}:/lib/modules/netkmod.so"
  )
  if(CONFIG_IDF_TARGET_ESP32P4)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    set(_xv6_empty_modules_conf "${CMAKE_BINARY_DIR}/generated/modules.empty.conf")
    file(WRITE "${_xv6_empty_modules_conf}" "# ESP32-P4: netkmod autoload disabled until RISC-V module runtime is stable\n")
    list(APPEND _xv6_extra_resources "${_xv6_empty_modules_conf}:/etc/modules.conf")
  endif()
  if(XV6_WIFIMOD_SO)
    list(APPEND _xv6_extra_resources "${XV6_WIFIMOD_SO}:/lib/modules/wifimod.so")
  endif()
  if(XV6_ENABLE_RUST_POC)
    list(APPEND _xv6_extra_resources
      "${XV6_RUST_POC_APPLET_SO}:/bin/rust_poc"
      "${XV6_RUST_POC_KMOD_SO}:/lib/modules/rustpoc.so"
    )
  endif()

  xv6_emit_applet_build_graph(
    APPLET_OUT_DIR "${APPLET_BUILD_DIR}"
    FSROOT_ROOT_DIR "${XV6FS_ROOT_DIR}"
    FSROOT_STAGE "${FSROOT_STAGE}"
    FSROOT_STAMP "${FSROOT_STAMP}"
    STAMP_DIR "${CMAKE_BINARY_DIR}/applet_stamps"
    DEFAULT_CFLAGS ${ESP_USER_FLAGS}
    DEFAULT_DEPS ${APPLET_INCLUDE_FILES}
    FSROOT_SOURCE_FILES ${FSROOT_SOURCE_FILES}
    EXTRA_RESOURCES
      ${_xv6_extra_resources}
  )
endif()

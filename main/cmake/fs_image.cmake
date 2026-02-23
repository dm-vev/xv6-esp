if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  add_custom_command(
    OUTPUT ${XV6FS_IMAGE}
    COMMAND ${MKFS_TOOL} ${XV6FS_IMAGE} -s ${XV6FS_BLOCKS} --from-dir ${FSROOT_STAGE}
    DEPENDS ${MKFS_TOOL} ${FSROOT_STAMP}
    VERBATIM
  )

  add_custom_target(xv6fs_img_gen ALL DEPENDS ${XV6FS_IMAGE})
  add_dependencies(${COMPONENT_LIB} xv6fs_img_gen)

  idf_component_get_property(main_args esptool_py FLASH_ARGS)
  idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
  esptool_py_flash_target(xv6fs-flash "${main_args}" "${sub_args}" ALWAYS_PLAINTEXT)
  esptool_py_flash_to_partition(xv6fs-flash "xv6fs" "${XV6FS_IMAGE}")
  add_dependencies(xv6fs-flash xv6fs_img_gen)

  esptool_py_flash_to_partition(flash "xv6fs" "${XV6FS_IMAGE}")
  add_dependencies(flash xv6fs_img_gen)
endif()

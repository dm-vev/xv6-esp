set(XV6FS_IMAGE "${CMAKE_BINARY_DIR}/xv6fs.bin")

idf_component_register(
  SRCS
    "app_main.c"
    "../kernel/xv6_esp_boot.c"
    "../kernel/hal_esp32s3.c"
    "../kernel/esp_flash_disk.c"
    "../kernel/elf_loader.c"
    "../kernel/xv6fs_ro.c"
    "../kernel/ksh.c"
  INCLUDE_DIRS
    "."
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  set(XV6FS_BLOCKS 896)
  set(XV6FS_ROOT_DIR "${PROJECT_DIR}/fsroot")
  set(ESP_USER_DIR "${CMAKE_BINARY_DIR}/esp_user")
  set(FSROOT_STAGE "${CMAKE_BINARY_DIR}/fsroot_stage")
  set(FSROOT_BIN "${FSROOT_STAGE}/bin")
  set(MKFS_TOOL "${CMAKE_BINARY_DIR}/mkfs_host")
  set(ESP_USER_LS "${ESP_USER_DIR}/ls")
  set(ESP_USER_ECHO "${ESP_USER_DIR}/echo")
  set(ESP_USER_MEM "${ESP_USER_DIR}/mem")
  set(ESP_USER_TICKS "${ESP_USER_DIR}/ticks")
  set(ESP_USER_MKDIR "${ESP_USER_DIR}/mkdir")
  set(ESP_USER_WRITE "${ESP_USER_DIR}/write")
  set(ESP_USER_CAT "${ESP_USER_DIR}/cat")
  set(ESP_USER_RM "${ESP_USER_DIR}/rm")
  set(FSROOT_STAMP "${CMAKE_BINARY_DIR}/fsroot_stage.stamp")
  file(GLOB_RECURSE FSROOT_SOURCE_FILES CONFIGURE_DEPENDS "${XV6FS_ROOT_DIR}/*")
  set(ESP_USER_FLAGS
    -Os
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fPIC
    -nostdlib
    -Wl,-shared
    -Wl,-e,main
    -Wl,--unresolved-symbols=ignore-all
  )

  add_custom_command(
    OUTPUT ${MKFS_TOOL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
    COMMAND cc -Wno-unknown-attributes -I${PROJECT_DIR} -o ${MKFS_TOOL} ${PROJECT_DIR}/mkfs/mkfs.c
    DEPENDS ${PROJECT_DIR}/mkfs/mkfs.c ${PROJECT_DIR}/kernel/fs.h ${PROJECT_DIR}/kernel/param.h
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_LS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_LS} ${PROJECT_DIR}/esp_user/ls.c
    DEPENDS ${PROJECT_DIR}/esp_user/ls.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_ECHO}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_ECHO} ${PROJECT_DIR}/esp_user/echo.c
    DEPENDS ${PROJECT_DIR}/esp_user/echo.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_MEM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_MEM} ${PROJECT_DIR}/esp_user/mem.c
    DEPENDS ${PROJECT_DIR}/esp_user/mem.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_TICKS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_TICKS} ${PROJECT_DIR}/esp_user/ticks.c
    DEPENDS ${PROJECT_DIR}/esp_user/ticks.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_MKDIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_MKDIR} ${PROJECT_DIR}/esp_user/mkdir.c
    DEPENDS ${PROJECT_DIR}/esp_user/mkdir.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_WRITE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_WRITE} ${PROJECT_DIR}/esp_user/write.c
    DEPENDS ${PROJECT_DIR}/esp_user/write.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_CAT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_CAT} ${PROJECT_DIR}/esp_user/cat.c
    DEPENDS ${PROJECT_DIR}/esp_user/cat.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${ESP_USER_RM}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ESP_USER_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${ESP_USER_FLAGS} -o ${ESP_USER_RM} ${PROJECT_DIR}/esp_user/rm.c
    DEPENDS ${PROJECT_DIR}/esp_user/rm.c
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${FSROOT_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XV6FS_ROOT_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${FSROOT_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${XV6FS_ROOT_DIR} ${FSROOT_STAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FSROOT_BIN}
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_LS} ${FSROOT_BIN}/ls
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_ECHO} ${FSROOT_BIN}/echo
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_MEM} ${FSROOT_BIN}/mem
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_TICKS} ${FSROOT_BIN}/ticks
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_MKDIR} ${FSROOT_BIN}/mkdir
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_WRITE} ${FSROOT_BIN}/write
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_CAT} ${FSROOT_BIN}/cat
    COMMAND ${CMAKE_COMMAND} -E copy ${ESP_USER_RM} ${FSROOT_BIN}/rm
    COMMAND ${CMAKE_COMMAND} -E touch ${FSROOT_STAMP}
    DEPENDS ${ESP_USER_LS} ${ESP_USER_ECHO} ${ESP_USER_MEM} ${ESP_USER_TICKS} ${ESP_USER_MKDIR}
            ${ESP_USER_WRITE} ${ESP_USER_CAT} ${ESP_USER_RM} ${FSROOT_SOURCE_FILES}
    VERBATIM
  )

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

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
  set(APPLET_BUILD_DIR "${CMAKE_BINARY_DIR}/applets")
  set(APPLET_SRC_DIR "${PROJECT_DIR}/applets")
  set(APPLET_INCLUDE_DIR "${APPLET_SRC_DIR}/include")
  set(FSROOT_STAGE "${CMAKE_BINARY_DIR}/fsroot_stage")
  set(FSROOT_STAMP "${CMAKE_BINARY_DIR}/fsroot_stage.stamp")
  set(MKFS_TOOL "${CMAKE_BINARY_DIR}/mkfs_host")
  set(LIBC_SYMBOLS_C "${CMAKE_BINARY_DIR}/generated/libc_host_symbols.c")
  set(XV6_LIBDEMO_SO "${CMAKE_BINARY_DIR}/shared/libdemo.so")
  set(LIBC_SYMBOLS_GEN "${PROJECT_DIR}/scripts/gen_libc_symbols.py")
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libc.a
    OUTPUT_VARIABLE LIBC_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libm.a
    OUTPUT_VARIABLE LIBM_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libgcc.a
    OUTPUT_VARIABLE LIBGCC_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libnosys.a
    OUTPUT_VARIABLE LIBNOSYS_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  foreach(_lib_path IN ITEMS ${LIBC_A_PATH} ${LIBM_A_PATH} ${LIBGCC_A_PATH} ${LIBNOSYS_A_PATH})
    if(NOT EXISTS "${_lib_path}")
      message(FATAL_ERROR "toolchain archive not found: ${_lib_path}")
    endif()
  endforeach()
  if(NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()
  if(NOT CMAKE_NM)
    find_program(CMAKE_NM NAMES ${CMAKE_C_COMPILER_TARGET}-nm nm)
  endif()
  if(NOT CMAKE_NM)
    message(FATAL_ERROR "nm tool not found")
  endif()

  file(GLOB_RECURSE FSROOT_SOURCE_FILES CONFIGURE_DEPENDS "${XV6FS_ROOT_DIR}/*")

  set(ESP_USER_FLAGS
    -Os
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fPIC
    -nostdlib
    -I${APPLET_INCLUDE_DIR}
    -Wl,-shared
    -Wl,-e,main
    -Wl,--unresolved-symbols=ignore-all
  )

  add_custom_command(
    OUTPUT ${LIBC_SYMBOLS_C}
    COMMAND ${Python3_EXECUTABLE} ${LIBC_SYMBOLS_GEN}
            --nm ${CMAKE_NM}
            --lib ${LIBC_A_PATH}
            --lib ${LIBM_A_PATH}
            --lib ${LIBGCC_A_PATH}
            --lib ${LIBNOSYS_A_PATH}
            --out ${LIBC_SYMBOLS_C}
            --header ${PROJECT_DIR}/kernel/elf_loader.h
    DEPENDS ${LIBC_SYMBOLS_GEN} ${LIBC_A_PATH} ${LIBM_A_PATH} ${LIBGCC_A_PATH} ${LIBNOSYS_A_PATH}
    VERBATIM
  )
  set_source_files_properties(${LIBC_SYMBOLS_C} PROPERTIES GENERATED TRUE)
  target_sources(${COMPONENT_LIB} PRIVATE ${LIBC_SYMBOLS_C})

  include(${CMAKE_CURRENT_LIST_DIR}/cmake/applets.cmake)

  add_custom_command(
    OUTPUT ${XV6_LIBDEMO_SO}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shared"
    COMMAND ${CMAKE_C_COMPILER}
            -Os
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fPIC
            -nostdlib
            -I${APPLET_INCLUDE_DIR}
            -Wl,-shared
            -Wl,--unresolved-symbols=ignore-all
            -o ${XV6_LIBDEMO_SO}
            ${PROJECT_DIR}/applets/dlhello/libdemo.c
    DEPENDS ${PROJECT_DIR}/applets/dlhello/libdemo.c
    VERBATIM
  )
  add_custom_target(xv6_libdemo_so DEPENDS ${XV6_LIBDEMO_SO})

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
    DEPENDS ${PROJECT_DIR}/mkfs/mkfs.c ${PROJECT_DIR}/kernel/fs.h ${PROJECT_DIR}/kernel/param.h
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

  add_custom_target(qemu_smoke
    COMMAND ${PROJECT_DIR}/scripts/qemu_smoke_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_stress
    COMMAND ${PROJECT_DIR}/scripts/qemu_stress_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_soak
    COMMAND ${PROJECT_DIR}/scripts/qemu_soak_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_applets
    COMMAND ${PROJECT_DIR}/scripts/qemu_applets_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(hil_applets
    COMMAND ${PROJECT_DIR}/scripts/hil_applets_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(static_analysis
    COMMAND ${PROJECT_DIR}/scripts/static_analysis.sh
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )
endif()

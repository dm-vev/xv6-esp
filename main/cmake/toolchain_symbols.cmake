if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -fno-rtti -print-file-name=libc.a
    OUTPUT_VARIABLE LIBC_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -fno-rtti -print-file-name=libm.a
    OUTPUT_VARIABLE LIBM_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -fno-rtti -print-file-name=libnosys.a
    OUTPUT_VARIABLE LIBNOSYS_A_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -fno-rtti -print-file-name=libgcc.a
    OUTPUT_VARIABLE LIBGCC_A_PATH
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
  file(GLOB_RECURSE APPLET_INCLUDE_FILES CONFIGURE_DEPENDS "${APPLET_INCLUDE_DIR}/*")

  if(CONFIG_IDF_TARGET)
    set(XV6_TARGET_NODE "${CONFIG_IDF_TARGET}")
  else()
    set(XV6_TARGET_NODE "esp32")
  endif()
  if(CONFIG_IDF_TARGET_ARCH)
    set(XV6_TARGET_MACHINE "${CONFIG_IDF_TARGET_ARCH}-${XV6_TARGET_NODE}")
  else()
    set(XV6_TARGET_MACHINE "unknown-${XV6_TARGET_NODE}")
  endif()

  set(ESP_USER_FLAGS
    -Os
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fPIC
    -nostdlib
    -DFD_SETSIZE=512
    -I${APPLET_INCLUDE_DIR}
    -Wl,-shared
    -Wl,-e,main
    -Wl,--unresolved-symbols=ignore-all
    -DXV6_TARGET_NODE=\"${XV6_TARGET_NODE}\"
    -DXV6_TARGET_MACHINE=\"${XV6_TARGET_MACHINE}\"
  )

  add_custom_command(
    OUTPUT ${LIBC_SYMBOLS_C}
    COMMAND ${Python3_EXECUTABLE} ${LIBC_SYMBOLS_GEN}
            --nm ${CMAKE_NM}
            --lib ${LIBC_A_PATH}
            --lib ${LIBM_A_PATH}
            --lib ${LIBGCC_A_PATH}
            --lib ${LIBNOSYS_A_PATH}
            --needed-elf ${XV6_LIBC_SO}
            --needed-elf ${XV6_LIBDEMO_SO}
            --needed-elf-dir ${APPLET_BUILD_DIR}
            --out ${LIBC_SYMBOLS_C}
            --header ${PROJECT_DIR}/kernel/loader/elf_loader.h
    DEPENDS ${LIBC_SYMBOLS_GEN} ${LIBC_A_PATH} ${LIBM_A_PATH} ${LIBGCC_A_PATH} ${LIBNOSYS_A_PATH} ${FSROOT_STAMP}
    VERBATIM
  )
  set_source_files_properties(${LIBC_SYMBOLS_C} PROPERTIES GENERATED TRUE)
  target_sources(${COMPONENT_LIB} PRIVATE ${LIBC_SYMBOLS_C})
endif()

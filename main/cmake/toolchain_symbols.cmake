if(NOT CMAKE_BUILD_EARLY_EXPANSION)
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
            --header ${PROJECT_DIR}/kernel/loader/elf_loader.h
    DEPENDS ${LIBC_SYMBOLS_GEN} ${LIBC_A_PATH} ${LIBM_A_PATH} ${LIBGCC_A_PATH} ${LIBNOSYS_A_PATH}
    VERBATIM
  )
  set_source_files_properties(${LIBC_SYMBOLS_C} PROPERTIES GENERATED TRUE)
  target_sources(${COMPONENT_LIB} PRIVATE ${LIBC_SYMBOLS_C})
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
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

  add_custom_command(
    OUTPUT ${XV6_LIBDEMO_ARTIFACT}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy ${XV6_LIBDEMO_SO} ${XV6_LIBDEMO_ARTIFACT}
    DEPENDS xv6_libdemo_so
    VERBATIM
  )
  add_custom_target(xv6_libdemo_artifact ALL DEPENDS ${XV6_LIBDEMO_ARTIFACT})

  add_custom_command(
    OUTPUT ${XV6_LIBC_SO}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/lib"
    COMMAND ${CMAKE_C_COMPILER}
            -Os
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fPIC
            -nostdlib
            -shared
            -Wl,--unresolved-symbols=ignore-all
            -I${APPLET_INCLUDE_DIR}
            -o ${XV6_LIBC_SO}
            ${PROJECT_DIR}/applets/libc_shim/libc_shim.c
    DEPENDS ${PROJECT_DIR}/applets/libc_shim/libc_shim.c
    VERBATIM
  )
  add_custom_target(xv6_libc_so ALL DEPENDS ${XV6_LIBC_SO})
endif()

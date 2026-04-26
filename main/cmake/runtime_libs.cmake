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
            -DFD_SETSIZE=512
            -I${APPLET_INCLUDE_DIR}
            -Wl,-shared
            -Wl,--unresolved-symbols=ignore-all
            -o ${XV6_LIBDEMO_SO}
            ${PROJECT_DIR}/applets/dlhello/libdemo.c
    DEPENDS ${PROJECT_DIR}/applets/dlhello/libdemo.c ${APPLET_INCLUDE_FILES}
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
            -DFD_SETSIZE=512
            -I${APPLET_INCLUDE_DIR}
            -o ${XV6_LIBC_SO}
            ${PROJECT_DIR}/applets/libc_shim/libc_shim.c
    DEPENDS ${PROJECT_DIR}/applets/libc_shim/libc_shim.c ${APPLET_INCLUDE_FILES}
    VERBATIM
  )
  add_custom_target(xv6_libc_so ALL DEPENDS ${XV6_LIBC_SO})

  add_custom_command(
    OUTPUT ${XV6_NETKMOD_SO}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shared"
    COMMAND ${CMAKE_C_COMPILER}
            -Os
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fPIC
            -nostdlib
            -shared
            -Wl,--unresolved-symbols=ignore-all
            -DFD_SETSIZE=512
            -I${APPLET_INCLUDE_DIR}
            -o ${XV6_NETKMOD_SO}
            ${PROJECT_DIR}/kernel/modules/netkmod/netkmod.c
    DEPENDS ${PROJECT_DIR}/kernel/modules/netkmod/netkmod.c ${APPLET_INCLUDE_FILES}
    VERBATIM
  )
  add_custom_target(xv6_netkmod_so ALL DEPENDS ${XV6_NETKMOD_SO})

  set(XV6_WIFIMOD_SO "")
  if(CONFIG_SOC_WIFI_SUPPORTED)
    set(XV6_WIFIMOD_SO "${CMAKE_BINARY_DIR}/shared/wifimod.so")
    set(XV6_WIFIMOD_SRCS
        ${PROJECT_DIR}/kernel/modules/wifimod/wifimod.c
        ${PROJECT_DIR}/kernel/modules/wifimod/wifimod_state.c
        ${PROJECT_DIR}/kernel/modules/wifimod/wifimod_events.c
        ${PROJECT_DIR}/kernel/modules/wifimod/wifimod_wifi.c
    )

    add_library(xv6_wifimod_obj OBJECT ${XV6_WIFIMOD_SRCS})
    target_include_directories(xv6_wifimod_obj
      PRIVATE
        ${APPLET_INCLUDE_DIR}
        ${PROJECT_DIR}/kernel
        ${PROJECT_DIR}/kernel/modules/wifimod
    )
    target_compile_options(xv6_wifimod_obj
      PRIVATE
        -Os
        -g0
        -ffreestanding
        -fno-builtin
        -fno-stack-protector
        -fPIC
    )
    target_link_libraries(xv6_wifimod_obj
      PRIVATE
        idf::esp_wifi
        idf::esp_netif
        idf::esp_event
        idf::esp_hw_support
        idf::freertos
        idf::lwip
        idf::esp_system
        idf::log
        idf::esp_common
    )

    add_custom_command(
      OUTPUT ${XV6_WIFIMOD_SO}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shared"
      COMMAND ${CMAKE_C_COMPILER}
              -nostdlib
              -shared
              -Wl,-S
              -Wl,--unresolved-symbols=ignore-all
              -o ${XV6_WIFIMOD_SO}
              $<TARGET_OBJECTS:xv6_wifimod_obj>
      DEPENDS xv6_wifimod_obj
      COMMAND_EXPAND_LISTS
      VERBATIM
    )
    add_custom_target(xv6_wifimod_so ALL DEPENDS ${XV6_WIFIMOD_SO})
  else()
    message(STATUS "wifimod disabled: target has no Wi-Fi")
  endif()

  if(XV6_ENABLE_RUST_POC)
    find_program(XV6_RUSTC rustc)
    if(NOT XV6_RUSTC)
      message(FATAL_ERROR "XV6_ENABLE_RUST_POC=ON requires rustc in PATH")
    endif()

    set(_xv6_rust_common_flags
      --crate-type cdylib
      --edition=2021
      -C opt-level=z
      -C panic=abort
      -C relocation-model=pic
      -C link-arg=-shared
      -C link-arg=-nostdlib
      -C link-arg=-Wl,--unresolved-symbols=ignore-all
    )
    if(XV6_RUST_TARGET)
      list(APPEND _xv6_rust_common_flags --target=${XV6_RUST_TARGET})
    endif()
    if(XV6_RUST_FLAGS)
      separate_arguments(_xv6_rust_user_flags UNIX_COMMAND "${XV6_RUST_FLAGS}")
      list(APPEND _xv6_rust_common_flags ${_xv6_rust_user_flags})
    endif()

    add_custom_command(
      OUTPUT ${XV6_RUST_POC_APPLET_SO}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shared"
      COMMAND ${XV6_RUSTC}
              ${_xv6_rust_common_flags}
              -o ${XV6_RUST_POC_APPLET_SO}
              ${PROJECT_DIR}/applets/rust_poc/rust_poc.rs
      DEPENDS ${PROJECT_DIR}/applets/rust_poc/rust_poc.rs
      VERBATIM
    )
    add_custom_target(xv6_rust_poc_applet_so ALL DEPENDS ${XV6_RUST_POC_APPLET_SO})

    add_custom_command(
      OUTPUT ${XV6_RUST_POC_KMOD_SO}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shared"
      COMMAND ${XV6_RUSTC}
              ${_xv6_rust_common_flags}
              -o ${XV6_RUST_POC_KMOD_SO}
              ${PROJECT_DIR}/kernel/modules/rustpoc/rustpoc_kmod.rs
      DEPENDS ${PROJECT_DIR}/kernel/modules/rustpoc/rustpoc_kmod.rs
      VERBATIM
    )
    add_custom_target(xv6_rust_poc_kmod_so ALL DEPENDS ${XV6_RUST_POC_KMOD_SO})
  endif()
endif()

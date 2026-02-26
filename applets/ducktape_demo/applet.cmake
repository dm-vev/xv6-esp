set(_duktape_enabled OFF)
if(XV6_ENABLE_DUCKTAPE_DEMO)
  set(_duktape_enabled ON)
endif()

if(NOT _duktape_enabled)
  xv6_applet(
    NAME duktape_demo
    ENABLED OFF
    INSTALL_PATH /bin/duktape_demo
    SOURCES duktape_demo.c
  )
  return()
endif()

set(_duktape_dist_dir "")
set(_duktape_candidates)

if(XV6_DUCKTAPE_SOURCE_DIR)
  list(APPEND _duktape_candidates
    "${XV6_DUCKTAPE_SOURCE_DIR}"
    "${XV6_DUCKTAPE_SOURCE_DIR}/src")
endif()

list(APPEND _duktape_candidates
  "${PROJECT_DIR}/third_party/duktape"
  "${PROJECT_DIR}/third_party/duktape/src")

foreach(_cand IN LISTS _duktape_candidates)
  if(EXISTS "${_cand}/duktape.c" AND EXISTS "${_cand}/duktape.h")
    set(_duktape_dist_dir "${_cand}")
    break()
  endif()
endforeach()

set(_lib_cflags
  -Os
  -ffreestanding
  -fno-builtin
  -fno-stack-protector
  -fPIC
  -nostdlib
  -shared
  -Wl,--unresolved-symbols=ignore-all
  -I${APPLET_INCLUDE_DIR}
  -I${CMAKE_CURRENT_LIST_DIR}
)

set(_engine_so "${APPLET_BUILD_DIR}/dt_eng.so")
set(_mod_math_so "${APPLET_BUILD_DIR}/dt_math.so")
set(_mod_meta_so "${APPLET_BUILD_DIR}/dt_meta.so")

if(_duktape_dist_dir)
  message(STATUS "duktape demo: using real Duktape sources from ${_duktape_dist_dir}")
  set(_engine_sources
    "${CMAKE_CURRENT_LIST_DIR}/duktape_engine_real.c"
    "${_duktape_dist_dir}/duktape.c")
  set(_engine_cflags -I${_duktape_dist_dir})
  set(_engine_deps
    "${CMAKE_CURRENT_LIST_DIR}/duktape_engine_real.c"
    "${_duktape_dist_dir}/duktape.c"
    "${_duktape_dist_dir}/duktape.h")
else()
  message(WARNING "duktape demo: duktape.c/duktape.h not found; building stub engine")
  set(_engine_sources "${CMAKE_CURRENT_LIST_DIR}/duktape_engine_stub.c")
  set(_engine_cflags)
  set(_engine_deps "${CMAKE_CURRENT_LIST_DIR}/duktape_engine_stub.c")
endif()

add_custom_command(
  OUTPUT "${_engine_so}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${APPLET_BUILD_DIR}"
  COMMAND ${CMAKE_C_COMPILER} ${_lib_cflags} ${_engine_cflags} -o "${_engine_so}" ${_engine_sources}
  DEPENDS ${_engine_deps} "${CMAKE_CURRENT_LIST_DIR}/ducktape_engine_api.h"
  VERBATIM
)
add_custom_target(xv6_duktape_engine_so DEPENDS "${_engine_so}")

add_custom_command(
  OUTPUT "${_mod_math_so}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${APPLET_BUILD_DIR}"
  COMMAND ${CMAKE_C_COMPILER} ${_lib_cflags} -o "${_mod_math_so}" "${CMAKE_CURRENT_LIST_DIR}/duktape_mod_math.c"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/duktape_mod_math.c" "${CMAKE_CURRENT_LIST_DIR}/ducktape_engine_api.h"
  VERBATIM
)
add_custom_target(xv6_duktape_mod_math_so DEPENDS "${_mod_math_so}")

add_custom_command(
  OUTPUT "${_mod_meta_so}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${APPLET_BUILD_DIR}"
  COMMAND ${CMAKE_C_COMPILER} ${_lib_cflags} -o "${_mod_meta_so}" "${CMAKE_CURRENT_LIST_DIR}/duktape_mod_meta.c"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/duktape_mod_meta.c" "${CMAKE_CURRENT_LIST_DIR}/ducktape_engine_api.h"
  VERBATIM
)
add_custom_target(xv6_duktape_mod_meta_so DEPENDS "${_mod_meta_so}")

xv6_applet(
  NAME duktape_demo
  ENABLED ON
  INSTALL_PATH /bin/duktape_demo
  SOURCES duktape_demo.c
  BUILD_DEPS
    xv6_duktape_engine_so
    xv6_duktape_mod_math_so
    xv6_duktape_mod_meta_so
  RESOURCES
    ${_engine_so}:/lib/duktape/dt_eng.so
    ${_mod_math_so}:/lib/duktape/dt_math.so
    ${_mod_meta_so}:/lib/duktape/dt_meta.so
)

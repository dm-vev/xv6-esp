include(CMakeParseArguments)

function(_xv6_applet_prop_key applet_name out_key)
  string(MAKE_C_IDENTIFIER "${applet_name}" _applet_id)
  set(${out_key} "XV6_APPLET_${_applet_id}" PARENT_SCOPE)
endfunction()

macro(xv6_applet)
  xv6_register_applet(MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}" ${ARGV})
endmacro()

function(xv6_register_applet)
  cmake_parse_arguments(APP "" "NAME;ENABLED;INSTALL_PATH;MANIFEST_DIR" "SOURCES;BUILD_DEPS;RUNTIME_DEPS;RESOURCES;CFLAGS;LDFLAGS" ${ARGN})

  if("${APP_NAME}" STREQUAL "")
    message(FATAL_ERROR "xv6_register_applet: NAME is required")
  endif()
  if("${APP_INSTALL_PATH}" STREQUAL "")
    message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): INSTALL_PATH is required")
  endif()
  if("${APP_SOURCES}" STREQUAL "")
    message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): SOURCES is required")
  endif()
  if(NOT APP_MANIFEST_DIR)
    set(APP_MANIFEST_DIR "${PROJECT_DIR}")
  endif()
  if("${APP_ENABLED}" STREQUAL "")
    set(APP_ENABLED "ON")
  endif()

  string(TOUPPER "${APP_ENABLED}" _enabled_upper)
  if(NOT _enabled_upper STREQUAL "ON" AND NOT _enabled_upper STREQUAL "OFF")
    message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): ENABLED must be ON or OFF")
  endif()

  string(SUBSTRING "${APP_INSTALL_PATH}" 0 1 _install_prefix)
  if(NOT _install_prefix STREQUAL "/")
    message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): INSTALL_PATH must start with '/'")
  endif()

  get_property(_existing_names GLOBAL PROPERTY XV6_APPLET_NAMES)
  list(FIND _existing_names "${APP_NAME}" _dup_name_idx)
  if(NOT _dup_name_idx EQUAL -1)
    message(FATAL_ERROR "Duplicate applet NAME detected: ${APP_NAME}")
  endif()

  set(_source_files)
  foreach(_src IN LISTS APP_SOURCES)
    if(IS_ABSOLUTE "${_src}")
      set(_src_abs "${_src}")
    else()
      set(_src_abs "${APP_MANIFEST_DIR}/${_src}")
    endif()
    if(NOT EXISTS "${_src_abs}")
      message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): source not found: ${_src_abs}")
    endif()
    list(APPEND _source_files "${_src_abs}")
  endforeach()

  set(_build_deps)
  foreach(_dep IN LISTS APP_BUILD_DEPS)
    if(TARGET "${_dep}")
      list(APPEND _build_deps "${_dep}")
    elseif(IS_ABSOLUTE "${_dep}")
      if(NOT EXISTS "${_dep}")
        message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): BUILD_DEPS path not found: ${_dep}")
      endif()
      list(APPEND _build_deps "${_dep}")
    elseif(EXISTS "${APP_MANIFEST_DIR}/${_dep}")
      list(APPEND _build_deps "${APP_MANIFEST_DIR}/${_dep}")
    else()
      list(APPEND _build_deps "${_dep}")
    endif()
  endforeach()

  set(_resource_pairs)
  foreach(_resource IN LISTS APP_RESOURCES)
    string(FIND "${_resource}" ":" _sep_idx)
    if(_sep_idx LESS 1)
      message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): RESOURCES entry must be src:dst, got '${_resource}'")
    endif()

    string(SUBSTRING "${_resource}" 0 ${_sep_idx} _resource_src)
    math(EXPR _dst_start "${_sep_idx} + 1")
    string(SUBSTRING "${_resource}" ${_dst_start} -1 _resource_dst)

    if(IS_ABSOLUTE "${_resource_src}")
      set(_resource_src_abs "${_resource_src}")
    else()
      set(_resource_src_abs "${APP_MANIFEST_DIR}/${_resource_src}")
    endif()
    if(NOT EXISTS "${_resource_src_abs}")
      message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): resource source not found: ${_resource_src_abs}")
    endif()

    string(SUBSTRING "${_resource_dst}" 0 1 _resource_dst_prefix)
    if(NOT _resource_dst_prefix STREQUAL "/")
      message(FATAL_ERROR "xv6_register_applet(${APP_NAME}): resource destination must start with '/': ${_resource_dst}")
    endif()

    list(APPEND _resource_pairs "${_resource_src_abs}:${_resource_dst}")
  endforeach()

  _xv6_applet_prop_key("${APP_NAME}" _prop_key)
  set_property(GLOBAL APPEND PROPERTY XV6_APPLET_NAMES "${APP_NAME}")
  set_property(GLOBAL PROPERTY "${_prop_key}_ENABLED" "${_enabled_upper}")
  set_property(GLOBAL PROPERTY "${_prop_key}_INSTALL_PATH" "${APP_INSTALL_PATH}")
  set_property(GLOBAL PROPERTY "${_prop_key}_MANIFEST_DIR" "${APP_MANIFEST_DIR}")
  set_property(GLOBAL PROPERTY "${_prop_key}_SOURCES" "${_source_files}")
  set_property(GLOBAL PROPERTY "${_prop_key}_BUILD_DEPS" "${_build_deps}")
  set_property(GLOBAL PROPERTY "${_prop_key}_RUNTIME_DEPS" "${APP_RUNTIME_DEPS}")
  set_property(GLOBAL PROPERTY "${_prop_key}_RESOURCES" "${_resource_pairs}")
  set_property(GLOBAL PROPERTY "${_prop_key}_CFLAGS" "${APP_CFLAGS}")
  set_property(GLOBAL PROPERTY "${_prop_key}_LDFLAGS" "${APP_LDFLAGS}")
endfunction()

function(xv6_emit_applet_build_graph)
  cmake_parse_arguments(ARG "" "APPLET_OUT_DIR;FSROOT_ROOT_DIR;FSROOT_STAGE;FSROOT_STAMP;STAMP_DIR" "DEFAULT_CFLAGS;FSROOT_SOURCE_FILES" ${ARGN})

  if(NOT ARG_APPLET_OUT_DIR OR NOT ARG_FSROOT_ROOT_DIR OR NOT ARG_FSROOT_STAGE OR NOT ARG_FSROOT_STAMP)
    message(FATAL_ERROR "xv6_emit_applet_build_graph: required args missing")
  endif()
  if(NOT ARG_STAMP_DIR)
    set(ARG_STAMP_DIR "${CMAKE_BINARY_DIR}/applet_stamps")
  endif()

  get_property(_applet_names GLOBAL PROPERTY XV6_APPLET_NAMES)
  if(NOT _applet_names)
    message(FATAL_ERROR "No applets were registered")
  endif()

  set(_enabled_install_paths)
  set(_enabled_install_names)
  foreach(_name IN LISTS _applet_names)
    _xv6_applet_prop_key("${_name}" _prop_key)
    get_property(_enabled GLOBAL PROPERTY "${_prop_key}_ENABLED")
    get_property(_install_path GLOBAL PROPERTY "${_prop_key}_INSTALL_PATH")
    if(_enabled STREQUAL "ON")
      list(FIND _enabled_install_paths "${_install_path}" _path_idx)
      if(NOT _path_idx EQUAL -1)
        list(GET _enabled_install_names ${_path_idx} _owner)
        message(FATAL_ERROR "Duplicate INSTALL_PATH '${_install_path}' for applets '${_owner}' and '${_name}'")
      endif()
      list(APPEND _enabled_install_paths "${_install_path}")
      list(APPEND _enabled_install_names "${_name}")
    endif()
  endforeach()

  set(_base_stamp "${ARG_STAMP_DIR}/base.stamp")
  add_custom_command(
    OUTPUT "${_base_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_STAMP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_FSROOT_ROOT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${ARG_FSROOT_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ARG_FSROOT_ROOT_DIR}" "${ARG_FSROOT_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_base_stamp}"
    DEPENDS ${ARG_FSROOT_SOURCE_FILES}
    VERBATIM
  )

  set(_stage_stamps)
  foreach(_name IN LISTS _applet_names)
    _xv6_applet_prop_key("${_name}" _prop_key)
    get_property(_enabled GLOBAL PROPERTY "${_prop_key}_ENABLED")
    get_property(_install_path GLOBAL PROPERTY "${_prop_key}_INSTALL_PATH")
    get_property(_sources GLOBAL PROPERTY "${_prop_key}_SOURCES")
    get_property(_build_deps GLOBAL PROPERTY "${_prop_key}_BUILD_DEPS")
    get_property(_runtime_deps GLOBAL PROPERTY "${_prop_key}_RUNTIME_DEPS")
    get_property(_resources GLOBAL PROPERTY "${_prop_key}_RESOURCES")
    get_property(_cflags GLOBAL PROPERTY "${_prop_key}_CFLAGS")
    get_property(_ldflags GLOBAL PROPERTY "${_prop_key}_LDFLAGS")

    if(NOT _enabled STREQUAL "ON")
      message(STATUS "applet disabled: ${_name}")
      continue()
    endif()

    set(_applet_out "${ARG_APPLET_OUT_DIR}/${_name}")
    get_filename_component(_applet_out_dir "${_applet_out}" DIRECTORY)

    add_custom_command(
      OUTPUT "${_applet_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_applet_out_dir}"
      COMMAND ${CMAKE_C_COMPILER} ${ARG_DEFAULT_CFLAGS} ${_cflags} ${_ldflags} -o "${_applet_out}" ${_sources}
      DEPENDS ${_sources} ${_build_deps}
      VERBATIM
    )

    string(REGEX REPLACE "^/" "" _install_rel "${_install_path}")
    set(_install_abs "${ARG_FSROOT_STAGE}/${_install_rel}")
    get_filename_component(_install_dir "${_install_abs}" DIRECTORY)
    set(_install_stamp "${ARG_STAMP_DIR}/applet_${_name}.stamp")

    set(_install_depends "${_base_stamp}" "${_applet_out}")
    set(_resource_commands)
    foreach(_resource_pair IN LISTS _resources)
      string(FIND "${_resource_pair}" ":" _sep_idx)
      string(SUBSTRING "${_resource_pair}" 0 ${_sep_idx} _res_src)
      math(EXPR _res_dst_start "${_sep_idx} + 1")
      string(SUBSTRING "${_resource_pair}" ${_res_dst_start} -1 _res_dst)
      string(REGEX REPLACE "^/" "" _res_rel "${_res_dst}")
      set(_res_abs "${ARG_FSROOT_STAGE}/${_res_rel}")
      get_filename_component(_res_dir "${_res_abs}" DIRECTORY)

      list(APPEND _resource_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${_res_dir}")
      list(APPEND _resource_commands COMMAND ${CMAKE_COMMAND} -E copy "${_res_src}" "${_res_abs}")
      list(APPEND _install_depends "${_res_src}")
    endforeach()

    foreach(_runtime_dep IN LISTS _runtime_deps)
      string(SUBSTRING "${_runtime_dep}" 0 1 _runtime_dep_prefix)
      if(_runtime_dep_prefix STREQUAL "/")
        list(FIND _enabled_install_paths "${_runtime_dep}" _runtime_dep_idx)
        if(NOT _runtime_dep_idx EQUAL -1)
          list(GET _enabled_install_names ${_runtime_dep_idx} _runtime_dep_name)
          list(APPEND _install_depends "${ARG_STAMP_DIR}/applet_${_runtime_dep_name}.stamp")
        else()
          string(REGEX REPLACE "^/" "" _runtime_dep_rel "${_runtime_dep}")
          if(NOT EXISTS "${ARG_FSROOT_ROOT_DIR}/${_runtime_dep_rel}")
            message(FATAL_ERROR "applet ${_name}: RUNTIME_DEPS file not found in fsroot: ${_runtime_dep}")
          endif()
        endif()
      else()
        if(NOT EXISTS "${ARG_FSROOT_ROOT_DIR}/${_runtime_dep}")
          message(FATAL_ERROR "applet ${_name}: RUNTIME_DEPS file not found in fsroot: ${_runtime_dep}")
        endif()
      endif()
    endforeach()

    add_custom_command(
      OUTPUT "${_install_stamp}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_install_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy "${_applet_out}" "${_install_abs}"
      ${_resource_commands}
      COMMAND ${CMAKE_COMMAND} -E touch "${_install_stamp}"
      DEPENDS ${_install_depends}
      VERBATIM
    )

    list(APPEND _stage_stamps "${_install_stamp}")
    message(STATUS "applet enabled: ${_name} -> ${_install_path}")
  endforeach()

  add_custom_command(
    OUTPUT "${ARG_FSROOT_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E touch "${ARG_FSROOT_STAMP}"
    DEPENDS "${_base_stamp}" ${_stage_stamps}
    VERBATIM
  )
endfunction()

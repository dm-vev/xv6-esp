set(XV6FS_IMAGE "${CMAKE_BINARY_DIR}/xv6fs.bin")

idf_component_register(
  SRCS
    "app_main.c"
    "../kernel/platform/xv6_esp_boot.c"
    "../kernel/platform/hal_esp32s3.c"
    "../kernel/platform/esp_flash_disk.c"
    "../kernel/loader/elf_loader_state.c"
    "../kernel/loader/elf_loader_segments.c"
    "../kernel/loader/elf_loader_symbols.c"
    "../kernel/loader/elf_loader_reloc.c"
    "../kernel/loader/elf_loader_module.c"
    "../kernel/loader/elf_loader_dlfcn.c"
    "../kernel/hostabi/hostabi_dirent.c"
    "../kernel/hostabi/exports_registry.c"
    "../kernel/hostabi/hostabi_posix_fs.c"
    "../kernel/hostabi/hostabi_posix_io.c"
    "../kernel/hostabi/hostabi_pty.c"
    "../kernel/modules/module_manager_state.c"
    "../kernel/modules/module_manager_parse.c"
    "../kernel/modules/module_manager_load.c"
    "../kernel/modules/module_manager_ops.c"
    "../kernel/modules/module_manager_manifest.c"
    "../kernel/vfs/vfs.c"
    "../kernel/vfs/task_ctx.c"
    "../kernel/vfs/vfs_fd.c"
    "../kernel/vfs/vfs_pty.c"
    "../kernel/vfs/vfs_dev.c"
    "../kernel/runtime/shell_runtime.c"
    "../kernel/runtime/shell_parse.c"
  INCLUDE_DIRS
    "."
    "../kernel"
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  option(XV6_ENABLE_DUCKTAPE_DEMO "Build optional Duktape demo applet" OFF)
  set(XV6_DUCKTAPE_SOURCE_DIR "" CACHE PATH "Path to Duktape dist source dir containing duktape.c and duktape.h")
  option(XV6_ENABLE_RUST_POC "Build optional proof-of-idea Rust applet and kernel module" OFF)
  set(XV6_RUST_TARGET "" CACHE STRING "Rust target triple for optional Rust PoC artifacts (empty means host rustc target)")
  set(XV6_RUST_FLAGS "" CACHE STRING "Extra rustc flags for optional Rust PoC artifacts")

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
  set(XV6_LIBDEMO_ARTIFACT "${CMAKE_BINARY_DIR}/lib/libdemo.so")
  set(XV6_LIBC_SO "${CMAKE_BINARY_DIR}/lib/libc.so")
  set(XV6_NETKMOD_SO "${CMAKE_BINARY_DIR}/shared/netkmod.so")
  set(XV6_RUST_POC_APPLET_SO "${CMAKE_BINARY_DIR}/shared/rust_poc.so")
  set(XV6_RUST_POC_KMOD_SO "${CMAKE_BINARY_DIR}/shared/rustpoc.so")
  set(LIBC_SYMBOLS_GEN "${PROJECT_DIR}/scripts/gen_libc_symbols.py")
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain_symbols.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/runtime_libs.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/applet_pipeline.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/fs_image.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/test_targets.cmake)
endif()

# Runtime Layers (ESP32-S3 Port)

This document defines target boundaries for the runtime architecture.

## Layers

1. `shell`:
   - Interactive shell UX and command semantics.
   - Parsing, redirections, pipelines, jobs, builtins.
   - Must not own low-level POSIX/ELF export registry.

2. `hostabi`:
   - Host symbol/export surface for ELF applets.
   - POSIX/newlib bridge (`open/read/write`, `fcntl/ioctl`, `dirent`, `termios`, `pty`).
   - Reentrant wrappers (`_read_r`, `_write_r`, ...).

3. `vfs`:
   - FD table, inode/file/device abstraction, `/dev/*`.
   - Pipe and PTY data path.
   - Task-local stdio/cwd mapping.

4. `build-packaging`:
   - Applet compile graph.
   - Runtime shared libs (`libc.so`, optional demos).
   - fsroot staging + `mkfs`.
   - flash/test targets.

## Current transitional state

- Build/packaging has been split into CMake modules under `main/cmake/`:
  - `toolchain_symbols.cmake`
  - `runtime_libs.cmake`
  - `applet_pipeline.cmake`
  - `fs_image.cmake`
  - `test_targets.cmake`
- Runtime code is still partially concentrated in `kernel/shell/ksh.c` and `kernel/vfs/xv6fs_ro.c`.
- ABI export registry has moved to `kernel/hostabi/exports_registry.c`
  with support for module-driven extension/override and priority ordering.
- `dirent` host ABI has been extracted to `kernel/hostabi/hostabi_dirent.c`
  (`opendir/readdir/closedir/rewinddir/dirfd/fdopendir`), and export wiring now uses this module.
- PTY host ABI entrypoints have been extracted to `kernel/hostabi/hostabi_pty.c`
  (`posix_openpt/grantpt/unlockpt/ptsname/ptsname_r`), and export wiring now uses this module.
- POSIX FS host ABI has been extracted to `kernel/hostabi/hostabi_posix_fs.c`
  (`open/read/write/close/dup/dup2/lseek/stat/lstat/fstat/readlink/pipe`), while `ksh.c`
  keeps thin adapters and shell-specific logic.
- Runtime kernel modules are managed by `kernel/modules/module_manager.c` (`kmod` shell command),
  with autoload manifest support from `/etc/modules.conf`.

## Invariants

- No breaking changes for:
  - shell CLI behavior,
  - applet ABI/export names,
  - fsroot install paths.
- `idf.py build`, flash targets, and QEMU/HIL script targets remain functional.

## Next decomposition steps

1. Split `ksh.c` into shell-only modules.
2. Split `xv6fs_ro.c` into `vfs/dev/pty/pipe/fs_backend` files.
3. Add integration tests per layer boundary and ABI snapshot checks.

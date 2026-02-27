# Optional Rust PoC (Kernel Module + Applet)

This is an optional proof-of-idea path for building one kernel module and one applet in Rust.
It is disabled by default and does not affect the normal C build.

## Enable in CMake

```bash
source "$HOME/esp-idf/export.sh"
idf.py -D XV6_ENABLE_RUST_POC=ON build
```

Optional overrides:

- `-D XV6_RUST_TARGET=<target-triple>` (default: empty, i.e. host rustc target)
- `-D XV6_RUST_FLAGS="<extra rustc flags>"`

For ESP32 runtime experiments, set a real embedded target explicitly (for example via a custom Rust toolchain setup).

## Artifacts

- Applet: `/bin/rust_poc`
- Kernel module: `/lib/modules/rustpoc.so`

## Runtime smoke (manual)

```sh
kmod load /lib/modules/rustpoc.so
kmod list
rust_poc hello
```

The applet is intentionally simple and prints its args.
The module exports `rustpoc_magic` as an extension symbol and logs init/fini.

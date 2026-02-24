#!/usr/bin/env python3
import argparse
import re
import subprocess
from pathlib import Path

VALID_C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
BLOCKED_EXPORT_SYMBOLS = {
    # In ESP-IDF + picolibc this is a TLS-backed object from esp_libc.
    # Exporting it as a plain address causes TLS/non-TLS linker mismatch.
    "errno",
    # Applets must route process termination through shell runtime shims.
    # Exporting host libc termination symbols makes loaded applets call
    # ESP-IDF abort paths instead of returning to ELF loader.
    "exit",
    "_exit",
    "_Exit",
    "abort",
    "quick_exit",
}

BLOCKED_NEEDED_SYMBOLS = {
    "errno",
}

NONFORCED_SYMBOLS = {
    # Some ESP-IDF/newlib link pipelines do not expose this internal ctype
    # object as a regular host symbol. Keep it weak to avoid hard link failures.
    "_ctype_",
}

COMPAT_ALIASES = {
    # ESP-IDF no-rtti picolibc exports _ctype_b but some applets reference _ctype_.
    "_ctype_": "_ctype_b",
}

SHIM_SYMBOLS = {
    "__errno": "xv6_libc_shim___errno",
}


def collect_symbols(nm_bin: str, libs: list[str]) -> tuple[list[str], set[str]]:
    out = subprocess.check_output(
        [nm_bin, "--defined-only", "-g", *libs],
        text=True,
        stderr=subprocess.STDOUT,
    )
    symbols: set[str] = set()
    strong_symbols: set[str] = set()
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) < 3:
            continue
        sym_type = parts[-2]
        name = parts[-1]
        if sym_type not in {"T", "D", "B", "R", "A", "W", "V"}:
            continue
        if not VALID_C_IDENT.match(name):
            continue
        if name in BLOCKED_EXPORT_SYMBOLS:
            continue
        symbols.add(name)
        if sym_type in {"T", "D", "B", "R", "A"}:
            strong_symbols.add(name)
    return sorted(symbols), strong_symbols


def collect_needed_symbols(nm_bin: str, elf_paths: list[str]) -> set[str]:
    needed: set[str] = set()
    for elf_path in elf_paths:
        if not Path(elf_path).exists():
            continue
        out = subprocess.check_output(
            [nm_bin, "-u", elf_path],
            text=True,
            stderr=subprocess.STDOUT,
        )
        for line in out.splitlines():
            parts = line.strip().split()
            if not parts:
                continue
            name = parts[-1]
            if not VALID_C_IDENT.match(name):
                continue
            if name in BLOCKED_NEEDED_SYMBOLS:
                continue
            needed.add(name)
    return needed


def render(
    symbols: list[str],
    forced_symbols: set[str],
    alias_map: dict[str, str],
    shim_map: dict[str, str],
    header_path: str,
) -> str:
    lines: list[str] = []
    lines.append("/* Auto-generated from libc.a; do not edit manually. */")
    lines.append(f"#include \"{header_path}\"")
    lines.append("")
    if "__errno" in shim_map:
        lines.append("extern int errno;")
        lines.append("")
        lines.append("static int *xv6_libc_shim___errno(void)")
        lines.append("{")
        lines.append("  return &errno;")
        lines.append("}")
        lines.append("")
    lines.append("#pragma GCC diagnostic push")
    lines.append("#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"")
    lines.append("#pragma GCC diagnostic ignored \"-Warray-bounds\"")
    for s in symbols:
        if s in forced_symbols:
            lines.append(f"extern char {s};")
        else:
            lines.append(f"extern char {s} __attribute__((weak));")
    lines.append("#pragma GCC diagnostic pop")
    lines.append("")
    lines.append("static const elf_host_symbol_t g_libc_host_syms[] = {")
    for s in symbols:
        lines.append(f"  {{ \"{s}\", (void *)&{s} }},")
    for alias, target in sorted(alias_map.items()):
        lines.append(f"  {{ \"{alias}\", (void *)&{target} }},")
    for sym_name, shim_name in sorted(shim_map.items()):
        lines.append(f"  {{ \"{sym_name}\", (void *)&{shim_name} }},")
    lines.append("};")
    lines.append("")
    lines.append("int ksh_register_libc_host_symbols(void)")
    lines.append("{")
    lines.append("  return elf_loader_register_host_symbols(g_libc_host_syms,")
    lines.append(
        "                                          "
        "(int)(sizeof(g_libc_host_syms) / sizeof(g_libc_host_syms[0])));"
    )
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--lib", action="append", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--needed-elf", action="append", default=[])
    parser.add_argument("--needed-elf-dir", action="append", default=[])
    args = parser.parse_args()

    symbols, strong_symbols = collect_symbols(args.nm, args.lib)
    symbol_set = set(symbols)

    needed_elf_paths = list(args.needed_elf)
    for elf_dir in args.needed_elf_dir:
        p = Path(elf_dir)
        if not p.exists():
            continue
        for so in sorted(p.glob("*.so")):
            needed_elf_paths.append(str(so))

    needed_symbols = collect_needed_symbols(args.nm, needed_elf_paths)
    forced_symbols = {
        sym
        for sym in symbols
        if sym in needed_symbols and sym in strong_symbols and sym not in NONFORCED_SYMBOLS
    }

    alias_map: dict[str, str] = {}
    for alias, target in COMPAT_ALIASES.items():
        if alias in symbol_set:
            continue
        if alias not in needed_symbols:
            continue
        if target not in symbol_set:
            continue
        alias_map[alias] = target
        if target in strong_symbols:
            forced_symbols.add(target)

    shim_map: dict[str, str] = {}
    for sym_name, shim_name in SHIM_SYMBOLS.items():
        if sym_name in symbol_set:
            continue
        if sym_name not in needed_symbols:
            continue
        shim_map[sym_name] = shim_name

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(render(symbols, forced_symbols, alias_map, shim_map, args.header), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

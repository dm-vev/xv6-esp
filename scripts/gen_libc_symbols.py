#!/usr/bin/env python3
import argparse
import re
import subprocess
from pathlib import Path

VALID_C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
BLOCKED_SYMBOLS = {
    # In ESP-IDF + picolibc this is a TLS-backed object from esp_libc.
    # Exporting it as a plain address causes TLS/non-TLS linker mismatch.
    "errno",
}


def collect_symbols(nm_bin: str, libs: list[str]) -> list[str]:
    out = subprocess.check_output(
        [nm_bin, "--defined-only", "-g", *libs],
        text=True,
        stderr=subprocess.STDOUT,
    )
    symbols: set[str] = set()
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) < 3:
            continue
        sym_type = parts[-2]
        name = parts[-1]
        if sym_type not in {"T", "D", "B", "R", "W", "V"}:
            continue
        if not VALID_C_IDENT.match(name):
            continue
        if name in BLOCKED_SYMBOLS:
            continue
        symbols.add(name)
    return sorted(symbols)


def render(symbols: list[str], header_path: str) -> str:
    lines: list[str] = []
    lines.append("/* Auto-generated from libc.a; do not edit manually. */")
    lines.append(f"#include \"{header_path}\"")
    lines.append("")
    lines.append("#pragma GCC diagnostic push")
    lines.append("#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"")
    lines.append("#pragma GCC diagnostic ignored \"-Warray-bounds\"")
    for s in symbols:
        lines.append(f"extern char {s} __attribute__((weak));")
    lines.append("#pragma GCC diagnostic pop")
    lines.append("")
    lines.append("static const elf_host_symbol_t g_libc_host_syms[] = {")
    for s in symbols:
        lines.append(f"  {{ \"{s}\", (void *)&{s} }},")
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
    args = parser.parse_args()

    symbols = collect_symbols(args.nm, args.lib)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(render(symbols, args.header), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

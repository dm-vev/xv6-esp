#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

SYMS_BLOCK_RE = re.compile(
    r"static void register_default_symbols\(void\)\s*\{\s*"
    r"static const elf_host_symbol_t syms\[] = \{(.*?)\n\s*\};",
    re.S,
)
SYM_RE = re.compile(r'\{\s*"([^"]+)"\s*,')


def parse_exports(source_path: Path) -> list[str]:
    text = source_path.read_text(encoding="utf-8")
    match = SYMS_BLOCK_RE.search(text)
    if match is None:
        raise RuntimeError(f"failed to locate host ABI symbol table in {source_path}")
    symbols = SYM_RE.findall(match.group(1))
    if not symbols:
        raise RuntimeError(f"no symbols parsed from {source_path}")
    duplicates = sorted({name for name in symbols if symbols.count(name) > 1})
    if duplicates:
        dup_list = ", ".join(duplicates)
        raise RuntimeError(f"duplicate ABI export symbol(s): {dup_list}")
    return symbols


def read_snapshot(path: Path) -> list[str]:
    if not path.exists():
        return []
    out: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        out.append(line)
    return out


def write_snapshot(path: Path, symbols: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Host ABI export snapshot for applet/newlib runtime.",
        "# Order is intentionally fixed and treated as ABI-significant.",
        "# Generated from kernel/runtime/shell_runtime_shell.inc register_default_symbols().",
        *symbols,
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Check/freeze host ABI export surface.")
    parser.add_argument(
        "--exports-source",
        default=str(root / "kernel" / "runtime" / "shell_runtime_shell.inc"),
        help="path to source file that defines register_default_symbols()",
    )
    parser.add_argument(
        "--snapshot",
        default=str(root / "docs" / "abi" / "hostabi_exports.snapshot"),
        help="path to ABI snapshot file",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite snapshot from current exports instead of checking",
    )
    args = parser.parse_args()

    source_path = Path(args.exports_source)
    snapshot_path = Path(args.snapshot)
    current = parse_exports(source_path)

    if args.update:
        write_snapshot(snapshot_path, current)
        print(f"updated snapshot: {snapshot_path}")
        print(f"symbols: {len(current)}")
        return 0

    expected = read_snapshot(snapshot_path)
    if not expected:
        print(f"snapshot is missing or empty: {snapshot_path}", file=sys.stderr)
        print("run with --update to create it", file=sys.stderr)
        return 2

    if current == expected:
        print(f"host ABI snapshot OK ({len(current)} symbols)")
        return 0

    diff = difflib.unified_diff(
        [f"{line}\n" for line in expected],
        [f"{line}\n" for line in current],
        fromfile=str(snapshot_path),
        tofile="current:register_default_symbols",
    )
    print("host ABI snapshot mismatch:", file=sys.stderr)
    for line in diff:
        sys.stderr.write(line)
    print("\nIf change is intentional, run:", file=sys.stderr)
    print(f"  {Path(__file__).name} --update", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

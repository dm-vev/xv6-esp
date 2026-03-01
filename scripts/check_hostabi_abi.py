#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
from pathlib import Path

CORE_SYMS_BLOCK_RE = re.compile(
    r"static\s+[A-Za-z_][A-Za-z0-9_\s\*]*\s+register_default_symbols\(void\)\s*\{\s*"
    r"static const elf_host_symbol_t\s+[A-Za-z_][A-Za-z0-9_]*\s*\[\]\s*=\s*\{(.*?)\n\s*\};",
    re.S,
)
GEN_LIBC_SYMS_BLOCK_RE = re.compile(
    r"static\s+const\s+elf_host_symbol_t\s+[A-Za-z_][A-Za-z0-9_]*\s*\[\]\s*=\s*\{(.*?)\n\s*\};",
    re.S,
)
SYM_RE = re.compile(r'\{\s*"([^"]+)"\s*,')
VALID_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CMAKE_NM_RE = re.compile(r"^CMAKE_NM(?::[A-Z_]+)?=(.+)$")


def parse_exports(source_path: Path, block_re: re.Pattern[str], label: str) -> list[str]:
    text = source_path.read_text(encoding="utf-8")
    match = block_re.search(text)
    if match is None:
        raise RuntimeError(f"failed to locate {label} symbol table in {source_path}")
    symbols = SYM_RE.findall(match.group(1))
    if not symbols:
        raise RuntimeError(f"no {label} symbols parsed from {source_path}")
    duplicates = sorted({name for name in symbols if symbols.count(name) > 1})
    if duplicates:
        dup_list = ", ".join(duplicates)
        raise RuntimeError(f"duplicate {label} symbol(s): {dup_list}")
    return symbols


def parse_core_exports(source_path: Path) -> list[str]:
    return parse_exports(source_path, CORE_SYMS_BLOCK_RE, "core host ABI")


def parse_generated_exports(source_path: Path) -> list[str]:
    return parse_exports(source_path, GEN_LIBC_SYMS_BLOCK_RE, "generated libc host ABI")


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


def resolve_nm_bin(root: Path, arg_nm: str | None) -> str:
    if arg_nm:
        return arg_nm

    cache_path = root / "build" / "CMakeCache.txt"
    if cache_path.exists():
        for raw in cache_path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith(("#", "//")):
                continue
            m = CMAKE_NM_RE.match(line)
            if m:
                value = m.group(1).strip()
                if value:
                    return value
    return "nm"


def collect_undefined_symbols(nm_bin: str, applet_dir: Path) -> dict[str, set[str]]:
    by_applet: dict[str, set[str]] = {}
    applets = sorted(applet_dir.glob("*.so"))
    for so in applets:
        try:
            out = subprocess.check_output([nm_bin, "-u", str(so)], text=True, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(f"nm failed for applet {so}: {exc.output}") from exc
        syms: set[str] = set()
        for raw in out.splitlines():
            parts = raw.strip().split()
            if not parts:
                continue
            name = parts[-1]
            if name.endswith(":"):
                continue
            if not VALID_IDENT_RE.match(name):
                continue
            syms.add(name)
        by_applet[so.name] = syms
    return by_applet


def collect_module_exports(nm_bin: str, module_dir: Path) -> set[str]:
    exports: set[str] = set()
    if not module_dir.exists():
        return exports
    for so in sorted(module_dir.glob("*.so")):
        try:
            out = subprocess.check_output(
                [nm_bin, "--defined-only", "-g", str(so)],
                text=True,
                stderr=subprocess.STDOUT,
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(f"nm failed for module {so}: {exc.output}") from exc
        for raw in out.splitlines():
            parts = raw.strip().split()
            if len(parts) < 3:
                continue
            name = parts[-1]
            if not VALID_IDENT_RE.match(name):
                continue
            exports.add(name)
    return exports


def symbol_is_provided(name: str, provided: set[str]) -> bool:
    if name in provided:
        return True
    if name.startswith("_") and name[1:] in provided:
        return True
    if f"_{name}" in provided:
        return True
    return False


def validate_generated_coverage(
    core_symbols: list[str],
    generated_source: Path,
    applet_dir: Path,
    module_dir: Path,
    nm_bin: str,
) -> None:
    generated_symbols = parse_generated_exports(generated_source)
    by_applet = collect_undefined_symbols(nm_bin, applet_dir)
    module_exports = collect_module_exports(nm_bin, module_dir)

    needed: set[str] = set()
    for syms in by_applet.values():
        needed.update(syms)

    provided = set(core_symbols)
    provided.update(generated_symbols)
    provided.update(module_exports)

    missing = sorted(sym for sym in needed if not symbol_is_provided(sym, provided))
    if missing:
        lines = [
            "generated host ABI coverage mismatch:",
            f"  unresolved applet symbols not provided by core+generated+modules: {len(missing)}",
        ]
        for sym in missing:
            users = sorted(applet for applet, syms in by_applet.items() if sym in syms)
            lines.append(f"  - {sym}: {', '.join(users)}")
        raise RuntimeError("\n".join(lines))

    ctype_users = sorted(applet for applet, syms in by_applet.items() if "_ctype_" in syms)
    if ctype_users and "_ctype_" not in generated_symbols and "_ctype_" not in core_symbols:
        raise RuntimeError(
            "generated host ABI regression: _ctype_ is unresolved in applets but not exported "
            f"(applets: {', '.join(ctype_users)})"
        )

    print(
        "generated host ABI coverage OK "
        f"(core={len(core_symbols)} generated={len(generated_symbols)} "
        f"applet_undef={len(needed)} modules={len(module_exports)})"
    )


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
        "--generated-exports-source",
        default=str(root / "build" / "generated" / "libc_host_symbols.c"),
        help="path to generated libc host symbol source",
    )
    parser.add_argument(
        "--applet-dir",
        default=str(root / "build" / "applets"),
        help="path to built applet .so directory",
    )
    parser.add_argument(
        "--module-dir",
        default=str(root / "build" / "shared"),
        help="path to built shared module .so directory",
    )
    parser.add_argument(
        "--nm",
        default="",
        help="path to nm binary used to inspect applet/module symbols (default: CMAKE_NM or nm)",
    )
    parser.add_argument(
        "--skip-generated-checks",
        action="store_true",
        help="skip generated symbol table + applet unresolved coverage validation",
    )
    parser.add_argument(
        "--require-generated-checks",
        action="store_true",
        help="fail if generated symbols/applet artifacts are missing",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite snapshot from current exports instead of checking",
    )
    args = parser.parse_args()

    source_path = Path(args.exports_source)
    snapshot_path = Path(args.snapshot)
    current = parse_core_exports(source_path)

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

    if current != expected:
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

    print(f"host ABI snapshot OK ({len(current)} symbols)")

    if args.skip_generated_checks:
        return 0

    generated_source = Path(args.generated_exports_source)
    applet_dir = Path(args.applet_dir)
    module_dir = Path(args.module_dir)
    has_generated_inputs = generated_source.exists() and applet_dir.exists()

    if not has_generated_inputs:
        if args.require_generated_checks:
            print(
                "generated host ABI checks requested but required artifacts are missing:",
                file=sys.stderr,
            )
            print(f"  generated source: {generated_source}", file=sys.stderr)
            print(f"  applet dir: {applet_dir}", file=sys.stderr)
            return 2
        print(
            "generated host ABI checks skipped "
            f"(missing {generated_source if not generated_source.exists() else applet_dir})"
        )
        return 0

    nm_bin = resolve_nm_bin(root, args.nm or None)
    try:
        validate_generated_coverage(current, generated_source, applet_dir, module_dir, nm_bin)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

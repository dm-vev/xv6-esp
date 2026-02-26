#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
STRICT="${STRICT:-1}"
STATIC_VENV="${STATIC_VENV:-$ROOT_DIR/.venv_static}"

cd "$ROOT_DIR"

if [[ -x "$STATIC_VENV/bin/python3" ]]; then
  PATH="$STATIC_VENV/bin:$PATH"
fi

log() {
  printf '[static] %s\n' "$*"
}

warn() {
  printf '[static][warn] %s\n' "$*" >&2
}

run_or_warn() {
  local name="$1"
  shift
  if "$@"; then
    return 0
  fi
  if [[ "$STRICT" == "1" ]]; then
    return 1
  fi
  warn "$name failed (STRICT=0, continuing)"
  return 0
}

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    if [[ "$STRICT" == "1" ]]; then
      printf '[static][error] required tool is missing: %s\n' "$cmd" >&2
      exit 1
    fi
    warn "tool is missing: $cmd (skip)"
    return 1
  fi
  return 0
}

collect_c_sources() {
  find kernel main applets -type f -name '*.c' -print | sort
}

collect_py_sources() {
  find scripts -type f -name '*.py' -print | sort
}

run_cppcheck() {
  require_cmd cppcheck || return 0
  log "cppcheck"
  cppcheck \
    --quiet \
    --enable=warning,performance,portability \
    --std=c11 \
    --error-exitcode=1 \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=normalCheckLevelMaxBranches \
    -I kernel \
    -I applets/include \
    -I main \
    kernel main applets
}

run_clang_tidy() {
  require_cmd clang-tidy || return 0
  if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    warn "compile_commands.json not found in $BUILD_DIR (skip clang-tidy)"
    return 0
  fi
  if grep -qE 'xtensa-esp|riscv32-esp' "$BUILD_DIR/compile_commands.json"; then
    warn "compile DB targets embedded toolchains (xtensa/riscv). Host clang-tidy cannot parse those flags reliably; skip clang-tidy."
    return 0
  fi
  log "clang-tidy"
  local files
  files="$(collect_c_sources | tr '\n' ' ')"
  if [[ -z "$files" ]]; then
    return 0
  fi
  # shellcheck disable=SC2086
  clang-tidy -p "$BUILD_DIR" $files --warnings-as-errors='*'
}

run_semgrep() {
  require_cmd semgrep || return 0
  log "semgrep"
  semgrep --config auto --error \
    --exclude build \
    --exclude .git \
    --exclude .venv_static \
    --exclude .tools \
    .
}

run_ruff() {
  require_cmd ruff || return 0
  log "ruff"
  local py_files
  py_files="$(collect_py_sources | tr '\n' ' ')"
  if [[ -z "$py_files" ]]; then
    return 0
  fi
  # shellcheck disable=SC2086
  ruff check $py_files
}

run_bandit() {
  require_cmd bandit || return 0
  log "bandit"
  bandit -q -r scripts \
    -s B108,B404,B603,B607 \
    -x scripts/qemu_smoke_esp.py,scripts/qemu_stress_esp.py,scripts/qemu_soak_esp.py
}

run_codespell() {
  require_cmd codespell || return 0
  log "codespell"
  codespell --config .codespellrc -f -H -q 2
}

main() {
  run_or_warn cppcheck run_cppcheck
  run_or_warn clang-tidy run_clang_tidy
  run_or_warn semgrep run_semgrep
  run_or_warn ruff run_ruff
  run_or_warn bandit run_bandit
  run_or_warn codespell run_codespell
  log "done"
}

main "$@"

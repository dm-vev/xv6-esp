#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
STRICT="${STRICT:-1}"
STATIC_VENV="${STATIC_VENV:-$ROOT_DIR/.venv_static}"
SEMGREP_CONFIG="${SEMGREP_CONFIG:-$ROOT_DIR/.semgrep/static-analysis.yml}"
BANDIT_INI="${BANDIT_INI:-$ROOT_DIR/.bandit}"
CODESPELL_CONFIG="${CODESPELL_CONFIG:-$ROOT_DIR/.codespellrc}"

cd "$ROOT_DIR"
export LC_ALL=C

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

require_file() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    if [[ "$STRICT" == "1" ]]; then
      printf '[static][error] required file is missing: %s\n' "$file" >&2
      exit 1
    fi
    warn "required file is missing: $file (skip)"
    return 1
  fi
  return 0
}

collect_c_sources() {
  find kernel main applets -type f -name '*.c' -print0 | sort -z
}

collect_py_sources() {
  find scripts -type f -name '*.py' -print0 | sort -z
}

collect_repo_files() {
  if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -c core.quotepath=off ls-files -z
    return
  fi
  find . -type f -print0 | sort -z
}

run_cppcheck() {
  require_cmd cppcheck || return 0
  log "cppcheck"
  local -a c_files
  mapfile -d '' -t c_files < <(collect_c_sources)
  if [[ ${#c_files[@]} -eq 0 ]]; then
    return 0
  fi
  local file_list
  file_list="$(mktemp)"
  printf '%s\n' "${c_files[@]}" >"$file_list"
  local rc=0
  cppcheck \
    --quiet \
    --enable=warning,performance,portability \
    --std=c11 \
    --error-exitcode=1 \
    -j1 \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=normalCheckLevelMaxBranches \
    -I kernel \
    -I applets/include \
    -I main \
    --file-list="$file_list" || rc=$?
  rm -f "$file_list"
  return "$rc"
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
  local -a c_files
  mapfile -d '' -t c_files < <(collect_c_sources)
  if [[ ${#c_files[@]} -eq 0 ]]; then
    return 0
  fi
  clang-tidy -p "$BUILD_DIR" "${c_files[@]}" --warnings-as-errors='*'
}

run_semgrep() {
  require_cmd semgrep || return 0
  require_file "$SEMGREP_CONFIG" || return 0
  log "semgrep"
  semgrep \
    --config "$SEMGREP_CONFIG" \
    --error \
    --jobs 1 \
    --metrics=off \
    --disable-version-check \
    --exclude build \
    --exclude .git \
    --exclude .venv_static \
    --exclude .tools \
    scripts
}

run_ruff() {
  require_cmd ruff || return 0
  log "ruff"
  local -a py_files
  mapfile -d '' -t py_files < <(collect_py_sources)
  if [[ ${#py_files[@]} -eq 0 ]]; then
    return 0
  fi
  ruff check --config "$ROOT_DIR/.ruff.toml" --no-cache "${py_files[@]}"
}

run_bandit() {
  require_cmd bandit || return 0
  require_file "$BANDIT_INI" || return 0
  log "bandit"
  bandit --ini "$BANDIT_INI" -q -r scripts
}

run_codespell() {
  require_cmd codespell || return 0
  require_file "$CODESPELL_CONFIG" || return 0
  log "codespell"
  local -a repo_files
  mapfile -d '' -t repo_files < <(collect_repo_files)
  if [[ ${#repo_files[@]} -eq 0 ]]; then
    return 0
  fi
  codespell --config "$CODESPELL_CONFIG" -f -H -q 2 "${repo_files[@]}"
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

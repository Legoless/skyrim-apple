#!/bin/bash
# Usage: sky_verify_rewrite.sh [--equiv] <rewrite output dir> <NSO dir> [module ...]
# Static mode runs structck and gatefn only. --equiv executes instructions in
# Unicorn and is intended for synthetic fixtures, not unapproved guest-code runs.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
VERIFY="$HERE/verify"
HOST_ENTRY="$HERE/../skyhost/entry.S"
PY=${SK_PY:-${SKY_PYTHON:-python3}}
EQUIV=0
if [ "${1:-}" = --equiv ]; then EQUIV=1; shift; fi
if [ $# -lt 2 ]; then
  echo "usage: sky_verify_rewrite.sh [--equiv] <rewrite output dir> <NSO dir> [module ...]" >&2
  exit 2
fi
OUT=$1
NSO_DIR=$2
shift 2
MODS=${*:-main sdk}
if [ -z "${LLVM_BIN:-}" ]; then
  llvm_objcopy=$(command -v llvm-objcopy) || llvm_objcopy=
  if [ -n "$llvm_objcopy" ]; then
    LLVM_BIN=$(dirname "$llvm_objcopy")
  fi
fi
if [ ! -x "${LLVM_BIN:-}/llvm-objcopy" ] || [ ! -x "${LLVM_BIN:-}/llvm-objdump" ]; then
  echo "Set LLVM_BIN to a directory containing llvm-objcopy and llvm-objdump, or put both on PATH." >&2
  exit 1
fi
export LLVM_BIN
RUN=${SK_VERIFY_RUN:-$OUT/validation}
mkdir -p "$RUN"
TS=$(date +%Y%m%d_%H%M%S)_$$
LOG=$RUN/verify_$TS.log
export DM_VCACHE=$RUN/disassembly-cache

rc=0
for module in $MODS; do
  "$PY" "$VERIFY/structck.py" "$OUT" "$module" "$NSO_DIR/$module" >> "$LOG" 2>&1 || rc=1
  if [ $EQUIV = 1 ]; then
    extra=(--seed "${SEED:-1}" --tsv "$RUN/equiv_${module}_$TS.tsv")
    [ -n "${JOBS:-}" ] && extra+=(--jobs "$JOBS")
    "$PY" "$VERIFY/equiv.py" "$OUT" "$module" "$NSO_DIR/$module" "${extra[@]}" >> "$LOG" 2>&1 || rc=1
  fi
done

GT=$RUN/gate_test_$TS
if cc -O1 -o "$GT" "$VERIFY/gatefn/gate_test.c" "$VERIFY/gatefn/gate_tramp.S" "$HOST_ENTRY" >> "$LOG" 2>&1; then
  gate_files=()
  for module in $MODS; do
    [ -s "$OUT/$module.gates.tsv" ] && gate_files+=("$OUT/$module.gates.tsv")
  done
  "$GT" "${gate_files[@]}" >> "$LOG" 2>&1 || rc=1
  rm -f "$GT"
else
  echo "FAIL gatefn build" >> "$LOG"
  rc=1
fi

echo "---- Skyrim rewrite validation ($LOG)"
/usr/bin/grep -E '^(STRUCT|STATIC|EXITS|EQUIV|GATEFN|FAIL|DIFF|NOEFF|MUTMISS)' "$LOG" || true
if [ $rc = 0 ]; then
  echo "SKY_VERIFY OK $OUT"
else
  echo "SKY_VERIFY FAIL $OUT"
fi
exit $rc

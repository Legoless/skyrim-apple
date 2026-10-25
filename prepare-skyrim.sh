#!/bin/bash
# Interactive and CLI entry point to build, cache, and deploy Skyrim from an XCI dump and prod.keys.
set -eu
set -o pipefail

CALLER_PWD=$(pwd -P)
REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

say() { printf '[prepare-skyrim] %s\n' "$*"; }
die() { printf '[prepare-skyrim] error: %s\n' "$*" >&2; exit 1; }

supports_python() {
    "$1" -B -c 'import sys; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1
}

if [ -n "${SKY_PYTHON:-}" ]; then
    PYTHON_BIN=$(command -v "$SKY_PYTHON" 2>/dev/null) || PYTHON_BIN=
    if [ -z "$PYTHON_BIN" ] || ! supports_python "$PYTHON_BIN"; then
        echo "SKY_PYTHON must point to a working Python 3.9 or newer interpreter." >&2
        exit 1
    fi
else
    PYTHON_BIN=$(command -v python3 2>/dev/null) || PYTHON_BIN=
    if [ -z "$PYTHON_BIN" ] || ! supports_python "$PYTHON_BIN"; then
        PYTHON_BIN=/usr/bin/python3
        if ! supports_python "$PYTHON_BIN"; then
            echo "Python 3.9 or newer is required. Install/select Xcode or set SKY_PYTHON=/path/to/python3." >&2
            exit 1
        fi
    fi
fi

usage() {
    cat <<'EOF'
Usage: ./prepare-skyrim.sh [DUMP] [options]

Interactive entry point that asks for your Skyrim cartridge dump (.xci file or
split-dump folder), console prod.keys, and build target, then runs the full
local build-and-cache pipeline (pinned tools, XCI extraction, module rewrite,
Metal shader cache build, and macOS / iOS build or deployment). No game update
NSP is required.

Options:
  --base PATH         Cartridge .xci file or split-dump .xci directory
  --keys PATH         Console prod.keys file (never copied or logged)
  --target TARGET     Target to build after caching modules & shaders:
                        macos        Build macOS host (play via ./skyhost/run.sh)
                        ios-prepare  Prepare iOS Release Xcode project
                        ios-deploy   Prepare, sign, install, sync RomFS & launch on iOS
                        both         Build macOS host + prepare iOS Xcode project
                        all          Build macOS host + prepare & deploy to iOS
  --work-dir PATH     Local workspace directory (default: <repo>/skyrim-work)
  --bundle-id ID      iOS bundle ID (prepare default: dev.skyhost.skyrim;
                      deploy default: dev.skyhost.skyrim.t<team>)
  --team TEAM_ID      10-character Apple development team ID for ios-deploy / all
  --device SELECTOR   Physical iOS device UDID or name for ios-deploy / all
  --run               Launch ./skyhost/run.sh after building the macOS host
  -h, --help          Show this help message and exit
EOF
}

normalize_path() {
    "$PYTHON_BIN" -B - "$CALLER_PWD" "$1" <<'PY'
from pathlib import Path
import shlex
import sys

cwd = Path(sys.argv[1])
raw = sys.argv[2].strip()
if not raw:
    sys.exit(1)
candidates = []
if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in ("'", '"'):
    candidates.append(raw[1:-1])
else:
    candidates.append(raw)
    if "\\" in raw:
        try:
            parts = shlex.split(raw)
            if len(parts) == 1:
                candidates.append(parts[0])
        except ValueError:
            pass
chosen = candidates[0]
for item in candidates:
    expanded = Path(item).expanduser()
    resolved = expanded if expanded.is_absolute() else (cwd / expanded)
    if resolved.exists():
        chosen = item
        break
out = Path(chosen).expanduser()
if not out.is_absolute():
    out = cwd / out
print(out.resolve())
PY
}

discover_default_input() {
    "$PYTHON_BIN" -B - "$REPO_DIR/inputs" "$1" <<'PY'
from pathlib import Path
import re
import sys

inputs_dir = Path(sys.argv[1])
kind = sys.argv[2]
if not inputs_dir.is_dir():
    sys.exit(0)
entries = list(inputs_dir.iterdir())
if kind == "base":
    candidates = [p for p in entries if p.suffix.lower() == ".xci" and (p.is_file() or p.is_dir())]
    if any(re.fullmatch(r"[0-9]+", p.name) for p in entries):
        candidates.append(inputs_dir)
    if len(candidates) == 1:
        print(candidates[0].resolve())
    elif len(candidates) > 1:
        print("AMBIGUOUS")
elif kind == "keys":
    keys = inputs_dir / "prod.keys"
    if keys.is_file() and keys.stat().st_size > 0:
        print(keys.resolve())
PY
}

valid_base_path() {
    [ -n "${1:-}" ] || return 1
    "$PYTHON_BIN" -B - "$REPO_DIR/tools" "$1" <<'PY' >/dev/null 2>&1
from pathlib import Path
import sys

sys.path.insert(0, sys.argv[1])
import sky_dump

source = Path(sys.argv[2]).expanduser().resolve()
if source.is_file():
    if source.suffix.lower() != ".xci":
        sys.exit(1)
    sky_dump._validate_header(source)
elif source.is_dir():
    directory = sky_dump._parts_directory(source)
    parts, _ = sky_dump._parts_metadata(directory)
    sky_dump._validate_header(parts[0])
else:
    sys.exit(1)
PY
}

valid_keys_path() {
    [ -n "${1:-}" ] && [ -f "$1" ] && [ -s "$1" ]
}

validate_work_outside_dump() {
    [ -n "${1:-}" ] && [ -d "$1" ] || return 0
    "$PYTHON_BIN" -B - "$1" "$2" <<'PY'
from pathlib import Path
import sys

dump = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
if work == dump or work.is_relative_to(dump):
    sys.exit(1)
PY
}

prompt_file_path() {
    local prompt_label=$1 default_val=$2 validator=$3 hint=$4
    local answer resolved
    while true; do
        if [ -n "$default_val" ]; then
            printf '%s [%s]: ' "$prompt_label" "$default_val" >&2
        else
            printf '%s: ' "$prompt_label" >&2
        fi
        if ! IFS= read -r answer; then
            die "No input received for $prompt_label."
        fi
        answer=$(printf '%s' "$answer" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        if [ -z "$answer" ] && [ -n "$default_val" ]; then
            resolved=$default_val
        elif [ -n "$answer" ]; then
            resolved=$(normalize_path "$answer" 2>/dev/null) || resolved=
        else
            resolved=
        fi
        if [ -n "$resolved" ] && "$validator" "$resolved"; then
            printf '%s\n' "$resolved"
            return 0
        fi
        printf '%s\n' "$hint" >&2
    done
}

normalize_target() {
    local raw
    raw=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    case "$raw" in
        1|macos|mac) printf 'macos\n' ;;
        2|ios-prepare|prepare-ios) printf 'ios-prepare\n' ;;
        3|ios-deploy|deploy-ios) printf 'ios-deploy\n' ;;
        4|both) printf 'both\n' ;;
        5|all) printf 'all\n' ;;
        *) return 1 ;;
    esac
}

prompt_target() {
    local answer parsed
    cat >&2 <<'EOF'
Select target to build after caching modules & shaders:
  1) macos        - Build macOS host (play via ./skyhost/run.sh)
  2) ios-prepare  - Prepare iOS Release Xcode project
  3) ios-deploy   - Prepare, sign, install, sync RomFS & launch on iPhone/iPad
  4) both         - Build macOS host + prepare iOS Xcode project
  5) all          - Build macOS host + prepare & deploy to iPhone/iPad
EOF
    while true; do
        printf 'Target [1]: ' >&2
        if ! IFS= read -r answer; then
            die "No target selected."
        fi
        if [ -z "$(printf '%s' "$answer" | tr -d '[:space:]')" ]; then
            printf 'macos\n'
            return 0
        fi
        if parsed=$(normalize_target "$answer"); then
            printf '%s\n' "$parsed"
            return 0
        fi
        printf 'Enter 1-5 or one of: macos, ios-prepare, ios-deploy, both, all.\n' >&2
    done
}

prompt_team_id() {
    local answer cleaned
    while true; do
        printf 'Apple Development Team ID (10 characters from Xcode > Settings > Accounts): ' >&2
        if ! IFS= read -r answer; then
            die "No Apple Development Team ID supplied."
        fi
        cleaned=$(printf '%s' "$answer" | tr '[:lower:]' '[:upper:]' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        if printf '%s' "$cleaned" | grep -Eq '^[A-Z0-9]{10}$'; then
            printf '%s\n' "$cleaned"
            return 0
        fi
        printf 'Enter a valid 10-character alphanumeric Team ID (for example ABCDE12345).\n' >&2
    done
}

prompt_bundle_id() {
    local default_bundle=$1 answer cleaned
    while true; do
        printf 'iOS Bundle ID [%s]: ' "$default_bundle" >&2
        if ! IFS= read -r answer; then
            die "No iOS Bundle ID supplied."
        fi
        cleaned=$(printf '%s' "$answer" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        [ -n "$cleaned" ] || cleaned=$default_bundle
        if printf '%s' "$cleaned" | grep -Eq '^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$'; then
            printf '%s\n' "$cleaned"
            return 0
        fi
        printf 'Enter a reverse-domain bundle ID such as dev.example.skyrim.\n' >&2
    done
}

DUMP_ARG=
BASE_ARG=
KEYS_ARG=
TARGET_ARG=
WORK_DIR_ARG=
BUNDLE_ID=
TEAM_ID=
DEVICE_SELECTOR=
RUN_AFTER=0

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --base)
            [ $# -ge 2 ] || die "--base requires a path."
            BASE_ARG=$2
            shift 2
            ;;
        --base=*)
            BASE_ARG=${1#*=}
            shift
            ;;
        --keys)
            [ $# -ge 2 ] || die "--keys requires a path."
            KEYS_ARG=$2
            shift 2
            ;;
        --keys=*)
            KEYS_ARG=${1#*=}
            shift
            ;;
        --target)
            [ $# -ge 2 ] || die "--target requires a value."
            TARGET_ARG=$2
            shift 2
            ;;
        --target=*)
            TARGET_ARG=${1#*=}
            shift
            ;;
        --work-dir)
            [ $# -ge 2 ] || die "--work-dir requires a path."
            WORK_DIR_ARG=$2
            shift 2
            ;;
        --work-dir=*)
            WORK_DIR_ARG=${1#*=}
            shift
            ;;
        --bundle-id)
            [ $# -ge 2 ] || die "--bundle-id requires an identifier."
            BUNDLE_ID=$2
            shift 2
            ;;
        --bundle-id=*)
            BUNDLE_ID=${1#*=}
            shift
            ;;
        --team)
            [ $# -ge 2 ] || die "--team requires a 10-character Team ID."
            TEAM_ID=$2
            shift 2
            ;;
        --team=*)
            TEAM_ID=${1#*=}
            shift
            ;;
        --device)
            [ $# -ge 2 ] || die "--device requires a device UDID or name."
            DEVICE_SELECTOR=$2
            shift 2
            ;;
        --device=*)
            DEVICE_SELECTOR=${1#*=}
            shift
            ;;
        --run)
            RUN_AFTER=1
            shift
            ;;
        -*)
            die "Unknown option: $1 (run with --help for usage)."
            ;;
        *)
            [ -z "$DUMP_ARG" ] || die "Unexpected extra positional argument: $1"
            DUMP_ARG=$1
            shift
            ;;
    esac
done

if [ -n "$DUMP_ARG" ] && [ -n "$BASE_ARG" ]; then
    die "Use either the positional dump path or --base, not both."
fi
[ -n "$BASE_ARG" ] || BASE_ARG=$DUMP_ARG

if [ -n "$WORK_DIR_ARG" ]; then
    WORK_DIR=$(normalize_path "$WORK_DIR_ARG") || die "Invalid --work-dir path: $WORK_DIR_ARG"
else
    WORK_DIR="$REPO_DIR/skyrim-work"
fi

if [ -n "$TARGET_ARG" ]; then
    TARGET=$(normalize_target "$TARGET_ARG") || die "Invalid --target '$TARGET_ARG' (expected: macos, ios-prepare, ios-deploy, both, or all)."
fi

BASE_PATH=
KEYS_PATH=

if [ -n "$BASE_ARG" ]; then
    BASE_PATH=$(normalize_path "$BASE_ARG") || BASE_PATH=
    valid_base_path "$BASE_PATH" || die "Cartridge XCI not found or invalid: $BASE_ARG"
    validate_work_outside_dump "$BASE_PATH" "$WORK_DIR" || die "Keep --work-dir outside the original split dump; setup must not modify that dump."
fi
if [ -n "$KEYS_ARG" ]; then
    KEYS_PATH=$(normalize_path "$KEYS_ARG") || KEYS_PATH=
    valid_keys_path "$KEYS_PATH" || die "Console prod.keys file not found or empty: $KEYS_ARG"
fi

SKIP_PREPARE_INPUTS=0
if [ "${TARGET:-}" = "ios-deploy" ] && [ -z "$BASE_PATH" ] && [ -z "$KEYS_PATH" ]; then
    if SKY_PYTHON="$PYTHON_BIN" "$REPO_DIR/setup-skyrim.sh" --check --ios --work-dir "$WORK_DIR" >/dev/null 2>&1; then
        SKIP_PREPARE_INPUTS=1
        say "Prepared iOS workspace found at $WORK_DIR; skipping dump and key prompts."
    fi
fi

if [ "$SKIP_PREPARE_INPUTS" = 0 ]; then
    DEFAULT_BASE=$(discover_default_input base)
    DEFAULT_KEYS=$(discover_default_input keys)

    if [ -z "$BASE_PATH" ]; then
        if [ "$DEFAULT_BASE" = "AMBIGUOUS" ] && [ ! -t 0 ]; then
            die "Multiple game dumps found in inputs/; supply --base PATH explicitly."
        fi
        [ "$DEFAULT_BASE" != "AMBIGUOUS" ] || DEFAULT_BASE=
        if [ -t 0 ]; then
            BASE_PATH=$(prompt_file_path \
                "Cartridge dump (.xci file or split-dump folder)" \
                "$DEFAULT_BASE" \
                valid_base_path \
                "Enter an existing valid .xci file or split-dump directory path.")
        elif [ -n "$DEFAULT_BASE" ]; then
            BASE_PATH=$DEFAULT_BASE
        else
            die "Missing cartridge dump; supply --base PATH or run in a terminal."
        fi
        validate_work_outside_dump "$BASE_PATH" "$WORK_DIR" || die "Keep --work-dir outside the original split dump; setup must not modify that dump."
    fi

    if [ -z "$KEYS_PATH" ]; then
        if [ -t 0 ]; then
            KEYS_PATH=$(prompt_file_path \
                "Console prod.keys file" \
                "$DEFAULT_KEYS" \
                valid_keys_path \
                "Enter an existing non-empty prod.keys file path.")
        elif [ -n "$DEFAULT_KEYS" ]; then
            KEYS_PATH=$DEFAULT_KEYS
        else
            die "Missing prod.keys; supply --keys PATH or run in a terminal."
        fi
    fi
fi

if [ -z "${TARGET:-}" ]; then
    if [ -t 0 ]; then
        TARGET=$(prompt_target)
    else
        die "Missing --target (macos, ios-prepare, ios-deploy, both, or all) when running without a terminal."
    fi
fi

NEEDS_MACOS=0
NEEDS_IOS_PREPARE=0
NEEDS_IOS_DEPLOY=0

case "$TARGET" in
    macos)
        NEEDS_MACOS=1
        ;;
    ios-prepare)
        NEEDS_IOS_PREPARE=1
        ;;
    ios-deploy)
        [ "$SKIP_PREPARE_INPUTS" = 1 ] || NEEDS_IOS_PREPARE=1
        NEEDS_IOS_DEPLOY=1
        ;;
    both)
        NEEDS_MACOS=1
        NEEDS_IOS_PREPARE=1
        ;;
    all)
        NEEDS_MACOS=1
        NEEDS_IOS_PREPARE=1
        NEEDS_IOS_DEPLOY=1
        ;;
esac

if [ "$NEEDS_IOS_DEPLOY" = 1 ]; then
    if [ -n "$TEAM_ID" ]; then
        TEAM_ID=$(printf '%s' "$TEAM_ID" | tr '[:lower:]' '[:upper:]' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        printf '%s' "$TEAM_ID" | grep -Eq '^[A-Z0-9]{10}$' || die "--team must be a 10-character Apple development team ID."
    elif [ -t 0 ]; then
        TEAM_ID=$(prompt_team_id)
    else
        die "--team is required for target '$TARGET' when running without a terminal."
    fi

    DEFAULT_DEPLOY_BUNDLE="dev.skyhost.skyrim.t$(printf '%s' "$TEAM_ID" | tr '[:upper:]' '[:lower:]')"
    if [ -n "$BUNDLE_ID" ]; then
        printf '%s' "$BUNDLE_ID" | grep -Eq '^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$' || die "--bundle-id must be a reverse-domain identifier."
    elif [ -t 0 ]; then
        BUNDLE_ID=$(prompt_bundle_id "$DEFAULT_DEPLOY_BUNDLE")
    else
        BUNDLE_ID=$DEFAULT_DEPLOY_BUNDLE
    fi

    if [ -z "$DEVICE_SELECTOR" ] && [ -t 0 ]; then
        printf 'Physical iOS device UDID or name [auto-detect connected device]: ' >&2
        IFS= read -r DEVICE_ANSWER || DEVICE_ANSWER=
        DEVICE_SELECTOR=$(printf '%s' "$DEVICE_ANSWER" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    fi
elif [ -n "$BUNDLE_ID" ]; then
    printf '%s' "$BUNDLE_ID" | grep -Eq '^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$' || die "--bundle-id must be a reverse-domain identifier."
fi

if [ "$NEEDS_MACOS" = 1 ]; then
    say "Preparing workspace, caching shaders, and building macOS host..."
    SKY_PYTHON="$PYTHON_BIN" "$REPO_DIR/setup-skyrim.sh" \
        --base "$BASE_PATH" \
        --keys "$KEYS_PATH" \
        --work-dir "$WORK_DIR"
fi

if [ "$NEEDS_IOS_PREPARE" = 1 ]; then
    say "Preparing iOS Release Xcode project and caching resources..."
    SKY_PYTHON="$PYTHON_BIN" "$REPO_DIR/setup-skyrim.sh" \
        --base "$BASE_PATH" \
        --keys "$KEYS_PATH" \
        --work-dir "$WORK_DIR" \
        --ios \
        --bundle-id "${BUNDLE_ID:-dev.skyhost.skyrim}"
fi

if [ "$NEEDS_IOS_DEPLOY" = 1 ]; then
    say "Signing, installing, verifying RomFS, and launching on iOS device..."
    DEPLOY_CMD=(
        "$REPO_DIR/setup-skyrim.sh"
        --deploy-ios
        --work-dir "$WORK_DIR"
        --team "$TEAM_ID"
        --bundle-id "$BUNDLE_ID"
    )
    if [ -n "$DEVICE_SELECTOR" ]; then
        DEPLOY_CMD+=(--device "$DEVICE_SELECTOR")
    fi
    SKY_PYTHON="$PYTHON_BIN" "${DEPLOY_CMD[@]}"
fi

if [ "$RUN_AFTER" = 1 ] && [ "$NEEDS_MACOS" = 1 ]; then
    say "Launching macOS host..."
    SKY_WORK_DIR="$WORK_DIR" exec "$REPO_DIR/skyhost/run.sh"
fi

say "Done."

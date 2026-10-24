#!/bin/bash
# Extract and pin updated Skyrim Switch files from separately dumped packages.
# Usage: sky_extract.sh <base.xci> <update.nsp> <prod.keys> <update-pin>
#        sky_extract.sh --program-only <base.xci> <update.nsp> <prod.keys> <update-pin>
# Full output: $SK_ROOT/data/switch/skyrim/<update-pin>/{Update,meta/packages.tsv}
# Program-only output: $SK_ROOT/data/switch/skyrim/<update-pin>/{Program,meta/program.tsv}
# SK_ROOT defaults to <repo>/skyrim-work. Set HACTOOL to override the hactool executable.
set -u
set -o pipefail

usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; }
die() { printf '[sky_extract] error: %s\n' "$*" >&2; exit 1; }
say() { printf '[sky_extract] %s\n' "$*"; }
path_exists() { [ -e "$1" ] || [ -L "$1" ]; }

PROGRAM_ONLY=0
if [ "${1:-}" = "--program-only" ]; then
    PROGRAM_ONLY=1
    shift
fi
[ $# -eq 4 ] || { usage; exit 2; }
BASE_PKG=$1
UPDATE_PKG=$2
KEYS=$3
PIN=$4
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SK_ROOT=${SK_ROOT:-$REPO_ROOT/skyrim-work}
TITLE_ID=01000a10041ea000
UPDATE_META_TITLE_ID=01000a10041ea800

[[ "$PIN" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "update-pin must be a safe single path component"
[ "$PIN" != "." ] && [ "$PIN" != ".." ] || die "update-pin must be a safe single path component"
[ -f "$BASE_PKG" ] || die "base XCI not found: $BASE_PKG"
[ -f "$UPDATE_PKG" ] || die "update NSP not found: $UPDATE_PKG"
[ -f "$KEYS" ] || die "key file not found: $KEYS"
case "$BASE_PKG" in *.xci|*.XCI) ;; *) die "base package must be an XCI" ;; esac
case "$UPDATE_PKG" in *.nsp|*.NSP) ;; *) die "update package must be an NSP" ;; esac

if [ -n "${HACTOOL:-}" ]; then
    HT=$HACTOOL
else
    HT=$(command -v hactool 2>/dev/null || true)
fi
if [ -n "$HT" ] && [[ "$HT" != */* ]]; then
    HT=$(command -v "$HT" 2>/dev/null || true)
fi
[ -n "$HT" ] && [ -x "$HT" ] || die "hactool not found; put it on PATH or set HACTOOL"
if command -v shasum >/dev/null 2>&1; then
    SHA256_MODE=shasum
elif command -v sha256sum >/dev/null 2>&1; then
    SHA256_MODE=sha256sum
else
    die "shasum or sha256sum is required"
fi
ht() { "$HT" -k "$KEYS" --disablekeywarns --suppresskeys "$@"; }
sha256() {
    if [ "$SHA256_MODE" = shasum ]; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

DEST=$SK_ROOT/data/switch/skyrim/$PIN
if [ "$PROGRAM_ONLY" = 1 ]; then
    OUT=$DEST/Program
    PART=$DEST/Program.part
    MANIFEST=program.tsv
else
    OUT=$DEST/Update
    PART=$DEST/Update.part
    MANIFEST=packages.tsv
fi
META=$DEST/meta
MANIFEST_OUT=$META/$MANIFEST
for path in "$OUT" "$PART"; do
    path_exists "$path" && die "refusing to overwrite existing path: $path"
done
if path_exists "$META"; then
    [ -d "$META" ] || die "metadata path is not a directory: $META"
fi
path_exists "$MANIFEST_OUT" && die "refusing to overwrite existing path: $MANIFEST_OUT"

TMP_PARENT=$SK_ROOT/build/cache/skyrim
mkdir -p "$TMP_PARENT" || die "cannot create temporary workspace parent: $TMP_PARENT"
T=$(mktemp -d "$TMP_PARENT/extract.XXXXXX") || die "cannot create temporary workspace"
PART_CREATED=0
OUT_PUBLISHED=0
META_CREATED=0
MANIFEST_TMP=
DEST_CREATED=0
SUCCESS=0
cleanup() {
    rc=$?
    trap - EXIT
    rm -rf "$T"
    if [ "$SUCCESS" != 1 ]; then
        [ -z "$MANIFEST_TMP" ] || rm -f "$MANIFEST_TMP"
        [ "$PART_CREATED" != 1 ] || rm -rf "$PART"
        [ "$OUT_PUBLISHED" != 1 ] || rm -rf "$OUT"
        [ "$META_CREATED" != 1 ] || rmdir "$META" 2>/dev/null || true
        [ "$DEST_CREATED" != 1 ] || rmdir "$DEST" 2>/dev/null || true
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

if [ "$PROGRAM_ONLY" != 1 ]; then
    mkdir -p "$T/base/nca" || die "cannot create base package workspace"
    say "Unpacking base XCI"
    ht -x -t xci --securedir="$T/base/nca" "$BASE_PKG" >/dev/null 2>&1 || die "could not unpack base XCI"
fi
mkdir -p "$T/update/nca" || die "cannot create update package workspace"
say "Unpacking update NSP"
ht -x -t pfs0 --outdir="$T/update/nca" "$UPDATE_PKG" >/dev/null 2>&1 || die "could not unpack update NSP"

nca_field() {
    printf '%s\n' "$INFO" | awk -F: -v key="$1" '
        $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
            value = $2
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
            print value
            exit
        }
    '
}

extract_content_version() {
    local role=$1 meta_nca=$2 out="$T/$3-cnmt"
    local cnmts version
    VERSION=
    mkdir "$out" || die "cannot create CNMT workspace"
    ht -x -t nca --section0dir="$out" "$meta_nca" >/dev/null 2>&1 || die "could not extract $role content metadata"
    cnmts=("$out"/*.cnmt)
    [ ${#cnmts[@]} -eq 1 ] || die "expected exactly one CNMT in $role content metadata"
    version=$(od -An -tu4 -j 8 -N 4 "${cnmts[0]}" 2>/dev/null | tr -d '[:space:]') || die "could not read $role content version"
    [[ "$version" =~ ^[0-9]+$ ]] || die "invalid $role content version"
    VERSION=$version
}

select_ncas() {
    local role=$1 dir=$2 want_bktr=$3 wanted_meta_tid=$4
    local f ctype tid has_bktr
    local program_count=0 meta_count=0 wrong_title=0 wrong_role=0
    local files=("$dir"/*.nca)
    SELECTED=
    META_SELECTED=
    CONTENT_VERSION=
    [ ${#files[@]} -gt 0 ] || die "no NCAs found in $role package"
    for f in "${files[@]}"; do
        [ -f "$f" ] || continue
        INFO=$(ht -i -t nca "$f" 2>/dev/null) || die "could not inspect NCA metadata in $role package"
        ctype=$(nca_field "Content Type")
        tid=$(nca_field "Title ID" | tr '[:upper:]' '[:lower:]')
        case "$ctype" in
            Program)
                if [ "$tid" != "$TITLE_ID" ]; then
                    wrong_title=$((wrong_title + 1))
                    continue
                fi
                if printf '%s\n' "$INFO" | grep -Eq '^[[:space:]]*Partition Type:[[:space:]]*Patch RomFS[[:space:]]*$|BKTR'; then
                    has_bktr=1
                else
                    has_bktr=0
                fi
                if [ "$has_bktr" != "$want_bktr" ]; then
                    wrong_role=$((wrong_role + 1))
                    continue
                fi
                SELECTED=$f
                program_count=$((program_count + 1))
                ;;
            Meta)
                if [ "$tid" = "$wanted_meta_tid" ]; then
                    META_SELECTED=$f
                    meta_count=$((meta_count + 1))
                fi
                ;;
        esac
    done
    if [ "$program_count" -eq 0 ]; then
        [ "$wrong_title" -eq 0 ] || die "$role Program NCA title ID does not match $TITLE_ID"
        if [ "$wrong_role" -gt 0 ]; then
            if [ "$want_bktr" = 1 ]; then
                die "update Program NCA must have a BKTR Patch RomFS"
            else
                die "base Program NCA must not be a BKTR update"
            fi
        fi
        die "no matching $role Program NCA found"
    fi
    [ "$program_count" -eq 1 ] || die "ambiguous $role Program NCA selection"
    [ "$meta_count" -gt 0 ] || die "no matching $role Meta NCA found for content version"
    [ "$meta_count" -eq 1 ] || die "ambiguous $role Meta NCA selection"
    extract_content_version "$role" "$META_SELECTED" "$role"
    CONTENT_VERSION=$VERSION
}

shopt -s nullglob
if [ "$PROGRAM_ONLY" != 1 ]; then
    select_ncas base "$T/base/nca" 0 "$TITLE_ID"
    BASE_NCA=$SELECTED
    BASE_VERSION=$CONTENT_VERSION
fi
select_ncas update "$T/update/nca" 1 "$UPDATE_META_TITLE_ID"
UPDATE_NCA=$SELECTED
UPDATE_VERSION=$CONTENT_VERSION
if [[ "$PIN" =~ ^v[0-9]+$ ]]; then
    PIN_VERSION=$(printf '%s' "${PIN#v}" | sed 's/^0*//') || die "could not read update-pin"
    [ -n "$PIN_VERSION" ] || PIN_VERSION=0
    [ "$UPDATE_VERSION" = "$PIN_VERSION" ] || die "update CNMT content version $UPDATE_VERSION does not match pin $PIN"
fi
if [ "$PROGRAM_ONLY" = 1 ]; then
    say "Selected update title $TITLE_ID (content version $UPDATE_VERSION)"
else
    say "Selected title $TITLE_ID (base version $BASE_VERSION, update version $UPDATE_VERSION)"
fi

[ ! -d "$DEST" ] && DEST_CREATED=1
mkdir -p "$DEST" || die "cannot create destination: $DEST"
mkdir "$PART" || die "cannot create temporary output: $PART"
PART_CREATED=1
if [ "$PROGRAM_ONLY" = 1 ]; then
    say "Extracting updated ExeFS directly from the update NCA"
    ht -x -t nca --exefsdir="$PART/ExeFS" "$UPDATE_NCA" >/dev/null 2>&1 || die "program-only ExeFS extraction failed"
else
    say "Layering update over base and extracting ExeFS/RomFS"
    ht -x -t nca --basenca="$BASE_NCA" --exefsdir="$PART/ExeFS" --romfsdir="$PART/RomFS" "$UPDATE_NCA" >/dev/null 2>&1 || die "layered update extraction failed"
fi

has_files() { [ -d "$1" ] && [ -n "$(find "$1" -type f -print -quit 2>/dev/null)" ]; }
has_files "$PART/ExeFS" || die "extracted ExeFS is missing or contains no files"
if [ "$PROGRAM_ONLY" != 1 ]; then
    has_files "$PART/RomFS" || die "extracted RomFS is missing or contains no files"
fi
if [ ! -d "$META" ]; then
    mkdir "$META" || die "cannot create shared metadata directory: $META"
    META_CREATED=1
fi
MANIFEST_TMP=$(mktemp "$META/.${MANIFEST}.XXXXXX") || die "cannot create temporary manifest"
if [ "$PROGRAM_ONLY" != 1 ]; then
    BASE_PKG_SHA=$(sha256 "$BASE_PKG") || die "could not hash base XCI"
    UPDATE_PKG_SHA=$(sha256 "$UPDATE_PKG") || die "could not hash update NSP"
    BASE_NCA_SHA=$(sha256 "$BASE_NCA") || die "could not hash base Program NCA"
    UPDATE_NCA_SHA=$(sha256 "$UPDATE_NCA") || die "could not hash update Program NCA"
    {
        printf 'package_sha256\tnca_sha256\ttitle_id\tcontent_version\trole\tsource_package\n'
        printf '%s\t%s\t%s\t%s\tbase\t%s\n' "$BASE_PKG_SHA" "$BASE_NCA_SHA" "$TITLE_ID" "$BASE_VERSION" "$BASE_PKG"
        printf '%s\t%s\t%s\t%s\tupdate\t%s\n' "$UPDATE_PKG_SHA" "$UPDATE_NCA_SHA" "$TITLE_ID" "$UPDATE_VERSION" "$UPDATE_PKG"
    } > "$MANIFEST_TMP" || die "could not write packages.tsv"
else
    UPDATE_PKG_SHA=$(sha256 "$UPDATE_PKG") || die "could not hash update NSP"
    UPDATE_NCA_SHA=$(sha256 "$UPDATE_NCA") || die "could not hash update Program NCA"
    {
        printf 'package_sha256\tnca_sha256\ttitle_id\tcontent_version\trole\tsource_package\n'
        printf '%s\t%s\t%s\t%s\tupdate\t%s\n' "$UPDATE_PKG_SHA" "$UPDATE_NCA_SHA" "$TITLE_ID" "$UPDATE_VERSION" "$UPDATE_PKG"
    } > "$MANIFEST_TMP" || die "could not write program.tsv"
fi

path_exists "$OUT" && die "refusing to overwrite existing path: $OUT"
path_exists "$MANIFEST_OUT" && die "refusing to overwrite existing path: $MANIFEST_OUT"
mv "$PART" "$OUT" || die "could not publish extracted files"
PART_CREATED=0
OUT_PUBLISHED=1
mv "$MANIFEST_TMP" "$MANIFEST_OUT" || die "could not publish manifest"
MANIFEST_TMP=
SUCCESS=1
say "Wrote $OUT"
say "Wrote $META/$MANIFEST"

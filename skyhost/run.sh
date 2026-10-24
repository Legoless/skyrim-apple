#!/bin/sh
set -eu

skyhost_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$skyhost_dir/.." && pwd)
: "${SKY_WORK_DIR:=$repo_dir/skyrim-work}"
case "$SKY_WORK_DIR" in
    /*) ;;
    *) SKY_WORK_DIR="$(pwd -P)/$SKY_WORK_DIR" ;;
esac
export SKY_WORK_DIR
if [ -x "$SKY_WORK_DIR/build/tools/python/bin/python3" ]; then
    tool_env=$("$SKY_WORK_DIR/build/tools/python/bin/python3" -B "$repo_dir/tools/sky_tools.py" --env --work-dir "$SKY_WORK_DIR")
    eval "$tool_env"
fi
: "${SKY_REWRITE_DIR:=$SKY_WORK_DIR/build/rewrite-native}"
: "${SKY_ROMFS:=$SKY_WORK_DIR/data/switch/skyrim/base/Update/RomFS}"
: "${SKY_SAVE_DIR:=$SKY_WORK_DIR/run/save}"
: "${SKY_SHADER_CACHE_DIR:=$repo_dir/skyrim-work/build/shaders}"
: "${SKY_SHADER_CAPTURE_DIR:=$SKY_WORK_DIR/build/shader-captures}"
export SKY_ROMFS SKY_SAVE_DIR SKY_SHADER_CACHE_DIR SKY_SHADER_CAPTURE_DIR

if [ "${1:-}" = "--headless" ]; then
    export SKY_HEADLESS=1
    shift
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--headless]" >&2
    exit 2
fi
mkdir -p "$(dirname -- "$SKY_SAVE_DIR")"
make -C "$skyhost_dir" -j4
exec "$skyhost_dir/skyhost" --run "$SKY_REWRITE_DIR"

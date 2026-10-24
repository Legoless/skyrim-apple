#!/bin/bash
# Prepare Skyrim from the user's own cartridge dump and console keys.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

supports_python() {
    "$1" -B -c 'import sys; sys.exit(sys.version_info < (3, 9))' >/dev/null 2>&1
}

if [ -n "${SKY_PYTHON:-}" ]; then
    python_bin=$(command -v "$SKY_PYTHON") || python_bin=
    if [ -z "$python_bin" ] || ! supports_python "$python_bin"; then
        echo "SKY_PYTHON must point to a working Python 3.9 or newer interpreter." >&2
        exit 1
    fi
else
    python_bin=$(command -v python3) || python_bin=
    if [ -z "$python_bin" ] || ! supports_python "$python_bin"; then
        python_bin=/usr/bin/python3
        if ! supports_python "$python_bin"; then
            echo "Python 3.9 or newer is required. Install/select Xcode or set SKY_PYTHON=/path/to/python3." >&2
            exit 1
        fi
    fi
fi
exec "$python_bin" -B "$repo_dir/tools/sky_setup.py" "$@"

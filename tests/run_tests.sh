#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/textlite-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

CC="${CC:-cc}"
CFLAGS=(-std=c11 -O2 -Wall -Wextra -Werror)
PJSIP_FLAGS=()
while IFS= read -r token; do
    [[ -n "$token" ]] && PJSIP_FLAGS+=("$token")
done < <(pkg-config --static --cflags --libs libpjproject | tr ' ' '\n')

echo "Building binaries in $BUILD_DIR..."
"$CC" "${CFLAGS[@]}" "$ROOT_DIR/decode.c" -lm -o "$BUILD_DIR/decode"
"$CC" "${CFLAGS[@]}" "$ROOT_DIR/encode.c" -lm -o "$BUILD_DIR/encode"
"$CC" "${CFLAGS[@]}" "$ROOT_DIR/sip-onetime.c" -o "$BUILD_DIR/sip-onetime" "${PJSIP_FLAGS[@]}" -pthread
"$CC" "${CFLAGS[@]}" -DDECODE_LIBRARY_ONLY "$ROOT_DIR/sip-dialog.c" \
    "$ROOT_DIR/decode.c" -lm -o "$BUILD_DIR/sip-dialog" "${PJSIP_FLAGS[@]}" -lutil -pthread

export TEXTLITE_ROOT="$ROOT_DIR"
export TEXTLITE_DECODE="$BUILD_DIR/decode"
export TEXTLITE_ENCODE="$BUILD_DIR/encode"
export TEXTLITE_SIP_ONETIME="$BUILD_DIR/sip-onetime"
export TEXTLITE_SIP_DIALOG="$BUILD_DIR/sip-dialog"
export TEXTLITE_SIP="$BUILD_DIR/sip-onetime"

python3 -m unittest discover -s "$ROOT_DIR/tests" -p 'test_*.py' -v "$@"

#!/bin/sh
set -eu

runtime_packages="$HOME/runtime-packages"
if [ -d "$runtime_packages/usr/bin" ]; then
    PATH="$runtime_packages/usr/bin:$runtime_packages/usr/sbin:$runtime_packages/usr/games:$PATH"
    export PATH
fi
if [ -d "$runtime_packages/usr/lib" ] || [ -d "$runtime_packages/lib" ]; then
    LD_LIBRARY_PATH="$runtime_packages/lib:$runtime_packages/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
if [ -d "$runtime_packages/usr/share" ]; then
    current_xdg_data_dirs="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
    XDG_DATA_DIRS="$runtime_packages/usr/share:$current_xdg_data_dirs"
    export XDG_DATA_DIRS
fi

public_address="${SIP_PUBLIC_ADDRESS:-${SERVER_IP:-}}"
espeak_runtime="$HOME/espeak-runtime"
if [ -x "$espeak_runtime/usr/bin/espeak-ng" ]; then
    PATH="$HOME/espeak-bin:$PATH"
    ESPEAK_DATA_PATH="$espeak_runtime/usr/share/espeak-ng-data"
    export PATH ESPEAK_DATA_PATH
fi

set -- \
    --registrar "$SIP_REGISTRAR" \
    --identity "$SIP_IDENTITY" \
    --username "$SIP_USERNAME" \
    --command "$SIP_COMMAND" \
    --port "$SERVER_PORT" \
    --rtp-port "$SIP_RTP_PORT" \
    --bind "$SIP_BIND_ADDRESS" \
    --baud "$SIP_BAUD"

if [ -n "$public_address" ] && [ "$public_address" != "127.0.0.1" ]; then
    set -- "$@" --public-address "$public_address"
fi
if [ -n "${SIP_MARGIN:-}" ]; then
    set -- "$@" --margin "$SIP_MARGIN"
fi
if [ "${SIP_SPEAK:-0}" = "1" ]; then
    set -- "$@" --speak
fi
if [ -n "${SIP_LANG:-}" ]; then
    set -- "$@" --lang "$SIP_LANG"
fi
if [ -n "${SIP_GAP:-}" ]; then
    set -- "$@" --gap "$SIP_GAP"
fi

exec ./sip-onetime "$@"

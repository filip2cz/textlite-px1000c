#!/bin/sh
set -eu

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

exec ./sip "$@"

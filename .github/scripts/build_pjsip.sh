#!/usr/bin/env bash
set -euo pipefail

# Build and install minimal static PJSIP libraries for a target architecture.
# Only core SIP, UDP transport, WAV player, resampler, and waveform codecs
# (PCMA/PCMU/G.722) are kept; audio devices, video, TLS, SRTP, and third-party
# vocoders are disabled so the resulting sip binary has minimal dependencies.

TARBALL=""
PREFIX=""
HOST=""
CC_CMD=""
CXX_CMD=""
AR_CMD=""
RANLIB_CMD=""
CFLAGS_OPT="-O2"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tarball)
      TARBALL="$2"
      shift 2
      ;;
    --prefix)
      PREFIX="$2"
      shift 2
      ;;
    --host)
      HOST="$2"
      shift 2
      ;;
    --cc)
      CC_CMD="$2"
      shift 2
      ;;
    --cxx)
      CXX_CMD="$2"
      shift 2
      ;;
    --ar)
      AR_CMD="$2"
      shift 2
      ;;
    --ranlib)
      RANLIB_CMD="$2"
      shift 2
      ;;
    --cflags)
      CFLAGS_OPT="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$TARBALL" || -z "$PREFIX" || -z "$CC_CMD" || -z "$CXX_CMD" ]]; then
  echo "Usage: $0 --tarball PATH --prefix DIR --cc CC --cxx CXX [--host HOST] [--ar AR] [--ranlib RANLIB] [--cflags CFLAGS]" >&2
  exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

tar -xzf "$TARBALL" -C "$WORKDIR"
SRCDIR="$(find "$WORKDIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)"

# Strip C-only -std=... flags from CXXFLAGS; C++ is only probed by configure.
CXXFLAGS_OPT="${CFLAGS_OPT//-std=gnu99/}"

CONFIGURE_ARGS=(
  "--prefix=$PREFIX"
  --disable-shared
  --disable-pjsua2
  --disable-sound
  --disable-video
  --disable-sdl
  --disable-ffmpeg
  --disable-v4l2
  --disable-openh264
  --disable-vpx
  --disable-ssl
  --disable-upnp
  --disable-libuuid
  --disable-speex-aec
  --disable-gsm-codec
  --disable-g7221-codec
  --disable-speex-codec
  --disable-ilbc-codec
  --disable-opencore-amr
  --disable-silk
  --disable-opus
  --disable-bcg729
  --disable-lyra
  --disable-libsrtp
  --disable-libyuv
  --disable-libwebrtc
  "CC=$CC_CMD"
  "CXX=$CXX_CMD"
  "CFLAGS=$CFLAGS_OPT"
  "CXXFLAGS=$CXXFLAGS_OPT"
)

if [[ -n "$HOST" ]]; then
  CONFIGURE_ARGS+=("--host=$HOST")
fi
if [[ -n "$AR_CMD" ]]; then
  CONFIGURE_ARGS+=("AR=$AR_CMD")
fi
if [[ -n "$RANLIB_CMD" ]]; then
  CONFIGURE_ARGS+=("RANLIB=$RANLIB_CMD")
fi

pushd "$SRCDIR" >/dev/null
./configure "${CONFIGURE_ARGS[@]}"
make -j"$(nproc)" dep
make -j"$(nproc)" lib
make install
popd >/dev/null

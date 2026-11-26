# textlite-px1000c

Command-line tools for receiving and sending text over the audio modem in a
TEXT LITE PX1000 C, plus a SIP responder that can answer calls with generated
messages.

## Quick start

The easiest way to build is with the included Nix shell. It provides the
compiler and libraries needed for all three programs:

```sh
nix-shell
make
```

`make` builds `decode` and `encode`. It also builds `sip` when PJSIP is
available. To decode a recording or create a WAV message:

```sh
./decode recording.mp3
./encode -o message.wav "test abc123"
```

See [Dependencies](#dependencies) for the separate runtime and compilation
requirements.

## Dependencies

### Runtime

| Program or feature | Runtime dependencies |
| --- | --- |
| `decode` | FFmpeg, available as `ffmpeg` on `PATH` |
| `encode` | None; eSpeak NG (`espeak-ng` on `PATH`) only when speech is enabled with `--speak` |
| `sip` | The `encode` executable and `/bin/sh` for the configured command. A dynamically linked build also needs its PJSIP shared libraries and their runtime dependencies. |

The Pterodactyl egg installs `sip` and `encode` as static musl binaries, so
that deployment does not need PJSIP shared libraries. It bundles eSpeak NG
for the optional speech feature. `decode` is not part of the egg.

### Compilation

All programs are C11/POSIX programs. Building with the Makefile requires a C
compiler and `make`; `decode` and `encode` also link the system math library.
`make` includes `sip` when PJSIP development files are available.

| Target | Additional build requirements |
| --- | --- |
| `decode`, `encode` | No project-specific libraries beyond the C compiler and math library |
| `sip` | PJSIP development files (`libpjproject` headers and libraries), `pkg-config`, and POSIX threads; PJSIP's pkg-config metadata supplies its link flags |

To run `make test`, also install Bash, Python 3, FFmpeg, eSpeak NG, and the
PJSIP `pjsua` command-line client. The test target builds all three programs,
so the `sip` compilation requirements apply as well.

## Decode a recording

`decode` accepts an audio file supported by FFmpeg. It converts the audio to
mono, signed 16-bit PCM at 22,050 Hz, then searches for PX FSK transmissions at
300, 600, or 1200 baud. The supported signal uses 1300 Hz and 1700 Hz tones
and 7E2 serial frames. The baud rate is detected automatically.

```sh
./decode recording.mp3
./decode -v recording.wav
```

Decoded text goes to standard output. `-v` also prints the detected baud rate
to standard error. FFmpeg must be installed and available as `ffmpeg` on
`PATH`.

To build just the decoder without Nix:

```sh
cc -std=c11 -O2 -Wall -Wextra decode.c -lm -o decode
```

## Encode a message

`encode` creates a mono, signed 16-bit PCM WAV file at 44,100 Hz. It accepts
printable 7-bit ASCII, tabs, and line breaks, and supports 300, 600, and 1200
baud. Text can be passed as an argument or read from redirected standard
input:

```sh
./encode -o message.wav "test abc123"
./encode -b 1200 -o message.wav "test abc123"
printf '%s' "test abc123" | ./encode -o message.wav
```

Options:

| Option | Purpose | Default |
| --- | --- | --- |
| `-o path` | Output WAV path | `encoded.wav` |
| `-b baud` | Baud rate: 300, 600, or 1200 | 300 |
| `-m value` | Margin byte, from 0 to 127 | 40 |
| `-s`, `--speak` | Speak the message before the modem transmission; needs `espeak-ng` | Off |
| `-l`, `--lang code` | eSpeak NG voice or language code | eSpeak NG default |
| `-g`, `--gap seconds` | Silence after speech, from 0 to 600 seconds | 3 seconds |

For example, to speak the message in Czech before sending it:

```sh
./encode --speak --lang cs --gap 3 -o message.wav "test abc123"
```

Without text arguments, `encode` reads redirected input. Interactive or empty
input displays the usage help. Speech is optional; without `--speak`, eSpeak
NG is not run. To build just the encoder without Nix:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
```

## Answer calls with SIP

`sip` registers one SIP account over UDP and answers one incoming call at a
time. It runs a shell command, passes the command's standard output to
`encode`, plays the resulting WAV into the call, and hangs up when playback
finishes. Command output must be valid encoder input and is limited to 7,400
bytes. A concurrent incoming call is rejected as busy.

Build inside `nix-shell` (or another environment with PJSIP's
`libpjproject` pkg-config metadata):

```sh
cc -std=c11 -O2 -Wall -Wextra sip.c -o sip $(pkg-config --static --cflags --libs libpjproject) -pthread
```

Set the account password in the environment and start the responder:

```sh
export SIP_PASSWORD='your-sip-password'
./sip --registrar sip:pbx.example.net \
  --identity sip:1001@pbx.example.net --username 1001 \
  --command 'printf "%s\\n" "test abc123"' --encoder ./encode
```

Required options are `--registrar`, `--identity`, `--username`, and
`--command`. The password can instead be passed with `--password`. Useful
network and encoder options are:

| Option | Purpose |
| --- | --- |
| `--encoder path` | Encoder executable; default is `./encode` |
| `--port N` | Local SIP UDP port; default 0 selects an available port |
| `--rtp-port N` | Local even RTP UDP port; default 0 selects one automatically; RTCP uses the next port |
| `--bind address-or-interface` | Local IPv4 address or interface name; default binds all interfaces |
| `--public-address IPv4` | Public IPv4 address advertised in SIP and RTP SDP, for hosts behind NAT |
| `--baud N` | Encoder baud rate: 300, 600, or 1200; default 300 |
| `--margin N` | Encoder margin byte, from 0 to 127; default 40 |
| `--speak`, `--lang code`, `--gap seconds` | Pass speech options through to `encode` |
| `--encoder-arg ARG` | Forward an additional argument to `encode`; may be repeated |
| `-- ARG...` | Forward trailing arguments to `encode` |
| `-h`, `--help` | Show the full option summary |

The `--bind` address applies to SIP and RTP. When binding by interface name,
the first listed IPv4 address is used. If the server is behind NAT, configure
`--public-address` with an IPv4 address reachable by the SIP provider. `sip`
disables VAD and echo cancellation and prioritizes waveform codecs so FSK
tones are preserved. Stop it with Ctrl+C.

### Pterodactyl deployment

Import [the SIP egg](pterodactyl/egg-sip.json) from Admin → Eggs → Import Egg.
The installer clones the configured repository and branch, builds PJSIP 2.16
and statically links `sip` and `encode` with musl. It also bundles eSpeak NG,
its libraries, and voice data, so speech works without eSpeak NG in the
runtime image.

Allocate a UDP port for SIP and set it as the server's primary allocation.
Also allocate the configured RTP port and the following port for RTCP. The
default RTP port is 40000, so allocate UDP ports 40000 and 40001. These ports
must be reachable from the SIP provider for call audio to work. The startup
wrapper advertises Pterodactyl's `SERVER_IP`; if it is behind another NAT, set
the egg's Public IPv4 Address to the externally reachable address.

Set the registrar, account identity, authentication username, password, and
message command in the egg's Startup settings. The command's standard output
becomes the transmitted message. The password is passed through
`SIP_PASSWORD`. Enable Speak Message in Startup to have eSpeak NG read the
message before its modem transmission.

## Development

The project has three POSIX C command-line programs: `decode`, `encode`, and
`sip`. The Makefile builds the available programs and runs the test suite:

```sh
make
make test
```

The Nix shell is started from the project directory with `nix-shell`. It
includes the compiler, FFmpeg, eSpeak NG, PJSIP, pkg-config, OpenSSL, and ALSA
link libraries. See [AGENTS.md](AGENTS.md) for the detailed architecture and
map of the implementation in each source file. Keep it and this README in
sync with code and configuration changes.

## Project files

- `decode.c`, `encode.c`, `sip.c` — decoder, encoder, and SIP responder
- `Makefile` — local build and test targets
- `LICENSE` — MIT license
- `tests/` — test suite for the command-line tools
- `shell.nix` — Nix development environment
- `pterodactyl/egg-sip.json`, `pterodactyl/start.sh` — Pterodactyl egg and startup wrapper
- `.github/workflows/main.yml` — CI, cross-compilation, QEMU smoke tests, and draft releases
- `.github/scripts/build_pjsip.sh` — static PJSIP build helper used by CI and the egg
- `test_abc123-*.mp3` — sample recordings carrying `test abc123` at different speeds
- `AGENTS.md` — agent instructions and map of the implementation in each source file

## Development tools and protocol references

This project was developed with OpenAI Codex using the GPT-6 Luna model.
Development tools included a C compiler (`cc`), FFmpeg for audio conversion
and inspection, and Python 3 for signal analysis. Protocol details were
checked against the [PX-1000 technical manual](https://www.cryptomuseum.com/crypto/philips/px1000/files/px1000_service_manual.pdf)
and the [PX 1000 C operating instructions](https://www.manualslib.com/manual/3453877/Text-Lite-Px-1000-C.html).

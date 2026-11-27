# Agent Guide

This file is the source-oriented map of the project. Read it before changing
behavior, then inspect the referenced source and tests. The C files are the
source of truth; this guide and `README.md` must describe their current
behavior.

## Working rules

- Keep code, comments, and user-facing program output in English.
- Start every C source file with a concise comment stating its purpose.
- Comment protocol details, non-obvious algorithms, ownership, and thread
  interactions. Do not narrate straightforward code.
- For every code, configuration, or documentation change, update both this
  file and `README.md` in the same change. Keep the architecture map, user
  instructions, and dependency descriptions consistent with the implementation.
- Keep development-tool and AI-model attribution in `README.md` accurate; if
  it changes, update both documents.
- Do not claim support for a modem profile, protocol feature, or input format
  unless the code implements it.
- Keep generated binaries and temporary test artifacts out of source control.

## Project map

This repository contains four POSIX C command-line programs:

- `decode.c`: FFmpeg-backed audio normalization, FSK demodulation, serial-frame
  search, and decoded text output. Its in-process PCM entry point is declared
  in `decode.h` and is used by `sip-dialog.c`.
- `encode.c`: text normalization, optional eSpeak NG speech capture, FSK sample
  generation, and RIFF/WAV output.
- `sip-onetime.c`: PJSIP registration and call handling; runs a configured
  shell command once, gives its output to `encode`, and plays the WAV into an
  incoming call.
- `sip-dialog.c`: PJSIP registration and call handling; keeps a configured
  interactive shell command alive for the call, decodes caller messages, feeds
  them to the command, and plays its output back into the call.

Other files:

- `decode.h`: in-process PCM decoder interface shared by `decode.c` and
  `sip-dialog.c`.
- `Makefile`: builds `decode` and `encode`, conditionally adds
  `sip-onetime` and `sip-dialog` when `pkg-config` can find `libpjproject`,
  retains `sip` as an alias for the one-time responder, and exposes `test`.
- `shell.nix`: Nix development environment with the C compiler, FFmpeg,
  eSpeak NG, PJSIP, pkg-config, OpenSSL, and ALSA libraries.
- `tests/run_tests.sh`, `tests/helpers.py`, and `tests/test_*.py`: temporary
  test builds, shared test helpers, and CLI/audio/SIP integration coverage.
- `.github/workflows/main.yml`: tests, cross-compilation, QEMU smoke checks,
  and draft releases.
- `.github/scripts/build_pjsip.sh`: builds the reduced static PJSIP used by CI
  and both Pterodactyl egg installers.
- `pterodactyl/egg-sip-onetime.json` and `egg-sip-dialog.json`: importable
  Pterodactyl eggs and install scripts for the one-time and interactive
  responders.
- `pterodactyl/start-onetime.sh` and `start-dialog.sh`: source templates for
  their eggs' argument-safe launchers; each installer embeds its matching
  launcher in the server directory. They also expose optional bundled Alpine
  packages to the responder command.
- `test_abc123-{300,600,1200}Bd.mp3`: sample recordings containing the text
  `test abc123` at the indicated baud rate.
- `README.md`: user guide and command examples. Its Dependencies section
  separates runtime, compilation, and additional test-suite requirements.

## Shared protocol and data conventions

- The implemented PX audio profile uses 1300 Hz for MARK/bit 1 and 1700 Hz
  for SPACE/bit 0.
- Serial bytes use 7E2: one zero start bit, seven data bits least-significant
  bit first, one even-parity bit, and two one stop bits.
- The supported baud rates are 300, 600, and 1200. Do not imply support for
  other baud rates, tone pairs, serial layouts, encryption, or modem profiles.
- Text is 7-bit ASCII. `encode` accepts printable ASCII, tabs, CR, and LF;
  decoded output keeps printable ASCII and tabs and translates line endings.
- The normal text transmission is unencrypted. The encoder writes the PX
  synchronization sequence, margin byte, text, EOT padding, and MARK tail.
- `decode` works with FFmpeg-normalized mono signed 16-bit little-endian PCM at
  22,050 Hz. `encode` writes mono signed 16-bit little-endian PCM WAV at
  44,100 Hz.
- Errors and usage go to standard error. `decode` writes decoded text to
  standard output; the other programs create files or use child-process pipes.

## `decode.c`: receive path

### Entry point and ownership

`main` parses one audio path and optional `-v`; `-v` can appear before or after
the path, and `--` makes the next argument a path even if it starts with `-`.
Invalid or repeated positional arguments print usage and return 2. Processing
errors return 1. `main` owns and frees the PCM sample array, each rate's symbol
array, candidate byte arrays, and the winning message.

The receive pipeline is:

```text
audio path -> read_ffmpeg_pcm -> normalized PCM
           -> demodulate for each baud -> Symbol windows
           -> find_message -> best 7E2 candidate
           -> print_message -> stdout
```

### Input conversion: `read_ffmpeg_pcm`

- Creates a pipe and forks; the child redirects stdout and calls `execlp` for
  `ffmpeg` directly. The path is an argv item, never interpolated into a shell.
- FFmpeg decodes the container/codec and writes raw mono `pcm_s16le` at
  22,050 Hz. Diagnostics remain on stderr.
- The parent reads up to 512 MiB, growing its buffer as needed, waits for the
  child, and rejects a nonzero child status, empty output, or an odd byte
  count. It converts little-endian byte pairs to `int16_t` samples.
- Therefore FFmpeg is a runtime dependency of `decode`; audio formats depend
  on the installed FFmpeg build.

### FSK windows: `demodulate`

- Precomputes sine/cosine reference values for 1300 Hz and 1700 Hz at the
  selected window length.
- Slides the window by one PCM sample. For each window, correlates samples
  with both tones, compares squared correlation energy, and records the
  stronger tone as a bit.
- Stores `Symbol { bit, confidence, rms }`: confidence is the normalized
  difference between tone energies, and RMS measures overall signal level.
  It also returns the maximum window RMS for relative level gating.
- Returns no symbols when the input is too short or allocation fails.

### Clock and frame search: `find_message`

- `main` invokes demodulation separately for 300, 600, and 1200 baud. The
  nominal bit period is `22050 / baud`; windows are 60 samples at 300 baud,
  30 at 600 baud, and 19 at 1200 baud.
- Searches bit periods from -0.75% to +0.75% of nominal: 0.05-sample steps,
  narrowed to 0.01 at 1200 baud. For each period it sweeps phase in
  half-sample increments and all 11 possible frame alignments.
- Each candidate frame samples the center of its bit window. A frame is valid
  only if every symbol passes the RMS gate `max(200, 0.06 * max_rms)` and the
  baud-specific confidence gate (0.22 at 300, 0.10 at 600, 0.035 at 1200),
  the start bit is zero, even parity is correct, and both stop bits are one.
- Consecutive valid frames form a run. Runs shorter than 12 bytes are ignored.
  The candidate score is byte count plus 0.35 times the count of printable or
  accepted whitespace bytes; the highest score across alignments and baud
  rates wins.
- The winning candidate stores bytes, count, score, and detected baud rate.
  Keep timing, thresholds, and frame-format edits consistent with the
  encoder and sample/round-trip tests.

### Output: `print_message`

- Removes leading NUL synchronization bytes and trailing control-byte/space
  padding.
- Converts CR and LF to output newlines, avoiding an extra newline for CRLF.
  Printable ASCII and tabs are emitted; other bytes are skipped.
- Adds a final newline when needed. It returns whether any printable content
  was written, allowing `main` to report a valid-but-empty/nonprintable run.
- `-v` reports the selected baud rate on stderr only after a candidate is
  found.

## `encode.c`: transmit path

### Entry point and input normalization

`main` parses `-o`, `-m`, `-b`, `-s`/`--speak`, `-l`/`--lang`, `-g`/`--gap`,
and `--`. Defaults are `encoded.wav`, margin 40, 300 baud, speech disabled, and
3 seconds of speech gap. Text arguments after options are joined by spaces;
otherwise redirected stdin is read. With no text, an interactive stdin prints
usage. Empty input and whitespace-only line endings are rejected.

- Text is capped at 7,400 bytes. Valid content is printable 7-bit ASCII plus
  tabs and line breaks; CRLF, LF, and CR normalize to CR.
- `normalize_text` trims trailing CR bytes, rejects empty text, then appends
  two CR terminator bytes. It allocates a separate normalized buffer; `main`
  frees raw input and later frees normalized text.
- Margin must be 0..127. Baud must be 300, 600, or 1200. Gap must be finite
  and 0..600 seconds. These option errors return 2; content/encoding errors
  return 1.

### Optional speech path

- When speech is enabled, `synthesize_speech` strips only the normalized
  text's trailing CR bytes, changes internal CR and tabs to spaces, and forks
  `espeak-ng` without a shell. It captures `--stdout` WAV data through a pipe
  and caps captured output at 128 MiB. `-l`/`--lang` maps to eSpeak NG's `-v`.
- `parse_speech_wav` scans RIFF chunks (including padding and intervening
  chunks), requiring PCM format 1, 16-bit samples, one or two channels, and a
  sample rate from 8,000 through 192,000 Hz. eSpeak's placeholder data size is
  accepted as extending to captured EOF when the data chunk is truncated by
  the pipe.
- `append_speech` averages stereo channels to mono and linearly interpolates
  frames to 44,100 Hz. `write_silence` appends the selected silence after the
  speech. Speech audio and gap precede the modem synchronization header.
- Without `--speak`, no eSpeak process is launched. eSpeak NG is an optional
  runtime dependency, not a normal encoder dependency.

### PCM and protocol generation

`AudioWriter` owns the output `FILE`, an 8 KiB byte buffer, buffered byte
count, emitted PCM byte count, FSK phase, baud, and sample remainder.

- `write_sample` writes signed samples as little-endian byte pairs;
  `flush_pcm` batches them to disk and updates the data-byte count.
- `write_tone_bit` emits a sine wave at 1300 Hz for 1 or 1700 Hz for 0. The
  phase continues across consecutive bits. A remainder accumulator
  distributes fractional samples per bit so the long-term bit rate remains
  exact at 44,100 Hz, including 600 and 1200 baud.
- `write_mark_duration` emits a continuous-phase 1300 Hz MARK for a requested
  number of milliseconds. `write_serial_byte` frames a 7-bit byte as 7E2;
  `write_serial_bytes` repeats it over a buffer.
- `encode` writes an initial placeholder 44-byte PCM WAV header. It then
  writes optional speech and silence, 500 ms MARK, 16 NUL frames, 800 ms
  MARK, four NUL sync frames, margin byte, normalized text, 32 `0x04` EOT
  bytes, and a one-second MARK tail. Finally it flushes samples, seeks back,
  and patches RIFF/data sizes. Output is mono 16-bit PCM at 44,100 Hz.
- WAV size fields are 32-bit; oversized output is rejected. File creation,
  buffering, child synthesis, and parse failures are reported to stderr and
  return failure.

## `sip-onetime.c`: one-time SIP responder

### Configuration and network setup

`AppConfig` holds parsed credentials, command/encoder settings, baud/margin,
speech options, forwarded encoder arguments, SIP/RTP ports, bind target, and
public address. Defaults are encoder `./encode`, 300 baud, and SIP port 0;
RTP port 0 requests automatic assignment. `--rtp-port` otherwise requires an
even port from 2 through 65534. At most 64 extra encoder arguments are stored.

`parse_arguments` accepts the options shown by `print_usage`; `--speak` is a
flag, while `--` forwards all remaining words to the encoder. The required
registrar and identity must start with `sip:`; username and command must be
nonempty. Password comes from `--password` or, if absent, `SIP_PASSWORD`.
Public address must be IPv4. `--bind` accepts IPv4 or an interface name;
`resolve_bind_address` uses the interface's first IPv4 address. The selected
local address is applied to SIP and RTP; public address is advertised for SIP
and RTP SDP.

`main` creates and initializes PJSUA, installs call/registration callbacks,
disables VAD and echo cancellation, applies modem codec priorities, creates a
UDP transport, starts PJSUA, and selects the null sound device. It adds one
account with optional SIP Digest credentials and then waits for shutdown
signals while reaping completed call workers. The null sound device means no
local microphone or speaker is used.

### Shared call state and callback ownership

The globals around `state_mutex` and `state_changed` coordinate the active
call ID, active audio slot, media readiness, player ID, playback completion,
worker joinability/completion, shell process group, and shutdown flag. Every
shared state access is protected by the mutex except `stop_requested`, which
is a `sig_atomic_t` set by the signal handler. Condition waits release the
mutex while sleeping.

- `on_incoming_call` reserves the one-call slot before answering. It rejects
  the call with BUSY if a call is active, a worker is still joinable, or
  shutdown started. Otherwise it answers and creates `call_worker`.
- `on_call_media_state` finds active audio media with encoding/transmit
  direction and publishes its conference slot, then wakes the worker.
- `on_call_state` marks the matching call inactive when PJSUA reports
  disconnect. `on_registration_state` logs registration status.
- `on_player_eof` marks matching WAV playback complete and wakes the worker.
- PJSUA callbacks update state and signal waiters; command execution,
  encoding, playback waits, and cleanup belong to the worker.
- `reap_finished_worker` claims a completed thread under the mutex and joins
  it outside the lock. Do not join while holding `state_mutex`.

### Command and encoder subprocesses

`call_worker` registers itself with PJ, prepares the encoded WAV, waits up to
30 seconds for transmit media, starts a WAV player, connects it to the call,
and waits for EOF. The playback timeout is WAV duration plus 30 seconds. On
EOF it allows a 250 ms RTP tail, then disconnects/destroys the player, removes
the temporary WAV, and hangs up. Every path converges on cleanup; failures
also end the accepted call.

- `spawn_shell_command` uses `posix_spawn` for `/bin/sh -c`, captures stdout,
  sends stdin from `/dev/null`, and puts the shell in its own process group.
  It accepts at most 7,400 bytes, requires nonempty output and zero exit
  status, and stops the group on errors or shutdown. This shell is the
  configured message-command interface.
- `run_encoder` uses `posix_spawnp`, not a shell. It sends captured text on
  encoder stdin and constructs argv from `--encoder`, baud, optional margin,
  speech options, gap, and forwarded arguments. It waits for a successful
  encoder exit.
- `make_call_audio` creates a unique `/tmp/textlite-sip-XXXXXX.wav` path,
  closes the pre-created file, runs the encoder, checks that the output is
  longer than a 44-byte header with an even-length PCM payload, and returns
  the sample count. The worker removes the path in all cleanup cases.
- `stop_command_group` sends SIGTERM to the shell process group (falling back
  to the child PID); shutdown signals the current group and hangs up the call
  before joining the worker and destroying PJSUA.

### RTP audio quality

`configure_modem_codecs` raises PCMA to highest priority, PCMU just below it,
and G.722 above normal; it lowers Speex, iLBC, and GSM. Waveform codecs retain
FSK tones better than speech vocoders. The reduced static PJSIP build also
disables unsupported vocoders and multimedia features, retaining the audio
components needed by this responder.

## `sip-dialog.c`: interactive SIP responder

### Call and command lifecycle

The dialog responder shares the SIP configuration, registration, account,
codec, worker, and encoder options with `sip-onetime.c`. `--command` remains a
shell command and is started once per accepted call with stdin and stdout
connected to a pseudo-terminal with input echo disabled. Its stdout is
collected until 500 ms of inactivity (up to 7,400 bytes). Before encoding,
ANSI escape sequences and unsupported terminal control bytes are removed,
CR/CRLF endings are normalized, and all blank lines, including
whitespace-only lines, are removed. Indentation on nonblank lines remains.
The normalized output is cached for `?repeat`. The command remains running after
playback so later caller messages can be sent to its stdin. A call ends when
the caller hangs up, the command exits, or an operation fails.

An incoming message that contains only `?repeat` after trimming ASCII spaces,
tabs, and line endings is handled by the responder. It replays the most recent
command output that was sent, without writing the request to the command. If no
response has been sent yet, the request is ignored.
Every successfully decoded caller message is printed in escaped one-line form
to standard error before the responder handles it. `sip-dialog` removes one
leading `(` from the decoded text because it is the default margin byte (40),
not message content. Command responses are logged separately as `Sending to
PX: ...` after output trimming and line-ending normalization.

### Receive audio and message boundaries

- The media bridge is configured at 22,050 Hz. A custom conference sink
  receives mono signed 16-bit PCM resampled to that rate; its callback only
  collects samples and signals the worker.
- Capture starts after each outgoing response. Leading silence is discarded;
  a frame must exceed 220 RMS and show at least 0.08 normalized energy at the
  1300/1700 Hz tones to begin or continue a message. At least 250 ms of tone
  activity and then 600 ms without it closes the message. The sample buffer is
  capped at 16 Mi samples.
- `decode_pcm_samples` in `decode.c` runs the same 300/600/1200 baud, 7E2
  decoder used by the file CLI, without FFmpeg. Decoded text is written to the
  command's stdin. When the command produces output, the worker encodes and
  plays it, then waits for the next caller message.
- The worker owns the command pseudo-terminal, receive buffers, and temporary
  WAVs. The process keeps one custom conference sink and its pool alive across
  calls; its `on_destroy` handler releases the pool. Each worker connects and
  disconnects it for its active call. PJSUA callbacks only publish media and
  call state changes under `state_mutex`.

## Build, dependencies, and tests

- `make all` builds `decode` and `encode`; it adds `sip-onetime` and
  `sip-dialog` when `pkg-config --static --libs libpjproject` returns flags.
  The `sip` target remains an alias for `sip-onetime`. Default C flags are
  C11, optimization, common warnings, and `-Werror`.
- `decode` and `encode` link `libm`. Both SIP programs link PJSIP's pkg-config
  flags and POSIX threads; `sip-dialog` also links the in-process decoder,
  `libm`, and the pseudo-terminal library. The Nix shell supplies the project
  development dependencies.
- Runtime programs: `decode` invokes `ffmpeg`; `encode` invokes `espeak-ng`
  only for `--speak`; both SIP programs require `encode` and `/bin/sh` for the
  configured command. `sip-dialog` performs PCM decoding in process and does
  not invoke FFmpeg. Dynamically linked SIP programs also require their PJSIP
  shared libraries. Each Pterodactyl egg statically links its SIP responder
  and `encode` with musl and bundles eSpeak NG for the optional speech path.
- `make test` depends on Bash, Python 3, PJSIP plus `pkg-config`, the
  pseudo-terminal development library, FFmpeg, eSpeak NG, and the `pjsua`
  command-line client. It first builds configured
  targets, then `tests/run_tests.sh` builds fresh binaries in a temporary
  directory, exports their paths to the test process, and runs unittest
  discovery. Keep test dependencies distinct from normal runtime requirements.

Test coverage map:

- `tests/test_decode.py`: CLI validation, missing files, silence rejection,
  sample recordings at all supported rates, option ordering/`--`, and
  resampling a stereo MP3 through FFmpeg.
- `tests/test_encode.py`: CLI validation, empty and invalid text, size limit,
  WAV structure, encoder/decoder round trips at all rates, stdin and `--`, and
  speech language/gap behavior.
- `tests/test_sip.py`: help and CLI validation for both SIP programs; one-time
  incoming-call playback, encoder option forwarding, failure recovery, and busy
  rejection; plus dialog call persistence, response playback, and blank-line
  filtering. It uses PJSIP's `pjsua` client to place local calls.
- `tests/helpers.py`: environment-selected binary paths, subprocess helpers,
  WAV inspection, silence WAV generation, and free UDP/RTP port selection.

## CI and cross-build helper

`.github/workflows/main.yml` runs the test suite in `nix-shell` first. Once
tests pass, architecture jobs build `decode`, `encode`, `sip-onetime`, and
`sip-dialog` for
x86_64, x86, AArch64, ARMv7, ARMv5, and RISC-V 64-bit using target toolchains
and PJSIP 2.16. Artifacts feed a QEMU smoke-check job that runs each binary's
help path and checks RISC-V ELF metadata. A push to `main` creates a draft
release containing the architecture binaries after tests and smoke checks
pass; pull requests do not publish releases.

`.github/scripts/build_pjsip.sh` takes a PJSIP source tarball, install prefix,
and compiler/tool settings. It configures PJSIP as static-only and disables
audio devices, video, TLS, SRTP, FFmpeg integration, and optional speech
codecs. It keeps SIP/UDP, WAV playback, resampling, and PCMA/PCMU/G.722 needed
for tone-safe call audio, then runs PJSIP's dependency, library, and install
build steps. Update this helper and its callers together if the feature set or
target matrix changes.

## Pterodactyl packaging

`pterodactyl/egg-sip-onetime.json` and `pterodactyl/egg-sip-dialog.json` are
PTDL_v2 exports. Their installers:

1. Installs Alpine build tools and downloads eSpeak NG plus musl into a private
   server-directory root. When `SIP_PACKAGES` is set, it also bundles those APK
   packages below `/mnt/server/runtime-packages`.
2. Clones the configured source repository and branch, downloads PJSIP 2.16,
   and uses `.github/scripts/build_pjsip.sh` to build the minimal static libs.
3. Statically links the matching `sip-onetime` or `sip-dialog` responder and
   `encode`, installs them, embeds the matching startup script, and installs an
   eSpeak wrapper that invokes the bundled musl loader.

Each startup script sources/configures `SERVER_PORT` for SIP, the configured
RTP port, bind target, baud, and optional encoder settings; it reads the SIP
password from `SIP_PASSWORD`. It adds bundled package binary and library paths
to `PATH` and `LD_LIBRARY_PATH` before starting the responder. It advertises
`SIP_PUBLIC_ADDRESS` if supplied, otherwise `SERVER_IP`. The allocation must
include the SIP UDP port and both the RTP port and its following RTCP port.
The default RTP port is 40000.
`pterodactyl/start-onetime.sh` and `pterodactyl/start-dialog.sh` are the
maintained source templates for the launchers embedded by their respective
egg install scripts; keep each embedded copy in sync with its template.

The egg metadata uses a JSON-encoded startup-detection string, email-formatted
author, and text field types. Egg variables are documented in the JSON; when
adding or changing a variable, update the embedded wrapper and README setup
instructions too.

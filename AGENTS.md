# Agent Instructions

## Project conventions

- Keep the program and all user-facing output in English.
- Write source-code comments in English.
- For every code, configuration, or documentation change, update both
  `AGENTS.md` and `README.md` in the same change. Keep this architecture
  description and the README's user instructions in sync with the code.
- Keep the development-tool and AI-model attribution in the README accurate;
  update both documents together if that attribution changes.
- Do not describe support for a modem profile unless the implementation
  actually supports it.

## Project architecture

The project contains three POSIX C command-line tools. `decode.c` and
`encode.c` implement the audio receive and send paths. `sip.c` uses PJSIP for
SIP signaling and audio transport, and connects incoming calls to the encoder.

### 1. Command-line entry point

`main` accepts one audio-file path and an optional `-v` flag, which may appear
before or after the path. It coordinates input conversion, signal
demodulation, frame search, and text output. Errors and usage information go
to standard error; decoded text goes to standard output. The detected baud
rate is written to standard error only when `-v` is supplied. Invalid command
lines print the usage syntax and a short summary of the `-v`, `--`, and input
path arguments.

### 2. Audio input and normalization

`read_ffmpeg_pcm` starts FFmpeg with `fork`/`exec` and reads its standard
output through a pipe. The file path is passed as an argument rather than
interpolated into a shell command. FFmpeg decodes the container and codec and
normalizes the audio to mono, signed 16-bit little-endian PCM at 22,050 Hz.
The function checks the child process status, rejects empty or incomplete
PCM, and limits the amount of buffered audio.

FFmpeg is therefore a runtime dependency. The decoder can accept any input
format supported by the installed FFmpeg build; `decode.c` itself only
processes the normalized PCM stream.

### 3. FSK demodulation

`demodulate` examines overlapping windows sized for the candidate baud rate.
It measures signal energy at the PX modem's two frequencies, 1300 Hz and
1700 Hz, and selects the stronger tone as the bit value. Each window also
receives a tone-confidence value and an RMS level. These values let the frame
search reject weak or ambiguous audio.

### 4. Bit timing and serial-frame search

`main` tries all supported speeds: 300, 600, and 1200 baud. `find_message`
searches each candidate bit period at the chosen 22,050 Hz sample rate. The
period is swept over a small clock-error range, and half-sample phase steps
accommodate recording and MP3 timing offsets. Detection windows are 60 samples
at 300 baud, 30 at 600 baud, and 19 at 1200 baud. The 1200-baud period sweep
uses finer increments because small timing errors accumulate quickly at that
rate.

The parser treats each serial frame as 11 bits: one zero start bit, seven
data bits sent least-significant bit first, one even-parity bit, and two one
stop bits (7E2). It also requires sufficient signal level and tone
confidence. Consecutive valid frames form a candidate run; the longest
readable run is retained, with a minimum length to avoid accepting isolated
noise as a message.

The current implementation supports the 300, 600, and 1200-baud, 1300/1700 Hz
profiles. Supporting other frequencies or serial formats requires changing
the demodulator and frame parser, then updating this file and `README.md`.

### 5. Text output

`print_message` removes leading NUL synchronization bytes, converts carriage
returns and line feeds to output newlines, and omits trailing control-byte
padding. Printable ASCII text is written to standard output.

### 6. Text-to-audio encoding

`encode.c` accepts text as command-line arguments or from standard input. It
validates 7-bit printable ASCII (plus tabs and line breaks), normalizes line
endings to carriage returns, and appends the PX text terminator. The default
margin byte is 40; `-m` can choose another value from 0 to 127. Invalid
command lines print the usage syntax and a summary of `-o`, `-m`, `-b`, `-s`,
`-l`, `-g`, `--`, and text/stdin input. If text is omitted, redirected stdin is read; an
interactive or empty stdin produces the usage help instead.

The encoder writes a RIFF WAV file containing mono, signed 16-bit PCM at
44,100 Hz. Its `AudioWriter` emits continuous-phase FSK samples at 300, 600,
or 1200 baud. A sample-remainder accumulator distributes fractional samples
across bits, preserving the selected average baud rate (600 and 1200 do not
have an integer number of samples per bit). It writes the PX synchronization
header, then 7E2-framed bytes for the margin and text, followed by 32 ASCII
0x04 end-of-text bytes and a one-second MARK tone. This is the normal,
unencrypted text path.

When `-s` or `--speak` is selected, `synthesize_speech` runs the optional
`espeak-ng` executable as a child process without a shell and captures its WAV
output through a pipe. If `-l` or `--lang` is supplied, its eSpeak NG voice or
language code is passed to eSpeak NG as `-v`; otherwise eSpeak NG chooses its
default voice. The parent parses RIFF PCM chunks, accepts mono or
stereo 16-bit PCM at 8,000 to 192,000 Hz, downmixes stereo to mono, and
linearly resamples speech to the encoder's 44,100 Hz rate. eSpeak NG's WAV
stream uses placeholder RIFF/data sizes, so a data chunk declaring more bytes
than were captured is treated as extending to end of stream. It appends this
speech before the PX synchronization header. A configurable silence follows
the speech; `-g` or `--gap` selects its length in seconds, with a default of 3
seconds and an accepted range of 0 to 600 seconds. Only trailing terminator
carriage returns are removed from the spoken text; internal line breaks and
tabs become spaces. Without `--speak`, encoding does not invoke a TTS program.

The encoder defaults to 300 baud and `encoded.wav`; `-b` selects 300, 600, or
1200 baud, while `-o` selects another output path. `-m` selects the margin
byte. `-s` enables speech and `-g` sets the silence after it. Invalid command
lines print the usage syntax and a summary of `-o`, `-m`, `-b`, `-s`, `-l`,
`-g`, `--`, and text/stdin input. It does not depend on FFmpeg because it
writes PCM WAV directly; eSpeak NG is required only for the optional speech
path.

### 7. Memory and process cleanup

Each stage reports failure to its caller. `main` frees PCM samples, demodulated
symbols, and candidate bytes as the pipeline completes. The FFmpeg child is
waited for, and a non-zero child status is treated as an input error. The
encoder also waits for the optional eSpeak NG child and rejects failed speech
synthesis or unsupported speech WAV data.

### 8. SIP call responder

`sip.c` is a UDP SIP user agent built on PJSIP's PJSUA C API. It registers one
account, accepts one incoming call at a time, and rejects a concurrent call as
busy. A POSIX worker thread runs the configured `/bin/sh -c` command, captures
up to 7,400 bytes from stdout, and sends that text to the separate `encode`
executable through stdin. The encoder writes a temporary mono PCM WAV file.

After the call's audio media becomes active, the worker creates a PJSUA WAV
player, registers a `pjmedia_wav_player_set_eof_cb2` completion callback, and
connects the player's conference port to the call's audio transmit slot. When
the WAV finishes playing (after a short tail delay for RTP flushing), the
worker disconnects and destroys the player and sends BYE. The temporary WAV is
removed on both success and failure. A failed command, encoder, media setup, or
playback also ends the accepted call. PJSUA callbacks only update synchronized
call state; command execution, encoding, waiting for playback completion, and
cleanup run in the worker. The sound device is replaced by PJSUA's null device
so no local microphone or speaker is needed. VAD and echo cancellation are
disabled, and waveform codecs (`PCMA`, `PCMU`, `G722`) are prioritized over
low-bitrate speech vocoders so FSK tones are preserved.

The SIP program requires a registrar URI, account identity URI, authentication
username, and command. A password may be supplied through `SIP_PASSWORD` or
`--password`. The encoder path defaults to `./encode`, the baud rate to 300,
and the local UDP SIP port to 0 (ephemeral port). Other supported baud rates are 600 and
1200. Encoder options `--margin`, `--speak`, `--lang`, and `--gap` map to
`encode`'s `-m`, `-s`, `-l`, and `-g` flags; `--encoder-arg` (repeatable) and
trailing arguments after `--` forward additional arguments to the encoder.
Build using PJSIP's `libpjproject` pkg-config metadata.

### 9. Nix development shell

`shell.nix` defines a `pkgs.mkShell` environment containing the Nixpkgs C
compiler, FFmpeg, eSpeak NG, PJSIP, pkg-config, OpenSSL, and ALSA link
libraries.
FFmpeg is needed by the decoder at runtime; eSpeak NG is needed only when the
encoder's speech option is used; PJSIP is needed by `sip`. Enter the shell from
the project directory with `nix-shell`. Keep this dependency list consistent
with the runtime requirements in this file and `README.md`.

## Build and run

Build with a C11 compiler, POSIX process APIs, and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra decode.c -lm -o decode
```

Run with an audio path supported by FFmpeg:

```sh
./decode recording.mp3
./decode -v recording.mp3
```

For the encoder, build with a C11 compiler and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
```

Pass text directly, or omit it to read from standard input:

```sh
./encode -o message.wav "test abc123"
./encode -b 1200 -o message.wav "test abc123"
./encode --speak --gap 3 -o message.wav "test abc123"
./encode --speak --lang cs -o message.wav "test abc123"
printf '%s' "test abc123" | ./encode -o message.wav
```

Build the SIP responder in the Nix shell with PJSIP's supplied link flags:

```sh
cc -std=c11 -O2 -Wall -Wextra sip.c -o sip $(pkg-config --static --cflags --libs libpjproject) -pthread
```

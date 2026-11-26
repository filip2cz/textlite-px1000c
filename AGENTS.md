# Agent Instructions

## Project conventions

- Keep the program and all user-facing output in English.
- Write source-code comments in English.
- For every code, configuration, or documentation change, update both
  `AGENTS.md` and `README.md` in the same change. Keep this architecture
  description and the README's user instructions in sync with the code.
- Do not describe support for a modem profile unless the implementation
  actually supports it.

## Project architecture

The project contains two small POSIX C command-line tools. `decode.c` and
`encode.c` implement the receive and send paths; there is no separate
application framework or audio library.

### 1. Command-line entry point

`main` accepts one audio-file path. It coordinates input conversion, signal
demodulation, frame search, and text output. Errors and usage information go
to standard error; decoded text goes to standard output.

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
margin byte is 40; `-m` can choose another value from 0 to 127.

The encoder writes a RIFF WAV file containing mono, signed 16-bit PCM at
44,100 Hz. Its `AudioWriter` emits continuous-phase FSK samples at 300, 600,
or 1200 baud. A sample-remainder accumulator distributes fractional samples
across bits, preserving the selected average baud rate (600 and 1200 do not
have an integer number of samples per bit). It writes the PX synchronization
header, then 7E2-framed bytes for the margin and text, followed by 32 ASCII
0x04 end-of-text bytes and a one-second MARK tone. This is the normal,
unencrypted text path.

The encoder defaults to 300 baud and `encoded.wav`; `-b` selects 300, 600, or
1200 baud, while `-o` selects another output path. It does not depend on
FFmpeg because it writes PCM WAV directly.

### 7. Memory and process cleanup

Each stage reports failure to its caller. `main` frees PCM samples, demodulated
symbols, and candidate bytes as the pipeline completes. The FFmpeg child is
waited for, and a non-zero child status is treated as an input error.

## Build and run

Build with a C11 compiler, POSIX process APIs, and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra decode.c -lm -o decode
```

Run with an audio path supported by FFmpeg:

```sh
./decode recording.mp3
```

For the encoder, build with a C11 compiler and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
```

Pass text directly, or omit it to read from standard input:

```sh
./encode -o message.wav "test abc123"
./encode -b 1200 -o message.wav "test abc123"
printf '%s' "test abc123" | ./encode -o message.wav
```

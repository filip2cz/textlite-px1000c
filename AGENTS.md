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

The project is a small POSIX C command-line decoder. `decode.c` contains the
complete processing pipeline; it does not use a separate application
framework or audio library.

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

`demodulate` examines overlapping 60-sample windows. It measures signal energy
at the PX modem's two frequencies, 1300 Hz and 1700 Hz, and selects the
stronger tone as the bit value. Each window also receives a tone-confidence
value and an RMS level. These values let the frame search reject weak or
ambiguous audio.

### 4. Bit timing and serial-frame search

`find_message` searches for the recorded profile at 300 baud. At the chosen
22,050 Hz sample rate, one bit is about 73.5 samples. The search allows a
small clock-error range and checks half-sample phase steps to accommodate
recording and MP3 timing offsets.

The parser treats each serial frame as 11 bits: one zero start bit, seven
data bits sent least-significant bit first, one even-parity bit, and two one
stop bits (7E2). It also requires sufficient signal level and tone
confidence. Consecutive valid frames form a candidate run; the longest
readable run is retained, with a minimum length to avoid accepting isolated
noise as a message.

The current implementation is specific to this 300-baud, 1300/1700 Hz
profile. Supporting other baud rates or serial formats requires changing the
timing search and frame parser, then updating this file and `README.md`.

### 5. Text output

`print_message` removes leading NUL synchronization bytes, converts carriage
returns and line feeds to output newlines, and omits trailing control-byte
padding. Printable ASCII text is written to standard output.

### 6. Memory and process cleanup

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

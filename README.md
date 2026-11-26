# textlite-px1000c
Tools for decoding audio data transfers from a TEXT LITE PX1000 C.

## What is there?

`README.md` - this document
`LICENSE` - MIT License
`AGENTS.md` - project conventions and detailed encoder/decoder architecture
`decode.c` - C command-line decoder for PX1000 C audio transmissions
`encode.c` - C command-line encoder from text to PX1000 C audio
`test_abc123-xBd.mp3` - test recordings; its transmitted text includes `test abc123`, in speed x Bd

## Architecture and maintenance

The encoder/decoder pipelines and coding conventions are documented in
[AGENTS.md](AGENTS.md). Keep `AGENTS.md` and this README updated together with
every code, configuration, or documentation change.

## Decode a recording

The decoder accepts audio formats supported by FFmpeg. It converts the input
to mono, 16-bit PCM at 22,050 Hz before demodulating the 1300/1700 Hz FSK
signal recorded at 300, 600, or 1200 baud. The decoder checks all three
speeds automatically using rate-specific detection windows and reports the
detected rate to standard error. For a
new recording, lossless WAV with mono,
16-bit PCM at 44.1 kHz is a good interchange format; the decoder will resample
it internally.

Build it with a C compiler and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra decode.c -lm -o decode
```

FFmpeg must be installed and available as `ffmpeg` on `PATH`. Then run:

```sh
./decode test_abc123.mp3
```

## Encode text

`encode.c` writes a mono, 16-bit PCM WAV file at 44.1 kHz. It accepts printable
7-bit ASCII text and generates a 300-baud PX audio transmission. The default
margin byte is 40; use `-m` to select a value from 0 to 127. The default output
is `encoded.wav`, or use `-o` to choose another path.

Build and encode a message:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
./encode -o message.wav "test abc123"
```

If the text argument is omitted, the encoder reads text from standard input.

# textlite-px1000c
Tools for decoding audio data transfers from a TEXT LITE PX1000 C.

## What is there?

`README.md` - this document
`LICENSE` - MIT License
`decode.c` - C command-line decoder for PX1000 C audio transmissions
`test_abc123.mp3` - test recording; its transmitted text includes `test abc123`

## Decode a recording

The decoder accepts audio formats supported by FFmpeg. It converts the input
to mono, 16-bit PCM at 22,050 Hz before demodulating the 1300/1700 Hz FSK
signal recorded at 300 baud. For a new recording, lossless WAV with mono,
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

# textlite-px1000c
Tools for decoding audio data transfers from a TEXT LITE PX1000 C.

## What is here?

`README.md` - this document

`LICENSE` - MIT License

`decode.c` - C command-line decoder for PX1000 C audio transmissions

`encode.c` - C command-line encoder from text to PX1000 C audio

`shell.nix` - Nix development shell with the compiler and audio tools

`test_abc123-xBd.mp3` - test recordings; its transmitted text includes `test abc123`, in speed x Bd

`AGENTS.md` - project conventions and detailed encoder/decoder architecture, designed for AI

## Architecture and maintenance

The encoder/decoder pipelines and coding conventions are documented in
[AGENTS.md](AGENTS.md). Keep `AGENTS.md` and this README updated together with
every code, configuration, or documentation change.

## Nix development shell

Enter the project shell with:

```sh
nix-shell
```

It provides a C compiler, FFmpeg for decoding, and eSpeak NG for the optional
`encode --speak` feature. Build either program with the commands below while
inside the shell.

## Decode a recording

The decoder accepts audio formats supported by FFmpeg. It converts the input
to mono, 16-bit PCM at 22,050 Hz before demodulating the 1300/1700 Hz FSK
signal recorded at 300, 600, or 1200 baud. The decoder checks all three
speeds automatically using rate-specific detection windows. By default it
prints only the decoded text; add `-v` to report the detected rate to standard
error. For a new recording, lossless WAV with mono, 16-bit PCM at 44.1 kHz is
a good interchange format; the decoder will resample it internally.

Build it with a C compiler and the math library:

```sh
cc -std=c11 -O2 -Wall -Wextra decode.c -lm -o decode
```

FFmpeg must be installed and available as `ffmpeg` on `PATH`. Then run:

```sh
./decode test_abc123-300Bd.mp3
./decode -v test_abc123-1200Bd.mp3
```

An invalid invocation prints the decoder's usage and option summary to standard error.

## Encode text

`encode.c` writes a mono, 16-bit PCM WAV file at 44.1 kHz. It accepts printable
7-bit ASCII text and generates a 300, 600, or 1200-baud PX audio transmission.
Use `-b` to select the baud rate; the default is 300. The default margin byte
is 40; use `-m` to select a value from 0 to 127. The default output is
`encoded.wav`, or use `-o` to choose another path. Optional text-to-speech can
read the message before the modem signal with `-s` or `--speak`; this requires
eSpeak NG to be installed and available as `espeak-ng`. The default silence
between the spoken text and transmission is 3 seconds. Set it with `-g` or
`--gap`, using a value from 0 to 600 seconds. The encoder accepts eSpeak NG's
streamed WAV output and converts it to the output file's mono 44.1 kHz format.

Build and encode a message:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
./encode -o message.wav "test abc123"
./encode -b 1200 -o message.wav "test abc123"
./encode --speak --gap 3 -o message.wav "test abc123"
```

If the text argument is omitted, the encoder reads text from redirected standard
input. When run interactively without text, or with empty input, it prints the
encoder's usage, options, and defaults to standard error. `--speak` reads the
message text before the encoded transmission; the gap setting controls the
silence between them. The encoded text's terminating carriage returns are not
spoken.

## Development note

This project was vibecoded with OpenAI Codex using the GPT-6 Luna model.
Development tools included a C compiler (`cc`), FFmpeg for audio conversion and
inspection, and Python 3 for signal analysis. Protocol details were checked
against the [PX-1000 technical manual](https://www.cryptomuseum.com/crypto/philips/px1000/files/px1000_service_manual.pdf)
and the [PX 1000 C operating instructions](https://www.manualslib.com/manual/3453877/Text-Lite-Px-1000-C.html).

At runtime, `decode` requires FFmpeg. `encode` writes WAV directly and does not
require FFmpeg or Python. eSpeak NG is an optional runtime dependency when
`encode --speak` is used.

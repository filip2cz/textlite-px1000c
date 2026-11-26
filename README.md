# textlite-px1000c
Tools for decoding audio data transfers from a TEXT LITE PX1000 C.

## What is here?

`README.md` - this document

`LICENSE` - MIT License

`decode.c` - C command-line decoder for PX1000 C audio transmissions

`encode.c` - C command-line encoder from text to PX1000 C audio

`sip.c` - SIP client that answers calls with command-generated PX audio

`Makefile` - builds `decode`, `encode`, and conditionally `sip` (when PJSIP is available)

`tests/` - test suite for `decode`, `encode`, and `sip`

`shell.nix` - Nix development shell with the compiler and audio tools

`.github/workflows/main.yml` - GitHub Actions CI workflow for testing, multi-architecture cross-compilation (`decode`, `encode`, and static `sip`), QEMU smoke tests, and draft releases

`.github/scripts/build_pjsip.sh` - CI helper script that builds minimal static PJSIP libraries for each target architecture

`egg-textlite-px1000c.json` - Pterodactyl Panel (`PTDL_v2`) egg for hosting the SIP responder

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

It provides a C compiler, FFmpeg for decoding, eSpeak NG for the optional
`encode --speak` feature, and PJSIP plus its build tools for `sip`. Build the
programs with the commands below while inside the shell.

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
Select the speech language with `-l` or `--lang`, using an eSpeak NG voice code
such as `cs` or `en-us`; without it, eSpeak NG's default voice is used. List
available codes with `espeak-ng --voices`.

Build and encode a message:

```sh
cc -std=c11 -O2 -Wall -Wextra encode.c -lm -o encode
./encode -o message.wav "test abc123"
./encode -b 1200 -o message.wav "test abc123"
./encode --speak --gap 3 -o message.wav "test abc123"
./encode --speak --lang cs -o message.wav "test abc123"
```

If the text argument is omitted, the encoder reads text from redirected standard
input. When run interactively without text, or with empty input, it prints the
encoder's usage, options, and defaults to standard error. `--speak` reads the
message text before the encoded transmission; the gap setting controls the
silence between them. The encoded text's terminating carriage returns are not
spoken.

## Answer SIP calls with an encoded message

`sip.c` registers a SIP account over UDP and automatically answers one incoming
call at a time. It runs the selected shell command, passes its standard output
to `encode`, plays the resulting WAV into the call, and hangs up when playback
finishes. Command output must be valid encoder input and is limited to 7,400
bytes. If another call arrives while one is being handled, it is rejected as
busy. The command is executed by `/bin/sh -c`.

Build it inside `nix-shell`:

```sh
cc -std=c11 -O2 -Wall -Wextra sip.c -o sip $(pkg-config --static --cflags --libs libpjproject) -pthread
```

Set the SIP password in the environment, then start the client:

```sh
export SIP_PASSWORD='your-sip-password'
./sip --registrar sip:pbx.example.net \
  --identity sip:1001@pbx.example.net --username 1001 \
  --command 'printf "%s\\n" "test abc123"' --encoder ./encode
```

`--registrar`, `--identity`, `--username`, and `--command` are required. The
password may instead be supplied with `--password`. The local SIP UDP port
defaults to 0 (automatically picking an available ephemeral port); use
`--port N` to bind a specific port. By default PJSIP binds on all local
interfaces; use `--bind eth0` to select an interface or `--bind 192.0.2.10`
to select one of its IPv4 addresses. For an interface name, `sip` uses its
first listed IPv4 address. The selected address is used for both SIP signaling
and RTP audio. The baud rate defaults to 300 and accepts
300, 600, or 1200. Additional encoder parameters can be configured with
`--margin` (`-m`), `--speak` (`-s`), `--lang` (`-l`), `--gap` (`-g`),
repeatable `--encoder-arg`, or trailing arguments after `--`. To preserve FSK
modem tones, `sip` disables VAD and echo cancellation and prefers waveform
codecs (`PCMA`, `PCMU`, `G.722`) over speech vocoders. Stop the client with
Ctrl+C.

## Pterodactyl Panel egg

Import [`egg-textlite-px1000c.json`](egg-textlite-px1000c.json) in the
Pterodactyl Admin Panel under **Nests → Import Egg**.

- Uses `ghcr.io/pterodactyl/yolks:debian` at runtime and binds `sip` to the
  server's allocated UDP port (`{{SERVER_PORT}}`).
- Downloads matching prebuilt `sip`, `encode`, and `decode` binaries from the
  selected GitHub release (`VERSION=latest` by default), or automatically builds
  them from source if `VERSION=main` or no published release exists yet.
- Configure `SIP_REGISTRAR`, `SIP_IDENTITY`, `SIP_USERNAME`, `SIP_PASSWORD`,
  `SIP_COMMAND`, `BAUD_RATE`, and `MARGIN` in the server's **Startup** tab.
  Enabling `SPEAK` requires a custom container image with `espeak-ng` installed.

## Development note

This project was vibecoded with OpenAI Codex using the GPT-6 Luna model.
Development tools included a C compiler (`cc`), FFmpeg for audio conversion and
inspection, and Python 3 for signal analysis. Protocol details were checked
against the [PX-1000 technical manual](https://www.cryptomuseum.com/crypto/philips/px1000/files/px1000_service_manual.pdf)
and the [PX 1000 C operating instructions](https://www.manualslib.com/manual/3453877/Text-Lite-Px-1000-C.html).

At runtime, `decode` requires FFmpeg. `encode` writes WAV directly and does not
require FFmpeg or Python. eSpeak NG is an optional runtime dependency when
`encode --speak` is used. `sip` requires PJSIP and the `encode` executable.

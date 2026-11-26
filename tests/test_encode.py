import pathlib
import tempfile
import unittest

from helpers import DECODE_BIN, ENCODE_BIN, read_wav_info, run_cmd


class EncodeTests(unittest.TestCase):
    def test_cli_validation_errors(self):
        cases = [
            (["--unknown"], b"Usage:"),
            (["-o"], b"Usage:"),
            (["-m", "-1", "hi"], b"Margin must be an integer from 0 to 127."),
            (["-m", "128", "hi"], b"Margin must be an integer from 0 to 127."),
            (["-m", "abc", "hi"], b"Margin must be an integer from 0 to 127."),
            (["-b", "2400", "hi"], b"Baud rate must be 300, 600, or 1200."),
            (["-g", "-0.1", "hi"], b"Gap must be a number from 0 to 600 seconds."),
            (["-g", "600.1", "hi"], b"Gap must be a number from 0 to 600 seconds."),
            (["-l", "-bad", "hi"], b"Language must be an eSpeak NG voice code"),
            (["-l", "", "hi"], b"Language must be an eSpeak NG voice code"),
        ]
        for args, expected_err in cases:
            with self.subTest(args=args):
                res = run_cmd([ENCODE_BIN, *args], input_bytes=b"")
                self.assertEqual(res.returncode, 2)
                self.assertIn(expected_err, res.stderr)

    def test_empty_and_whitespace_stdin_rejected(self):
        empty = run_cmd([ENCODE_BIN], input_bytes=b"")
        self.assertEqual(empty.returncode, 2)
        self.assertIn(b"Usage:", empty.stderr)

        only_newlines = run_cmd([ENCODE_BIN], input_bytes=b"\r\n\n\r")
        self.assertEqual(only_newlines.returncode, 2)
        self.assertIn(b"Usage:", only_newlines.stderr)

    def test_rejects_non_ascii_and_control_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_wav = pathlib.Path(tmp) / "bad.wav"
            non_ascii = run_cmd([ENCODE_BIN, "-o", str(out_wav), "Šestajovice"])
            self.assertEqual(non_ascii.returncode, 1)
            self.assertIn(b"Input must contain printable 7-bit ASCII text.", non_ascii.stderr)
            self.assertFalse(out_wav.exists())

            ctrl = run_cmd([ENCODE_BIN, "-o", str(out_wav)], input_bytes=b"abc\x07def")
            self.assertEqual(ctrl.returncode, 1)
            self.assertIn(b"Input must contain printable 7-bit ASCII text.", ctrl.stderr)

    def test_rejects_input_exceeding_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_wav = pathlib.Path(tmp) / "toolong.wav"
            res = run_cmd([ENCODE_BIN, "-o", str(out_wav)], input_bytes=b"A" * 7401)
            self.assertEqual(res.returncode, 1)
            self.assertIn(b"exceeds the PX1000 C memory limit", res.stderr)

    def test_wav_header_and_all_baud_rates_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            for baud in (300, 600, 1200):
                wav_path = pathlib.Path(tmp) / f"msg_{baud}.wav"
                with self.subTest(baud=baud):
                    enc = run_cmd([
                        ENCODE_BIN, "-b", str(baud), "-m", "50",
                        "-o", str(wav_path), "hello", "world",
                    ])
                    self.assertEqual(enc.returncode, 0, enc.stderr.decode())
                    info = read_wav_info(wav_path)
                    self.assertEqual(info["audio_format"], 1)
                    self.assertEqual(info["channels"], 1)
                    self.assertEqual(info["sample_rate"], 44100)
                    self.assertEqual(info["bits_per_sample"], 16)
                    self.assertEqual(info["data_tag"], b"data")
                    self.assertEqual(info["riff_size"] + 8, info["file_size"])
                    self.assertEqual(info["data_size"] + 44, info["file_size"])

                    dec = run_cmd([DECODE_BIN, "-v", str(wav_path)])
                    self.assertEqual(dec.returncode, 0, dec.stderr.decode())
                    self.assertEqual(dec.stdout, b"2hello world\n")
                    self.assertIn(
                        f"Detected transmission at {baud} baud.".encode(),
                        dec.stderr,
                    )

    def test_stdin_and_double_dash_leading_hyphen(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav1 = pathlib.Path(tmp) / "stdin.wav"
            enc1 = run_cmd(
                [ENCODE_BIN, "-b", "1200", "-o", str(wav1)],
                input_bytes=b"row1\r\nrow2\nrow3\r\n",
            )
            self.assertEqual(enc1.returncode, 0, enc1.stderr.decode())
            dec1 = run_cmd([DECODE_BIN, str(wav1)])
            self.assertEqual(dec1.returncode, 0, dec1.stderr.decode())
            self.assertEqual(dec1.stdout, b"(row1\nrow2\nrow3\n")

            wav2 = pathlib.Path(tmp) / "dash.wav"
            enc2 = run_cmd([ENCODE_BIN, "-b", "1200", "-o", str(wav2), "--", "-flag-like"])
            self.assertEqual(enc2.returncode, 0, enc2.stderr.decode())
            dec2 = run_cmd([DECODE_BIN, str(wav2)])
            self.assertEqual(dec2.returncode, 0, dec2.stderr.decode())
            self.assertEqual(dec2.stdout, b"(-flag-like\n")

    def test_speak_language_and_gap_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_gap0 = pathlib.Path(tmp) / "tts_gap0.wav"
            wav_gap1 = pathlib.Path(tmp) / "tts_gap1.wav"

            enc0 = run_cmd([
                ENCODE_BIN, "-b", "1200", "--speak", "--lang", "cs", "--gap", "0",
                "-o", str(wav_gap0), "test abc123",
            ])
            self.assertEqual(enc0.returncode, 0, enc0.stderr.decode())

            enc1 = run_cmd([
                ENCODE_BIN, "-b", "1200", "-s", "-l", "cs", "-g", "1",
                "-o", str(wav_gap1), "test abc123",
            ])
            self.assertEqual(enc1.returncode, 0, enc1.stderr.decode())

            info0 = read_wav_info(wav_gap0)
            info1 = read_wav_info(wav_gap1)
            # 1 second of 44.1 kHz 16-bit mono PCM is 88200 bytes.
            self.assertEqual(info1["data_size"] - info0["data_size"], 44100 * 2)

            dec1 = run_cmd([DECODE_BIN, "-v", str(wav_gap1)])
            self.assertEqual(dec1.returncode, 0, dec1.stderr.decode())
            self.assertEqual(dec1.stdout, b"(test abc123\n")
            self.assertIn(b"Detected transmission at 1200 baud.", dec1.stderr)


if __name__ == "__main__":
    unittest.main()

import pathlib
import tempfile
import unittest

from helpers import DECODE_BIN, ENCODE_BIN, ROOT_DIR, run_cmd, write_silence_wav


class DecodeTests(unittest.TestCase):
    def test_cli_validation_errors(self):
        cases = [
            [],
            ["-x", "file.wav"],
            ["a.wav", "b.wav"],
            ["-v"],
            ["--"],
        ]
        for args in cases:
            with self.subTest(args=args):
                res = run_cmd([DECODE_BIN, *args])
                self.assertEqual(res.returncode, 2)
                self.assertEqual(res.stdout, b"")
                self.assertIn(b"Usage:", res.stderr)

    def test_missing_file_returns_error(self):
        res = run_cmd([DECODE_BIN, "/nonexistent/path/missing.wav"])
        self.assertEqual(res.returncode, 1)
        self.assertEqual(res.stdout, b"")
        self.assertIn(b"FFmpeg could not read audio file", res.stderr)

    def test_silence_returns_no_transmission(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = pathlib.Path(tmp) / "silence.wav"
            write_silence_wav(wav_path, duration_seconds=0.8)
            res = run_cmd([DECODE_BIN, str(wav_path)])
            self.assertEqual(res.returncode, 1)
            self.assertEqual(res.stdout, b"")
            self.assertIn(b"No valid TEXT LITE PX1000 C transmission was found.", res.stderr)

    def test_sample_mp3_recordings_all_baud_rates(self):
        samples = [
            ("test_abc123-300Bd.mp3", 300),
            ("test_abc123-600Bd.mp3", 600),
            ("test_abc123-1200Bd.mp3", 1200),
        ]
        for filename, baud in samples:
            path = ROOT_DIR / filename
            with self.subTest(filename=filename, baud=baud):
                quiet = run_cmd([DECODE_BIN, str(path)])
                self.assertEqual(quiet.returncode, 0, quiet.stderr.decode())
                self.assertEqual(quiet.stdout, b"(test abc123\n")
                self.assertEqual(quiet.stderr, b"")

                verbose = run_cmd([DECODE_BIN, "-v", str(path)])
                self.assertEqual(verbose.returncode, 0, verbose.stderr.decode())
                self.assertEqual(verbose.stdout, b"(test abc123\n")
                self.assertIn(
                    f"Detected transmission at {baud} baud.".encode(),
                    verbose.stderr,
                )

    def test_double_dash_and_trailing_verbose_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = pathlib.Path(tmp) / "-dash-name.wav"
            enc = run_cmd([ENCODE_BIN, "-b", "600", "-o", str(wav_path), "hello dash"])
            self.assertEqual(enc.returncode, 0, enc.stderr.decode())

            res = run_cmd([DECODE_BIN, "--", str(wav_path)])
            self.assertEqual(res.returncode, 0, res.stderr.decode())
            self.assertEqual(res.stdout, b"(hello dash\n")

            sample = ROOT_DIR / "test_abc123-300Bd.mp3"
            res_after = run_cmd([DECODE_BIN, str(sample), "-v"])
            self.assertEqual(res_after.returncode, 0, res_after.stderr.decode())
            self.assertIn(b"Detected transmission at 300 baud.", res_after.stderr)

    def test_ffmpeg_resampled_stereo_mp3_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = pathlib.Path(tmp) / "src.wav"
            mp3_path = pathlib.Path(tmp) / "stereo16k.mp3"
            enc = run_cmd([ENCODE_BIN, "-b", "1200", "-o", str(wav_path), "Line 1\nLine 2\tOK"])
            self.assertEqual(enc.returncode, 0, enc.stderr.decode())

            ff = run_cmd([
                "ffmpeg", "-y", "-v", "error", "-i", str(wav_path),
                "-ac", "2", "-ar", "16000", "-b:a", "64k", str(mp3_path),
            ])
            self.assertEqual(ff.returncode, 0, ff.stderr.decode())

            dec = run_cmd([DECODE_BIN, "-v", str(mp3_path)])
            self.assertEqual(dec.returncode, 0, dec.stderr.decode())
            self.assertEqual(dec.stdout, b"(Line 1\nLine 2\tOK\n")
            self.assertIn(b"Detected transmission at 1200 baud.", dec.stderr)


if __name__ == "__main__":
    unittest.main()

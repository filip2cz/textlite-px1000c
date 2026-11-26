import glob
import os
import pathlib
import selectors
import shlex
import signal
import subprocess
import tempfile
import time
import unittest

from helpers import (
    DECODE_BIN,
    ENCODE_BIN,
    SIP_BIN,
    find_free_rtp_port_pair,
    find_free_udp_port,
    run_cmd,
)


def wait_for_file_text(path, needle, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            text = path.read_text(errors="replace")
            if needle in text:
                return text
        time.sleep(0.05)
    content = path.read_text(errors="replace") if path.exists() else "<missing>"
    raise TimeoutError(f"Timed out waiting for {needle!r} in {path}:\n{content}")


def run_pjsua_call(target_uri, rec_wav, *, wait_seconds=15.0, dis_vocoders=True):
    caller_sip_port = find_free_udp_port()
    caller_rtp_port = find_free_rtp_port_pair()
    cmd = [
        "pjsua",
        "--null-audio",
        "--no-tcp",
        "--no-vad",
        f"--local-port={caller_sip_port}",
        f"--rtp-port={caller_rtp_port}",
        "--id=sip:caller@127.0.0.1",
        f"--rec-file={rec_wav}",
        "--auto-rec",
        "--app-log-level=3",
    ]
    if dis_vocoders:
        cmd.extend(["--dis-codec=speex", "--dis-codec=iLBC", "--dis-codec=GSM"])
    cmd.append(target_uri)

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    chunks = []
    disconnected = False
    sel = selectors.DefaultSelector()
    try:
        assert proc.stdout is not None
        fd = proc.stdout.fileno()
        os.set_blocking(fd, False)
        sel.register(fd, selectors.EVENT_READ)
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            remaining = max(0.05, deadline - time.monotonic())
            events = sel.select(timeout=min(0.2, remaining))
            if events:
                data = os.read(fd, 4096)
                if not data:
                    break
                chunks.append(data)
                text = b"".join(chunks).decode(errors="replace")
                if "is DISCONNECTED" in text:
                    disconnected = True
                    break
            elif proc.poll() is not None:
                break

        if proc.poll() is None:
            try:
                assert proc.stdin is not None
                proc.stdin.write(b"q\n")
                proc.stdin.flush()
            except BrokenPipeError:
                pass
            try:
                rest, _ = proc.communicate(timeout=5)
                if rest:
                    chunks.append(rest)
            except subprocess.TimeoutExpired:
                proc.kill()
                rest, _ = proc.communicate(timeout=5)
                if rest:
                    chunks.append(rest)
        else:
            rest = proc.stdout.read()
            if rest:
                chunks.append(rest)
    finally:
        sel.close()
        if proc.poll() is None:
            proc.kill()
            proc.communicate(timeout=5)
    output = b"".join(chunks).decode(errors="replace")
    if "is DISCONNECTED" in output:
        disconnected = True
    return output, disconnected


class SipTests(unittest.TestCase):
    def setUp(self):
        self._processes = []

    def tearDown(self):
        for proc, log_fh in self._processes:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            log_fh.close()

    def start_sip(self, extra_args, log_path, env=None):
        merged_env = os.environ.copy()
        if env:
            merged_env.update(env)
        log_fh = open(log_path, "w+", encoding="utf-8")
        proc = subprocess.Popen(
            [SIP_BIN, *extra_args],
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            env=merged_env,
        )
        self._processes.append((proc, log_fh))
        wait_for_file_text(log_path, "SIP client started; waiting for incoming calls.")
        return proc

    def test_help_and_cli_validation(self):
        help_res = run_cmd([SIP_BIN, "--help"])
        self.assertEqual(help_res.returncode, 0)
        self.assertIn(b"--encoder-arg", help_res.stderr)
        self.assertIn(b"--speak", help_res.stderr)

        base = [
            "--registrar", "sip:127.0.0.1",
            "--identity", "sip:u@127.0.0.1",
            "--username", "u",
            "--command", "echo hi",
        ]
        bad_cases = [
            ["--registrar", "sip:127.0.0.1"],
            ["--registrar", "http://127.0.0.1", "--identity", "sip:u@127.0.0.1", "--username", "u", "--command", "echo x"],
            ["--registrar", "sip:127.0.0.1", "--identity", "u@127.0.0.1", "--username", "u", "--command", "echo x"],
            [*base, "--baud", "9600"],
            [*base, "--port", "70000"],
            [*base, "--margin", "128"],
            [*base, "--gap", "601"],
            [*base, "--lang", "-bad"],
            [*base, "--unknown"],
            [*base, "--port"],
        ]
        for args in bad_cases:
            with self.subTest(args=args):
                res = run_cmd([SIP_BIN, *args])
                self.assertEqual(res.returncode, 2)

    def test_incoming_call_playback_and_hangup_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sip_port = find_free_udp_port()
            log_path = tmp_dir / "sip.log"
            rec_wav = tmp_dir / "rec.wav"

            self.start_sip([
                "--registrar", "sip:127.0.0.1:59999",
                "--identity", "sip:service@127.0.0.1",
                "--username", "service",
                "--command", "echo test abc123",
                "--encoder", ENCODE_BIN,
                "--baud", "1200",
                "--port", str(sip_port),
            ], log_path, env={"SIP_PASSWORD": "secret"})

            pjsua_out, disconnected = run_pjsua_call(
                f"sip:service@127.0.0.1:{sip_port}", rec_wav
            )
            self.assertTrue(disconnected, pjsua_out)
            log_text = wait_for_file_text(log_path, "Call 0 disconnected.")
            self.assertIn("Accepting incoming call 0.", log_text)
            self.assertIn("Playing encoded message to call 0.", log_text)
            self.assertIn("Message playback finished.", log_text)

            dec = run_cmd([DECODE_BIN, "-v", str(rec_wav)])
            self.assertEqual(dec.returncode, 0, dec.stderr.decode())
            self.assertEqual(dec.stdout, b"(test abc123\n")
            self.assertIn(b"Detected transmission at 1200 baud.", dec.stderr)

    def test_encoder_parameters_and_passthrough_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sip_port = find_free_udp_port()
            log_path = tmp_dir / "sip.log"
            rec_wav = tmp_dir / "rec.wav"
            args_log = tmp_dir / "encoder_args.txt"
            wrapper = tmp_dir / "wrap_encode.sh"
            wrapper.write_text(
                f"#!/bin/sh\n"
                f"printf '%s\\n' \"$@\" > {shlex.quote(str(args_log))}\n"
                f"exec {shlex.quote(ENCODE_BIN)} \"$@\"\n",
                encoding="utf-8",
            )
            wrapper.chmod(0o755)

            before_tmp = set(glob.glob("/tmp/textlite-sip-*.wav"))
            self.start_sip([
                "--registrar", "sip:127.0.0.1:59999",
                "--identity", "sip:service@127.0.0.1",
                "--username", "service",
                "--command", "printf 'hello ' && echo world",
                "--encoder", str(wrapper),
                "--baud", "1200",
                "--margin", "50",
                "--speak",
                "--lang", "cs",
                "--gap", "0.5",
                "--port", str(sip_port),
                "--", "-m", "51",
            ], log_path)

            pjsua_out, disconnected = run_pjsua_call(
                f"sip:service@127.0.0.1:{sip_port}", rec_wav, wait_seconds=15.0
            )
            self.assertTrue(disconnected, pjsua_out)
            wait_for_file_text(log_path, "Call 0 disconnected.")

            recorded_args = args_log.read_text(encoding="utf-8").splitlines()
            self.assertIn("-b", recorded_args)
            self.assertIn("1200", recorded_args)
            self.assertIn("-s", recorded_args)
            self.assertIn("-l", recorded_args)
            self.assertIn("cs", recorded_args)
            self.assertIn("-g", recorded_args)
            self.assertIn("0.5", recorded_args)
            # Trailing -- -m 51 overrides earlier -m 50 in encode.c
            self.assertEqual(recorded_args[-2:], ["-m", "51"])

            dec = run_cmd([DECODE_BIN, "-v", str(rec_wav)])
            self.assertEqual(dec.returncode, 0, dec.stderr.decode())
            self.assertEqual(dec.stdout, b"3hello world\n")
            self.assertIn(b"Detected transmission at 1200 baud.", dec.stderr)

            after_tmp = set(glob.glob("/tmp/textlite-sip-*.wav"))
            self.assertEqual(after_tmp - before_tmp, set())

    def test_failed_command_hangs_up_and_recovers_for_next_call(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sip_port = find_free_udp_port()
            log_path = tmp_dir / "sip.log"
            flag_file = tmp_dir / "allow_success"
            rec_fail = tmp_dir / "rec_fail.wav"
            rec_ok = tmp_dir / "rec_ok.wav"

            cmd = f"test -f {str(flag_file)!r} && echo recovered ok"
            self.start_sip([
                "--registrar", "sip:127.0.0.1:59999",
                "--identity", "sip:service@127.0.0.1",
                "--username", "service",
                "--command", cmd,
                "--encoder", ENCODE_BIN,
                "--baud", "1200",
                "--port", str(sip_port),
            ], log_path)

            pjsua_out1, disconnected1 = run_pjsua_call(
                f"sip:service@127.0.0.1:{sip_port}", rec_fail, wait_seconds=8.0
            )
            self.assertTrue(disconnected1, pjsua_out1)
            log1 = wait_for_file_text(log_path, "Call 0 disconnected.")
            self.assertIn("Text command exited unsuccessfully.", log1)

            flag_file.write_text("ok", encoding="utf-8")
            pjsua_out2, disconnected2 = run_pjsua_call(
                f"sip:service@127.0.0.1:{sip_port}", rec_ok, wait_seconds=12.0
            )
            self.assertTrue(disconnected2, pjsua_out2)
            wait_for_file_text(log_path, "Call 1 disconnected.")

            dec = run_cmd([DECODE_BIN, "-v", str(rec_ok)])
            self.assertEqual(dec.returncode, 0, dec.stderr.decode())
            self.assertEqual(dec.stdout, b"(recovered ok\n")

    def test_rejects_second_concurrent_call_as_busy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sip_port = find_free_udp_port()
            log_path = tmp_dir / "sip.log"
            rec1 = tmp_dir / "rec1.wav"
            rec2 = tmp_dir / "rec2.wav"

            self.start_sip([
                "--registrar", "sip:127.0.0.1:59999",
                "--identity", "sip:service@127.0.0.1",
                "--username", "service",
                "--command", "sleep 1.5 && echo busy test",
                "--encoder", ENCODE_BIN,
                "--baud", "1200",
                "--port", str(sip_port),
            ], log_path)

            caller1_port = find_free_udp_port()
            caller1_rtp = find_free_rtp_port_pair()
            proc1 = subprocess.Popen(
                [
                    "pjsua",
                    "--null-audio", "--no-tcp", "--no-vad",
                    f"--local-port={caller1_port}",
                    f"--rtp-port={caller1_rtp}",
                    "--id=sip:caller1@127.0.0.1",
                    f"--rec-file={rec1}", "--auto-rec", "--app-log-level=3",
                    "--dis-codec=speex", "--dis-codec=iLBC", "--dis-codec=GSM",
                    f"sip:service@127.0.0.1:{sip_port}",
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                wait_for_file_text(log_path, "Accepting incoming call 0.")
                pjsua_out2, disconnected2 = run_pjsua_call(
                    f"sip:service@127.0.0.1:{sip_port}", rec2, wait_seconds=6.0
                )
                self.assertTrue(disconnected2, pjsua_out2)
                self.assertIn("Busy", pjsua_out2)

                wait_for_file_text(log_path, "Call 0 disconnected.", timeout=12.0)
            finally:
                if proc1.poll() is None:
                    try:
                        assert proc1.stdin is not None
                        proc1.stdin.write("q\n")
                        proc1.stdin.flush()
                    except BrokenPipeError:
                        pass
                    proc1.communicate(timeout=5)

            log_text = log_path.read_text(encoding="utf-8")
            self.assertIn("Rejecting incoming call: already processing a call.", log_text)
            dec = run_cmd([DECODE_BIN, str(rec1)])
            self.assertEqual(dec.returncode, 0, dec.stderr.decode())
            self.assertEqual(dec.stdout, b"(busy test\n")


if __name__ == "__main__":
    unittest.main()

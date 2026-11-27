import os
import pathlib
import socket
import struct
import subprocess
import wave

ROOT_DIR = pathlib.Path(os.environ.get("TEXTLITE_ROOT", pathlib.Path(__file__).resolve().parent.parent))
DECODE_BIN = os.environ.get("TEXTLITE_DECODE", str(ROOT_DIR / "decode"))
ENCODE_BIN = os.environ.get("TEXTLITE_ENCODE", str(ROOT_DIR / "encode"))
SIP_ONETIME_BIN = os.environ.get(
    "TEXTLITE_SIP_ONETIME",
    os.environ.get("TEXTLITE_SIP", str(ROOT_DIR / "sip-onetime")),
)
SIP_DIALOG_BIN = os.environ.get("TEXTLITE_SIP_DIALOG", str(ROOT_DIR / "sip-dialog"))
# Keep the old helper name available for callers that still target sip-onetime.
SIP_BIN = SIP_ONETIME_BIN


def run_cmd(cmd, *, input_bytes=None, env=None, timeout=30):
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        cmd,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged_env,
        timeout=timeout,
        check=False,
    )


def write_silence_wav(path, duration_seconds=0.5, sample_rate=22050):
    frames = int(duration_seconds * sample_rate)
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(b"\x00\x00" * frames)


def read_wav_info(path):
    raw = pathlib.Path(path).read_bytes()
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise AssertionError(f"Invalid WAV header in {path}")
    riff_size = struct.unpack_from("<I", raw, 4)[0]
    audio_format, channels, sample_rate, byte_rate, block_align, bits = struct.unpack_from(
        "<HHIIHH", raw, 20
    )
    data_tag = raw[36:40]
    data_size = struct.unpack_from("<I", raw, 40)[0]
    return {
        "file_size": len(raw),
        "riff_size": riff_size,
        "audio_format": audio_format,
        "channels": channels,
        "sample_rate": sample_rate,
        "byte_rate": byte_rate,
        "block_align": block_align,
        "bits_per_sample": bits,
        "data_tag": data_tag,
        "data_size": data_size,
    }


def find_free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def find_free_rtp_port_pair():
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rtp_sock:
            rtp_sock.bind(("127.0.0.1", 0))
            port = rtp_sock.getsockname()[1]
            if port % 2 != 0 or port >= 65534:
                continue
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rtcp_sock:
                    rtcp_sock.bind(("127.0.0.1", port + 1))
                    return port
            except OSError:
                continue
    raise RuntimeError("Could not allocate a free even/odd UDP port pair for RTP")

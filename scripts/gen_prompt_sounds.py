"""Regenerate the on-device prompt sounds with the server's TTS voice.

The device plays these clips when it cannot reach the server, so they should
sound like the voice the server uses for replies. This reuses kodo-server's
Volcengine Doubao provider and saved runtime config, then encodes each clip to
the Ogg Opus format the firmware embeds (16 kHz, mono).

Run with the kodo-server environment:

    D:/codespace/kodo-server/.venv/Scripts/python.exe scripts/gen_prompt_sounds.py \
        --server D:/codespace/kodo-server --ffmpeg <path-to-ffmpeg>

Rerun scripts/gen_lang.py afterwards only if a file name is added or removed.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import subprocess
import sys
from pathlib import Path

PROMPTS = {
    "wozaime": "我在呢",
    "shike": "时刻准备着",
    "laila": "来啦，你说",
    "nonet": "哎呀，我把网络弄丢啦，请帮我检查一下",
    "noserver": "网是通的呀，可是服务器没人应我",
}

SAMPLE_RATE = 16000


def load_server_tts(server_dir: Path):
    sys.path.insert(0, str(server_dir / "src"))
    # Settings reads .env and relative paths from the server directory.
    os.chdir(server_dir)
    from robot.config import get_settings
    from robot.providers.base import TTSOptions
    from robot.providers.tts.volc_doubao import VolcDoubaoTTS
    from robot.runtime_config import RuntimeStore

    settings = get_settings()
    store = RuntimeStore(settings.resolve_path(settings.runtime_config_path))
    if store.path.exists():
        store.load_or_seed(settings).overlay(settings)
    if settings.tts_provider != "volc_doubao":
        raise SystemExit(f"server TTS provider is {settings.tts_provider!r}, expected volc_doubao")
    voice = settings.tts_voice_for("chat")
    opts = TTSOptions(
        voice=voice, speed=settings.tts_speed, sample_rate=SAMPLE_RATE, language="zh-CN"
    )
    return VolcDoubaoTTS(settings), opts, voice


def encode_ogg(ffmpeg: str, pcm: bytes, out_path: Path) -> None:
    cmd = [
        ffmpeg, "-y", "-loglevel", "error",
        "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1", "-i", "pipe:0",
        "-c:a", "libopus", "-b:a", "24k", "-application", "voip",
        "-frame_duration", "60", "-ar", str(SAMPLE_RATE), "-ac", "1",
        str(out_path),
    ]
    subprocess.run(cmd, input=pcm, check=True)


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--server", required=True, type=Path, help="kodo-server directory")
    parser.add_argument("--ffmpeg", required=True, help="ffmpeg executable with libopus")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "main" / "assets" / "common",
    )
    args = parser.parse_args()
    out_dir = args.out.resolve()

    tts, opts, voice = load_server_tts(args.server.resolve())
    print(f"voice={voice} speed={opts.speed}")
    for name, text in PROMPTS.items():
        pcm = await tts.synthesize(text, opts)
        if not pcm:
            raise SystemExit(f"empty audio for {name}: {text}")
        out_path = out_dir / f"{name}.ogg"
        encode_ogg(args.ffmpeg, pcm, out_path)
        seconds = len(pcm) / 2 / SAMPLE_RATE
        print(f"{out_path.name}: {seconds:.2f}s, {out_path.stat().st_size} bytes  {text}")


if __name__ == "__main__":
    asyncio.run(main())

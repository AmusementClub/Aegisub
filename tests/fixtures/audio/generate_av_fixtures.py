"""Regenerate the AAC/MOV cache fixtures from a synthetic PCM source."""

import argparse
import pathlib
import struct
import subprocess
import tempfile
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("tests/fixtures/audio"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="aegisub-av-fixtures-") as temporary:
        source = pathlib.Path(temporary) / "source.wav"
        frames = 48000 * 90
        with wave.open(str(source), "wb") as stream:
            stream.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            chunk = struct.pack("<1024h", *[1000 + index for index in range(1024)])
            stream.writeframes((chunk * ((frames + 1023) // 1024))[:frames * 2])
        for name, video_delay, audio_delay in [("zero", 0, 0), ("positive", 0, 0.24), ("negative", 0.24, 0)]:
            subprocess.run([
                args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                "-itsoffset", str(video_delay), "-f", "lavfi", "-i", "color=s=16x16:r=25:d=2",
                "-itsoffset", str(audio_delay), "-i", str(source),
                "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-fps_mode", "passthrough",
                "-c:a", "aac", "-aac_pns", "0", "-t", "1.2",
                str(args.output_dir / f"aac-av-{name}.mp4"),
            ], check=True, timeout=10)


if __name__ == "__main__":
    main()

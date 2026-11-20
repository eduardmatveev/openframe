"""Create independent, bit-exact SDR/PQ/HLG 10/12-bit ramp fixtures.

Each movie contains 24 identical frames. In four 16-row bands, x is the
integer code value of gray, red, green and blue respectively. Metadata tags
define how those codes are interpreted; no reference-product pixels are used.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1] / "test-results" / "fixtures")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    results = []
    for bits in (10, 12):
        width, height = 1 << bits, 64
        ramp = struct.pack("<{}H".format(width), *range(width))
        zero = bytes(width * 2)
        # FFmpeg planar order is G, B, R; band order is gray, red, green, blue.
        planes = []
        for channel_band in (2, 3, 1):
            planes.append(b"".join((ramp if band in (0, channel_band) else zero) * 16 for band in range(4)))
        frame = b"".join(planes)
        pixel_format = "gbrp{}le".format(bits)
        for label, primaries, transfer in (("sdr", "bt709", "iec61966-2-1"),
                                            ("pq", "bt2020", "smpte2084"),
                                            ("hlg", "bt2020", "arib-std-b67")):
            path = args.directory / "{}-ramp-{}bit.mkv".format(label, bits)
            command = [args.ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "rawvideo", "-pixel_format", pixel_format,
                       "-video_size", "{}x{}".format(width, height), "-framerate", "24", "-i", "pipe:0",
                       "-frames:v", "24", "-an", "-vf",
                       "setparams=range=full:color_primaries={}:color_trc={}:colorspace=gbr".format(primaries, transfer),
                       "-c:v", "ffv1", "-level", "3", "-pix_fmt", pixel_format,
                       "-color_range", "pc", "-colorspace", "rgb", "-color_primaries", primaries,
                       "-color_trc", transfer, str(path)]
            subprocess.run(command, input=frame * 24, check=True, stdout=subprocess.DEVNULL)
            probe = json.loads(subprocess.check_output([args.ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(path)]))["streams"][0]
            restored = subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", str(path),
                                                "-frames:v", "1", "-pix_fmt", pixel_format, "-f", "rawvideo", "pipe:1"])
            exact = restored == frame
            metadata_ok = (probe["pix_fmt"] == pixel_format and probe["color_transfer"] == transfer
                           and probe["color_primaries"] == primaries and probe["color_range"] == "pc"
                           and int(probe["nb_read_frames"]) == 24)
            if not exact or not metadata_ok:
                raise AssertionError("Fixture verification failed: " + str(path))
            results.append({"path": path.name, "bits": bits, "width": width, "height": height,
                            "frames": 24, "fps": "24/1", "pixelFormat": pixel_format,
                            "colorPrimaries": primaries, "colorTransfer": transfer, "range": "full",
                            "rawFirstFrameSha256": hashlib.sha256(frame).hexdigest(),
                            "fileSha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                            "independentDecodeBitExact": exact})
    report = {"scope": "Synthetic coded ramps, not a reference tone-mapping algorithm",
              "formula": "x code value; y bands16 rows each: gray, red, green, blue; G/B/R planes; full range",
              "fixtures": results}
    (args.directory / "color-fixtures.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"generated": len(results), "allBitExact": all(item["independentDecodeBitExact"] for item in results)}))


if __name__ == "__main__":
    main()

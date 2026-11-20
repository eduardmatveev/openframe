"""Independently decode outputs of our synthetic Maya capture test.

Requires an owned test OpenFrame endpoint with the reported sequence sources
still loaded. Only newly generated fixtures and outputs are inspected.
"""
import argparse
import array
import hashlib
import json
import math
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maya-report", type=Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--maya-module-path", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--image-decoder", choices=("ffmpeg", "windows"), default="ffmpeg")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "checks": {}, "scope": "Own Maya fixtures; independent file decoding, not reference parity",
              "imageDecoder": args.image_decoder, "imageDecodes": [],
              "imageComparison": "RGB over opaque black: image export uses the default black output background"}

    def check(name, passed, detail=None):
        report["checks"][name] = {"passed": bool(passed), "detail": detail}
        if not passed:
            raise AssertionError("{}: {}".format(name, detail))

    def probe(path):
        return json.loads(subprocess.check_output([
            args.ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(path)
        ]))["streams"]

    def rgb(path):
        if args.image_decoder == "windows":
            output = args.directory.resolve() / (hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:16] + ".rgb")
            decoded = json.loads(subprocess.check_output([
                "pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("decode_test_image.ps1")),
                "-InputPath", str(path), "-OutputPath", str(output), "-CompositeOverBlack"
            ], text=True))
            pixels = output.read_bytes()
            report["imageDecodes"].append({"path": str(path), **decoded})
            if len(pixels) != decoded["width"] * decoded["height"] * 3:
                raise AssertionError("Incomplete Windows raster output")
            return pixels
        rgba = subprocess.check_output([
            args.ffmpeg, "-v", "error", "-nostdin", "-i", str(path), "-frames:v", "1",
            "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"
        ])
        pixels = bytearray()
        for index in range(0, len(rgba), 4):
            alpha = rgba[index + 3]
            pixels.extend((value * alpha + 127) // 255 for value in rgba[index:index + 3])
        return bytes(pixels)

    try:
        maya_report = json.loads(args.maya_report.read_text(encoding="utf-8-sig"))
        check("maya_test_completed", maya_report.get("passed") is True)
        streams = probe(maya_report["playblastPath"])
        video = next(stream for stream in streams if stream["codec_type"] == "video")
        check("base_playblast_video", (int(video["nb_read_frames"]), video["avg_frame_rate"], video["width"], video["height"]) == (24, "24/1", 320, 180))
        for capture in maya_report["soundCaptures"]:
            label = "sound_{}_".format(int(capture["includeSound"]))
            streams = probe(capture["path"])
            videos = [stream for stream in streams if stream["codec_type"] == "video"]
            audio = [stream for stream in streams if stream["codec_type"] == "audio"]
            check(label + "video", len(videos) == 1 and
                  (int(videos[0]["nb_read_frames"]), videos[0]["avg_frame_rate"], videos[0]["width"], videos[0]["height"]) == (24, "24/1", 128, 72))
            check(label + "stream_selection", len(audio) == int(capture["includeSound"]), len(audio))
            if capture["includeSound"]:
                raw = subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", capture["path"],
                    "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "pipe:1"])
                samples = array.array("f")
                samples.frombytes(raw)
                if sys.byteorder != "little":
                    samples.byteswap()
                check(label + "sample_duration", abs(len(samples) - 48000) <= 2000, len(samples))
                selected = samples[4800:-4800]
                rms = math.sqrt(sum(value * value for value in selected) / max(1, len(selected)))
                crossings, armed = [], False
                for index, value in enumerate(selected):
                    if value < -0.01:
                        armed = True
                    elif value > 0.01 and armed:
                        crossings.append(index)
                        armed = False
                frequency = ((len(crossings) - 1) * 48000 / (crossings[-1] - crossings[0])) if len(crossings) > 1 else 0
                check(label + "signal", 0.08 < rms < 0.25 and abs(frequency - 440) < 1,
                      {"rms": rms, "frequencyHz": frequency, "toleranceHz": 1})
        sys.path.insert(0, str(args.maya_module_path.resolve()))
        from openframe_maya.client import Client
        with Client(args.endpoint, timeout=5) as client:
            client.call("play", playing=False)
            for sequence in maya_report["sequences"]:
                count = sequence["frames"]
                client.call("viewer.assign", pane=0, id=sequence["sourceId"])
                for index, original in ((0, sequence["firstPath"]), (count - 1, sequence["lastPath"])):
                    output = args.directory / "sequence-{}-frame-{}".format(count, index)
                    client.call("export.start", path=str(output.resolve()), kind="images", first=index, last=index,
                                width=128, height=72, viewerComposition=False, includeAnnotations=False,
                                includeAudio=False, imageFormat="bmp")
                    deadline = time.monotonic() + 30
                    while True:
                        status = client.call("export.status")
                        if status.get("state") != "running":
                            break
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Sequence image export timed out")
                        time.sleep(0.025)
                    if status.get("success") is not True:
                        raise AssertionError("Sequence image export failed: " + str(status))
                    images = list(output.glob("*.bmp"))
                    if len(images) != 1:
                        raise AssertionError("Expected one exported frame")
                    expected, actual = rgb(original), rgb(images[0])
                    check("sequence_{}_frame_{}_rgb".format(count, index),
                          len(actual) == 128 * 72 * 3 and actual == expected,
                          {"expectedSha256": hashlib.sha256(expected).hexdigest(),
                           "actualSha256": hashlib.sha256(actual).hexdigest()})
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        (args.directory / "capture-file-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

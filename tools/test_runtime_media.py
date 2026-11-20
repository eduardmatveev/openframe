"""Exercise an actual OpenFrame process with synthetic media via its public API.

The output is decoded by a separate FFmpeg executable. Checks video frame
order, pulse alignment, non-silent audio, JSON persistence and CSV escaping.
Only the supplied synthetic fixture and newly generated outputs are read.
"""
import argparse
import array
import json
import math
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--media", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--maya-module-path", type=Path, help="Test the adapter copied into the portable package")
    args = parser.parse_args()
    module_path = (args.maya_module_path or Path(__file__).resolve().parents[1] / "maya").resolve()
    sys.path.insert(0, str(module_path))
    from openframe_maya.client import Client
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "checks": {}, "scope": "OpenFrame functional check, not reference-product parity",
              "adapterModulePath": str(module_path)}

    def check(name, condition, detail=None):
        report["checks"][name] = {"passed": bool(condition), "detail": detail}
        if not condition:
            raise AssertionError(name + ": " + str(detail))

    def wait(predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            time.sleep(0.03)
        raise TimeoutError("OpenFrame operation did not complete")

    try:
        with Client(args.endpoint, timeout=5) as client:
            original_call = client.call
            def traced_call(method, **parameters):
                report["lastMethod"] = method
                return original_call(method, **parameters)
            client.call = traced_call
            client.call("project.new")
            client.call("media.import", paths=[str(args.media.resolve())])
            wait(lambda: not client.call("status")["busy"])
            status = client.call("status")
            check("frame_count", status["durationFrames"] == 120, status["durationFrames"])
            note = 'Review "quoted", line one\nСтрока два'
            client.call("notes.add", ownerId=status["activeTimelineId"], notes=[{"frame": 48, "text": note}], author="Synthetic QA")
            client.call("bookmarks.add", name="Pulse", frames=[96], endFrame=96)
            project_path = args.directory / "roundtrip.oframe"
            csv_path = args.directory / "notes.csv"
            client.call("project.save", path=str(project_path.resolve()))
            client.call("notes.exportCsv", path=str(csv_path.resolve()))
            client.call("project.new")
            client.call("project.open", path=str(project_path.resolve()))
            notes = client.call("notes.list")
            check("project_notes_roundtrip", any(item.get("text") == note for item in notes), len(notes))
            import csv
            with csv_path.open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.reader(stream))
            check("csv_multiline_unicode", any(note in row for row in rows), len(rows))
            output = args.directory / "sync-export.mkv"
            client.call("export.start", path=str(output.resolve()), kind="video", first=0, last=119,
                        width=320, height=180, viewerComposition=False, includeAnnotations=False,
                        includeAudio=True, videoCodec="ffv1", audioCodec="pcm_s16le")
            result = wait(lambda: (lambda value: value if value.get("state") != "running" else None)(client.call("export.status")), 90)
            check("export_finished", result.get("state") == "complete" and result.get("success") is True, result)
            check("export_file_published", output.is_file() and output.stat().st_size > 0)
        metadata = json.loads(subprocess.check_output([args.ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(output)]))
        video = next(stream for stream in metadata["streams"] if stream["codec_type"] == "video")
        audio = next(stream for stream in metadata["streams"] if stream["codec_type"] == "audio")
        check("decoded_video_frames", int(video["nb_read_frames"]) == 120, video["nb_read_frames"])
        check("decoded_video_rate", video["avg_frame_rate"] == "24/1", video["avg_frame_rate"])
        check("audio_stream", int(audio["sample_rate"]) == 48000 and int(audio["channels"]) == 2, {"rate": audio["sample_rate"], "channels": audio["channels"]})
        pixels = subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", str(output), "-an", "-vf", "crop=32:32:0:0,scale=1:1:flags=area", "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"])
        flashes = [index for index, value in enumerate(pixels) if value >= 240]
        check("all_video_flash_positions", flashes == [0, 24, 48, 72, 96], flashes)
        raw_audio = subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", str(output), "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "pipe:1"])
        samples = array.array("f")
        samples.frombytes(raw_audio)
        if sys.byteorder != "little":
            samples.byteswap()
        check("audio_length", abs(len(samples) - 240000) <= 1, len(samples))
        rms = math.sqrt(sum(value * value for value in samples) / len(samples))
        check("audio_not_silent", rms > 0.02, rms)
        errors = []
        for second in range(5):
            expected = second * 48000
            low, high = max(0, expected - 2000), min(len(samples), expected + 2000)
            onset = next((index for index in range(low, high) if abs(samples[index]) > 0.03), None)
            errors.append(None if onset is None else (onset - expected) / 48000)
        check("audio_video_flash_sync", all(error is not None and abs(error) <= 1 / 24 for error in errors), {"seconds": errors, "tolerance": 1 / 24})
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        (args.directory / "runtime-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()

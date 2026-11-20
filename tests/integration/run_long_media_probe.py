"""Ninety-second synthetic H.264/B-frame tail checks on an explicitly owned API.

No user media or pixels are printed. System FFmpeg independently decodes the inputs
from their beginning; exported FFV1/PCM tails isolate seek/timing from encoding loss.
This short regression is not a playback/GUI-latency test or a long-running soak.
"""
import argparse
import array
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, os.environ.get("OPENFRAME_TEST_MAYA_PATH", str(Path(__file__).resolve().parents[2] / "maya")))
from openframe_maya import Client
from openframe_maya.paths import local_path


CASES = (
    {"name": "long2997-gop900-aac", "frames": 2700, "fps": Fraction(30000, 1001), "gop": 900, "tail": 48},
    {"name": "long60-gop1800-aac", "frames": 5400, "fps": Fraction(60, 1), "gop": 1800, "tail": 60},
)
RGB_LIMITS = {"mae": 3.0, "rmse": 8.0, "p99_absolute_error": 24}


def run_tool(arguments, *, binary=False):
    process = subprocess.run(arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=180)
    if process.returncode:
        raise RuntimeError("{} failed: {}".format(Path(arguments[0]).name,
                           process.stderr.decode("utf-8", errors="replace")))
    return process.stdout if binary else process.stdout.decode("utf-8")


def metadata(ffprobe, path):
    return json.loads(run_tool([ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe",
                                "-count_frames", "-show_streams", "-show_format", "-of", "json", path]))


def frame_index(ffprobe, path):
    result = json.loads(run_tool([ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe",
                                  "-select_streams", "v:0", "-show_frames", "-show_entries",
                                  "frame=best_effort_timestamp_time,pkt_duration_time,duration_time,key_frame,pict_type",
                                  "-of", "json", path]))
    return result["frames"]


def rgb_pair(ffmpeg, path, first, last, width, height):
    # No -ss: the independent reference fully decodes in display order from frame zero.
    select = r"select=eq(n\,{})+eq(n\,{}),format=rgb24".format(first, last)
    pixels = run_tool([ffmpeg, "-v", "error", "-nostdin", "-protocol_whitelist", "file,pipe",
                       "-i", path, "-map", "0:v:0", "-an", "-vf", select, "-fps_mode", "passthrough",
                       "-frames:v", "2", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], binary=True)
    size = width * height * 3
    if len(pixels) != size * 2:
        raise RuntimeError("Independent RGB decode did not return exactly two frames")
    return pixels[:size], pixels[size:]


def rgb_metrics(reference, actual):
    if len(reference) != len(actual) or not reference:
        raise RuntimeError("RGB frame dimensions do not match")
    errors = [abs(a - b) for a, b in zip(reference, actual)]
    mae = math.fsum(errors) / len(errors)
    rmse = math.sqrt(math.fsum(value * value for value in errors) / len(errors))
    p99 = sorted(errors)[math.ceil(len(errors) * .99) - 1]
    return {"mae": mae, "rmse": rmse, "p99_absolute_error": p99,
            "maximum_absolute_error": max(errors),
            "psnr_db": 20 * math.log10(255 / rmse) if rmse else None,
            "identical": rmse == 0,
            "within_limits": mae <= RGB_LIMITS["mae"] and rmse <= RGB_LIMITS["rmse"]
                             and p99 <= RGB_LIMITS["p99_absolute_error"]}


def audio_metrics(ffmpeg, path):
    pcm = run_tool([ffmpeg, "-v", "error", "-nostdin", "-protocol_whitelist", "file,pipe",
                    "-i", path, "-map", "0:a:0", "-ar", "48000", "-ac", "2",
                    "-f", "s16le", "pipe:1"], binary=True)
    values = array.array("h")
    values.frombytes(pcm)
    if sys.byteorder != "little":
        values.byteswap()
    rms = []
    for channel in (0, 1):
        samples = values[channel::2]
        rms.append(math.sqrt(math.fsum(value * value for value in samples) / len(samples)) / 32768
                   if samples else 0)
    return {"samples": len(values) // 2, "sample_rate": 48000,
            "seconds": len(values) / 2 / 48000, "channel_rms": rms,
            "channel_rms_dbfs": [20 * math.log10(value) if value else None for value in rms],
            "not_silent": all(value > .01 for value in rms)}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_import(client):
    deadline = time.monotonic() + 120
    while client.call("status")["busy"]:
        require(time.monotonic() < deadline, "Synthetic long-media import timeout")
        time.sleep(.1)


def wait_export(client):
    deadline = time.monotonic() + 120
    while True:
        state = client.call("export.status")
        if state.get("state") in ("complete", "failed"):
            return state
        if time.monotonic() >= deadline:
            client.call("export.cancel")
            raise RuntimeError("Synthetic long-media tail export timeout")
        time.sleep(.1)


def check_case(client, args, case, entry):
    path = local_path(str(Path(args.fixture_dir) / (case["name"] + ".mp4")))
    first, last = case["frames"] - case["tail"], case["frames"] - 1
    fps, expected_seconds = case["fps"], case["tail"] / case["fps"]
    entry.update(fixture=path, source_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                 expected_source_frames=case["frames"], expected_fps=str(fps),
                 tail_first=first, tail_last=last, expected_tail_frames=case["tail"],
                 expected_tail_seconds=float(expected_seconds))
    entry["stage"] = "independent input validation"
    source = metadata(args.ffprobe, path)
    video = next(stream for stream in source["streams"] if stream["codec_type"] == "video")
    audio = next(stream for stream in source["streams"] if stream["codec_type"] == "audio")
    frames = frame_index(args.ffprobe, path)
    keys = [index for index, frame in enumerate(frames) if frame["key_frame"]]
    types = Counter(frame["pict_type"] for frame in frames)
    previous_key = max(frame for frame in keys if frame <= first)
    gaps = [b - a for a, b in zip(keys, keys[1:])]
    entry["source"] = {"video_codec": video["codec_name"], "audio_codec": audio["codec_name"],
                       "frame_count": int(video["nb_read_frames"]), "fps": video["avg_frame_rate"],
                       "seconds": float(video["duration"]), "width": video["width"], "height": video["height"],
                       "keyframe_indices": keys, "maximum_gop_frames": max(gaps),
                       "maximum_gop_seconds": float(max(gaps) / fps), "picture_types": dict(types),
                       "tail_start_frames_after_keyframe": first - previous_key}
    require(video["codec_name"] == "h264" and audio["codec_name"] == "aac", "Fixture codec mismatch")
    require(int(video["nb_read_frames"]) == case["frames"] and len(frames) == case["frames"],
            "Independent input frame count mismatch")
    require(Fraction(video["avg_frame_rate"]) == fps, "Independent input FPS mismatch")
    require(abs(float(video["duration"]) - float(case["frames"] / fps)) < .000002,
            "Independent input duration mismatch")
    require(types["B"] > 0 and max(gaps) == case["gop"] and first - previous_key > 20 * fps,
            "Fixture does not exercise a long B-frame GOP tail")
    reference = rgb_pair(args.ffmpeg, path, first, last, video["width"], video["height"])

    entry["stage"] = "import and standalone source seek"
    client.call("project.new", empty=True)
    started = time.monotonic()
    client.call("media.import", paths=[path])
    wait_import(client)
    entry["import_seconds"] = time.monotonic() - started
    sources = client.call("objects.list", type="source")
    require(len(sources) == 1 and sources[0]["frame_count"] == case["frames"], "Imported frame count mismatch")
    require(Fraction(sources[0]["fps_num"], sources[0]["fps_den"]) == fps, "Imported FPS mismatch")
    source_id = sources[0]["id"]
    client.call("viewer.assign", pane=0, id=source_id)
    require(client.call("status")["activeOwnerId"] == source_id, "Source was not activated")
    require(client.call("objects.list", type="timeline") == [], "Probe must use a standalone source")
    seeks = []
    for frame in (first, last, first + case["tail"] // 2, first):
        started = time.monotonic()
        actual = client.call("seek", frame=frame, audio=False, ignoreLoopState=True)
        state = client.call("status")
        require(actual == frame and state["frame"] == frame, "Tail seek frame mismatch")
        seeks.append({"requested": frame, "actual": actual, "api_seconds": time.monotonic() - started})
    entry["seeks"] = seeks

    entry["stage"] = "tail export"
    destination = local_path(str(Path(args.output_dir) / (case["name"] + "-tail.mkv")), output=True)
    require(not Path(destination).exists(), "Export destination already exists; use a fresh output directory")
    started = time.monotonic()
    client.call("export.start", path=destination, kind="video", first=first, last=last,
                width=video["width"], height=video["height"], viewerComposition=False,
                includeAnnotations=False, includeAudio=True, videoCodec="ffv1", audioCodec="pcm_s16le")
    result = wait_export(client)
    entry.update(export_path=destination, engine_result=result, export_seconds=time.monotonic() - started)
    require(result.get("success"), "Engine rejected tail export: " + str(result.get("message", result)))

    entry["stage"] = "independent exported tail validation"
    output = metadata(args.ffprobe, destination)
    output_video = next(stream for stream in output["streams"] if stream["codec_type"] == "video")
    output_audio = next(stream for stream in output["streams"] if stream["codec_type"] == "audio")
    output_frames = frame_index(args.ffprobe, destination)
    last_frame = output_frames[-1]
    end = float(last_frame["best_effort_timestamp_time"]) + float(
        last_frame.get("duration_time", last_frame.get("pkt_duration_time", 1 / fps)))
    pcm = audio_metrics(args.ffmpeg, destination)
    actual_pair = rgb_pair(args.ffmpeg, destination, 0, case["tail"] - 1, video["width"], video["height"])
    comparisons = {position: rgb_metrics(expected, actual)
                   for position, expected, actual in zip(("first", "last"), reference, actual_pair)}
    entry["output"] = {"frame_count": int(output_video["nb_read_frames"]),
                       "fps": output_video["avg_frame_rate"], "video_end_seconds": end,
                       "format_seconds": float(output["format"]["duration"]),
                       "pcm": pcm, "expected_pcm_samples": round(expected_seconds * 48000),
                       "rgb": comparisons}
    # Matroska timestamps quantize to milliseconds; PCM remains sample accurate.
    require(output_video["codec_name"] == "ffv1" and output_audio["codec_name"] == "pcm_s16le",
            "Tail export codecs changed")
    require(int(output_video["nb_read_frames"]) == case["tail"] and len(output_frames) == case["tail"],
            "Exported tail frame count mismatch")
    require(Fraction(output_video["avg_frame_rate"]) == fps, "Exported tail FPS mismatch")
    require(abs(float(output_frames[0]["best_effort_timestamp_time"])) <= .001,
            "Exported tail did not start at zero")
    require(abs(end - float(expected_seconds)) <= .002 and
            abs(float(output["format"]["duration"]) - float(expected_seconds)) <= .002,
            "Exported tail duration mismatch")
    require(abs(pcm["samples"] - round(expected_seconds * 48000)) <= 1 and pcm["not_silent"],
            "Exported tail audio is silent or has an incorrect sample count")
    require(all(item["within_limits"] for item in comparisons.values()),
            "Exported tail RGB differs from independent source frames beyond limits")
    entry.update(success=True, stage="validated")
    client.call("project.new", empty=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--owned-instance", required=True)
    parser.add_argument("--fixture-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ffmpeg", default=r"C:\Program Files\ffmpeg\bin\ffmpeg.exe")
    parser.add_argument("--ffprobe", default=r"C:\Program Files\ffmpeg\bin\ffprobe.exe")
    args = parser.parse_args()
    args.fixture_dir = local_path(args.fixture_dir, directory=True)
    args.output_dir = local_path(args.output_dir, directory=True)
    args.ffmpeg, args.ffprobe = local_path(args.ffmpeg), local_path(args.ffprobe)
    report = {"success": False, "instance": args.owned_instance, "rgb_limits": RGB_LIMITS,
              "client_module": sys.modules["openframe_maya.client"].__file__,
              "reference_ffmpeg": run_tool([args.ffmpeg, "-version"]).splitlines()[0], "cases": {},
              "limitations": ["Ninety-second fixtures are not a multi-hour soak.",
                              "API seek checks frame state; RGB checks exported decoding, not GUI render completion.",
                              "RGB comparisons cover the first and last tail frame, allowing bounded conversion differences."]}
    try:
        with Client(args.endpoint, timeout=10) as client:
            require(client.call("status")["instance"] == args.owned_instance,
                    "Not the explicitly owned synthetic process")
            for case in CASES:
                entry = {"success": False}
                report["cases"][case["name"]] = entry
                try:
                    check_case(client, args, case, entry)
                except Exception as error:
                    entry["error"] = str(error)
            report["success"] = all(entry["success"] for entry in report["cases"].values())
    except Exception as error:
        report["error"] = str(error)
    (Path(args.output_dir) / "long-media-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

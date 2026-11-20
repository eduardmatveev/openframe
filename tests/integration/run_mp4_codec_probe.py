"""Own-process MP4 export validation. Reads only explicitly supplied synthetic media."""
import argparse
import array
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, os.environ.get("OPENFRAME_TEST_MAYA_PATH", str(Path(__file__).resolve().parents[2] / "maya")))
from openframe_maya import Client, OpenFrameError
from openframe_maya.paths import local_path


def run_tool(arguments, *, binary=False):
    process = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    if process.returncode:
        raise RuntimeError("{} failed: {}".format(Path(arguments[0]).name, process.stderr.decode("utf-8", errors="replace")))
    return process.stdout if binary else process.stdout.decode("utf-8")


def probe(ffprobe, path):
    return json.loads(run_tool([ffprobe, "-v", "error", "-protocol_whitelist", "file,pipe", "-count_frames",
                               "-show_streams", "-show_format", "-of", "json", path]))


def pcm_stats(ffmpeg, path):
    pcm = run_tool([ffmpeg, "-v", "error", "-protocol_whitelist", "file,pipe", "-i", path,
                    "-map", "0:a:0", "-ar", "48000", "-ac", "2", "-f", "s16le", "pipe:1"], binary=True)
    values = array.array("h")
    values.frombytes(pcm)
    if sys.byteorder != "little":
        values.byteswap()
    count = len(values) // 2
    channels = []
    for channel in (0, 1):
        samples = values[channel::2]
        rms = math.sqrt(math.fsum(value * value for value in samples) / len(samples)) / 32768 if samples else 0
        peak = max(map(abs, samples), default=0) / 32768
        channels.append({"rms": rms, "rms_dbfs": 20 * math.log10(rms) if rms else None,
                         "peak": peak, "nonzero_fraction": sum(value != 0 for value in samples) / len(samples) if samples else 0})
    return {"sample_rate": 48000, "channels": channels, "sample_count": count,
            "duration_seconds": count / 48000, "not_silent": all(item["peak"] > 0.05 and item["rms"] > 0.001 for item in channels)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--owned-instance", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ffmpeg", default=r"C:\Program Files\ffmpeg\bin\ffmpeg.exe")
    parser.add_argument("--ffprobe", default=r"C:\Program Files\ffmpeg\bin\ffprobe.exe")
    parser.add_argument("--codecs", nargs="+", default=["mpeg4", "h264_mf"])
    args = parser.parse_args()
    fixture = local_path(args.fixture)
    output_dir = local_path(args.output_dir, directory=True)
    source = probe(args.ffprobe, fixture)
    video = next(stream for stream in source["streams"] if stream["codec_type"] == "video")
    expected_frames = int(video["nb_read_frames"])
    numerator, denominator = map(int, video["avg_frame_rate"].split("/"))
    expected_fps = numerator / denominator
    expected_duration = expected_frames / expected_fps
    report = {"fixture": fixture, "source_video_frames": expected_frames, "source_fps": expected_fps,
              "expected_duration_seconds": expected_duration, "source_audio": pcm_stats(args.ffmpeg, fixture),
              "instance": args.owned_instance, "codecs": {}}
    with Client(args.endpoint, timeout=5) as client:
        status = client.call("status")
        if status["instance"] != args.owned_instance:
            raise RuntimeError("Not the explicitly owned synthetic process")
        client.call("project.new")
        client.call("media.import", paths=[fixture])
        deadline = time.monotonic() + 30
        while client.call("status")["busy"]:
            if time.monotonic() >= deadline:
                raise RuntimeError("Synthetic fixture import timeout")
            time.sleep(0.05)
        imported = client.call("objects.list", type="source")
        if len(imported) != 1 or imported[0]["frame_count"] != expected_frames:
            raise RuntimeError("Synthetic import frame count mismatch")
        for codec in args.codecs:
            destination = local_path(os.path.join(output_dir, codec + "-aac.mp4"), output=True)
            entry = {"video_encoder": codec, "audio_encoder": "aac", "path": destination}
            report["codecs"][codec] = entry
            try:
                job = client.call("export.start", path=destination, kind="video", first=0, last=expected_frames - 1,
                                  width=video["width"], height=video["height"], viewerComposition=False,
                                  includeAnnotations=False, includeAudio=True, videoCodec=codec, audioCodec="aac")
                entry["job_id"] = job["jobId"]
                deadline = time.monotonic() + 120
                while True:
                    state = client.call("export.status")
                    if state.get("state") in ("complete", "failed"):
                        break
                    if time.monotonic() >= deadline:
                        client.call("export.cancel")
                        raise RuntimeError("Export timeout")
                    time.sleep(0.1)
                entry["engine_result"] = state
                if not state.get("success"):
                    entry.update(success=False, stage="engine")
                    continue
                metadata = probe(args.ffprobe, destination)
                Path(output_dir, codec + "-ffprobe.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
                video_stream = next(stream for stream in metadata["streams"] if stream["codec_type"] == "video")
                audio_stream = next(stream for stream in metadata["streams"] if stream["codec_type"] == "audio")
                pcm = pcm_stats(args.ffmpeg, destination)
                frame_count = int(video_stream["nb_read_frames"])
                fps_num, fps_den = map(int, video_stream["avg_frame_rate"].split("/"))
                padding = pcm["sample_count"] - round(expected_duration * 48000)
                entry.update(video_codec=video_stream["codec_name"], audio_codec=audio_stream["codec_name"],
                             video_frames=frame_count, fps=fps_num / fps_den,
                             video_duration_seconds=float(video_stream.get("duration", expected_duration)),
                             video_start_seconds=float(video_stream.get("start_time", 0)),
                             audio_start_seconds=float(audio_stream.get("start_time", 0)),
                             audio_stream_duration_seconds=float(audio_stream.get("duration", pcm["duration_seconds"])),
                             format_duration_seconds=float(metadata["format"]["duration"]), decoded_audio=pcm,
                             aac_padding_samples=padding, aac_padding_seconds=padding / 48000,
                             audio_padding_within_one_aac_frame=0 <= padding <= 1024)
                entry["success"] = (frame_count == expected_frames and fps_num / fps_den == expected_fps
                                    and "duration" in video_stream
                                    and abs(entry["video_duration_seconds"] - expected_duration) < 0.000002
                                    and abs(entry["video_start_seconds"]) < 0.000002
                                    and abs(entry["audio_start_seconds"]) < 0.000002
                                    and expected_duration <= entry["format_duration_seconds"] <= expected_duration + 1024 / 48000 + 0.000002
                                    and audio_stream["codec_name"] == "aac" and pcm["not_silent"] and 0 <= padding <= 1024)
                entry["stage"] = "validated"
            except OpenFrameError as error:
                entry.update(success=False, stage="api", error=str(error), error_code=error.code)
            except Exception as error:
                entry.update(success=False, stage="validation", error=str(error))
        client.call("project.new", empty=True)
    report["success"] = all(entry["success"] for entry in report["codecs"].values())
    report_path = Path(output_dir) / "codec-report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

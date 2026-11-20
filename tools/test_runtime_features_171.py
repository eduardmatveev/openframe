"""Exercise the packaged 1.7.1 feature wave through the real local API.

All images and audio are mathematical fixtures. This is OpenFrame validation,
not a claim of differential equivalence to another application.
"""
import argparse
import hashlib
import json
import math
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path


def bitmap(path, color, size=64):
    stride = (size * 3 + 3) & ~3
    row = bytes(reversed(color)) * size + bytes(stride - size * 3)
    pixels = row * size
    header = b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    path.write_bytes(header + dib + pixels)


def audio_measurement(path):
    with wave.open(str(path), "rb") as stream:
        channels, rate, width, frames = stream.getnchannels(), stream.getframerate(), stream.getsampwidth(), stream.getnframes()
        if width != 2:
            raise AssertionError("Expected PCM16, got sample width " + str(width))
        samples = struct.unpack("<{}h".format(frames * channels), stream.readframes(frames))
    measurements = []
    for channel in range(channels):
        selected = samples[rate // 4 * channels + channel:int(rate * 2.75) * channels:channels]
        # Resampling can ring by a few quantization steps near zero. A 1% FS
        # Schmitt threshold counts complete cycles, not those tiny recrossings.
        crossings = []
        armed = False
        for index, value in enumerate(selected):
            if value < -327:
                armed = True
            elif value > 327 and armed:
                crossings.append(index)
                armed = False
        frequency = (len(crossings) - 1) * rate / (crossings[-1] - crossings[0]) if len(crossings) > 1 else 0
        rms = math.sqrt(sum((value / 32768) ** 2 for value in selected) / max(1, len(selected)))
        measurements.append({"frequencyHz": frequency, "rms": rms})
    return {"frames": frames, "rate": rate, "channels": channels, "measurements": measurements}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--tracks", type=Path, required=True)
    parser.add_argument("--sync", type=Path, help="Enable the 0.3.0 audio-offset/export checks with the synthetic pulse fixture")
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--maya-module-path", type=Path, help="Test the adapter copied into the portable package")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    module_path = (args.maya_module_path or root / "maya").resolve()
    sys.path.insert(0, str(module_path))
    from openframe_maya.client import Client
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "checks": {}, "scope": "Packaged OpenFrame functionality; reference parity unverified",
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
              "adapterModulePath": str(module_path)}

    def check(name, condition, detail=None):
        report["checks"][name] = {"passed": bool(condition), "detail": detail}
        if not condition:
            raise AssertionError("{}: {}".format(name, detail))

    def wait(predicate, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.025)
        raise TimeoutError("Operation did not complete")

    try:
        black = args.directory / "synthetic-black.bmp"
        magenta = args.directory / "synthetic-magenta.bmp"
        bitmap(black, (0, 0, 0))
        bitmap(magenta, (255, 0, 255))
        with Client(args.endpoint, timeout=5) as client:
            def import_source(path):
                before = {item["id"] for item in client.call("objects.list", type="source")}
                client.call("media.import", paths=[str(path.resolve())])
                wait(lambda: not client.call("status")["busy"])
                found = [item for item in client.call("objects.list", type="source") if item["id"] not in before]
                if len(found) != 1:
                    raise AssertionError("Expected one new source, got " + str(len(found)))
                return found[0]

            def export(path, **parameters):
                client.call("export.start", path=str(path.resolve()), **parameters)
                result = wait(lambda: (lambda value: value if value.get("state") != "running" else None)(client.call("export.status")), 60)
                if not result.get("success"):
                    raise AssertionError("Export failed: " + str(result))

            client.call("project.new", empty=True)
            black_source = import_source(black)
            client.call("viewer.assign", pane=0, id=black_source["id"])
            client.call("viewer.set", mode="single", showOverlay=True, showAnnotations=False, zoom=1, panX=0, panY=0)
            client.call("overlays.set", type="image", imagePath=str(magenta.resolve()), imageRect=[0.25, 0.25, 0.5, 0.5], imageOpacity=1, opacity=1, imageKeepAspect=False)
            overlay_dir = args.directory / "overlay-export"
            export(overlay_dir, kind="images", first=0, last=0, width=320, height=180,
                   viewerComposition=True, includeAnnotations=False, includeAudio=False, imageFormat="bmp")
            images = sorted(overlay_dir.glob("*.bmp"))
            check("one_composited_image", len(images) == 1, len(images))
            pixels = subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", str(images[0]), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"])
            def pixel(x, y):
                at = (y * 320 + x) * 3
                return list(pixels[at:at + 3])
            check("image_overlay_center", pixel(160, 90) == [255, 0, 255], pixel(160, 90))
            # The square source is fitted inside the wide viewer; choose a
            # point in the source, outside the overlay, not the letterbox.
            check("image_overlay_outside", pixel(160, 10) == [0, 0, 0], pixel(160, 10))

            source = import_source(args.tracks)
            client.call("viewer.assign", pane=0, id=source["id"])
            tracks = client.call("media.audioTracks", id=source["id"])
            descriptors = tracks["audioTracks"]
            check("two_audio_tracks", len(descriptors) == 2, descriptors)
            check("audio_track_metadata", [(t["sampleRate"], t["channels"], t["language"]) for t in descriptors] == [(48000, 1, "eng"), (44100, 2, "fra")])
            for ordinal, expected in enumerate(([440, 440], [880, 660])):
                index = descriptors[ordinal]["streamIndex"]
                client.call("media.selectAudioTrack", id=source["id"], streamIndex=index)
                path = args.directory / ("track-{}.wav".format(ordinal))
                export(path, kind="audio", first=0, last=71, viewerComposition=False, includeAudio=True, audioCodec="pcm_s16le")
                result = audio_measurement(path)
                check("track_{}_duration".format(ordinal), result["frames"] == 144000 and result["rate"] == 48000 and result["channels"] == 2, result)
                check("track_{}_signal".format(ordinal), all(abs(item["frequencyHz"] - frequency) < 1 and item["rms"] > 0.04 for item, frequency in zip(result["measurements"], expected)), result["measurements"])
            client.call("viewer.set", zoom=2, panX=0.125, panY=-0.25)
            client.call("viewer.assign", pane=0, id=black_source["id"])
            check("owner_restores_default_pan_zoom", client.call("viewer.settings")["panZoom"]["zoom"] == 1)
            client.call("viewer.assign", pane=0, id=source["id"])
            check("owner_restores_custom_pan_zoom", client.call("viewer.settings")["panZoom"] == {"zoom": 2, "panX": 0.125, "panY": -0.25, "enabled": True}, client.call("viewer.settings")["panZoom"])
            client.call("range.set", first=0, last=71, loop="loop", loopDelayMs=500, loopHoldEmpty=True)
            project = args.directory / "feature-roundtrip.oframe"
            client.call("project.save", path=str(project.resolve()))
            client.call("project.new", empty=True)
            client.call("project.open", path=str(project.resolve()))
            check("saved_audio_track", client.call("media.audioTracks", id=source["id"])["selectedAudioTrack"] == descriptors[1]["streamIndex"])
            settings = client.call("viewer.settings")
            check("saved_pan_zoom", settings["panZoom"] == {"zoom": 2, "panX": 0.125, "panY": -0.25, "enabled": True}, settings["panZoom"])
            overlay = client.call("overlays.get")
            check("saved_image_overlay", overlay["type"] == "image" and Path(overlay["imagePath"]).resolve() == magenta.resolve(), overlay)
            status = client.call("status")
            check("saved_loop_delay", status["loopDelayMs"] == 500 and status["loopHoldEmpty"] is True)
            client.call("range.set", first=0, last=1)
            client.call("seek", frame=0, audio=False)
            client.call("play", playing=True, rate=1)
            delayed = wait(lambda: (lambda value: value if value.get("loopDelayActive") else None)(client.call("status")), 5)
            check("live_loop_delay_entered", delayed["playing"] and delayed["loopDelayActive"])
            client.call("play", playing=False)
            check("pause_cancels_loop_delay", client.call("status")["loopDelayActive"] is False)
            if args.sync:
                client.call("project.new", empty=True)
                pulse_source = import_source(args.sync)
                client.call("viewer.assign", pane=0, id=pulse_source["id"])
                check("time_stretch_disable", client.call("audio.timeStretch", enabled=False) == {"enabled": False})
                check("time_stretch_enable", client.call("audio.timeStretch", enabled=True) == {"enabled": True})
                offset_audio = {}
                # The fixture pulses at every integer second for five seconds;
                # advancing it by 250 ms brings both 1 s and 2 s into this range.
                for offset, expected_onsets in ((250000, [0.75, 1.75]), (-250000, [0.25, 1.25])):
                    updated = client.call("media.setAudioOffset", id=pulse_source["id"], offsetUs=offset)
                    check("source_offset_{}".format(offset), updated["audioOffsetUs"] == offset)
                    client.call("play", playing=False, rate=2, audioTimeStretching=True)
                    path = args.directory / "offset-{}.wav".format(offset)
                    export(path, kind="audio", first=0, last=47, viewerComposition=False, includeAudio=True, audioCodec="pcm_s16le")
                    with wave.open(str(path), "rb") as stream:
                        channels, rate, frames = stream.getnchannels(), stream.getframerate(), stream.getnframes()
                        raw = stream.readframes(frames)
                    values = struct.unpack("<{}h".format(frames * channels), raw)[::channels]
                    onsets = []
                    previous = -rate
                    for index, value in enumerate(values):
                        if abs(value) > 983:
                            if index - previous > rate // 10:
                                onsets.append(index / rate)
                            previous = index
                    check("offset_{}_export_duration_at_playback_2x".format(offset), frames == 96000 and rate == 48000 and channels == 2,
                          {"frames": frames, "rate": rate, "channels": channels})
                    check("offset_{}_pulse_positions".format(offset), len(onsets) == len(expected_onsets) and
                          all(abs(actual - expected) < 0.001 for actual, expected in zip(onsets, expected_onsets)),
                          {"actualSeconds": onsets, "expectedSeconds": expected_onsets, "toleranceSeconds": 0.001})
                    offset_audio[offset] = raw
                client.call("play", playing=False, rate=-2, audioTimeStretching=False)
                reverse_path = args.directory / "offset-export-while-reverse-selected.wav"
                export(reverse_path, kind="audio", first=0, last=47, viewerComposition=False, includeAudio=True, audioCodec="pcm_s16le")
                with wave.open(str(reverse_path), "rb") as stream:
                    reverse_raw = stream.readframes(stream.getnframes())
                check("export_independent_of_playback_direction_and_stretch", reverse_raw == offset_audio[-250000])
                offset_project = args.directory / "audio-offset-roundtrip.oframe"
                client.call("project.save", path=str(offset_project.resolve()))
                client.call("project.new", empty=True)
                client.call("project.open", path=str(offset_project.resolve()))
                check("source_offset_project_roundtrip", client.call("object.get", id=pulse_source["id"])["audioOffsetUs"] == -250000)
            client.call("project.new", empty=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        (args.directory / "feature-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "error": report.get("error")}, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

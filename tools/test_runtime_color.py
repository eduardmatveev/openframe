"""Validate a packaged OpenFrame color pipeline with independent FFmpeg decoding.

Only the six mathematically generated fixtures from make_color_fixtures.py
are accepted. This does not compare tone mapping with a proprietary product.
"""
import argparse
import array
import hashlib
import json
import struct
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--maya-module-path", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "checks": {}, "scope": "Independent verification of own coded ramps; reference parity unverified",
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}

    def check(name, condition, detail=None):
        report["checks"][name] = {"passed": bool(condition), "detail": detail}
        if not condition:
            raise AssertionError("{}: {}".format(name, detail))

    def wait(predicate, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.025)
        raise TimeoutError("OpenFrame color operation did not complete")

    def probe(path):
        streams = json.loads(subprocess.check_output([args.ffprobe, "-v", "error", "-count_frames",
            "-show_streams", "-of", "json", str(path)]))["streams"]
        return next(stream for stream in streams if stream["codec_type"] == "video")

    def decode(path, pixel_format, all_frames=False):
        command = [args.ffmpeg, "-v", "error", "-nostdin", "-i", str(path), "-an"]
        if not all_frames:
            command += ["-frames:v", "1"]
        command += ["-pix_fmt", pixel_format, "-f", "rawvideo", "pipe:1"]
        return subprocess.check_output(command)

    try:
        fixtures = json.loads(args.fixtures.read_text(encoding="utf-8"))["fixtures"]
        if len(fixtures) != 6:
            raise ValueError("Expected the six independently generated color fixtures")
        sys.path.insert(0, str(args.maya_module_path.resolve()))
        from openframe_maya.client import Client
        with Client(args.endpoint, timeout=5) as client:
            def export(path, **parameters):
                client.call("export.start", path=str(path.resolve()), **parameters)
                result = wait(lambda: (lambda item: item if item.get("state") != "running" else None)(client.call("export.status")))
                if not result.get("success"):
                    raise AssertionError("Color export failed: " + str(result))

            client.call("project.new", empty=True)
            source_ids = {}
            for fixture in fixtures:
                source_path = args.fixtures.parent / fixture["path"]
                label = source_path.stem
                check(label + "_fixture_identity", hashlib.sha256(source_path.read_bytes()).hexdigest() == fixture["fileSha256"])
                before = {item["id"] for item in client.call("objects.list", type="source")}
                client.call("media.import", paths=[str(source_path.resolve())])
                wait(lambda: not client.call("status")["busy"])
                added = [item for item in client.call("objects.list", type="source") if item["id"] not in before]
                check(label + "_one_source", len(added) == 1)
                source_ids[label] = added[0]["id"]
                client.call("viewer.assign", pane=0, id=added[0]["id"])
                native_output = args.directory / (label + "-native.mkv")
                export(native_output, kind="video", first=0, last=23,
                       width=fixture["width"], height=fixture["height"],
                       viewerComposition=False, includeAnnotations=False, includeAudio=False,
                       videoCodec="ffv1", colorMode="preserve-source", bitDepth=0)
                stream = probe(native_output)
                check(label + "_native_metadata", stream["pix_fmt"] == fixture["pixelFormat"] and
                      stream.get("color_primaries") == fixture["colorPrimaries"] and
                      stream.get("color_transfer") == fixture["colorTransfer"] and stream.get("color_range") == "pc",
                      {key: stream.get(key) for key in ("pix_fmt", "color_primaries", "color_transfer", "color_range", "color_space")})
                check(label + "_native_timing", (int(stream["nb_read_frames"]), stream["avg_frame_rate"], stream["width"], stream["height"]) ==
                      (24, "24/1", fixture["width"], fixture["height"]))
                raw = decode(native_output, fixture["pixelFormat"], all_frames=True)
                frame_bytes = fixture["width"] * fixture["height"] * 3 * 2
                hashes = [hashlib.sha256(raw[index:index + frame_bytes]).hexdigest() for index in range(0, len(raw), frame_bytes)]
                check(label + "_all_native_frames_exact", len(raw) == frame_bytes * 24 and
                      all(value == fixture["rawFirstFrameSha256"] for value in hashes),
                      {"decodedFrames": len(hashes), "uniqueHashes": sorted(set(hashes)), "expectedHash": fixture["rawFirstFrameSha256"]})
                gray_codes = struct.unpack_from("<{}H".format(fixture["width"]), raw)
                check(label + "_all_neighbor_codes_distinct", gray_codes == tuple(range(1 << fixture["bits"])),
                      {"distinctGrayCodes": len(set(gray_codes)), "expected": 1 << fixture["bits"]})
                if label.startswith("sdr-"):
                    png_directory = args.directory / (label + "-png16")
                    export(png_directory, kind="images", first=0, last=0,
                           width=fixture["width"], height=fixture["height"],
                           viewerComposition=False, includeAnnotations=False, includeAudio=False,
                           imageFormat="png", colorMode="preserve-source", bitDepth=16)
                    images = list(png_directory.glob("*.png"))
                    check(label + "_one_png16", len(images) == 1)
                    png_info = probe(images[0])
                    check(label + "_png16_format", png_info["pix_fmt"] in ("rgb48be", "rgb48le", "rgba64be", "rgba64le"), png_info["pix_fmt"])
                    expected = array.array("H")
                    maximum = (1 << fixture["bits"]) - 1
                    for y in range(fixture["height"]):
                        band = y // 16
                        for code in range(fixture["width"]):
                            value = (code * 65535 + maximum // 2) // maximum
                            expected.extend(value if band in (0, component + 1) else 0 for component in range(3))
                    if sys.byteorder != "little":
                        expected.byteswap()
                    actual = decode(images[0], "rgb48le")
                    check(label + "_png16_normalized_codes_exact", actual == expected.tobytes(),
                          {"actualSha256": hashlib.sha256(actual).hexdigest(), "expectedSha256": hashlib.sha256(expected.tobytes()).hexdigest(),
                           "formula": "round(sourceCode*65535/(2^sourceBits-1))"})
                else:
                    # Both outputs use the explicit SDR display conversion.
                    # This checks a common result, not equality with an unknown reference curve.
                    client.call("viewer.set", mode="single", showOverlay=False, showAnnotations=False, zoom=1, panX=0, panY=0)
                    image_directory = args.directory / (label + "-display-image")
                    display_video = args.directory / (label + "-display-video.mkv")
                    common = dict(first=0, last=0, width=fixture["width"], height=fixture["height"],
                                  viewerComposition=True, includeAnnotations=False, includeAudio=False,
                                  colorMode="display-converted", bitDepth=8)
                    export(image_directory, kind="images", imageFormat="bmp", **common)
                    export(display_video, kind="video", videoCodec="ffv1", **common)
                    image = next(image_directory.glob("*.bmp"))
                    image_rgb, video_rgb = decode(image, "rgb24"), decode(display_video, "rgb24")
                    check(label + "_display_image_video_match", image_rgb == video_rgb,
                          {"imageSha256": hashlib.sha256(image_rgb).hexdigest(), "videoSha256": hashlib.sha256(video_rgb).hexdigest()})
                    gray = image_rgb[:fixture["width"] * 3:3]
                    check(label + "_display_gray_monotonic", len(set(gray)) > 32 and all(a <= b for a, b in zip(gray, gray[1:])),
                          {"minimum": min(gray), "maximum": max(gray), "distinctValues": len(set(gray))})
                    display_info = probe(display_video)
                    check(label + "_display_has_sdr_tags", display_info.get("color_primaries") == "bt709" and
                          display_info.get("color_transfer") in ("iec61966-2-1", "bt709"),
                          {key: display_info.get(key) for key in ("color_primaries", "color_transfer", "color_space", "color_range")})
            client.call("viewer.assign", pane=0, id=source_ids["sdr-ramp-12bit"])
            invalid_output = args.directory / "unsupported-12bit-mpeg4.mkv"
            error = None
            try:
                export(invalid_output, kind="video", first=0, last=1, width=4096, height=64,
                       viewerComposition=False, includeAnnotations=False, includeAudio=False,
                       videoCodec="mpeg4", colorMode="preserve-source", bitDepth=12)
            except Exception as exc:
                error = str(exc)
            check("unsupported_depth_rejected_without_publication", bool(error) and not invalid_output.exists(), error)
            client.call("project.new", empty=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        (args.directory / "color-runtime-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

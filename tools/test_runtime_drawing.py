"""Check actual exported wipe geometry and pressure/eraser composition.

All raster sources and .oframe inputs are independently generated fixtures.
Tablet hardware and reference-product geometry are not tested by this script.
"""
import argparse
import copy
import hashlib
import json
import math
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path


WIDTH, HEIGHT = 160, 96


def bitmap(path, color):
    stride = (WIDTH * 3 + 3) & ~3
    pixels = (bytes(reversed(color)) * WIDTH + bytes(stride - WIDTH * 3)) * HEIGHT
    path.write_bytes(b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54) +
        struct.pack("<IiiHHIIiiII", 40, WIDTH, HEIGHT, 1, 24, 0, len(pixels), 2835, 2835, 0, 0) + pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--maya-module-path", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "checks": {}, "scope": "Own mathematical geometry and project fixtures; no physical tablet or reference parity claim",
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}

    def check(name, condition, detail=None):
        report["checks"][name] = {"passed": bool(condition), "detail": detail}
        if not condition:
            raise AssertionError("{}: {}".format(name, detail))

    def wait(predicate):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.025)
        raise TimeoutError("OpenFrame drawing operation did not complete")

    def pixel(rgb, x, y):
        offset = (y * WIDTH + x) * 3
        return tuple(rgb[offset:offset + 3])

    try:
        sys.path.insert(0, str(args.maya_module_path.resolve()))
        from openframe_maya.client import Client
        with Client(args.endpoint, timeout=5) as client:
            def render(label):
                directory = args.directory / label
                client.call("export.start", path=str(directory.resolve()), kind="images", first=0, last=0,
                            width=WIDTH, height=HEIGHT, viewerComposition=True, includeAnnotations=True,
                            includeAudio=False, imageFormat="bmp", colorMode="display-converted", bitDepth=8)
                result = wait(lambda: (lambda value: value if value.get("state") != "running" else None)(client.call("export.status")))
                if result.get("success") is not True:
                    raise AssertionError("Drawing export failed: " + str(result))
                images = list(directory.glob("*.bmp"))
                if len(images) != 1:
                    raise AssertionError("Expected one drawing export")
                return subprocess.check_output([args.ffmpeg, "-v", "error", "-nostdin", "-i", str(images[0]),
                    "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"])

            client.call("project.new", empty=True)
            sources = {}
            for name, color in (("red", (255, 0, 0)), ("blue", (0, 0, 255)), ("black", (0, 0, 0))):
                path = args.directory / (name + ".bmp")
                bitmap(path, color)
                before = {item["id"] for item in client.call("objects.list", type="source")}
                client.call("media.import", paths=[str(path.resolve())])
                wait(lambda: not client.call("status")["busy"])
                added = [item for item in client.call("objects.list", type="source") if item["id"] not in before]
                if len(added) != 1:
                    raise AssertionError("Expected one fixture source")
                sources[name] = added[0]["id"]
            client.call("viewer.assign", pane=0, id=sources["red"])
            client.call("viewer.assign", pane=1, id=sources["blue"])
            client.call("viewer.set", mode="wipe", showOverlay=False, showAnnotations=True, zoom=1, panX=0, panY=0)
            for angle, pivot_x, pivot_y in ((0, .5, .5), (45, .5, .5), (90, .5, .5),
                                             (180, .5, .5), (270, .5, .5), (37, .23, .71)):
                label = "wipe-{}".format(angle)
                client.call("viewer.set", wipe=pivot_x, wipePivotY=pivot_y, wipeAngleDegrees=angle)
                rgb = render(label)
                failures, checked = [], 0
                theta = math.radians(angle)
                for y in range(8, HEIGHT, 16):
                    for x in range(8, WIDTH, 16):
                        signed_distance = math.cos(theta) * (x + .5 - pivot_x * WIDTH) + math.sin(theta) * (y + .5 - pivot_y * HEIGHT)
                        if abs(signed_distance) <= 2:
                            continue
                        expected = (255, 0, 0) if signed_distance < 0 else (0, 0, 255)
                        actual = pixel(rgb, x, y)
                        checked += 1
                        if actual != expected:
                            failures.append({"x": x, "y": y, "actual": actual, "expected": expected})
                check(label + "_pixel_half_planes", checked > 40 and not failures,
                      {"checkedPixels": checked, "failures": failures[:8], "aspectRatio": "160:96"})
            wipe_project = args.directory / "wipe-roundtrip.oframe"
            client.call("project.save", path=str(wipe_project.resolve()))
            client.call("project.new", empty=True)
            client.call("project.open", path=str(wipe_project.resolve()))
            settings = client.call("viewer.settings")
            check("wipe_geometry_roundtrip", settings["wipe"] == .23 and settings["wipePivotY"] == .71 and settings["wipeAngleDegrees"] == 37)

            client.call("viewer.assign", pane=0, id=sources["black"])
            client.call("viewer.set", mode="single", showOverlay=False, showAnnotations=True)
            base_path = args.directory / "drawing-base.oframe"
            client.call("project.save", path=str(base_path.resolve()))
            base = json.loads(base_path.read_text(encoding="utf-8-sig"))

            def stroke(points, *, scales=None, tool=0, width=120, opacity=1, background=False, color="#ffff0000"):
                return {"id": str(uuid.uuid4()), "timelineId": sources["black"], "frame": "0", "endFrame": "0",
                        "layer": "Default", "visible": True, "hold": False, "ghostFrames": 0,
                        "tool": tool, "opacity": opacity, "background": background,
                        "points": points, "widthScales": scales or [], "color": color,
                        "width": width, "text": "", "textPosition": [0, 0], "fontSize": 28}

            def load_drawings(label, drawings):
                project = copy.deepcopy(base)
                project["annotations"] = drawings
                path = args.directory / (label + ".oframe")
                path.write_text(json.dumps(project), encoding="utf-8")
                client.call("project.open", path=str(path.resolve()))

            horizontal = [[.1, .5], [.9, .5]]
            load_drawings("pressure", [stroke(horizontal, scales=[.2, 1])])
            pressure = render("pressure-export")
            thickness = [sum(pixel(pressure, x, y)[0] > 128 for y in range(HEIGHT)) for x in (32, 128)]
            check("pressure_changes_rendered_width", 1 <= thickness[0] <= 5 and thickness[1] >= thickness[0] + 4,
                  {"earlyWidthPixels": thickness[0], "lateWidthPixels": thickness[1]})
            saved_pressure = args.directory / "pressure-saved.oframe"
            client.call("project.save", path=str(saved_pressure.resolve()))
            preserved = json.loads(saved_pressure.read_text(encoding="utf-8-sig"))
            check("pressure_widths_roundtrip", preserved["annotations"][0]["widthScales"] == [.2, 1])
            invalid = copy.deepcopy(preserved)
            invalid["annotations"][0]["widthScales"] = [.5]
            invalid_path = args.directory / "invalid-width-count.oframe"
            invalid_path.write_text(json.dumps(invalid), encoding="utf-8")
            error = None
            try:
                client.call("project.open", path=str(invalid_path.resolve()))
            except Exception as exc:
                error = str(exc)
            unchanged = render("pressure-after-invalid")
            check("invalid_pressure_project_is_atomic", bool(error) and unchanged == pressure, error)

            load_drawings("zero-pressure", [stroke(horizontal, scales=[0, 0])])
            zero = render("zero-pressure-export")
            check("zero_pressure_draws_no_ink", not any(zero))
            load_drawings("single-opacity", [stroke(horizontal, opacity=.5)])
            single = render("single-opacity-export")
            load_drawings("retraced-opacity", [stroke(horizontal + [horizontal[0]], opacity=.5)])
            retraced = render("retraced-opacity-export")
            check("one_stroke_applies_opacity_once", pixel(single, 80, 48) == pixel(retraced, 80, 48),
                  {"single": pixel(single, 80, 48), "retraced": pixel(retraced, 80, 48)})

            eraser = stroke([[.5, .1], [.5, .9]], tool=5)
            load_drawings("eraser", [stroke(horizontal, width=360), eraser])
            erased = render("eraser-export")
            check("eraser_reveals_video", pixel(erased, 80, 48) == (0, 0, 0) and pixel(erased, 40, 48) == (255, 0, 0),
                  {"erased": pixel(erased, 80, 48), "untouched": pixel(erased, 40, 48)})
            load_drawings("eraser-scope", [stroke(horizontal, width=360, background=True, color="#ff0000ff"),
                                            stroke(horizontal), eraser])
            scoped = render("eraser-scope-export")
            check("foreground_eraser_preserves_background_layer", pixel(scoped, 80, 48) == (0, 0, 255) and pixel(scoped, 40, 48) == (255, 0, 0),
                  {"erasedForeground": pixel(scoped, 80, 48), "untouchedForeground": pixel(scoped, 40, 48)})
            client.call("project.new", empty=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        (args.directory / "drawing-runtime-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

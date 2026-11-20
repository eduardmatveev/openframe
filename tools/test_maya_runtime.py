"""Integration check inside a separate, empty Maya process.

Run with mayapy for synchronization, or call run(..., playblast=True) from
an isolated GUI Maya. This deliberately refuses a modified or populated scene.
Only synthetic geometry and local files in the report directory are created.
"""
import argparse
import json
import sys
import time
import traceback
from pathlib import Path


def run(endpoint, media, report_path, *, playblast=False, maya_module_path=None, extended_capture=False):
    import maya.cmds as cmds
    import maya.utils as utils
    module_path = Path(maya_module_path) if maya_module_path else Path(__file__).resolve().parents[1] / "maya"
    sys.path.insert(0, str(module_path))
    from openframe_maya.adapter import Adapter, SyncMapping
    from openframe_maya.client import Client
    report_path = Path(report_path).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = {"mayaVersion": cmds.about(version=True), "batch": cmds.about(batch=True),
              "checks": {}, "passed": False, "playblastRequested": playblast,
              "extendedCaptureRequested": extended_capture}
    adapter = None
    client = None

    def check(name, actual, expected):
        passed = actual == expected
        report["checks"][name] = {"passed": passed, "actual": actual, "expected": expected}
        if not passed:
            raise AssertionError("{}: {!r} != {!r}".format(name, actual, expected))

    def pump():
        utils.processIdleEvents()
        try:
            from PySide2.QtCore import QCoreApplication
        except ImportError:
            from PySide6.QtCore import QCoreApplication
        if QCoreApplication.instance():
            QCoreApplication.processEvents()

    def until(predicate, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            pump()
            if predicate():
                return
            time.sleep(0.025)
        raise TimeoutError("Timed out waiting for Maya/OpenFrame synchronization")

    try:
        if cmds.file(query=True, modified=True) or cmds.ls(geometry=True):
            raise RuntimeError("Run this check in a separate empty Maya, never in a user scene")
        client = Client(endpoint, timeout=5)
        client.connect()
        client.call("project.new")
        client.call("media.import", paths=[str(Path(media).resolve())])
        until(lambda: not client.call("status")["busy"])
        check("synthetic_media_frames", client.call("status")["durationFrames"], 120)
        cmds.currentUnit(time="film")
        cmds.playbackOptions(minTime=1, maxTime=24)
        cmds.currentTime(1)
        mapping = SyncMapping(offset=10, multiplier=2)
        adapter = Adapter(endpoint_path=endpoint, poll_interval=0.04)
        adapter.start(direction="bidirectional", mapping=mapping)
        until(lambda: adapter.state["connected"])
        cmds.currentTime(11)
        until(lambda: client.call("status")["frame"] == 32)
        check("maya_to_openframe_offset_scale", client.call("status")["frame"], 32)
        client.call("seek", frame=70, audio=False)
        until(lambda: abs(cmds.currentTime(query=True) - 30.0) < 1e-8)
        check("openframe_to_maya_offset_scale", cmds.currentTime(query=True), 30.0)
        revision = client.call("status")["frameRevision"]
        end = time.monotonic() + 0.6
        while time.monotonic() < end:
            pump()
            time.sleep(0.02)
        check("no_feedback_loop", client.call("status")["frameRevision"], revision)
        cmds.playbackOptions(minTime=5, maxTime=20)
        adapter.send_range()
        until(lambda: client.call("status")["rangeIn"] == 20 and client.call("status")["rangeOut"] == 50)
        check("maya_range_mapping", [client.call("status")["rangeIn"], client.call("status")["rangeOut"]], [20, 50])
        client.call("range.set", first=30, last=70)
        until(lambda: adapter.state["status"].get("rangeIn") == 30)
        adapter.receive_range()
        check("openframe_range_mapping", [cmds.playbackOptions(query=True, minTime=True), cmds.playbackOptions(query=True, maxTime=True)], [10.0, 30.0])
        adapter.stop()
        check("adapter_stop", adapter.state["running"], False)
        adapter.start(direction="bidirectional", mapping=mapping)
        until(lambda: adapter.state["connected"])
        cmds.currentTime(17)
        until(lambda: client.call("status")["frame"] == 44)
        check("adapter_restart", client.call("status")["frame"], 44)
        if playblast:
            if cmds.about(batch=True):
                raise RuntimeError("Playblast validation requires the separate GUI Maya")
            import openframe_maya
            window = openframe_maya.show()
            pump()
            check("maya_adapter_window_visible", cmds.window(window.window, query=True, visible=True), True)
            import maya.OpenMayaUI as omui
            try:
                from shiboken6 import wrapInstance
                from PySide6.QtWidgets import QWidget
            except ImportError:
                from shiboken2 import wrapInstance
                from PySide2.QtWidgets import QWidget
            widget = wrapInstance(int(omui.MQtUtil.findWindow(window.window)), QWidget)
            check("maya_adapter_window_capture", widget.grab().save(str(report_path.parent / "maya-adapter-window.png")), True)
            cmds.deleteUI(window.window, window=True)
            adapter.configure(direction="off")
            cmds.playbackOptions(minTime=1, maxTime=24)
            cube = cmds.polyCube(name="OpenFrameSyntheticCube")[0]
            cmds.setKeyframe(cube, attribute="translateX", time=1, value=-2)
            cmds.setKeyframe(cube, attribute="translateX", time=24, value=2)
            cmds.select(cube)
            cmds.viewFit("persp", all=True)
            cmds.currentTime(17)
            callback_result = []
            clip_path = report_path.parent / ("maya-playblast-{}.avi".format(time.time_ns()))
            adapter.playblast(str(clip_path), viewer="B", width=320, height=180,
                              callback=lambda result, error: callback_result.append((result, error)))
            until(lambda: bool(callback_result), seconds=120)
            if callback_result[0][1]:
                raise RuntimeError(str(callback_result[0][1]))
            check("playblast_file_created", clip_path.exists() and clip_path.stat().st_size > 0, True)
            imported_source = callback_result[0][0]
            until(lambda: client.call("viewer.get", pane=1).get("id") == imported_source["id"])
            check("playblast_assigned_to_B", client.call("viewer.get", pane=1)["id"], imported_source["id"])
            check("playblast_imported_frames", imported_source["frame_count"], 24)
            check("playblast_moves_to_first_captured_frame", cmds.currentTime(query=True), 1.0)
            report["playblastPath"] = str(clip_path)
            if extended_capture:
                try:
                    from PySide2.QtGui import QImage
                except ImportError:
                    from PySide6.QtGui import QImage
                report["sequences"] = []
                for frame_count in (24, 257):
                    cmds.playbackOptions(minTime=1, maxTime=frame_count)
                    cmds.currentTime(17)
                    before = {item["id"] for item in client.call("objects.list", type="source")}
                    sequence_callbacks = []
                    sequence_dir = report_path.parent / ("maya-sequence-{}-{}".format(frame_count, time.time_ns()))
                    capture = adapter.playblast_sequence(str(sequence_dir), viewer="B", width=128, height=72,
                        timecode_offset=True,
                        callback=lambda result, error: sequence_callbacks.append((result, error)))
                    until(lambda: bool(sequence_callbacks), seconds=120)
                    if sequence_callbacks[0][1]:
                        raise RuntimeError(str(sequence_callbacks[0][1]))
                    sequence_source = sequence_callbacks[0][0]
                    label = "sequence_{}_".format(frame_count)
                    check(label + "typed_result", [capture.kind, capture.first, capture.last, capture.fps_num, capture.fps_den],
                          ["sequence", 1.0, float(frame_count), 24, 1])
                    expected = ["frame.{:04d}.png".format(index) for index in range(frame_count)]
                    check(label + "complete_file_set", sorted(path.name for path in sequence_dir.glob("*.png")), expected)
                    check(label + "returned_paths", [Path(path).name for path in capture.paths], expected)
                    check(label + "dimensions", [QImage(capture.paths[0]).width(), QImage(capture.paths[0]).height()], [128, 72])
                    check(label + "one_source", len([item for item in client.call("objects.list", type="source") if item["id"] not in before]), 1)
                    check(label + "frame_count", sequence_source["frame_count"], frame_count)
                    until(lambda: client.call("viewer.get", pane=1).get("id") == sequence_source["id"])
                    check(label + "assigned_to_B", client.call("viewer.get", pane=1)["id"], sequence_source["id"])
                    check(label + "first_frame", cmds.currentTime(query=True), 1.0)
                    check(label + "timecode", client.call("timecode.get", id=sequence_source["id"]), {"offset": 1, "enabled": True})
                    report["sequences"].append({"directory": str(sequence_dir), "frames": frame_count,
                                                "sourceId": sequence_source["id"], "firstPath": capture.paths[0], "lastPath": capture.paths[-1]})

                import math
                import struct
                import wave
                from openframe_maya.capture import CaptureSettings
                sound_path = report_path.parent / "synthetic-maya-440hz.wav"
                with wave.open(str(sound_path), "wb") as audio_file:
                    audio_file.setnchannels(1)
                    audio_file.setsampwidth(2)
                    audio_file.setframerate(48000)
                    audio_file.writeframes(struct.pack("<96000h", *(int(0.2 * 32767 * math.sin(2 * math.pi * 440 * index / 48000)) for index in range(96000))))
                sound_node = cmds.sound(file=str(sound_path), offset=1)
                controls = cmds.lsUI(type="timeControl") or []
                if not controls:
                    raise RuntimeError("No public timeControl to attach synthetic sound")
                cmds.timeControl(controls[0], edit=True, sound=sound_node, displaySound=True)
                cmds.playbackOptions(minTime=1, maxTime=24)
                report["soundCaptures"] = []
                for include_sound in (True, False):
                    cmds.currentTime(17)
                    callbacks = []
                    sound_movie = report_path.parent / ("maya-sound-{}-{}.avi".format(int(include_sound), time.time_ns()))
                    capture = adapter.capture(str(sound_movie), viewer="B",
                        settings=CaptureSettings(format_name="avi", width=128, height=72, include_sound=include_sound),
                        callback=lambda result, error: callbacks.append((result, error)))
                    until(lambda: bool(callbacks), seconds=120)
                    if callbacks[0][1]:
                        raise RuntimeError(str(callbacks[0][1]))
                    label = "movie_sound_{}_".format(int(include_sound))
                    check(label + "snapshot", capture.sound.node if capture.sound else None, sound_node if include_sound else None)
                    check(label + "first_frame", cmds.currentTime(query=True), 1.0)
                    check(label + "frames", callbacks[0][0]["frame_count"], 24)
                    report["soundCaptures"].append({"path": str(sound_movie), "includeSound": include_sound,
                        "soundNode": capture.sound.node if capture.sound else None,
                        "muted": capture.sound.muted if capture.sound else None})
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        report["traceback"] = traceback.format_exc()
    finally:
        if adapter:
            adapter.stop()
        if client:
            client.close()
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(report_path)}))
    return report["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--media", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    import maya.standalone
    maya.standalone.initialize(name="python")
    try:
        success = run(args.endpoint, args.media, args.report)
    finally:
        maya.standalone.uninitialize()
    raise SystemExit(0 if success else 1)

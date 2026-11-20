"""Exercise the independent Python facade against an owned OpenFrame process.

Run only with the endpoint of a disposable synthetic test instance. This test
replaces that instance's project. No existing media/scene files are read.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.environ.get("OPENFRAME_TEST_MAYA_PATH", str(Path(__file__).resolve().parents[2] / "maya")))
from openframe_maya.compatibility import KeyframePro2ClientApi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--owned-instance", required=True, help="Expected instance ID of the disposable process")
    args = parser.parse_args()
    api = KeyframePro2ClientApi(endpoint_path=args.endpoint, timeout=2)
    checks = []

    def check(condition, name):
        if not condition:
            raise AssertionError("{}: {}".format(name, api.last_error))
        checks.append(name)

    check(api.connect(), "connect")
    status = api.client.call("status")
    if status["instance"] != args.owned_instance:
        api.disconnect()
        raise RuntimeError("Endpoint is not the explicitly owned test instance")
    try:
        with tempfile.TemporaryDirectory(prefix="openframe-contract-") as directory:
            wave_path = os.path.join(directory, "synthetic.wav")
            with wave.open(wave_path, "wb") as output:
                output.setnchannels(2)
                output.setsampwidth(2)
                output.setframerate(48000)
                frames = bytearray()
                for sample in range(48000 * 2):
                    value = 12000 if sample % 12000 < 32 else 0
                    frames.extend(struct.pack("<hh", value, value))
                output.writeframes(frames)
            check(api.initialize(), "initialize")
            check(api.echo("OpenFrame independent contract") == "OpenFrame independent contract", "echo")
            check(api.activate_window(), "activate window")
            check(api.new_project(empty=True), "new empty project")
            check(api.get_timelines() == [], "empty project has no timelines")
            folder = api.add_folder("Synthetic Folder")
            check(folder and folder["type"] == "folder", "add folder")
            timeline = api.add_timeline("Synthetic Timeline", folder["id"])
            check(timeline and timeline["parent_id"] == folder["id"], "add timeline in folder")
            tid = timeline["id"]
            check(api.set_default_timeline(tid)["id"] == tid, "assign default timeline")
            api.client.call("timeline.activate", id=tid)
            source = api.import_file(wave_path, name="Synthetic Impulses", range_start=2, range_end=25, parent_id=folder["id"])
            check(source and source["name"] == "Synthetic Impulses", "import source and wait for indexing")
            check(source["parent_id"] == folder["id"], "import parent")
            elements = api.get_timeline_elements(tid)
            check(len(elements) == 1 and elements[0]["range_start"] == 2 and elements[0]["range_end"] == 25, "source trim propagated to default clip")
            check(api.set_active_in_viewer(tid, 0)["id"] == tid, "assign viewer A")
            check(api.get_active_in_viewer(0)["id"] == tid, "read viewer A")
            check(api.set_viewer_layout("horizontal"), "horizontal viewer")
            check(api.set_viewer_layout("vertical"), "vertical viewer")
            check(api.set_viewer_layout("single"), "single viewer")
            check(api.set_frame(5, audio=False) == 5, "one-based frame conversion")
            check(api.get_frame() == 5, "get frame")
            check(api.set_range(2, 20) == (2, 20), "range conversion")
            check(api.set_tc_frame_offset_override(101, True) == (101, True), "timecode override")
            check(api.get_frame() == 105, "timecode frame conversion")
            check(api.set_tc_frame_offset_override(0, False) == (0, False), "disable timecode override")
            check(api.create_bookmarks(tid, [2, 4, 4]) == 2, "bookmark deduplication")
            check(api.add_notes(tid, {2: "**Synthetic** note", 4: "Синтетическая заметка"}) == 2, "add Unicode/Markdown notes")
            check(api.add_notes(tid, {2: "ignored"}) == 0, "notes preserve existing content")
            bookmarks = api.get_bookmarks(tid, plain_text=True)
            check(bookmarks[0]["bookmarks"] == [2, 4], "bookmark display frames")
            check(bookmarks[0]["notes"][0]["note"] == "Synthetic note", "plain text strips Markdown")
            check(api.get_bookmarks(tid, plain_text=False)[0]["notes"][0]["note"] == "**Synthetic** note", "Markdown preserved")
            check(api.set_playing(False), "pause")
            check(type(api.is_autoplay()) is bool, "autoplay reports model")
            project_path = os.path.join(directory, "synthetic.oframe")
            check(api.save_project(project_path), "save own project")
            check(api.get_project_path() == project_path.replace("\\", "/"), "project path")
            check(api.new_project(), "new project")
            check(api.open_project(project_path), "open own project")
            check(len(api.get_sources()) == 1, "persist source")
            check(api.queue_export(directory, "audio-export", "wav", 64, 48, ignore_range=True), "queue audio export")
            success, error = api.query_export(timeout=30)
            check(success, "complete audio export: " + str(error))
            with wave.open(os.path.join(directory, "audio-export.wav"), "rb") as exported:
                check(exported.getnframes() == 48000, "exported trim duration")
            check(api.queue_export(directory, "bookmark", "bmp", 64, 48, exclude_audio=True,
                                   viewer_capture=True, frames_to_capture="bookmarks", frame_numbering="one_based",
                                   annotation_bg_color=[12, 34, 56, 255]), "queue bookmarks with background")
            success, error = api.query_export(timeout=30)
            check(success, "complete bookmarks export: " + str(error))
            check(os.path.isfile(os.path.join(directory, "bookmark_000001.bmp")) and
                  os.path.isfile(os.path.join(directory, "bookmark_000002.bmp")), "publish numbered files into existing directory")
            check(api.queue_export(directory, "match", "bmp", 64, 48, exclude_audio=True,
                                   viewer_capture=True, frames_to_capture="bookmarks", frame_numbering="match_frame"), "queue matching display numbers")
            success, error = api.query_export(timeout=30)
            check(success, "complete display-number export: " + str(error))
            check(os.path.isfile(os.path.join(directory, "match_000002.bmp")) and
                  os.path.isfile(os.path.join(directory, "match_000004.bmp")), "match_frame follows displayed bookmark frames")
            check(api.set_default_timeline("") == {}, "unassign default timeline")
            source2 = api.import_file(wave_path, name="Unassigned")
            check(source2 and len(api.get_timeline_elements(tid)) == 1, "import without default does not append")
            check(api.insert_element_in_timeline(source2["id"], tid), "insert clip")
            check(len(api.get_timeline_elements(tid)) == 2, "timeline insertion visible")
            check(api.remove_element_from_timeline(tid, 1), "remove clip")
            check(api.remove(source2["id"]), "remove unused source")
            check(api.clear_bookmarks(tid), "clear bookmarks and notes")
            check(api.get_bookmarks(tid, include_empty_timelines=False) == [], "cleared owner omitted")
            check(api.get_config()["authenticated"] is True, "get config")
            check(api.is_connected(), "connection remains usable")
            api.new_project(empty=True)
    finally:
        api.disconnect()
    print(json.dumps({"success": True, "checks": len(checks), "passed": checks}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

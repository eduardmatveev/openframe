"""Independently written method-name adapter for the public Python client API.

Contract reference (retrieved 2026-11-20):
https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-python-client-api-reference
No original client source or undocumented wire protocol is used. This class
connects ONLY to OpenFrame, through its authenticated endpoint file.
"""
import os
import time
from .client import Client, OpenFrameError, discover_endpoints, _read_endpoint, viewer_layout_params
from .paths import local_path


class KeyframePro2ClientApi:
    def __init__(self, timeout=2, *, endpoint_path=None):
        self.timeout = timeout
        self.endpoint_path = endpoint_path
        self.client = Client(endpoint_path, timeout)
        self.last_error = ""

    def _call(self, method, fallback=None, **params):
        try:
            value = self.client.call(method, **params)
            self.last_error = ""
            return value
        except (OpenFrameError, OSError, ValueError) as error:
            self.last_error = str(error)
            return fallback

    def connect(self, port=-1, display_errors=True):
        del display_errors  # Errors are exposed through last_error; no secret logging.
        try:
            if port != -1:
                if type(port) is not int or not 1 <= port <= 65535:
                    raise ValueError("Invalid local port")
                candidates = [self.endpoint_path] if self.endpoint_path else discover_endpoints()
                path = next((path for path in candidates if _read_endpoint(path)["port"] == port), None)
                if not path:
                    raise OpenFrameError("No authenticated endpoint for this port")
                self.client = Client(path, self.timeout)
            self.client.connect()
            self.last_error = ""
            return True
        except (OpenFrameError, OSError, ValueError) as error:
            self.last_error = str(error)
            return False

    def disconnect(self):
        was_connected = self.client.connected
        self.client.close()
        return was_connected

    def is_connected(self):
        return self.client.connected

    def initialize(self):
        return self._call("status") is not None

    def echo(self, text):
        return self._call("echo", text=text)

    def activate_window(self):
        return self._call("window.activate", False)

    def get_config(self):
        status = self._call("status")
        if status is None:
            return None
        return {"application": "OpenFrame", "api_version": 1, "autoplay": status["autoplay"],
                "authenticated": True, "frame_numbering": "one_based", "instance": status["instance"]}

    def new_project(self, empty=False):
        return self._call("project.new", False, empty=empty)

    def open_project(self, file_path):
        return self._call("project.open", False, path=os.fspath(file_path))

    def import_project(self, file_path):
        return self._call("project.import", False, path=os.fspath(file_path))

    def save_project(self, file_path):
        return self._call("project.save", False, path=os.fspath(file_path))

    def get_project_path(self):
        status = self._call("status")
        return status["projectPath"] if status else None

    def import_file(self, file_path, name="", range_start=-1, range_end=-1, parent_id=""):
        try:
            path = local_path(file_path)
        except (ValueError, OSError) as error:
            self.last_error = str(error)
            return None
        before = self.get_sources()
        if before is None:
            return None
        previous = {item["id"] for item in before}
        if self._call("media.import", paths=[path]) is None:
            return None
        deadline = time.monotonic() + max(self.timeout, 120)
        while time.monotonic() < deadline:
            sources = self.get_sources()
            if sources is None:
                return None
            source = next((s for s in sources if s["id"] not in previous and
                           os.path.normcase(os.path.abspath(s["path"])) == os.path.normcase(path)), None)
            if source:
                update = {"id": source["id"]}
                if name:
                    update["name"] = name
                if parent_id:
                    update["parentId"] = parent_id
                if range_start != -1:
                    update["inFrame"] = range_start
                if range_end != -1:
                    update["outFrame"] = range_end
                updated = self._call("object.update", **update) if len(update) > 1 else source
                if updated and (range_start != -1 or range_end != -1):
                    default = self.get_default_timeline()
                    if default:
                        for index, clip in enumerate(default.get("clips", [])):
                            if clip["sourceId"] == source["id"]:
                                if self._call("timeline.updateClip", timelineId=default["id"], index=index,
                                              inFrame=updated["range_start"], outFrame=updated["range_end"]) is None:
                                    return None
                return updated
            status = self._call("status")
            if not status or not status.get("busy"):
                self.last_error = (status or {}).get("lastError") or "Media import did not create a source"
                return None
            time.sleep(0.05)
        self.last_error = "Media import timed out"
        return None

    def add_folder(self, name="", parent_id=""):
        return self._call("folder.add", name=name, parentId=parent_id)

    def add_timeline(self, name="", parent_id=""):
        return self._call("timeline.add", name=name, parentId=parent_id)

    def remove(self, id, force=False):
        return self._call("object.remove", False, id=id, force=force)

    def update(self, obj):
        if not isinstance(obj, dict) or "id" not in obj:
            self.last_error = "Object with id is required"
            return None
        params = {key: obj[key] for key in ("id", "name") if key in obj}
        for old, new in (("parent_id", "parentId"), ("range_start", "inFrame"), ("range_end", "outFrame")):
            if old in obj:
                params[new] = obj[old]
        return self._call("object.update", **params)

    def insert_element_in_timeline(self, source_id, timeline_id, index=-1):
        return self._call("timeline.insert", sourceId=source_id, timelineId=timeline_id, index=index) is not None

    def remove_element_from_timeline(self, timeline_id, index):
        return self._call("timeline.removeClip", False, timelineId=timeline_id, index=index)

    def get_timeline_elements(self, timeline_id):
        timeline = self._call("object.get", id=timeline_id)
        if timeline is None or timeline.get("type") != "timeline":
            return None
        sources = self.get_sources()
        if sources is None:
            return None
        by_id = {item["id"]: item for item in sources}
        result = []
        for clip in timeline["clips"]:
            source = dict(by_id[clip["sourceId"]])
            source.update(range_start=clip["inFrame"], range_end=clip["outFrame"], clip_id=clip["id"])
            result.append(source)
        return result

    def get_folders(self):
        return self._call("objects.list", type="folder")

    def get_timelines(self):
        return self._call("objects.list", type="timeline")

    def get_sources(self):
        return self._call("objects.list", type="source")

    def _display_offset(self):
        result = self._call("timecode.get")
        return result["offset"] if result and result["enabled"] else 1

    def get_frame(self):
        status = self._call("status")
        return status["frame"] + self._display_offset() if status else None

    def set_frame(self, frame, audio=False, from_range_start=False, ignore_loop_state=False):
        raw = frame - (1 if from_range_start else self._display_offset())
        value = self._call("seek", frame=raw, audio=audio, fromRangeStart=from_range_start, ignoreLoopState=ignore_loop_state)
        return value + self._display_offset() if value is not None else None

    def get_range(self):
        status = self._call("status")
        if status is None:
            return None
        offset = self._display_offset()
        last = status["rangeOut"] if status["rangeOut"] >= 0 else max(0, status["durationFrames"] - 1)
        return (status["rangeIn"] + offset, last + offset)

    def set_range(self, start_frame, end_frame):
        offset = self._display_offset()
        result = self._call("range.set", first=start_frame - offset, last=end_frame - offset)
        return (result[0] + offset, result[1] + offset) if result is not None else None

    def get_tc_frame_offset_override(self):
        result = self._call("timecode.get")
        return (result["offset"], result["enabled"]) if result is not None else None

    def set_tc_frame_offset_override(self, offset=0, enabled=True):
        result = self._call("timecode.set", offset=offset, enabled=enabled)
        return (result["offset"], result["enabled"]) if result is not None else None

    def get_bookmarks(self, id="", include_empty_timelines=True, tc_frame_numbering=True, plain_text=True):
        objects = (self.get_timelines() or []) + (self.get_sources() or [])
        bookmarks = self._call("bookmarks.list", id=id)
        notes = self._call("notes.list", ownerId=id, plainText=plain_text)
        annotations = self._call("annotations.list", ownerId=id)
        if bookmarks is None or notes is None or annotations is None:
            return None
        result = []
        for item in objects:
            if id and item["id"] != id:
                continue
            offset = item.get("timecodeOffset", 0) if item.get("timecodeOffsetEnabled") else 1
            if not tc_frame_numbering:
                offset = 0
            frames = [b["frame"] + offset for b in bookmarks if b["ownerId"] == item["id"]]
            note_frames = [{"frame": n["frame"] + offset, "note": n["text"]} for n in notes if n["ownerId"] == item["id"]]
            annotation_frames = [a["frame"] + offset for a in annotations if a["ownerId"] == item["id"]]
            if frames or note_frames or annotation_frames or include_empty_timelines:
                result.append({"id": item["id"], "name": item["name"], "bookmarks": frames, "annotations": annotation_frames, "notes": note_frames})
        return result

    def _owner_offset(self, id):
        item = self._call("object.get", id=id)
        return item.get("timecodeOffset", 0) if item and item.get("timecodeOffsetEnabled") else 1

    def create_bookmarks(self, id, frames, tc_frame_numbering=True):
        offset = self._owner_offset(id) if tc_frame_numbering else 0
        return self._call("bookmarks.add", -1, ownerId=id, frames=[frame - offset for frame in frames])

    def clear_bookmarks(self, id, empty_bookmarks=True, annotations=True, notes=True):
        return self._call("bookmarks.clear", False, ownerId=id, bookmarks=empty_bookmarks, annotations=annotations, notes=notes)

    def add_notes(self, id, notes, tc_frame_numbering=True, overwrite=False):
        offset = self._owner_offset(id) if tc_frame_numbering else 0
        return self._call("notes.add", -1, ownerId=id, notes=[{"frame": int(frame) - offset, "text": text} for frame, text in notes.items()], overwrite=overwrite)

    def set_playing(self, playing, play_forwards=True):
        return self._call("play", False, playing=playing, rate=1 if play_forwards else -1)

    def is_autoplay(self):
        status = self._call("status")
        return status["autoplay"] if status else None

    def get_default_timeline(self):
        status = self._call("status")
        return self._call("object.get", id=status["defaultTimelineId"]) if status else None

    def set_default_timeline(self, id):
        return self._call("project.defaultTimeline", id=id)

    def get_active_in_viewer(self, index):
        result = self._call("viewer.get", pane=index)
        return result

    def set_active_in_viewer(self, id, index):
        return self._call("viewer.assign", id=id, pane=index)

    def set_viewer_layout(self, layout="single"):
        try:
            params = viewer_layout_params(layout)
        except ValueError as error:
            self.last_error = str(error)
            return False
        return self._call("viewer.set", False, **params)

    def queue_export(self, output_dir_path, base_file_name, ext, width, height, ignore_range=False,
                     exclude_audio=False, viewer_capture=False, frames_to_capture="all",
                     exclude_annotations=False, frame_numbering="zero_based", annotation_bg_color=None):
        if frames_to_capture not in ("all", "stepped", "bookmarks") or frame_numbering not in ("zero_based", "one_based", "match_frame"):
            self.last_error = "Unknown frame selection or numbering mode"
            return False
        if annotation_bg_color is not None and annotation_bg_color != []:
            if not isinstance(annotation_bg_color, (tuple, list)) or len(annotation_bg_color) != 4 or any(type(value) is not int or not 0 <= value <= 255 for value in annotation_bg_color):
                self.last_error = "annotation_bg_color must contain four RGBA integers"
                return False
        if not base_file_name or os.path.basename(base_file_name) != base_file_name or "/" in base_file_name or "\\" in base_file_name:
            self.last_error = "base_file_name must contain a filename only"
            return False
        ext = ext.lower().lstrip(".")
        if ext not in ("mp4", "avi", "mkv", "mov", "wav", "flac", "png", "jpg", "bmp", "tiff"):
            self.last_error = "Unsupported export extension"
            return False
        try:
            directory = local_path(output_dir_path, directory=True)
        except (ValueError, OSError) as error:
            self.last_error = str(error)
            return False
        status = self._call("status")
        if status is None:
            return False
        kind = "images" if ext in ("png", "jpg", "bmp", "tiff") else "audio" if ext in ("wav", "flac") else "video"
        if frames_to_capture == "bookmarks" and kind != "images":
            self.last_error = "Bookmarks-only export requires an image sequence"
            return False
        params = {"path": os.path.join(directory, base_file_name + "." + ext), "kind": kind, "width": width, "height": height,
                  "viewerComposition": viewer_capture, "includeAnnotations": not exclude_annotations,
                  "includeAudio": not exclude_audio, "first": 0 if ignore_range else status["rangeIn"],
                  "last": status["durationFrames"] - 1 if ignore_range else status["rangeOut"]}
        params["bookmarksOnly"] = frames_to_capture == "bookmarks"
        params["frameStep"] = status["playbackFrameStep"] if frames_to_capture == "stepped" else 1
        if annotation_bg_color:
            params["annotationBackground"] = list(annotation_bg_color)
        if kind == "images":
            params["imageFormat"] = ext
            params["path"] = directory
            params["publishSequenceIntoExistingDirectory"] = True
            params["sequenceBaseName"] = base_file_name
            params["frameNumbering"] = frame_numbering
        if kind == "audio":
            params["audioCodec"] = "pcm_s16le" if ext == "wav" else "flac"
        return self._call("export.start", **params) is not None

    def query_export(self, timeout=300, show_poll_timeout_errors=False):
        del show_poll_timeout_errors
        deadline = time.monotonic() + max(0, timeout)
        while True:
            result = self._call("export.status")
            if result is None:
                return (False, self.last_error)
            if result.get("state") in ("complete", "failed"):
                return (bool(result.get("success")), result.get("message", ""))
            if not result:
                return (False, "No export has been queued")
            if time.monotonic() >= deadline:
                return (False, "Export poll timed out; job continues")
            time.sleep(0.1)

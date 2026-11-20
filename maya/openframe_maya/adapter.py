"""Maya adapter with main-thread host calls and a reconnecting socket worker."""
import math
import os
import queue
import threading
import time
import uuid
from dataclasses import dataclass
from fractions import Fraction
from .client import Client, OpenFrameError, endpoint_directory, validate_timeout
from .paths import local_path
from .capture import CaptureResult, CaptureSettings, TimelineSound, MAX_SEQUENCE_FRAMES

_DIRECTIONS = {"off", "maya_to_openframe", "openframe_to_maya", "bidirectional"}


@dataclass(frozen=True)
class SyncMapping:
    offset: float = 0.0
    multiplier: float = 1.0
    from_range_start: bool = False

    def __post_init__(self):
        if not math.isfinite(self.offset) or abs(self.offset) > 1e9:
            raise ValueError("Offset must be finite and within +/-1e9")
        if not math.isfinite(self.multiplier) or not 0 < abs(self.multiplier) <= 1000:
            raise ValueError("Multiplier must be nonzero, finite and within +/-1000")
        if type(self.from_range_start) is not bool:
            raise ValueError("from_range_start must be boolean")

    def to_review(self, maya_frame, review_range_start=0):
        anchor = 1.0 if self.from_range_start else 0.0
        base = review_range_start if self.from_range_start else 0.0
        result = (maya_frame - anchor) * self.multiplier + base + self.offset
        # Explicit nearest frame, ties away from zero (not Python banker rounding).
        return int(math.floor(result + 0.5) if result >= 0 else math.ceil(result - 0.5))

    def to_maya(self, review_frame, review_range_start=0):
        anchor = 1.0 if self.from_range_start else 0.0
        base = review_range_start if self.from_range_start else 0.0
        return (review_frame - base - self.offset) / self.multiplier + anchor


class MayaHost:
    """Small public-Maya-API boundary, replaced by FakeMaya in CI."""
    def __init__(self):
        import maya.cmds as cmds
        import maya.utils as utils
        from maya.api import OpenMaya
        self.cmds, self.utils, self.api = cmds, utils, OpenMaya

    @staticmethod
    def assert_main_thread():
        if threading.current_thread() is not threading.main_thread():
            raise RuntimeError("Maya operations must run on the main thread")

    def snapshot(self):
        self.assert_main_thread()
        return {"frame": float(self.cmds.currentTime(query=True)),
                "range_start": float(self.cmds.playbackOptions(query=True, minTime=True)),
                "range_end": float(self.cmds.playbackOptions(query=True, maxTime=True))}

    def schedule(self, callback):
        # Only callables are supplied. No string-based Python/MEL execution.
        self.utils.executeDeferred(callback)

    def set_time(self, value):
        self.assert_main_thread()
        self.cmds.currentTime(float(value), edit=True)

    def set_range(self, first, last):
        self.assert_main_thread()
        self.cmds.playbackOptions(minTime=float(first), maxTime=float(last))

    def add_time_callback(self, callback):
        self.assert_main_thread()
        return self.api.MEventMessage.addEventCallback("timeChanged", lambda *_: callback())

    def remove_callback(self, callback_id):
        self.assert_main_thread()
        self.api.MMessage.removeCallback(callback_id)

    def playblast_range(self):
        """Read capture selection without changing the scene's playback range."""
        self.assert_main_thread()
        controls = self.cmds.lsUI(type="timeControl", long=True) or []
        if len(controls) > 1:
            raise OpenFrameError("Cannot identify a unique Maya Time Slider")
        if controls and self.cmds.timeControl(controls[0], query=True, rangeVisible=True):
            selected = self.cmds.timeControl(controls[0], query=True, rangeArray=True)
            if not isinstance(selected, (tuple, list)) or len(selected) != 2:
                raise OpenFrameError("Maya returned an invalid highlighted range")
            first, end_exclusive = map(float, selected)
            last = end_exclusive - 1.0
        else:
            first = float(self.cmds.playbackOptions(query=True, minTime=True))
            last = float(self.cmds.playbackOptions(query=True, maxTime=True))
        if not math.isfinite(first) or not math.isfinite(last) or last < first:
            raise OpenFrameError("Maya playblast range is invalid")
        return first, last

    def frame_rate(self):
        self.assert_main_thread()
        seconds = self.api.MTime(1.0, self.api.MTime.uiUnit()).asUnits(self.api.MTime.kSeconds)
        if not math.isfinite(seconds) or seconds <= 0:
            raise OpenFrameError("Maya returned an invalid time unit")
        rate = Fraction(1.0 / seconds).limit_denominator(1000000)
        if not 0.001 <= rate <= 1000 or rate.numerator > 1000000:
            raise OpenFrameError("Maya frame rate is outside the capture limits")
        return rate.numerator, rate.denominator

    def sound_snapshot(self):
        self.assert_main_thread()
        controls = self.cmds.lsUI(type="timeControl", long=True) or []
        if len(controls) > 1:
            raise OpenFrameError("Cannot identify a unique Maya Time Slider")
        if not controls:
            return None
        node = self.cmds.timeControl(controls[0], query=True, sound=True)
        if not node:
            return None
        # Typed queries return state only; no scene node or preference is changed.
        path = local_path(self.cmds.sound(node, query=True, file=True))
        timing = {key: float(self.cmds.sound(node, query=True, **{key: True}))
                  for key in ("offset", "sourceStart", "sourceEnd", "endTime")}
        return TimelineSound(node, path, timing["offset"], timing["sourceStart"],
                             timing["sourceEnd"], timing["endTime"],
                             bool(self.cmds.sound(node, query=True, mute=True)),
                             bool(self.cmds.timeControl(controls[0], query=True, displaySound=True)))

    def capture(self, path, first, last, settings, sound):
        self.assert_main_thread()
        options = {"filename": path, "startTime": first, "endTime": last,
                   "widthHeight": (settings.width, settings.height), "percent": settings.percent,
                   "quality": settings.quality,
                   "viewer": False, "forceOverwrite": False, "clearCache": False,
                   "showOrnaments": settings.show_ornaments, "offScreen": settings.off_screen,
                   "throwOnError": True, "format": settings.format_name,
                   "useTraxSounds": False, "combineSound": False}
        # Maya 2024 treats sound="" as a sound-loading error, including when no
        # timeline sound exists. Omitting this optional flag captures silently.
        if sound is not None and not sound.muted:
            options["sound"] = sound.node
        if settings.encoding is not None:
            options["compression"] = settings.encoding
        if settings.format_name == "image":
            options.update(framePadding=settings.padding, indexFromZero=True, rawFrameNumbers=False)
        return self.cmds.playblast(**options)


class Adapter:
    MAX_SYNC_HZ = 60

    def __init__(self, endpoint_path=None, *, host=None, client_factory=None, poll_interval=1 / 60, timeout=0.75):
        if not 1 / 60 <= poll_interval <= 5:
            raise ValueError("poll_interval must be between 1/60 and 5 seconds")
        timeout = validate_timeout(timeout)
        self.host = host or MayaHost()
        self.session_id = str(uuid.uuid4())
        self.name = "Maya {}".format(os.getpid())
        self._client_factory = client_factory or (lambda: Client(endpoint_path, cancel_event=self._stop))
        self._client = None
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread = None
        self._callback = None
        self._interval = poll_interval
        self._snapshot = {"frame": 1.0, "range_start": 1.0, "range_end": 120.0}
        self._sequence = 0
        self._sent_sequence = -1
        self._generation = 0
        self._applying = False
        self._suspended = 0
        self._pending_remote = None
        self._remote_scheduled = False
        self._last_remote_revision = None
        self._config = {"direction": "off", "offset": 0.0, "multiplier": 1.0,
                        "fromRangeStart": False, "timeoutSeconds": timeout}
        self._config_revision = -1
        self._config_pending = False
        self._commands = queue.Queue(maxsize=64)
        self._state = {"connected": False, "error": "", "status": {}, "running": False}
        self._owned_playblasts = {}
        self._owned_capture_groups = {}
        self._owned_directories = {}

    @property
    def state(self):
        with self._lock:
            return dict(self._state, config=dict(self._config))

    def start(self, direction="off", mapping=None):
        self.host.assert_main_thread()
        if self._thread and self._thread.is_alive():
            if self._stop.is_set():
                raise OpenFrameError("Previous adapter worker is still stopping")
            self.configure(direction=direction, mapping=mapping)
            return self
        self.configure(direction=direction, mapping=mapping)
        self._snapshot = self.host.snapshot()
        self._stop.clear()
        self._generation += 1
        self._callback = self.host.add_time_callback(self._time_changed)
        with self._lock:
            self._state.update(running=True, error="")
        self._thread = threading.Thread(target=self._run, name="OpenFrame-Maya-" + self.session_id[:8], daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self.host.assert_main_thread()
        with self._lock:
            self._stop.set()
            self._generation += 1
            client = self._client
        self._wake.set()
        if client is not None:
            client.close()
        if self._callback is not None:
            self.host.remove_callback(self._callback)
            self._callback = None
        # The worker never blocks waiting for Maya; joining cannot depend on idle.
        thread = self._thread
        if thread and thread is not threading.current_thread():
            thread.join(timeout=2.0)
        with self._lock:
            self._pending_remote = None
            self._remote_scheduled = False
            self._state.update(running=False, connected=False)

    def configure(self, *, direction=None, mapping=None, timeout=None):
        if direction is not None and direction not in _DIRECTIONS:
            raise ValueError("Unknown synchronization direction")
        if mapping is not None and not isinstance(mapping, SyncMapping):
            raise TypeError("mapping must be SyncMapping")
        if timeout is not None:
            timeout = validate_timeout(timeout)
        client = None
        with self._lock:
            if direction is not None:
                self._config["direction"] = direction
            if mapping is not None:
                self._config.update(offset=mapping.offset, multiplier=mapping.multiplier,
                                    fromRangeStart=mapping.from_range_start)
            if timeout is not None and timeout != self._config["timeoutSeconds"]:
                self._config["timeoutSeconds"] = timeout
                self._generation += 1
                self._pending_remote = None
                self._remote_scheduled = False
                client = self._client
            self._config_pending = True
            self._sequence += 1
        if client is not None:
            client.close()
        self._wake.set()

    def _time_changed(self):
        self.host.assert_main_thread()
        snapshot = self.host.snapshot()
        with self._lock:
            self._snapshot = snapshot
            if not self._applying and not self._suspended:
                self._sequence += 1
        self._wake.set()

    def _queue_remote(self, frame):
        with self._lock:
            self._pending_remote = frame
            if self._remote_scheduled:
                return
            self._remote_scheduled = True
            generation = self._generation

        def apply():
            self.host.assert_main_thread()
            with self._lock:
                if generation != self._generation or self._stop.is_set():
                    return
                frame = self._pending_remote
                self._pending_remote = None
                self._remote_scheduled = False
                if self._suspended or frame is None:
                    return
                if self._config["direction"] not in ("openframe_to_maya", "bidirectional"):
                    return
                self._applying = True
            try:
                self.host.set_time(frame)
                with self._lock:
                    self._snapshot = self.host.snapshot()
            except Exception as error:
                with self._lock:
                    self._state["error"] = "Maya time update failed: {}".format(error)
            finally:
                with self._lock:
                    self._applying = False
        self.host.schedule(apply)

    def request(self, method, callback=None, **params):
        """Queue an API operation. Optional result/error callback runs on Maya main thread."""
        if self._stop.is_set() or not self._thread or not self._thread.is_alive():
            raise OpenFrameError("Adapter is not running")
        self._commands.put_nowait((method, params, callback))
        self._wake.set()

    def _complete(self, callback, result=None, error=None, generation=None):
        if callback:
            generation = self._generation if generation is None else generation
            def deliver():
                if generation == self._generation and not self._stop.is_set():
                    callback(result, error)
            self.host.schedule(deliver)

    def _run(self):
        backoff = 0.1
        next_cycle = 0.0
        while not self._stop.is_set():
            try:
                if self._client is None:
                    with self._lock:
                        if self._stop.is_set():
                            break
                        self._client = self._client_factory()
                        config = dict(self._config)
                        snapshot = dict(self._snapshot)
                        generation = self._generation
                        self._client.timeout = config["timeoutSeconds"]
                    self._client.connect()
                    status = self._client.call("maya.register", sessionId=self.session_id, name=self.name,
                                               pid=os.getpid(), mayaRangeStart=snapshot["range_start"], **config)
                    self._last_remote_revision = None
                    self._sent_sequence = -1
                    with self._lock:
                        if generation != self._generation or self._stop.is_set():
                            raise OpenFrameError("Connection was superseded")
                        self._state.update(connected=True, error="", status=status)
                    backoff = 0.1
                # Maya callbacks can arrive faster than display refresh. Keep a
                # strict per-connection budget while coalescing the latest frame.
                if self._stop.wait(max(0.0, next_cycle - time.monotonic())):
                    break
                self._cycle()
                next_cycle = time.monotonic() + 1 / self.MAX_SYNC_HZ
                self._wake.wait(self._interval)
                self._wake.clear()
            except (OSError, ValueError, OpenFrameError) as error:
                with self._lock:
                    self._generation += 1
                    self._pending_remote = None
                    self._remote_scheduled = False
                    self._config_revision = -1
                    self._state.update(connected=False, error=str(error))
                if self._client:
                    self._client.close()
                    self._client = None
                if self._stop.wait(backoff):
                    break
                backoff = min(5.0, backoff * 2)
        if self._client:
            # TCP disconnect marks this session offline; never wait for a final
            # unregister response after stop has cancelled the transport.
            self._client.close()
            self._client = None
        while True:
            try:
                _, _, callback = self._commands.get_nowait()
            except queue.Empty:
                break
            self._complete(callback, error=OpenFrameError("Adapter stopped"))

    def _cycle(self):
        with self._lock:
            generation = self._generation
            config = dict(self._config)
            pending_config = self._config_pending
            self._config_pending = False
        if pending_config:
            try:
                self._client.call("maya.configure", sessionId=self.session_id, config=config)
            except (OpenFrameError, OSError):
                with self._lock:
                    self._config_pending = True
                raise
        # Bounded drain keeps synchronization responsive during queued UI work.
        for _ in range(8):
            try:
                method, params, callback = self._commands.get_nowait()
            except queue.Empty:
                break
            try:
                result = self._client.call(method, **params)
                self._complete(callback, result=result, generation=generation)
            except OpenFrameError as error:
                self._complete(callback, error=error, generation=generation)
                if not self._client.connected:
                    raise
        with self._lock:
            config = dict(self._config)
            snapshot, sequence = dict(self._snapshot), self._sequence
            suspended = self._suspended
        send_frame = (not suspended and config["direction"] in ("maya_to_openframe", "bidirectional")
                      and sequence != self._sent_sequence)
        params = {"sessionId": self.session_id}
        if send_frame:
            params["mayaFrame"] = snapshot["frame"]
        # The server applies its current direction/mapping/range atomically;
        # no stale client-side offset can move the frame after a UI config edit.
        status = self._client.call("maya.sync", **params)
        server_config = status.get("maya", {})
        with self._lock:
            if generation != self._generation or self._stop.is_set():
                raise OpenFrameError("Synchronization was superseded")
            revision = server_config.get("configRevision", -1)
            if revision != self._config_revision and not self._config_pending:
                candidate = {key: server_config.get(key, self._config[key]) for key in self._config}
                SyncMapping(candidate["offset"], candidate["multiplier"], candidate["fromRangeStart"])
                candidate["timeoutSeconds"] = validate_timeout(candidate["timeoutSeconds"])
                if candidate["direction"] not in _DIRECTIONS:
                    raise OpenFrameError("Invalid server sync direction")
                if candidate != self._config:
                    self._sequence += 1
                self._config = candidate
                self._config_revision = revision
            config = dict(self._config)
            self._client.timeout = config["timeoutSeconds"]
            self._state.update(connected=True, status=status, error="")
        mapping = SyncMapping(config["offset"], config["multiplier"], config["fromRangeStart"])
        if suspended:
            return
        direction = config["direction"]
        if send_frame and status.get("syncFrameApplied"):
            self._sent_sequence = sequence
        remote_revision = status.get("frameRevision")
        if direction in ("openframe_to_maya", "bidirectional") and remote_revision != self._last_remote_revision:
            if status.get("frameOrigin") != self.session_id:
                frame = mapping.to_maya(status["frame"], status.get("rangeIn", 0))
                self._queue_remote(frame)
        self._last_remote_revision = remote_revision

    def send_range(self):
        """Explicitly copy Maya range to OpenFrame; never automatic scene edits."""
        self.host.assert_main_thread()
        snapshot = self.host.snapshot()
        with self._lock:
            config = dict(self._config)
            range_start = self._state["status"].get("rangeIn", 0)
        mapping = SyncMapping(config["offset"], config["multiplier"], config["fromRangeStart"])
        frames = sorted((mapping.to_review(snapshot["range_start"], range_start),
                         mapping.to_review(snapshot["range_end"], range_start)))
        self.request("range.set", first=max(0, frames[0]), last=max(0, frames[1]))

    def receive_range(self):
        """Explicitly copy the latest OpenFrame range to Maya."""
        self.host.assert_main_thread()
        with self._lock:
            status = dict(self._state["status"])
            config = dict(self._config)
        if not self._state["connected"] or status.get("rangeOut", -1) < 0:
            raise OpenFrameError("OpenFrame has no finite connected range")
        mapping = SyncMapping(config["offset"], config["multiplier"], config["fromRangeStart"])
        first = mapping.to_maya(status["rangeIn"], status["rangeIn"])
        last = mapping.to_maya(status["rangeOut"], status["rangeIn"])
        self.host.set_range(min(first, last), max(first, last))

    def playblast_directory(self):
        """Create/return only this adapter session's local capture directory."""
        self.host.assert_main_thread()
        api_dir = local_path(endpoint_directory(), directory=True)
        root = os.path.join(os.path.dirname(api_dir), "playblasts")
        local_path(root, output=True, directory=True)
        try:
            os.mkdir(root)
        except FileExistsError:
            pass
        local_path(root, directory=True)
        session_dir = os.path.join(root, self.session_id)
        local_path(session_dir, output=True, directory=True)
        try:
            os.mkdir(session_dir)
        except FileExistsError:
            pass
        return local_path(session_dir, directory=True)

    def _query_capture_references(self, callback):
        """Compact server-side membership queries, including non-first frames."""
        checks = [("directory", path) for path in self._owned_directories]
        checks.extend(("path", path) for path in self._owned_playblasts
                      if os.path.dirname(path) not in self._owned_directories)
        protected, directories = set(), set()
        def next_check(index):
            if index == len(checks):
                callback(protected, directories, None)
                return
            kind, path = checks[index]
            def received(result, error):
                if not error and (not isinstance(result, dict) or type(result.get("referenced")) is not bool):
                    error = OpenFrameError("Invalid path-reference response; captures were preserved")
                if error:
                    callback(set(), set(), error)
                    return
                if result["referenced"]:
                    if kind == "directory":
                        directories.add(path)
                        protected.update(os.path.normcase(item) for item in self._owned_playblasts
                                         if os.path.dirname(item) == path)
                    else:
                        protected.add(os.path.normcase(path))
                next_check(index + 1)
            try:
                self.request("media.pathReferenced", callback=received, **{kind: path})
            except (OpenFrameError, queue.Full) as error:
                callback(set(), set(), error)
        next_check(0)

    def clear_playblasts(self, *, confirm, callback=None):
        """Remove unchanged, adapter-owned captures not loaded in OpenFrame.

        Only files created in the private session directory are eligible. A
        manually chosen output path, another session's files and loaded sources
        are always preserved. confirm receives the concrete eligible and
        preserved path lists on the main thread and must return exactly True.
        Completion callback receives a deletion summary.
        """
        self.host.assert_main_thread()
        if not callable(confirm):
            raise TypeError("A callable confirmation is required")
        def sources_received(protected, protected_directories, error):
            if error:
                if callback:
                    callback(None, error)
                return
            removed, preserved, eligible = [], [], []
            for path, identity in list(self._owned_playblasts.items()):
                if os.path.normcase(path) in protected:
                    preserved.append(path)
                    continue
                try:
                    checked = local_path(path)
                    info = os.stat(checked, follow_symlinks=False)
                    current = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
                    if current != identity:
                        preserved.append(path)
                        continue
                    eligible.append(path)
                except FileNotFoundError:
                    self._owned_playblasts.pop(path, None)
                except (OSError, ValueError):
                    preserved.append(path)
            directories = []
            for path, identity in list(self._owned_directories.items()):
                if path in protected_directories:
                    preserved.append(path)
                    continue
                try:
                    checked = local_path(path, directory=True)
                    info = os.stat(checked, follow_symlinks=False)
                    if (info.st_dev, info.st_ino) != identity:
                        preserved.append(path)
                    else:
                        directories.append(path)
                except (OSError, ValueError):
                    preserved.append(path)
            if (eligible or directories) and confirm(list(eligible) + directories, list(preserved)) is not True:
                if callback:
                    callback({"removed": [], "preserved": preserved + eligible + directories, "cancelled": True}, None)
                return
            def delete_checked(referenced, referenced_directories, error):
                if error:
                    if callback:
                        callback(None, error)
                    return
                # Query references again after confirmation, then recheck local
                # path policy and identity before each individual deletion.
                for path in eligible:
                    if os.path.normcase(path) in referenced:
                        preserved.append(path)
                        continue
                    try:
                        checked = local_path(path)
                        info = os.stat(checked, follow_symlinks=False)
                        if (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) != self._owned_playblasts.get(path):
                            preserved.append(path)
                            continue
                        os.unlink(checked)
                        del self._owned_playblasts[path]
                        removed.append(path)
                    except (OSError, ValueError):
                        preserved.append(path)
                for path in directories:
                    if path in referenced_directories:
                        preserved.append(path)
                        continue
                    try:
                        checked = local_path(path, directory=True)
                        info = os.stat(checked, follow_symlinks=False)
                        if (info.st_dev, info.st_ino) != self._owned_directories.get(path):
                            preserved.append(path)
                            continue
                        # Never recurse: untracked or changed files keep the directory intact.
                        os.rmdir(checked)
                        del self._owned_directories[path]
                        removed.append(path)
                    except (OSError, ValueError):
                        preserved.append(path)
                for first, paths in list(self._owned_capture_groups.items()):
                    if not any(path in self._owned_playblasts for path in paths):
                        del self._owned_capture_groups[first]
                if callback:
                    callback({"removed": removed, "preserved": preserved, "cancelled": False}, None)
            if eligible or directories:
                self._query_capture_references(delete_checked)
            else:
                delete_checked(protected, protected_directories, None)
        self._query_capture_references(sources_received)

    def playblast(self, path=None, *, viewer="A", width=960, height=540, format_name="avi",
                  timecode_offset=False, include_sound=True, callback=None):
        """Movie compatibility entry point: still returns the captured filename."""
        if format_name not in ("avi", "webm"):
            raise ValueError("Movie playblast supports AVI and WebM")
        result = self.capture(path, settings=CaptureSettings(format_name=format_name, width=width,
                              height=height, include_sound=include_sound), viewer=viewer,
                              timecode_offset=timecode_offset, callback=callback)
        return result.paths[0]

    def playblast_sequence(self, directory=None, *, viewer="A", width=960, height=540,
                           image_format="png", padding=4, timecode_offset=False, callback=None):
        """Capture into a new directory; return a typed result and import ONE source."""
        return self.capture(directory, settings=CaptureSettings(format_name="image",
                            compression=image_format, width=width, height=height, padding=padding,
                            include_sound=False), viewer=viewer, timecode_offset=timecode_offset,
                            callback=callback)

    def capture(self, path=None, *, settings=None, viewer="A", first=None, last=None,
                timecode_offset=False, callback=None):
        """Capture on the main thread, then enqueue asynchronous import.

        For image capture path is a NEW directory, not a filename prefix. Movie
        path is an exact filename. Omitted path allocates a session-owned output.
        The return value describes completed files; callback reports import.
        """
        self.host.assert_main_thread()
        settings = settings if settings is not None else CaptureSettings()
        if not isinstance(settings, CaptureSettings):
            raise TypeError("settings must be CaptureSettings")
        if viewer not in ("A", "B", "None"):
            raise ValueError("viewer must be A, B or None")
        if type(timecode_offset) is not bool:
            raise ValueError("timecode_offset must be boolean")
        if callback is not None and not callable(callback):
            raise TypeError("callback must be callable")
        if self._stop.is_set() or not self._thread or not self._thread.is_alive():
            raise OpenFrameError("Adapter is not running")
        with self._lock:
            if self._suspended:
                raise OpenFrameError("A capture is already in progress")
        if (first is None) != (last is None):
            raise ValueError("first and last must be provided together")
        snapshot = self.host.snapshot()
        capture_first, capture_last = self.host.playblast_range() if first is None else (first, last)
        for value in (capture_first, capture_last):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or abs(value) > 1e9:
                raise ValueError("Capture frames must be finite and within +/-1e9")
        if capture_last < capture_first:
            raise ValueError("Capture range is reversed")
        sequence = settings.format_name == "image"
        if sequence and (int(capture_first) != capture_first or int(capture_last) != capture_last
                         or capture_last - capture_first >= MAX_SEQUENCE_FRAMES):
            raise ValueError("Image capture needs 1..100000 consecutive integer frames")
        fps_num, fps_den = self.host.frame_rate()
        sound = self.host.sound_snapshot() if settings.include_sound and not sequence else None
        if sound is not None:
            if not isinstance(sound, TimelineSound):
                raise TypeError("Maya sound snapshot must be TimelineSound")
            local_path(sound.path)
        temporary_capture = path is None
        if sequence:
            if temporary_capture:
                path = os.path.join(self.playblast_directory(), uuid.uuid4().hex)
            directory = local_path(path, output=True, directory=True)
            if os.path.exists(directory):
                raise FileExistsError("Image capture requires a new output directory")
            expected = tuple(os.path.join(directory, "frame.{}.{}".format(str(index).zfill(settings.padding), settings.encoding))
                             for index in range(int(capture_last - capture_first) + 1))
            if sum(len(item) * 2 for item in expected) > 16 * 1024 * 1024:
                raise ValueError("Sequence paths exceed the 16 MiB enumeration budget")
            path = os.path.join(directory, "frame")
            descriptor = {"directory": directory, "baseName": "frame", "first": 0,
                          "last": len(expected) - 1, "padding": settings.padding,
                          "extension": settings.encoding, "fpsNum": fps_num, "fpsDen": fps_den}
            os.mkdir(directory)
            directory = local_path(directory, directory=True)
            if temporary_capture:
                info = os.stat(directory, follow_symlinks=False)
                self._owned_directories[directory] = (info.st_dev, info.st_ino)
        else:
            if temporary_capture:
                path = os.path.join(self.playblast_directory(), uuid.uuid4().hex + "." + settings.format_name)
            path = local_path(path, output=True)
            if not path.lower().endswith("." + settings.format_name):
                raise ValueError("Output extension must match the playblast format")
            expected = (path,)
        for item in expected:
            checked = local_path(item, output=True)
            if os.path.exists(checked):
                raise FileExistsError("Playblast refuses to overwrite existing files")
        with self._lock:
            if self._suspended:
                raise OpenFrameError("A capture is already in progress")
            self._suspended += 1
            self._pending_remote = None
        captured = False
        try:
            produced = self.host.capture(path, capture_first, capture_last, settings, sound)
            if not sequence and os.path.normcase(local_path(produced or path)) != os.path.normcase(path):
                raise OpenFrameError("Maya returned an unexpected playblast filename")
            for item in expected:
                checked = local_path(item)
                if os.stat(checked, follow_symlinks=False).st_size <= 0:
                    raise OpenFrameError("Maya produced an empty capture file")
            captured = True
        finally:
            try:
                if temporary_capture:
                    for item in expected:
                        try:
                            checked = local_path(item)
                            info = os.stat(checked, follow_symlinks=False)
                            self._owned_playblasts[checked] = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
                        except (OSError, ValueError):
                            pass
                    self._owned_capture_groups[expected[0]] = expected
                # Public v1.4.4 behavior: successful capture ends on its first frame.
                # A failed capture restores the original position instead.
                self.host.set_time(capture_first if captured else snapshot["frame"])
            finally:
                with self._lock:
                    self._suspended -= 1
                    self._pending_remote = None
                    self._snapshot = self.host.snapshot()
                    self._sequence += 1
        result = CaptureResult("sequence" if sequence else "movie", expected, capture_first,
                               capture_last, fps_num, fps_den, sound)
        self._import_capture(result, viewer, timecode_offset, callback,
                             descriptor if sequence else None)
        return result

    def _import_capture(self, result, viewer, timecode_offset, callback, descriptor=None):
        actual = result.paths[0]
        def imported(_result, error):
            if error:
                if callback:
                    callback(None, error)
                return
            # Import indexing is asynchronous. Poll from worker, never block Maya.
            deadline = time.monotonic() + 120
            def sources_ready(sources, failure):
                if failure:
                    if callback:
                        callback(None, failure)
                    return
                source = next((item for item in sources if os.path.normcase(os.path.abspath(item.get("path", ""))) == os.path.normcase(actual)), None)
                if source:
                    if timecode_offset:
                        self.request("timecode.set", id=source["id"], offset=int(result.first), enabled=True)
                    if viewer != "None":
                        self.request("viewer.assign", pane=0 if viewer == "A" else 1, id=source["id"])
                    self.request("window.activate")
                    if callback:
                        callback(source, None)
                elif time.monotonic() < deadline:
                    def import_status(status, status_error):
                        error = status_error
                        if not error and not status.get("busy", False) and status.get("lastError"):
                            error = OpenFrameError("Playblast import failed: " + status["lastError"])
                        if error:
                            if callback:
                                callback(None, error)
                        else:
                            self.request("objects.list", callback=sources_ready, type="source")
                    self.request("status", callback=import_status)
                elif callback:
                    callback(None, OpenFrameError("Playblast import timed out"))
            self.request("objects.list", callback=sources_ready, type="source")
        if descriptor is not None:
            self.request("media.sequenceDescriptor", callback=imported, **descriptor)
        else:
            self.request("media.import", callback=imported, paths=[actual])

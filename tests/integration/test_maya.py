"""Synthetic-only integration checks; no installed Maya or user scene is read."""
import json
import os
from pathlib import Path
import queue
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.environ.get("OPENFRAME_TEST_MAYA_PATH", str(Path(__file__).resolve().parents[2] / "maya")))
from openframe_maya import Adapter, Client, OpenFrameError, SyncMapping, CaptureSettings, CaptureResult, TimelineSound
from openframe_maya.adapter import MayaHost
from openframe_maya.client import _read_endpoint, discover_endpoints
from openframe_maya.paths import local_path, PathPolicyError


class FakeMaya:
    def __init__(self):
        self.frame = 1.0
        self.first, self.last = 1.0, 12.0
        self.highlighted_range = None
        self.timeline_sound = None
        self.callbacks = {}
        self.idle = queue.Queue()
        self.calls = []
        self._next = 1

    def assert_main_thread(self):
        if threading.current_thread() is not threading.main_thread():
            raise AssertionError("Maya call occurred on a background thread")

    def snapshot(self):
        self.assert_main_thread()
        return {"frame": self.frame, "range_start": self.first, "range_end": self.last}

    def schedule(self, callback):
        self.idle.put(callback)

    def pump(self):
        self.assert_main_thread()
        for _ in range(100):
            try:
                callback = self.idle.get_nowait()
            except queue.Empty:
                break
            callback()

    def set_time(self, frame):
        self.assert_main_thread()
        self.calls.append(("set_time", frame, threading.get_ident()))
        self.frame = float(frame)
        for callback in list(self.callbacks.values()):
            callback()

    def set_range(self, first, last):
        self.assert_main_thread()
        self.calls.append(("set_range", first, last))
        self.first, self.last = first, last

    def add_time_callback(self, callback):
        self.assert_main_thread()
        value = self._next
        self._next += 1
        self.callbacks[value] = callback
        return value

    def remove_callback(self, value):
        self.assert_main_thread()
        self.callbacks.pop(value)

    def playblast_range(self):
        self.assert_main_thread()
        # Exercise the real public-command boundary with synthetic Maya query replies.
        host = MayaHost.__new__(MayaHost)
        host.cmds = mock.Mock()
        host.cmds.lsUI.return_value = ["syntheticTimeSlider"]
        host.cmds.playbackOptions.side_effect = lambda **kwargs: self.first if kwargs.get("minTime") else self.last
        host.cmds.timeControl.side_effect = lambda _name, **kwargs: (
            self.highlighted_range is not None if kwargs.get("rangeVisible") else self.highlighted_range)
        return host.playblast_range()

    def frame_rate(self):
        self.assert_main_thread()
        return 24, 1

    def sound_snapshot(self):
        self.assert_main_thread()
        return self.timeline_sound

    def capture(self, path, first, last, settings, sound):
        self.assert_main_thread()
        self.calls.append(("playblast", path, first, last, settings.width, settings.height, settings.format_name, settings, sound))
        for frame in range(int(first), int(last) + 1):
            self.set_time(frame)
        if settings.format_name == "image":
            for index in range(int(last - first) + 1):
                with open("{}.{}.{}".format(path, str(index).zfill(settings.padding), settings.encoding), "wb") as output:
                    output.write(b"Synthetic capture file; pixel decoding is tested by the native API suite")
            return path
        with open(path, "wb") as output:
            output.write(b"Synthetic test playblast fixture; not a real movie")
        return path


class FakeApi:
    """Test fixture serving the real framing/auth contract over exact loopback."""
    def __init__(self, directory):
        self.token = "ab" * 32
        self.instance = "synthetic-instance"
        self.frame = 0
        self.range_start, self.range_end = 0, 99
        self.revision = 0
        self.origin = ""
        self.sessions = {}
        self.sources = []
        self.import_error = ""
        self.reference_error = False
        self.assignments = {}
        self.calls = []
        self.next_response = None
        self.response_plans = {}
        self.received = {}
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.connections = []
        self.threads = []
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(8)
        self.server.settimeout(0.1)
        self.path = str(Path(directory) / "endpoint-synthetic.json")
        self.endpoint = {"api": 1, "host": "127.0.0.1", "port": self.server.getsockname()[1],
                         "token": self.token, "pid": os.getpid(), "instance": self.instance}
        with open(self.path, "w", encoding="utf-8") as output:
            json.dump(self.endpoint, output)
        os.chmod(self.path, 0o600)
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while not self.stop.is_set():
            try:
                connection, peer = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            assert peer[0] == "127.0.0.1"
            connection.settimeout(0.2)
            self.connections.append(connection)
            worker = threading.Thread(target=self._serve, args=(connection,), daemon=True)
            self.threads.append(worker)
            worker.start()

    def _serve(self, connection):
        pending = b""
        while not self.stop.is_set():
            try:
                data = connection.recv(65536)
                if not data:
                    return
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    request = json.loads(line)
                    if request.get("token") != self.token:
                        response = {"jsonrpc": "2.0", "id": request.get("id"), "error": {"code": -32001, "message": "Authentication failed"}}
                    else:
                        with self.lock:
                            if self.next_response is not None:
                                response = self.next_response
                                self.next_response = None
                            elif request["method"] == "media.pathReferenced" and self.reference_error:
                                response = {"jsonrpc": "2.0", "id": request["id"],
                                            "error": {"code": -32010, "message": "Synthetic reference query failure"}}
                            else:
                                response = {"jsonrpc": "2.0", "id": request["id"], "result": self.dispatch(request["method"], request["params"])}
                    with self.lock:
                        plan = dict(self.response_plans.get(request["method"], {}))
                        self.received.setdefault(request["method"], threading.Event()).set()
                    if self.stop.wait(plan.get("delay", 0)):
                        return
                    payload = json.dumps(response).encode() + b"\n"
                    chunk_size = plan.get("chunk_size", len(payload))
                    for offset in range(0, len(payload), chunk_size):
                        connection.sendall(payload[offset:offset + chunk_size])
                        if self.stop.wait(plan.get("interval", 0)):
                            return
            except socket.timeout:
                continue
            except (OSError, ValueError):
                return

    def status(self, sid=""):
        result = {"api": 1, "instance": self.instance, "frame": self.frame,
                  "frameRevision": self.revision, "frameOrigin": self.origin,
                  "rangeIn": self.range_start, "rangeOut": self.range_end, "busy": False,
                  "lastError": self.import_error}
        if sid in self.sessions:
            result["maya"] = dict(self.sessions[sid])
        return result

    def dispatch(self, method, params):
        self.calls.append((method, dict(params)))
        if method == "status":
            return self.status(params.get("sessionId", ""))
        if method == "maya.sync":
            sid = params["sessionId"]
            config = self.sessions[sid]
            applied = "mayaFrame" in params and config["direction"] in ("maya_to_openframe", "bidirectional")
            if applied:
                mapping = SyncMapping(config["offset"], config["multiplier"], config["fromRangeStart"])
                self.frame = max(0, mapping.to_review(params["mayaFrame"], self.range_start))
                self.origin = sid
                self.revision += 1
            return dict(self.status(sid), syncFrameApplied=applied)
        if method == "echo":
            return params["text"]
        if method == "maya.register":
            self.sessions[params["sessionId"]] = dict(params, configRevision=0)
            return self.status(params["sessionId"])
        if method == "maya.configure":
            item = self.sessions[params["sessionId"]]
            item.update(params["config"])
            item["configRevision"] += 1
            return dict(item)
        if method == "maya.unregister":
            return True
        if method == "seek":
            self.frame = max(0, params["frame"])
            self.origin = params.get("origin", "")
            self.revision += 1
            return self.frame
        if method == "range.set":
            self.range_start, self.range_end = params["first"], params["last"]
            return [self.range_start, self.range_end]
        if method == "media.import":
            for path in params["paths"]:
                self.sources.append({"id": "source-" + str(len(self.sources)), "name": os.path.basename(path), "path": path})
            return {"accepted": True}
        if method == "media.sequenceDescriptor":
            if self.import_error:
                return {"accepted": True}
            path = os.path.join(params["directory"], "{}.{}.{}".format(params["baseName"],
                                str(params["first"]).zfill(params["padding"]), params["extension"]))
            self.sources.append({"id": "source-" + str(len(self.sources)), "name": params["baseName"],
                                 "path": path, "frame_count": params["last"] - params["first"] + 1,
                                 "_sequence_paths": [os.path.join(params["directory"], "{}.{}.{}".format(
                                     params["baseName"], str(frame).zfill(params["padding"]), params["extension"]))
                                     for frame in range(params["first"], params["last"] + 1)]})
            return {"accepted": True}
        if method == "media.pathReferenced":
            directory = "directory" in params
            target = os.path.normcase(os.path.abspath(params["directory" if directory else "path"]))
            paths = [path for source in self.sources for path in [source["path"]] + source.get("_sequence_paths", [])]
            referenced = any(os.path.normcase(os.path.abspath(path)) == target or
                             directory and os.path.normcase(os.path.abspath(path)).startswith(target.rstrip(os.sep) + os.sep)
                             for path in paths)
            return {"referenced": referenced}
        if method == "objects.list":
            return [{key: value for key, value in source.items() if not key.startswith("_")} for source in self.sources]
        if method == "viewer.assign":
            self.assignments[params["pane"]] = params["id"]
            return {"id": params["id"]}
        return True

    def ui_seek(self, frame):
        with self.lock:
            self.frame = frame
            self.origin = ""
            self.revision += 1

    def plan_reply(self, method, **plan):
        with self.lock:
            self.response_plans[method] = plan
            event = threading.Event()
            self.received[method] = event
            return event

    def drop_connections(self):
        for connection in list(self.connections):
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        self.connections.clear()

    def close(self):
        self.stop.set()
        self.drop_connections()
        self.server.close()
        self.thread.join(1)
        for thread in self.threads:
            thread.join(1)


class MappingTests(unittest.TestCase):
    def test_mapping_and_inverse(self):
        for offset in (-100, 0, 7.5):
            for multiplier in (-2, 0.5, 1, 2, 24 / 25):
                for relative in (False, True):
                    mapping = SyncMapping(offset, multiplier, relative)
                    for frame in (-12, 0, 1, 24, 101):
                        review = mapping.to_review(frame, 15)
                        restored = mapping.to_maya(review, 15)
                        self.assertLessEqual(abs(restored - frame), 0.5000001 / abs(multiplier))
        self.assertEqual(SyncMapping(from_range_start=True).to_review(1, 15), 15)
        self.assertEqual(SyncMapping(3, 2, True).to_review(4, 15), 24)

    def test_reject_nonfinite_and_zero(self):
        for offset, multiplier in ((0, 0), (0, float("nan")), (float("inf"), 1), (0, 1001)):
            with self.assertRaises(ValueError):
                SyncMapping(offset, multiplier)


class PathTests(unittest.TestCase):
    def test_remote_names_rejected_before_filesystem(self):
        values = ["https://example.invalid/a.mp4", "file:///C:/x", "\\\\server\\share\\x",
                  "//server/share/x", "\\\\?\\C:\\x", "C:relative", "C:/x:ads", "../x", "a\0b", "udp:host"]
        with mock.patch("os.stat", side_effect=AssertionError("Unexpected filesystem access")), \
             mock.patch("os.lstat", side_effect=AssertionError("Unexpected filesystem access")):
            for value in values:
                with self.subTest(value=value), self.assertRaises(PathPolicyError):
                    local_path(value)

    def test_valid_paths_and_missing_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "new.json")
            self.assertEqual(local_path(output, output=True), output)
            with open(output, "w") as stream:
                stream.write("{}")
            self.assertEqual(local_path(output), output)
            self.assertEqual(local_path(directory, directory=True), directory)
            with self.assertRaises((PathPolicyError, OSError)):
                local_path(os.path.join(directory, "absent", "x"), output=True)

    def test_symbolic_link_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            target, link = os.path.join(directory, "target"), os.path.join(directory, "link")
            os.mkdir(target)
            try:
                os.symlink(target, link, target_is_directory=True)
            except OSError:
                self.skipTest("Windows symbolic-link privilege unavailable")
            with self.assertRaises(PathPolicyError):
                local_path(link, directory=True)


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.server = FakeApi(self.directory.name)

    def tearDown(self):
        self.server.close()
        self.directory.cleanup()

    def test_authenticated_local_round_trip_ignores_proxies_and_dns(self):
        with mock.patch("socket.getaddrinfo", side_effect=AssertionError("DNS resolver invoked")), \
             mock.patch.dict(os.environ, {"HTTP_PROXY": "http://192.0.2.1:1234", "ALL_PROXY": "socks5://192.0.2.2:1234"}):
            with Client(self.server.path) as client:
                self.assertEqual(client.call("echo", text="Привет OpenFrame"), "Привет OpenFrame")
                self.assertTrue(client.connected)
                self.assertEqual(client.instance_id, "synthetic-instance")

    def test_endpoint_host_rejected_before_socket(self):
        for host in ("localhost", "127.0.0.2", "::1", "::ffff:127.0.0.1", "192.0.2.1"):
            endpoint = dict(self.server.endpoint, host=host)
            with open(self.server.path, "w", encoding="utf-8") as output:
                json.dump(endpoint, output)
            with mock.patch("socket.socket", side_effect=AssertionError("Socket created for forbidden host")):
                with self.assertRaises(OpenFrameError):
                    Client(self.server.path).connect()

    def test_bad_token_is_rejected(self):
        endpoint = dict(self.server.endpoint, token="cd" * 32)
        with open(self.server.path, "w", encoding="utf-8") as output:
            json.dump(endpoint, output)
        with self.assertRaises(OpenFrameError):
            Client(self.server.path).connect()

    def test_limits_and_nonfinite_json(self):
        with Client(self.server.path) as client:
            with self.assertRaises(OpenFrameError):
                client.call("echo", text="x" * 65536)
            with self.assertRaises(ValueError):
                client.call("seek", frame=float("nan"))
            self.assertEqual(client.call("echo", text="alive"), "alive")

    def test_discovery_returns_paths_without_connecting(self):
        with mock.patch("socket.socket", side_effect=AssertionError("Discovery attempted connection")):
            self.assertEqual(discover_endpoints(self.directory.name), [self.server.path])

    def test_removed_endpoint_reselects_only_unambiguous_instance(self):
        selected = self.server.path
        new_path = os.path.join(self.directory.name, "endpoint-restarted.json")
        os.rename(selected, new_path)
        with Client(selected) as client:
            self.assertEqual(client.endpoint_path, new_path)
            self.assertTrue(client.connected)
        duplicate = os.path.join(self.directory.name, "endpoint-other.json")
        with open(duplicate, "w", encoding="utf-8") as output:
            json.dump(self.server.endpoint, output)
        os.chmod(duplicate, 0o600)
        with mock.patch("socket.socket", side_effect=AssertionError("Ambiguous reconnect created socket")):
            with self.assertRaises(OpenFrameError):
                Client(selected).connect()

    def test_null_id_protocol_error_is_reported_and_closes_connection(self):
        with Client(self.server.path) as client:
            with self.server.lock:
                self.server.next_response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32004, "message": "Rate limit exceeded"}}
            with self.assertRaises(OpenFrameError) as failure:
                client.call("echo", text="rate")
            self.assertEqual(failure.exception.code, -32004)
            self.assertEqual(str(failure.exception), "Rate limit exceeded")
            self.assertFalse(client.connected)


class DeadlineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.server = FakeApi(self.directory.name)

    def tearDown(self):
        self.server.close()
        self.directory.cleanup()

    def test_timeout_types_bounds_and_property_rollback(self):
        client = Client(self.server.path, timeout=.75)
        for value in (True, False, None, "1", float("nan"), float("inf"), -.1, 0, .049, 120.001):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    Client(self.server.path, timeout=value)
                with self.assertRaises(ValueError):
                    client.timeout = value
                self.assertEqual(client.timeout, .75)
        for value in (.05, 120):
            client.timeout = value
            self.assertEqual(client.timeout, value)

    def test_delayed_reply_within_deadline_succeeds(self):
        self.server.plan_reply("echo", delay=.1)
        with Client(self.server.path, timeout=.5) as client:
            start = time.monotonic()
            self.assertEqual(client.call("echo", text="own delayed response"), "own delayed response")
            self.assertGreaterEqual(time.monotonic() - start, .08)

    def test_drip_feed_does_not_restart_whole_rpc_deadline(self):
        with Client(self.server.path, timeout=.15) as client:
            self.server.plan_reply("echo", chunk_size=1, interval=.02)
            start = time.monotonic()
            with self.assertRaisesRegex(OpenFrameError, "deadline"):
                client.call("echo", text="own slow bytes")
            elapsed = time.monotonic() - start
            self.assertGreaterEqual(elapsed, .12)
            self.assertLess(elapsed, .7)
            self.assertFalse(client.connected)

    def test_connect_handshake_uses_same_deadline(self):
        self.server.plan_reply("status", chunk_size=1, interval=.02)
        client = Client(self.server.path, timeout=.15)
        start = time.monotonic()
        with self.assertRaisesRegex(OpenFrameError, "deadline"):
            client.connect()
        self.assertLess(time.monotonic() - start, .7)
        self.assertFalse(client.connected)

    def test_connected_connect_keeps_deadline_after_waiting_for_rpc_lock(self):
        client = Client(self.server.path, timeout=.2)
        client.connect()
        request = self.server.plan_reply("echo", delay=.12)
        first = []
        def echo():
            try:
                first.append(client.call("echo", text="first"))
            except OpenFrameError as error:
                first.append(error)
        thread = threading.Thread(target=echo)
        thread.start()
        self.assertTrue(request.wait(1))
        self.server.plan_reply("status", delay=.12)
        start = time.monotonic()
        with self.assertRaisesRegex(OpenFrameError, "deadline"):
            client.connect()
        self.assertLess(time.monotonic() - start, .6)
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(first, ["first"])
        client.close()

    def test_close_interrupts_long_reply_and_client_reconnects(self):
        client = Client(self.server.path, timeout=120)
        client.connect()
        request = self.server.plan_reply("echo", delay=120)
        errors = []
        def call():
            try:
                client.call("echo", text="interrupted")
            except OpenFrameError as error:
                errors.append(error)
        thread = threading.Thread(target=call)
        thread.start()
        self.assertTrue(request.wait(1))
        start = time.monotonic()
        client.close()
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertLess(time.monotonic() - start, .7)
        self.assertEqual(len(errors), 1)
        self.server.plan_reply("echo")
        client.timeout = .5
        client.connect()
        self.assertEqual(client.call("echo", text="new connection"), "new connection")
        client.close()

    def test_pre_cancelled_connect_does_not_send_any_request(self):
        cancelled = threading.Event()
        cancelled.set()
        client = Client(self.server.path, timeout=120, cancel_event=cancelled)
        with self.assertRaisesRegex(OpenFrameError, "cancelled"):
            client.connect()
        self.assertFalse(client.connected)
        self.assertEqual(self.server.calls, [])


class CaptureBoundaryTests(unittest.TestCase):
    def test_explicit_capture_settings_validate_without_preference_access(self):
        for values in ({"width": True}, {"width": 31}, {"height": 9000}, {"percent": 0},
                       {"quality": 101}, {"padding": 13}, {"off_screen": 1},
                       {"include_sound": "yes"}, {"compression": "bad\ncodec"},
                       {"format_name": "image", "compression": "svg"}):
            with self.assertRaises(ValueError):
                CaptureSettings(**values)
        host = MayaHost.__new__(MayaHost)
        host.cmds = mock.Mock()
        settings = CaptureSettings(format_name="image", width=320, height=180, percent=50,
                                   quality=81, show_ornaments=True, off_screen=False, padding=6)
        host.capture("synthetic-prefix", 5, 9, settings, None)
        options = host.cmds.playblast.call_args.kwargs
        self.assertEqual((options["format"], options["compression"], options["indexFromZero"],
                          options["rawFrameNumbers"], options["framePadding"]), ("image", "png", True, False, 6))
        self.assertEqual((options["widthHeight"], options["percent"], options["quality"]), ((320, 180), 50, 81))
        self.assertTrue(options["showOrnaments"])
        self.assertFalse(options["offScreen"])
        self.assertFalse(options["viewer"] or options["forceOverwrite"] or options["clearCache"])
        self.assertNotIn("sound", options)
        host.cmds.optionVar.assert_not_called()

    def test_capture_omits_empty_sound_under_maya_strict_error_behavior(self):
        # Reproduced in a separate Maya 2024 GUI with an own synthetic scene:
        # sound="" emits "cannot load sound file"; throwOnError=True raises.
        # Other new flags, including throwOnError itself, succeed without sound.
        host = MayaHost.__new__(MayaHost)
        host.cmds = mock.Mock()

        def strict_playblast(**options):
            if options.get("throwOnError") and "sound" in options and not options["sound"]:
                raise RuntimeError("Maya command error: cannot load sound file")
            return options["filename"]

        host.cmds.playblast.side_effect = strict_playblast
        muted = TimelineSound("ownAudio", "own-synthetic.wav", 0, 0, 24, 24, True, True)
        cases = ((CaptureSettings(), None), (CaptureSettings(), muted),
                 (CaptureSettings(include_sound=False), None),
                 (CaptureSettings(format_name="image", include_sound=False), None))
        for settings, sound in cases:
            with self.subTest(format=settings.format_name, sound=sound, enabled=settings.include_sound):
                self.assertEqual(host.capture("own-capture", 1, 4, settings, sound), "own-capture")
                options = host.cmds.playblast.call_args.kwargs
                self.assertNotIn("sound", options)
                self.assertTrue(options["throwOnError"])

    def test_sound_snapshot_uses_typed_queries_and_passes_node_not_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "own-sound.wav")
            Path(path).write_bytes(b"Own synthetic file used for path validation only")
            values = {"file": path, "offset": 7.0, "sourceStart": 3.0, "sourceEnd": 27.0,
                      "endTime": 31.0, "mute": False}
            host = MayaHost.__new__(MayaHost)
            host.cmds = mock.Mock()
            host.cmds.lsUI.return_value = ["syntheticTimeSlider"]
            host.cmds.timeControl.side_effect = lambda _control, **kwargs: "ownAudio" if kwargs.get("sound") else False
            host.cmds.sound.side_effect = lambda _node, **kwargs: next(values[key] for key in values if kwargs.get(key))
            sound = host.sound_snapshot()
            self.assertEqual((sound.node, sound.path, sound.offset, sound.source_start, sound.source_end,
                              sound.end_time, sound.muted, sound.displayed),
                             ("ownAudio", path, 7, 3, 27, 31, False, False))
            host.capture("own.avi", 1, 24, CaptureSettings(), sound)
            self.assertEqual(host.cmds.playblast.call_args.kwargs["sound"], "ownAudio")
            self.assertTrue(all(call.kwargs.get("query") for call in host.cmds.sound.call_args_list))
            self.assertFalse(any(call.kwargs.get("edit") for call in host.cmds.sound.call_args_list))
            values["mute"] = True
            host.capture("own.avi", 1, 24, CaptureSettings(), host.sound_snapshot())
            self.assertNotIn("sound", host.cmds.playblast.call_args.kwargs)
            values["file"] = "https://example.invalid/audio.wav"
            with self.assertRaises(PathPolicyError):
                host.sound_snapshot()

    def test_frame_rate_uses_mtime_and_retains_fractional_rate(self):
        host = MayaHost.__new__(MayaHost)
        host.api = mock.Mock()
        host.api.MTime.return_value.asUnits.return_value = 1001 / 30000
        self.assertEqual(host.frame_rate(), (30000, 1001))
        host.api.MTime.return_value.asUnits.return_value = 0
        with self.assertRaises(OpenFrameError):
            host.frame_rate()


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.server = FakeApi(self.directory.name)
        self.hosts, self.adapters = [], []

    def tearDown(self):
        for adapter in self.adapters:
            adapter.stop()
        for host in self.hosts:
            host.pump()
        self.server.close()
        self.directory.cleanup()

    def create_adapter(self, direction="bidirectional", mapping=None, **options):
        host = FakeMaya()
        adapter = Adapter(self.server.path, host=host, poll_interval=0.02, **options)
        self.hosts.append(host)
        self.adapters.append(adapter)
        adapter.start(direction, mapping)
        self.wait(lambda: adapter.state["connected"])
        return adapter, host

    def wait(self, condition, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for host in self.hosts:
                host.pump()
            if condition():
                return
            time.sleep(0.01)
        self.fail("Condition did not become true before timeout")

    def settle(self, duration=0.12):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            for host in self.hosts:
                host.pump()
            time.sleep(0.005)

    def test_timeout_config_roundtrip_and_invalid_change_is_atomic(self):
        adapter, host = self.create_adapter("off", timeout=.4)
        self.wait(lambda: self.server.sessions[adapter.session_id].get("timeoutSeconds") == .4)
        before = adapter.state["config"]
        generation = adapter._generation
        with self.assertRaises(ValueError):
            adapter.configure(direction="bidirectional", mapping=SyncMapping(10, 2), timeout=True)
        self.assertEqual(adapter.state["config"], before)
        self.assertEqual(adapter._generation, generation)
        with self.server.lock:
            self.server.sessions[adapter.session_id]["timeoutSeconds"] = 2.5
            self.server.sessions[adapter.session_id]["configRevision"] += 1
        self.wait(lambda: adapter.state["config"]["timeoutSeconds"] == 2.5)
        self.assertEqual(adapter._client.timeout, 2.5)
        adapter.configure(timeout=.2)
        self.wait(lambda: adapter.state["connected"] and
                  self.server.sessions[adapter.session_id].get("timeoutSeconds") == .2)
        self.assertEqual(adapter._client.timeout, .2)

    def test_stop_interrupts_stalled_120_second_rpc_and_restart_is_clean(self):
        adapter, host = self.create_adapter("off", timeout=120)
        request = self.server.plan_reply("maya.sync", delay=120)
        self.assertTrue(request.wait(1))
        start = time.monotonic()
        adapter.stop()
        self.assertLess(time.monotonic() - start, .7)
        self.assertFalse(adapter._thread.is_alive())
        self.assertFalse(adapter.state["running"])
        self.server.plan_reply("maya.sync")
        host.set_time(17)
        adapter.start("off")
        self.wait(lambda: adapter.state["connected"])
        self.assertEqual(host.frame, 17)

    def test_reconnect_discards_old_result_and_remote_callbacks(self):
        adapter, host = self.create_adapter("off", timeout=120)
        results = []
        adapter._complete(lambda result, error: results.append(result), result="old-result")
        with adapter._lock:
            adapter._config["direction"] = "openframe_to_maya"
        adapter._queue_remote(99)
        adapter.configure(timeout=.2)
        # Deliver the queued old callbacks only after the generation changed.
        host.pump()
        self.assertEqual(results, [])
        self.assertNotEqual(host.frame, 99)
        self.wait(lambda: adapter.state["connected"])
        adapter.request("echo", callback=lambda result, error: results.append(result), text="new-result")
        self.wait(lambda: results)
        self.assertEqual(results, ["new-result"])

    def test_disabled_start_preserves_scene_time(self):
        adapter, host = self.create_adapter("off")
        self.server.ui_seek(42)
        self.settle()
        self.assertEqual(host.frame, 1)
        self.assertFalse(any(method == "maya.sync" and "mayaFrame" in params for method, params in self.server.calls))
        self.assertFalse(host.calls)

    def test_bidirectional_mapping_without_echo_loop(self):
        adapter, host = self.create_adapter(mapping=SyncMapping(3, 2, True))
        host.set_time(10)
        self.wait(lambda: self.server.frame == 21)
        self.settle()
        count = sum(method == "maya.sync" and "mayaFrame" in params for method, params in self.server.calls)
        self.server.ui_seek(43)
        self.wait(lambda: host.frame == 21)
        self.settle()
        self.assertEqual(sum(method == "maya.sync" and "mayaFrame" in params for method, params in self.server.calls), count)
        self.assertTrue(all(call[-1] == threading.get_ident() for call in host.calls if call[0] == "set_time"))

    def test_one_way_modes(self):
        adapter, host = self.create_adapter("openframe_to_maya")
        self.server.ui_seek(17)
        self.wait(lambda: host.frame == 17)
        host.set_time(30)
        self.settle()
        self.assertEqual(self.server.frame, 17)
        adapter.configure(direction="maya_to_openframe")
        host.set_time(31)
        self.wait(lambda: self.server.frame == 31)
        self.server.ui_seek(55)
        self.settle()
        self.assertEqual(host.frame, 31)

    def test_multiple_sessions_do_not_feed_back(self):
        first, host_a = self.create_adapter("maya_to_openframe")
        second, host_b = self.create_adapter("openframe_to_maya")
        host_a.set_time(9)
        self.wait(lambda: host_b.frame == 9)
        self.settle()
        outgoing_b = [params for method, params in self.server.calls if method == "maya.sync" and "mayaFrame" in params and params.get("sessionId") == second.session_id]
        self.assertFalse(outgoing_b)
        self.assertNotEqual(first.session_id, second.session_id)

    def test_reconnect_reregisters_session(self):
        adapter, host = self.create_adapter("maya_to_openframe")
        before = sum(method == "maya.register" for method, _ in self.server.calls)
        self.server.drop_connections()
        host.set_time(23)
        self.wait(lambda: sum(method == "maya.register" for method, _ in self.server.calls) > before)
        self.wait(lambda: self.server.frame == 23)

    def test_stop_cancels_deferred_updates_and_removes_callbacks(self):
        adapter, host = self.create_adapter("openframe_to_maya")
        self.settle()
        self.server.ui_seek(66)
        deadline = time.monotonic() + 2
        while host.idle.empty() and time.monotonic() < deadline:
            time.sleep(0.01)
        before = host.frame
        adapter.stop()
        host.pump()
        self.assertEqual(host.frame, before)
        self.assertFalse(host.callbacks)
        self.assertFalse(adapter.state["running"])

    def test_range_changes_are_explicit(self):
        adapter, host = self.create_adapter("off", SyncMapping(from_range_start=True))
        adapter.send_range()
        self.wait(lambda: self.server.range_end == 11)
        self.server.range_end = 24
        self.settle()
        self.assertEqual(host.last, 12)
        adapter.receive_range()
        self.assertEqual((host.first, host.last), (1, 25))

    def test_playblast_imports_to_b_and_suppresses_temporary_frames(self):
        adapter, host = self.create_adapter("maya_to_openframe")
        host.set_range(1, 24)
        host.set_time(17)
        self.wait(lambda: self.server.frame == 17)
        self.settle()
        completed = []
        output = os.path.join(self.directory.name, "synthetic.avi")
        before = len(self.server.calls)
        actual = adapter.playblast(output, viewer="B", callback=lambda result, error: completed.append((result, error)))
        self.assertEqual(actual, output)
        self.assertEqual(host.frame, 1)
        self.wait(lambda: completed and 1 in self.server.assignments and self.server.frame == 1
                  and any(method == "window.activate" for method, _ in self.server.calls[before:]))
        self.assertIsNone(completed[0][1])
        captures = [call for call in host.calls if call[0] == "playblast"]
        self.assertEqual(captures[-1][2:4], (1, 24))
        values = [params["mayaFrame"] for method, params in self.server.calls[before:] if method == "maya.sync" and "mayaFrame" in params]
        self.assertTrue(values)
        self.assertTrue(all(value == 1 for value in values))
        with self.assertRaises(FileExistsError):
            adapter.playblast(output)

    def test_failed_playblast_restores_previous_frame_and_resumes_sync(self):
        adapter, host = self.create_adapter("maya_to_openframe")
        host.set_range(1, 24)
        host.highlighted_range = (5, 10)
        host.set_time(17)
        self.wait(lambda: self.server.frame == 17)
        self.settle()
        before = len(self.server.calls)

        def fail_capture(_path, first, last, *_args):
            self.assertEqual((first, last), (5, 9))
            host.set_time(6)
            raise RuntimeError("Synthetic capture failure")

        with mock.patch.object(host, "capture", side_effect=fail_capture):
            with self.assertRaisesRegex(RuntimeError, "Synthetic capture failure"):
                adapter.playblast(os.path.join(self.directory.name, "failed.avi"))
        self.assertEqual(host.frame, 17)
        self.assertEqual(adapter._suspended, 0)
        host.set_time(18)
        self.wait(lambda: self.server.frame == 18)
        calls = self.server.calls[before:]
        self.assertFalse(any(method in ("media.import", "window.activate") for method, _ in calls))
        values = [params["mayaFrame"] for method, params in calls if method == "maya.sync" and "mayaFrame" in params]
        self.assertTrue(all(value in (17, 18) for value in values))

    def test_highlighted_playblast_uses_exclusive_end_without_changing_playback_range(self):
        adapter, host = self.create_adapter("maya_to_openframe")
        host.set_range(1, 24)
        host.highlighted_range = (5, 10)
        host.set_time(17)
        self.wait(lambda: self.server.frame == 17)
        completed = []
        output = os.path.join(self.directory.name, "highlighted.avi")
        adapter.playblast(output, viewer="None", timecode_offset=True,
                          callback=lambda result, error: completed.append((result, error)))
        self.assertEqual(host.frame, 5)
        self.assertEqual((host.first, host.last), (1, 24))
        captures = [call for call in host.calls if call[0] == "playblast"]
        self.assertEqual(captures[-1][2:4], (5, 9))
        self.wait(lambda: completed and any(method == "window.activate" for method, _ in self.server.calls))
        self.assertIsNone(completed[0][1])
        offsets = [params["offset"] for method, params in self.server.calls if method == "timecode.set"]
        self.assertEqual(offsets, [5])
        adapter.send_range()
        self.wait(lambda: self.server.range_end == 24)
        self.assertEqual((self.server.range_start, self.server.range_end), (1, 24))

    def test_invalid_highlighted_range_does_not_start_capture(self):
        adapter, host = self.create_adapter("off")
        host.set_range(1, 24)
        host.set_time(17)
        for selection in ((5, 5), (5, 5.5), (float("nan"), 10)):
            host.highlighted_range = selection
            with self.assertRaises(OpenFrameError):
                adapter.playblast(os.path.join(self.directory.name, "invalid-highlight.avi"))
        self.assertEqual(host.frame, 17)
        self.assertEqual(adapter._suspended, 0)
        self.assertFalse(any(call[0] == "playblast" for call in host.calls))

    def test_temp_cleanup_requires_confirmation_and_preserves_loaded_or_changed_files(self):
        adapter, host = self.create_adapter("off")
        api_dir = os.path.join(self.directory.name, "capture-root", "api")
        os.makedirs(api_dir)
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir):
            completed = []
            first = adapter.playblast(viewer="None", callback=lambda result, error: completed.append((result, error)))
            self.wait(lambda: completed)
            outcomes = []
            confirmation = mock.Mock(return_value=True)
            adapter.clear_playblasts(confirm=confirmation, callback=lambda result, error: outcomes.append((result, error)))
            self.wait(lambda: outcomes)
            self.assertTrue(os.path.isfile(first))
            self.assertEqual(outcomes[-1][0]["preserved"], [first])
            confirmation.assert_not_called()
            with self.server.lock:
                self.server.sources.clear()
            outcomes.clear()
            adapter.clear_playblasts(confirm=lambda eligible, preserved: False,
                                    callback=lambda result, error: outcomes.append((result, error)))
            self.wait(lambda: outcomes)
            self.assertTrue(outcomes[-1][0]["cancelled"])
            self.assertTrue(os.path.isfile(first))

            def change_during_confirmation(eligible, preserved):
                self.assertEqual(eligible, [first])
                with open(first, "ab") as stream:
                    stream.write(b"Externally changed synthetic fixture")
                return True
            outcomes.clear()
            adapter.clear_playblasts(confirm=change_during_confirmation,
                                    callback=lambda result, error: outcomes.append((result, error)))
            self.wait(lambda: outcomes)
            self.assertEqual(outcomes[-1][0]["removed"], [])
            self.assertTrue(os.path.isfile(first))
            completed.clear()
            second = adapter.playblast(viewer="None", callback=lambda result, error: completed.append((result, error)))
            self.wait(lambda: completed)
            unrelated = os.path.join(os.path.dirname(first), "untracked.txt")
            with open(unrelated, "w") as stream:
                stream.write("Not created by the adapter")
            with self.server.lock:
                self.server.sources.clear()
            outcomes.clear()
            adapter.clear_playblasts(confirm=lambda eligible, preserved: True,
                                    callback=lambda result, error: outcomes.append((result, error)))
            self.wait(lambda: outcomes)
            self.assertEqual(outcomes[-1][0]["removed"], [second])
            self.assertTrue(os.path.isfile(first))
            self.assertTrue(os.path.isfile(unrelated))
            self.assertFalse(os.path.exists(second))

    def test_long_sequence_capture_imports_one_source_and_preserves_movie_return_type(self):
        adapter, host = self.create_adapter("maya_to_openframe")
        host.set_range(1, 300)
        host.set_time(17)
        self.wait(lambda: self.server.frame == 17)
        self.settle()
        completed = []
        before = len(self.server.calls)
        directory = os.path.join(self.directory.name, "long-images")
        result = adapter.playblast_sequence(directory, viewer="B", timecode_offset=True,
                                             callback=lambda source, error: completed.append((source, error)))
        self.assertIsInstance(result, CaptureResult)
        self.assertEqual((result.kind, len(result.paths), result.first, result.last, result.fps_num, result.fps_den),
                         ("sequence", 300, 1, 300, 24, 1))
        self.assertIsNone(result.sound)
        self.assertEqual(os.path.basename(result.paths[0]), "frame.0000.png")
        self.assertEqual(os.path.basename(result.paths[-1]), "frame.0299.png")
        self.assertTrue(all(os.path.isfile(path) for path in result.paths))
        self.assertEqual(host.frame, 1)
        self.wait(lambda: completed and 1 in self.server.assignments and
                  any(method == "window.activate" for method, _ in self.server.calls[before:]))
        self.assertIsNone(completed[0][1])
        self.assertEqual(len(self.server.sources), 1)
        self.assertEqual(self.server.sources[0]["frame_count"], 300)
        imports = [(method, params) for method, params in self.server.calls[before:]
                   if method in ("media.import", "media.sequence", "media.sequenceDescriptor")]
        self.assertEqual(len(imports), 1)
        self.assertEqual(imports[0][0], "media.sequenceDescriptor")
        self.assertLess(len(json.dumps(imports[0][1])), 65536)
        values = [params["mayaFrame"] for method, params in self.server.calls[before:]
                  if method == "maya.sync" and "mayaFrame" in params]
        self.assertTrue(all(value == 1 for value in values))
        with self.assertRaises(FileExistsError):
            adapter.playblast_sequence(directory)

    def test_sequence_highlight_and_explicit_range_keep_playback_range(self):
        adapter, host = self.create_adapter("off")
        host.set_range(1, 24)
        host.highlighted_range = (5, 10)
        host.set_time(17)
        result = adapter.playblast_sequence(os.path.join(self.directory.name, "highlight"), viewer="None")
        self.assertEqual((result.first, result.last, len(result.paths), host.frame), (5, 9, 5, 5))
        self.assertEqual((host.first, host.last), (1, 24))
        result = adapter.capture(os.path.join(self.directory.name, "negative"),
                                 settings=CaptureSettings(format_name="image"), first=-2, last=2, viewer="None")
        self.assertEqual((result.first, result.last, len(result.paths), host.frame), (-2, 2, 5, -2))
        self.assertEqual(os.path.basename(result.paths[0]), "frame.0000.png")
        with self.assertRaises(ValueError):
            adapter.capture(settings=CaptureSettings(format_name="image"), first=0, last=100000)
        with self.assertRaises(ValueError):
            adapter.capture(settings=CaptureSettings(format_name="image"), first=0.5, last=2.5)
        with self.assertRaises(ValueError):
            adapter.capture(first=1)
        with self.assertRaises(TypeError):
            adapter.capture(callback="not a callable")

    def test_partial_sequence_failure_restores_time_and_keeps_owned_cleanup_scope(self):
        adapter, host = self.create_adapter("off")
        host.set_range(1, 24)
        host.set_time(17)
        api_dir = os.path.join(self.directory.name, "partial-root", "api")
        os.makedirs(api_dir)
        partial = []
        def incomplete(path, first, last, settings, sound):
            host.set_time(9)
            filename = path + ".0000.png"
            Path(filename).write_bytes(b"Own partial synthetic capture")
            partial.append(filename)
            return path
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir), \
                mock.patch.object(host, "capture", side_effect=incomplete):
            with self.assertRaises(PathPolicyError):
                adapter.playblast_sequence()
        self.assertEqual(host.frame, 17)
        self.assertEqual(adapter._suspended, 0)
        self.assertTrue(os.path.isfile(partial[0]))
        self.settle()
        self.assertFalse(any(method == "media.sequenceDescriptor" for method, _ in self.server.calls))
        outcomes = []
        confirmed = []
        adapter.clear_playblasts(confirm=lambda paths, preserved: confirmed.extend(paths) or True,
                                callback=lambda result, error: outcomes.append((result, error)))
        self.wait(lambda: outcomes)
        self.assertIn(partial[0], confirmed)
        self.assertIn(os.path.dirname(partial[0]), confirmed)
        self.assertFalse(os.path.exists(os.path.dirname(partial[0])))

    def test_sequence_path_budget_preflight_does_not_create_directory(self):
        adapter, host = self.create_adapter("off")
        host.set_time(17)
        directory = os.path.join(self.directory.name, "budget-" + "b" * 150)
        with self.assertRaisesRegex(ValueError, "16 MiB"):
            adapter.capture(directory, settings=CaptureSettings(format_name="image"), first=0, last=99999)
        self.assertFalse(os.path.exists(directory))
        self.assertEqual(host.frame, 17)
        self.assertFalse(any(call[0] == "playblast" for call in host.calls))

    def test_sequence_cleanup_preserves_loaded_group_and_unknown_directory_contents(self):
        adapter, host = self.create_adapter("off")
        host.set_range(1, 3)
        api_dir = os.path.join(self.directory.name, "sequence-root", "api")
        os.makedirs(api_dir)
        completed = []
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir):
            result = adapter.playblast_sequence(callback=lambda source, error: completed.append((source, error)))
        self.wait(lambda: completed)
        outcomes = []
        confirmation = mock.Mock(return_value=True)
        adapter.clear_playblasts(confirm=confirmation, callback=lambda result, error: outcomes.append((result, error)))
        self.wait(lambda: outcomes)
        confirmation.assert_not_called()
        self.assertTrue(all(os.path.isfile(path) for path in result.paths))
        with self.server.lock:
            self.server.sources.clear()
        directory = os.path.dirname(result.paths[0])
        unrelated = os.path.join(directory, "untracked.txt")
        Path(unrelated).write_text("Own fixture representing an untracked file")
        outcomes.clear()
        adapter.clear_playblasts(confirm=lambda paths, preserved: True,
                                callback=lambda result, error: outcomes.append((result, error)))
        self.wait(lambda: outcomes)
        self.assertFalse(any(os.path.exists(path) for path in result.paths))
        self.assertTrue(os.path.isfile(unrelated))
        self.assertIn(directory, outcomes[-1][0]["preserved"])
        os.unlink(unrelated)
        outcomes.clear()
        adapter.clear_playblasts(confirm=lambda paths, preserved: True,
                                callback=lambda result, error: outcomes.append((result, error)))
        self.wait(lambda: outcomes)
        self.assertFalse(os.path.exists(directory))

    def test_movie_sound_snapshot_is_forwarded_and_sequence_does_not_use_audio(self):
        adapter, host = self.create_adapter("off")
        sound_path = os.path.join(self.directory.name, "own-sound.wav")
        Path(sound_path).write_bytes(b"Own synthetic path fixture")
        host.timeline_sound = TimelineSound("ownAudio", sound_path, 7, 3, 27, 31, False, True)
        result = adapter.capture(os.path.join(self.directory.name, "sound.avi"), settings=CaptureSettings(width=320, height=180))
        self.assertEqual(result.sound.node, "ownAudio")
        self.assertEqual([call for call in host.calls if call[0] == "playblast"][-1][-1], host.timeline_sound)
        result = adapter.playblast_sequence(os.path.join(self.directory.name, "silent-images"))
        self.assertIsNone(result.sound)
        self.assertIsNone([call for call in host.calls if call[0] == "playblast"][-1][-1])
        result = adapter.playblast(os.path.join(self.directory.name, "no-sound.avi"), include_sound=False)
        self.assertIsInstance(result, str)
        self.assertIsNone([call for call in host.calls if call[0] == "playblast"][-1][-1])

    def test_cleanup_finds_nonfirst_frame_in_mixed_directory_sequence(self):
        adapter, host = self.create_adapter("off")
        host.set_range(1, 3)
        api_dir = os.path.join(self.directory.name, "mixed-root", "api")
        os.makedirs(api_dir)
        completed = []
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir):
            result = adapter.playblast_sequence(callback=lambda source, error: completed.append((source, error)))
        self.wait(lambda: completed)
        outside = os.path.join(self.directory.name, "external-first.bmp")
        Path(outside).write_bytes(b"Own unrelated synthetic image")
        with self.server.lock:
            self.server.sources[:] = [{"id": "mixed", "name": "mixed", "path": outside,
                                       "_sequence_paths": [outside, result.paths[1]]}]
        outcomes = []
        confirmation = mock.Mock(return_value=True)
        adapter.clear_playblasts(confirm=confirmation, callback=lambda value, error: outcomes.append((value, error)))
        self.wait(lambda: outcomes)
        confirmation.assert_not_called()
        self.assertIsNone(outcomes[-1][1])
        self.assertTrue(all(os.path.exists(path) for path in result.paths))

    def test_cleanup_reference_query_failure_after_confirmation_deletes_nothing(self):
        adapter, host = self.create_adapter("off")
        api_dir = os.path.join(self.directory.name, "query-failure-root", "api")
        os.makedirs(api_dir)
        completed = []
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir):
            path = adapter.playblast(callback=lambda source, error: completed.append((source, error)))
        self.wait(lambda: completed)
        with self.server.lock:
            self.server.sources.clear()
        outcomes = []
        def confirmed(paths, preserved):
            self.assertEqual(paths, [path])
            self.server.reference_error = True
            return True
        adapter.clear_playblasts(confirm=confirmed, callback=lambda value, error: outcomes.append((value, error)))
        self.wait(lambda: outcomes)
        self.assertIsNone(outcomes[-1][0])
        self.assertIsInstance(outcomes[-1][1], OpenFrameError)
        self.assertTrue(os.path.isfile(path))

    def test_cleanup_rechecks_new_reference_after_confirmation(self):
        adapter, host = self.create_adapter("off")
        api_dir = os.path.join(self.directory.name, "new-reference-root", "api")
        os.makedirs(api_dir)
        completed = []
        with mock.patch("openframe_maya.adapter.endpoint_directory", return_value=api_dir):
            path = adapter.playblast(callback=lambda source, error: completed.append((source, error)))
        self.wait(lambda: completed)
        with self.server.lock:
            self.server.sources.clear()
        outcomes = []
        def confirmed(paths, preserved):
            with self.server.lock:
                self.server.sources.append({"id": "new", "name": "new", "path": path})
            return True
        adapter.clear_playblasts(confirm=confirmed, callback=lambda value, error: outcomes.append((value, error)))
        self.wait(lambda: outcomes)
        self.assertEqual(outcomes[-1][0]["removed"], [])
        self.assertIn(path, outcomes[-1][0]["preserved"])
        self.assertTrue(os.path.isfile(path))

    def test_async_sequence_import_failure_reports_error_and_keeps_completed_files(self):
        adapter, host = self.create_adapter("off")
        host.set_time(17)
        self.server.import_error = "Synthetic decoder rejection"
        completed = []
        result = adapter.playblast_sequence(os.path.join(self.directory.name, "rejected-import"),
                                             callback=lambda source, error: completed.append((source, error)))
        self.wait(lambda: completed)
        self.assertIsInstance(completed[0][1], OpenFrameError)
        self.assertIn("Synthetic decoder rejection", str(completed[0][1]))
        self.assertTrue(all(os.path.isfile(path) for path in result.paths))
        self.assertEqual(host.frame, 1)
        self.assertFalse(self.server.sources)
        self.assertFalse(any(method in ("viewer.assign", "window.activate") for method, _ in self.server.calls))


if __name__ == "__main__":
    unittest.main(verbosity=2)

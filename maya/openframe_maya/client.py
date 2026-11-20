"""OpenFrame API v1. UTF-8 JSON-RPC 2.0 objects, one object per LF.

Never resolves hostnames, consults proxies, runs code, launches processes or
contacts another address. Authentication comes from a private endpoint file.
"""
import json
import errno
import math
import os
import re
import select
import socket
import stat
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from .paths import local_path

MAX_REQUEST = 64 * 1024
MAX_RESPONSE = 16 * 1024 * 1024
MIN_TIMEOUT = 0.05
MAX_TIMEOUT = 120.0


def validate_timeout(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not MIN_TIMEOUT <= value <= MAX_TIMEOUT:
        raise ValueError("timeout must be a finite number in [0.05, 120] seconds")
    return float(value)


class OpenFrameError(RuntimeError):
    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code


def endpoint_directory():
    if os.name == "nt":
        root = os.environ.get("LOCALAPPDATA")
        if not root:
            raise OpenFrameError("LOCALAPPDATA is unavailable")
    else:
        root = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    return os.path.join(root, "OpenFrame", "api")


def discover_endpoints(directory=None):
    """Return endpoint paths only. Does not connect or expose token contents."""
    directory = directory or endpoint_directory()
    try:
        directory = local_path(directory, directory=True)
    except (OSError, ValueError):
        return []
    found = []
    for entry in os.scandir(directory):
        if entry.name.startswith("endpoint-") and entry.name.endswith(".json"):
            try:
                path = local_path(entry.path)
                found.append((os.stat(path).st_mtime_ns, path))
            except (OSError, ValueError):
                continue
    return [path for _, path in sorted(found, reverse=True)]


def _read_endpoint(path):
    path = local_path(path)
    info = os.stat(path, follow_symlinks=False)
    if info.st_size > 4096:
        raise OpenFrameError("Endpoint file is too large")
    if os.name != "nt" and (info.st_uid != os.getuid() or info.st_mode & (stat.S_IRWXG | stat.S_IRWXO)):
        raise OpenFrameError("Endpoint file must be owned by the current user and private")
    # On Windows the server protects the endpoint directory with a noninheriting
    # current-user+SYSTEM DACL. Client-created endpoint files are never written.
    with open(path, "r", encoding="utf-8") as source:
        endpoint = json.load(source)
    if not isinstance(endpoint, dict) or set(endpoint) != {"api", "host", "port", "token", "pid", "instance"}:
        raise OpenFrameError("Malformed endpoint file")
    if endpoint["api"] != 1 or isinstance(endpoint["api"], bool):
        raise OpenFrameError("Unsupported API version")
    if endpoint["host"] != "127.0.0.1":
        raise OpenFrameError("Only numeric 127.0.0.1 is permitted")
    if type(endpoint["port"]) is not int or not 1 <= endpoint["port"] <= 65535:
        raise OpenFrameError("Invalid endpoint port")
    if not isinstance(endpoint["token"], str) or not re.fullmatch(r"[0-9a-f]{64}", endpoint["token"]):
        raise OpenFrameError("Invalid endpoint authentication token")
    if type(endpoint["pid"]) is not int or endpoint["pid"] <= 0 or not isinstance(endpoint["instance"], str):
        raise OpenFrameError("Invalid endpoint identity")
    return endpoint


class Client:
    def __init__(self, endpoint_path=None, timeout=2.0, *, cancel_event=None):
        self.endpoint_path = os.fspath(endpoint_path) if endpoint_path else None
        self._timeout = validate_timeout(timeout)
        self._socket = None
        self._endpoint = None
        self._pending = bytearray()
        self._id = 0
        self._lock = threading.RLock()
        self._state_lock = threading.RLock()
        self._generation = 0
        self._cancel_serial = 0
        self._cancel_event = cancel_event

    @property
    def timeout(self):
        with self._state_lock:
            return self._timeout

    @timeout.setter
    def timeout(self, value):
        value = validate_timeout(value)
        with self._state_lock:
            self._timeout = value

    @property
    def connected(self):
        with self._state_lock:
            return self._socket is not None

    @property
    def instance_id(self):
        with self._state_lock:
            return self._endpoint["instance"] if self._endpoint else None

    def _active(self, sock, generation, deadline):
        if self._cancel_event is not None and self._cancel_event.is_set():
            raise OpenFrameError("OpenFrame operation cancelled")
        with self._state_lock:
            if generation != self._generation or sock is not self._socket:
                raise OpenFrameError("OpenFrame operation cancelled")
        if time.monotonic() >= deadline:
            raise OpenFrameError("OpenFrame RPC deadline exceeded")

    @contextmanager
    def _serialized(self):
        deadline = time.monotonic() + self.timeout
        with self._state_lock:
            cancel_serial = self._cancel_serial
        acquired = False
        try:
            while not acquired:
                if self._cancel_event is not None and self._cancel_event.is_set():
                    raise OpenFrameError("OpenFrame operation cancelled")
                with self._state_lock:
                    if cancel_serial != self._cancel_serial:
                        raise OpenFrameError("OpenFrame operation cancelled")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise OpenFrameError("OpenFrame RPC deadline exceeded")
                acquired = self._lock.acquire(timeout=min(.05, remaining))
            with self._state_lock:
                if cancel_serial != self._cancel_serial:
                    raise OpenFrameError("OpenFrame operation cancelled")
            yield deadline, cancel_serial
        finally:
            if acquired:
                self._lock.release()

    def _wait(self, sock, generation, deadline, *, writing=False):
        while True:
            self._active(sock, generation, deadline)
            remaining = deadline - time.monotonic()
            readable, writable, exceptional = select.select(
                [] if writing else [sock], [sock] if writing else [], [sock], min(.05, max(0.0, remaining)))
            self._active(sock, generation, deadline)
            if exceptional:
                raise OSError(sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR), "Local socket failed")
            if readable or writable:
                return

    def _disconnect(self, generation=None, *, cancel=False):
        # Never take the RPC lock here: stop/reconnect must interrupt a stalled
        # receive immediately, even with a user-selected 120-second deadline.
        with self._state_lock:
            if cancel:
                self._cancel_serial += 1
            if generation is not None and generation != self._generation:
                return
            sock = self._socket
            self._socket = self._endpoint = None
            self._generation += 1
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def connect(self):
        with self._serialized() as (deadline, cancel_serial):
            with self._state_lock:
                sock, endpoint, generation = self._socket, self._endpoint, self._generation
            if sock is not None and endpoint is not None:
                return self._rpc("status", {}, deadline, sock, endpoint, generation)
            if self.endpoint_path:
                selected = local_path(self.endpoint_path, output=True)
                candidates = [selected]
                if not os.path.exists(selected):
                    candidates = discover_endpoints(os.path.dirname(selected))
                    if len(candidates) != 1:
                        raise OpenFrameError("Selected endpoint disappeared; select a running OpenFrame instance")
            else:
                candidates = discover_endpoints()
            last_error = None
            for path in candidates:
                sock = None
                generation = None
                try:
                    with self._state_lock:
                        if self._cancel_serial != cancel_serial:
                            raise OpenFrameError("OpenFrame operation cancelled")
                    if time.monotonic() >= deadline:
                        raise OpenFrameError("OpenFrame RPC deadline exceeded")
                    endpoint = _read_endpoint(path)
                    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    sock.setblocking(False)
                    with self._state_lock:
                        if self._cancel_serial != cancel_serial:
                            sock.close()
                            raise OpenFrameError("OpenFrame operation cancelled")
                        self._generation += 1
                        generation = self._generation
                        self._socket, self._endpoint = sock, endpoint
                    # Deliberately avoid create_connection/getaddrinfo and proxy APIs.
                    result = sock.connect_ex(("127.0.0.1", endpoint["port"]))
                    if result not in (0, errno.EISCONN):
                        if result not in (errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY, 10035, 10036, 10037):
                            raise OSError(result, "Local socket connect failed")
                        self._wait(sock, generation, deadline, writing=True)
                        result = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                        if result:
                            raise OSError(result, "Local socket connect failed")
                    self._active(sock, generation, deadline)
                    if sock.getpeername()[0] != "127.0.0.1":
                        raise OpenFrameError("Unexpected peer address")
                    self._pending.clear()
                    status = self._rpc("status", {}, deadline, sock, endpoint, generation)
                    if not isinstance(status, dict) or status.get("instance") != endpoint["instance"]:
                        raise OpenFrameError("Endpoint instance identity changed")
                    if self.endpoint_path:
                        self.endpoint_path = path
                    return status
                except (OSError, ValueError, OpenFrameError) as error:
                    last_error = error
                    if generation is not None:
                        self._disconnect(generation)
                    elif sock:
                        sock.close()
                    with self._state_lock:
                        if self._cancel_serial != cancel_serial:
                            raise OpenFrameError("OpenFrame operation cancelled") from error
                    if time.monotonic() >= deadline:
                        break
            raise OpenFrameError("OpenFrame is unavailable: {}".format(last_error or "no endpoint files"))

    def close(self):
        self._disconnect(cancel=True)

    def call(self, method, **params):
        with self._serialized() as (deadline, _):
            with self._state_lock:
                sock, endpoint, generation = self._socket, self._endpoint, self._generation
            if sock is None or endpoint is None:
                raise OpenFrameError("Client is disconnected")
            return self._rpc(method, params, deadline, sock, endpoint, generation)

    def _rpc(self, method, params, deadline, sock, endpoint, generation):
        started = False
        try:
            self._active(sock, generation, deadline)
            if not isinstance(method, str) or not method or len(method) > 96:
                raise ValueError("Invalid method")
            self._id += 1
            request = {"jsonrpc": "2.0", "api": 1, "id": self._id, "token": endpoint["token"],
                       "method": method, "params": params}
            payload = json.dumps(request, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
            if len(payload) - 1 > MAX_REQUEST:
                raise OpenFrameError("Request exceeds the 64 KiB limit")
            started = True
            try:
                sent = 0
                while sent < len(payload):
                    self._wait(sock, generation, deadline, writing=True)
                    try:
                        count = sock.send(payload[sent:])
                    except BlockingIOError:
                        continue
                    if not count:
                        raise OpenFrameError("OpenFrame closed the connection")
                    sent += count
                while b"\n" not in self._pending:
                    self._wait(sock, generation, deadline)
                    try:
                        chunk = sock.recv(65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise OpenFrameError("OpenFrame closed the connection")
                    self._pending.extend(chunk)
                    if len(self._pending) > MAX_RESPONSE:
                        raise OpenFrameError("Response exceeds the size limit")
                line, remainder = self._pending.split(b"\n", 1)
                self._pending = bytearray(remainder)
                response = json.loads(line.decode("utf-8"), parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
                self._active(sock, generation, deadline)
                if not isinstance(response, dict) or response.get("jsonrpc") != "2.0":
                    raise OpenFrameError("Malformed or mismatched response")
                if response.get("id") != self._id:
                    failure = response.get("error")
                    if ("id" in response and response["id"] is None and isinstance(failure, dict)
                            and failure.get("code") in (-32700, -32600, -32003, -32004)):
                        raise OpenFrameError(str(failure.get("message", "Protocol error")), failure["code"])
                    raise OpenFrameError("Malformed or mismatched response")
                if "error" in response:
                    failure = response["error"]
                    if not isinstance(failure, dict):
                        raise OpenFrameError("Malformed error response")
                    raise OpenFrameError(str(failure.get("message", "API error")), failure.get("code"))
                if set(response) != {"jsonrpc", "id", "result"}:
                    raise OpenFrameError("Malformed result response")
                return response["result"]
            except OpenFrameError as error:
                if error.code is None or error.code in (-32001, -32003, -32004):
                    self._disconnect(generation)
                raise
            except (OSError, UnicodeError, ValueError) as error:
                self._disconnect(generation)
                raise OpenFrameError("OpenFrame transport failed: {}".format(error)) from error
        except OpenFrameError as error:
            if started and (error.code is None or error.code in (-32001, -32003, -32004)):
                self._disconnect(generation)
            raise

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

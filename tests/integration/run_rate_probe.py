"""Sustained synthetic Maya event rates against an explicitly owned real API."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import sys
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, os.environ.get("OPENFRAME_TEST_MAYA_PATH", str(root / "maya")))
sys.path.insert(0, str(root / "tests" / "integration"))
from openframe_maya import Adapter, Client, OpenFrameError
from test_maya import FakeMaya


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--owned-instance", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--fps", type=int, choices=(60, 120), required=True)
    args = parser.parse_args()
    with Client(args.endpoint) as client:
        if client.call("status")["instance"] != args.owned_instance:
            raise RuntimeError("Not the explicitly owned synthetic process")
        client.call("project.new")
        client.call("media.import", paths=[args.fixture])
        deadline = time.monotonic() + 30
        while client.call("status")["busy"]:
            if time.monotonic() > deadline:
                raise RuntimeError("Synthetic import timed out")
            time.sleep(0.05)
    calls, errors = [], []

    class CountingClient(Client):
        def call(self, method, **params):
            calls.append((time.monotonic(), method))
            try:
                return super().call(method, **params)
            except OpenFrameError as error:
                errors.append({"method": method, "code": error.code, "error": str(error)})
                raise

    host = FakeMaya()
    adapter = Adapter(host=host, client_factory=lambda: CountingClient(args.endpoint, timeout=0.75))
    adapter.start("maya_to_openframe")
    deadline = time.monotonic() + 3
    while not adapter.state["connected"] and time.monotonic() < deadline:
        host.pump()
        time.sleep(0.005)
    started = time.monotonic()
    try:
        if not adapter.state["connected"]:
            raise RuntimeError("Adapter did not connect")
        for frame in range(args.fps * 3):
            target = started + frame / args.fps
            while time.monotonic() < target:
                host.pump()
                time.sleep(0.001)
            host.set_time(frame % 120)
            host.pump()
        time.sleep(0.1)
        host.pump()
    finally:
        adapter.stop()
    left, maximum = 0, 0
    for right in range(len(calls)):
        while calls[right][0] - calls[left][0] >= 1:
            left += 1
        maximum = max(maximum, right - left + 1)
    with Client(args.endpoint) as client:
        final_frame = client.call("status")["frame"]
        client.call("project.new", empty=True)
    report = {"input_fps": args.fps, "input_events": args.fps * 3, "duration_seconds": time.monotonic() - started,
              "request_count": len(calls), "maximum_requests_in_one_second": maximum,
              "requests_by_method": dict(Counter(method for _, method in calls)), "errors": errors,
              "final_frame": final_frame, "expected_final_frame": host.frame,
              "adapter_module": sys.modules["openframe_maya.adapter"].__file__,
              "success": not errors and maximum <= 120 and final_frame == host.frame}
    (Path(args.output_dir) / "rate-report-{}.json".format(args.fps)).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

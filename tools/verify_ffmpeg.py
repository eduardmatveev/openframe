"""Validate the exact FFmpeg DLLs used for a local OpenFrame deployment.

This checks compiled capabilities, not an all-process network trace.
No network activity is performed by this script.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="SDK bin or packaged application directory")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve(strict=True)
    candidates = list(directory.glob("avformat-*.dll"))
    if len(candidates) != 1:
        raise SystemExit("Expected exactly one avformat DLL")
    with os.add_dll_directory(str(directory)):
        library = ctypes.CDLL(str(candidates[0]))
        library.avformat_configuration.restype = ctypes.c_char_p
        library.avformat_license.restype = ctypes.c_char_p
        library.avio_enum_protocols.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
        library.avio_enum_protocols.restype = ctypes.c_char_p
        configuration = library.avformat_configuration().decode("utf-8")
        protocols = {}
        for name, output in (("input", 0), ("output", 1)):
            opaque = ctypes.c_void_p()
            values = []
            while value := library.avio_enum_protocols(ctypes.byref(opaque), output):
                values.append(value.decode("ascii"))
            protocols[name] = values
        checks = {
            "network_disabled": "--disable-network" in configuration.split(),
            "protocols_disabled_by_default": "--disable-protocols" in configuration.split(),
            "only_local_protocols": all(set(values) <= {"file", "pipe"} for values in protocols.values()),
            "no_gpl_or_nonfree": all(flag not in configuration.split() for flag in ("--enable-gpl", "--enable-nonfree")),
        }
        result = {"library": str(candidates[0]), "license": library.avformat_license().decode(),
                  "configuration": configuration, "protocols": protocols, "checks": checks,
                  "passed": all(checks.values()),
                  "scope": "Compiled FFmpeg capabilities only; does not replace runtime process tracing."}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "protocols": protocols, "checks": checks}))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()

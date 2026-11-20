"""Compare public Python call signatures without copying a reference implementation.

Only class/method names, argument names and literal defaults are inspected.
Method bodies, protocol constants and other implementation details are ignored.
This is a signature check, not a claim of behavioral or wire compatibility.
"""
import argparse
import ast
import json
from pathlib import Path


def signatures(path):
    module = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    cls = next(node for node in module.body if isinstance(node, ast.ClassDef)
               and node.name == "KeyframePro2ClientApi")
    methods = {}
    for method in cls.body:
        if not isinstance(method, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if method.name.startswith("_") and method.name != "__init__":
            continue
        positional = method.args.posonlyargs + method.args.args
        names = [argument.arg for argument in positional if argument.arg not in ("self", "cls")]
        defaults = [ast.literal_eval(value) for value in method.args.defaults]
        methods[method.name] = {"arguments": names, "defaults": defaults}
    return methods


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    own = Path(__file__).resolve().parents[1] / "maya" / "openframe_maya" / "compatibility.py"
    reference = signatures(args.reference)
    implementation = signatures(own)
    # Low-level transport routines are not the documented method facade. The
    # authenticated OpenFrame wire protocol is intentionally a separate API.
    transport_helpers = {"send", "recv", "is_valid_reply"}
    missing, differences, incompatible = [], {}, []
    for name, signature in reference.items():
        if name in transport_helpers:
            continue
        if name not in implementation:
            missing.append(name)
        elif signature != implementation[name]:
            differences[name] = {"reference": signature, "openframe": implementation[name]}
            old_args, new_args = signature["arguments"], implementation[name]["arguments"]
            old_required = len(old_args) - len(signature["defaults"])
            new_required = len(new_args) - len(implementation[name]["defaults"])
            if new_args[:len(old_args)] != old_args or new_required > old_required:
                incompatible.append(name)
    checked_count = len(reference) - len(transport_helpers.intersection(reference))
    report = {"scope": "Public call signatures only; method bodies not used",
              "reference": args.reference.name, "referenceMethodCount": len(reference),
              "documentedFacadeMethodsChecked": checked_count,
              "callCompatible": checked_count - len(missing) - len(incompatible),
              "missing": missing, "signatureDifferences": differences,
              "incompatibleCalls": incompatible,
              "outsideFacadeScope": sorted(transport_helpers.intersection(reference)),
              "behavioralParity": "Not established by a signature comparison",
              "passed": not missing and not incompatible}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()

"""Own-scene Maya GUI probe for the explicitly passed playblast flags.

Run only in a separate Maya process with isolated MAYA_APP_DIR. No API server,
existing scene, saved preferences or user media is accessed.
"""
import json
import math
import os
from pathlib import Path
import struct
import sys
import traceback
import wave


def run(directory):
    import maya.cmds as cmds
    import maya.api.OpenMaya as om

    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "report.json"
    if report_path.exists():
        raise FileExistsError("Choose a fresh probe directory")
    report = {"mayaVersion": cmds.about(version=True), "cases": []}
    messages = []
    callback = om.MCommandMessage.addCommandOutputCallback(
        lambda message, kind, *args: messages.append({"kind": int(kind), "message": message}))
    try:
        cmds.file(new=True, force=True)
        cube = cmds.polyCube(name="OpenFrameSyntheticOptionsProbe")[0]
        cmds.setKeyframe(cube, attribute="translateX", time=1, value=-1)
        cmds.setKeyframe(cube, attribute="translateX", time=4, value=1)
        cmds.currentUnit(time="film")
        cmds.playbackOptions(minTime=1, maxTime=4)
        cmds.currentTime(1)
        panels = cmds.getPanel(type="modelPanel") or []
        if panels:
            cmds.setFocus(panels[-1])
        base = dict(startTime=1, endTime=4, widthHeight=(128, 72), percent=100,
                    quality=70, viewer=False, forceOverwrite=False, clearCache=False,
                    showOrnaments=False, offScreen=True, format="avi", compression="none")
        added = dict(throwOnError=True, useTraxSounds=False, combineSound=False, sound="")
        cases = [("baseline", {})]
        cases += [("only_" + key, {key: value}) for key, value in added.items()]
        cases += [("all_new_options", added)]
        cases += [("all_except_" + key, {k: v for k, v in added.items() if k != key})
                  for key in added]
        for name, options in cases:
            path = output / (name + ".avi")
            messages.clear()
            item = {"name": name, "extraOptions": options}
            try:
                item["returned"] = cmds.playblast(filename=str(path), **base, **options)
                item["size"] = path.stat().st_size if path.is_file() else 0
                item["passed"] = item["size"] > 0
            except Exception as error:
                item.update(passed=False, error=str(error), traceback=traceback.format_exc())
            item["messages"] = list(messages)
            report["cases"].append(item)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        # Verify omission also with a real active Time Slider sound. These are
        # new synthetic samples, never an existing scene or user audio file.
        audio_path = output / "own-tone.wav"
        with wave.open(str(audio_path), "wb") as audio:
            audio.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
            audio.writeframes(b"".join(struct.pack("<h", round(6000 * math.sin(2 * math.pi * 440 * i / 48000)))
                                       for i in range(48000)))
        node = cmds.sound(file=str(audio_path), name="OpenFrameSyntheticOptionsSound", offset=0)
        controls = cmds.lsUI(type="timeControl", long=True) or []
        if len(controls) != 1:
            raise RuntimeError("Expected one Time Slider in the fresh probe Maya")
        cmds.timeControl(controls[0], edit=True, sound=node, displaySound=True)
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "maya"))
        from openframe_maya.adapter import MayaHost
        from openframe_maya.capture import CaptureSettings
        host = MayaHost()
        sound = host.sound_snapshot()
        sound_cases = (("active_slider_sound_omitted", CaptureSettings(width=128, height=72), None),
                       ("active_slider_sound_explicit", CaptureSettings(width=128, height=72), sound),
                       ("active_slider_sound_include_false", CaptureSettings(width=128, height=72, include_sound=False), None))
        for name, settings, selected_sound in sound_cases:
            path = output / (name + ".avi")
            messages.clear()
            item = {"name": name, "sliderSound": node, "soundArgument": selected_sound.node if selected_sound else None}
            try:
                item["returned"] = host.capture(str(path), 1, 4, settings, selected_sound)
                item["size"] = path.stat().st_size if path.is_file() else 0
                item["passed"] = item["size"] > 0
            except Exception as error:
                item.update(passed=False, error=str(error), traceback=traceback.format_exc())
            item["messages"] = list(messages)
            report["cases"].append(item)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    finally:
        om.MMessage.removeCallback(callback)
    return report


if __name__ == "__main__":
    run(os.environ["OPENFRAME_PLAYBLAST_PROBE_OUTPUT"])

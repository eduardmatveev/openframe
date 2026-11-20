"""Small Maya-native control window. Never edits shelves or global preferences."""
import os
from .adapter import Adapter, SyncMapping
from .capture import CaptureSettings
from .client import discover_endpoints, validate_timeout
from .paths import local_path

_controller = None


class Window:
    def __init__(self):
        import maya.cmds as cmds
        try:
            from PySide6.QtCore import QTimer
        except ImportError:
            from PySide2.QtCore import QTimer
        self.cmds = cmds
        self.adapter = None
        self._displayed_config = None
        self.window = cmds.window(title="OpenFrame · Maya", widthHeight=(480, 740), sizeable=True)
        cmds.scrollLayout(childResizable=True)
        cmds.columnLayout(adjustableColumn=True, rowSpacing=6)
        cmds.text(label="Authenticated local connection · 127.0.0.1", align="left")
        self.endpoint = cmds.textFieldButtonGrp(label="Endpoint", buttonLabel="Find", buttonCommand=self.find_endpoint)
        self.timeout = cmds.floatFieldGrp(label="API timeout (seconds)", numberOfFields=1,
                                         value1=.75, precision=3, changeCommand=self.configure)
        cmds.text(label="0.05–120 seconds per API request; capture duration is independent.", align="left")
        self.status = cmds.text(label="Disconnected", align="left")
        cmds.rowLayout(numberOfColumns=3, adjustableColumn=3)
        cmds.button(label="Connect", command=self.connect)
        cmds.button(label="Disconnect", command=self.disconnect)
        cmds.button(label="Refresh status", command=self.refresh)
        cmds.setParent("..")
        cmds.separator(style="in")
        self.direction = cmds.optionMenuGrp(label="Sync direction", changeCommand=self.configure)
        for label in ("Off", "Maya → OpenFrame", "OpenFrame → Maya", "Bidirectional"):
            cmds.menuItem(label=label)
        self.offset = cmds.floatFieldGrp(label="Offset", numberOfFields=1, value1=0, changeCommand=self.configure)
        self.multiplier = cmds.floatFieldGrp(label="Multiplier", numberOfFields=1, value1=1, changeCommand=self.configure)
        self.from_range = cmds.checkBoxGrp(label="From range start", numberOfCheckBoxes=1, value1=False, changeCommand=self.configure)
        cmds.text(label="Enabled: Maya frame 1 maps to OpenFrame range start.\nOffset is applied after multiplication.", align="left")
        cmds.rowLayout(numberOfColumns=2)
        cmds.button(label="Send Maya range", command=lambda *_: self.perform(lambda: self.adapter.send_range()))
        cmds.button(label="Receive OpenFrame range", command=lambda *_: self.perform(lambda: self.adapter.receive_range()))
        cmds.setParent("..")
        cmds.separator(style="in")
        cmds.text(label="Time control frame offset", align="left")
        self.tc_offset = cmds.intFieldGrp(label="Display offset", numberOfFields=1, value1=1)
        cmds.rowLayout(numberOfColumns=2)
        cmds.button(label="Apply display offset", command=lambda *_: self.perform(lambda: self.adapter.request("timecode.set", offset=cmds.intFieldGrp(self.tc_offset, query=True, value1=True), enabled=True)))
        cmds.button(label="Disable override", command=lambda *_: self.perform(lambda: self.adapter.request("timecode.set", offset=0, enabled=False)))
        cmds.setParent("..")
        cmds.separator(style="in")
        cmds.text(label="Viewer", align="left")
        cmds.rowLayout(numberOfColumns=4)
        cmds.button(label="Single", command=lambda *_: self.layout("single"))
        cmds.button(label="Horizontal", command=lambda *_: self.layout("horizontal"))
        cmds.button(label="Vertical", command=lambda *_: self.layout("vertical"))
        cmds.button(label="Swap A/B", command=self.swap)
        cmds.setParent("..")
        cmds.separator(style="in")
        cmds.text(label="Playblast", align="left")
        self.viewer = cmds.optionMenuGrp(label="Open in viewer")
        for label in ("A", "B", "None"):
            cmds.menuItem(label=label)
        self.format = cmds.optionMenuGrp(label="Output format")
        for label in ("avi", "webm", "png", "jpg", "bmp", "tif"):
            cmds.menuItem(label=label)
        self.dimensions = cmds.intFieldGrp(label="Width / height", numberOfFields=2, value1=960, value2=540)
        self.capture_quality = cmds.intFieldGrp(label="Quality / scale %", numberOfFields=2, value1=70, value2=100)
        self.capture_padding = cmds.intFieldGrp(label="Image frame padding", numberOfFields=1, value1=4)
        self.capture_ornaments = cmds.checkBoxGrp(label="Show ornaments", numberOfCheckBoxes=1, value1=False)
        self.capture_offscreen = cmds.checkBoxGrp(label="Render offscreen", numberOfCheckBoxes=1, value1=True)
        self.capture_sound = cmds.checkBoxGrp(label="Movie: Time Slider sound", numberOfCheckBoxes=1, value1=True)
        self.blast_tc = cmds.checkBoxGrp(label="Use Maya start as display offset", numberOfCheckBoxes=1, value1=True, columnWidth2=(220, 80))
        cmds.text(label="Explicit settings above; saved Maya Playblast options are not read.\nPNG/JPG/BMP/TIF create one image sequence source without audio.\nSuccessful capture returns to its first frame; failure restores previous time.\nWebM requires Maya WebM support.", align="left")
        cmds.button(label="Create playblast and import", command=self.playblast)
        cmds.rowLayout(numberOfColumns=2)
        cmds.button(label="Open Temp Folder", command=self.open_temp)
        cmds.button(label="Clear Temp Folder", command=self.clear_temp)
        cmds.setParent("..")
        cmds.showWindow(self.window)
        self.timer = QTimer()
        self.timer.setInterval(500)
        self.timer.timeout.connect(self.refresh)
        self.timer.start()
        cmds.scriptJob(uiDeleted=[self.window, self.close], runOnce=True)
        self.find_endpoint()

    def find_endpoint(self, *_):
        endpoints = discover_endpoints()
        self.cmds.textFieldButtonGrp(self.endpoint, edit=True, text=endpoints[0] if endpoints else "")
        if not endpoints:
            self.cmds.text(self.status, edit=True, label="Start OpenFrame, then select Find.")

    def connect(self, *_):
        try:
            timeout = validate_timeout(self.cmds.floatFieldGrp(self.timeout, query=True, value1=True))
            path = self.cmds.textFieldButtonGrp(self.endpoint, query=True, text=True).strip()
            adapter = Adapter(path or None, timeout=timeout)
            self.disconnect()
            self.adapter = adapter.start()
            self._displayed_config = None
            self.configure()
        except Exception as error:
            self.cmds.text(self.status, edit=True, label=str(error)[:200])
            self.cmds.warning("OpenFrame: " + str(error))

    def disconnect(self, *_):
        if self.adapter:
            self.adapter.stop()
            self.adapter = None
        self.cmds.text(self.status, edit=True, label="Disconnected")

    def configure(self, *_):
        if not self.adapter:
            return
        def action():
            direction = ("off", "maya_to_openframe", "openframe_to_maya", "bidirectional")[self.cmds.optionMenuGrp(self.direction, query=True, select=True) - 1]
            mapping = SyncMapping(self.cmds.floatFieldGrp(self.offset, query=True, value1=True),
                                  self.cmds.floatFieldGrp(self.multiplier, query=True, value1=True),
                                  self.cmds.checkBoxGrp(self.from_range, query=True, value1=True))
            timeout = self.cmds.floatFieldGrp(self.timeout, query=True, value1=True)
            self.adapter.configure(direction=direction, mapping=mapping, timeout=timeout)
        self.perform(action)

    def refresh(self, *_):
        if not self.cmds.window(self.window, exists=True):
            self.close()
            return
        if self.adapter:
            state = self.adapter.state
            label = "Connected · frame {} · {}".format(state["status"].get("frame", "?"), state["config"]["direction"]) if state["connected"] else "Reconnecting · " + state["error"]
            self.cmds.text(self.status, edit=True, label=label[:200])
            config = state["config"]
            if config != self._displayed_config:
                self._displayed_config = config
                index = ("off", "maya_to_openframe", "openframe_to_maya", "bidirectional").index(config["direction"]) + 1
                self.cmds.optionMenuGrp(self.direction, edit=True, select=index)
                self.cmds.floatFieldGrp(self.offset, edit=True, value1=config["offset"])
                self.cmds.floatFieldGrp(self.multiplier, edit=True, value1=config["multiplier"])
                self.cmds.checkBoxGrp(self.from_range, edit=True, value1=config["fromRangeStart"])
                self.cmds.floatFieldGrp(self.timeout, edit=True, value1=config["timeoutSeconds"])

    def perform(self, action):
        try:
            if not self.adapter:
                raise RuntimeError("Connect to OpenFrame first")
            return action()
        except Exception as error:
            self.cmds.text(self.status, edit=True, label=str(error)[:200])
            self.cmds.warning("OpenFrame: " + str(error))

    def layout(self, mode):
        self.perform(lambda: self.adapter.request("viewer.set", mode="single" if mode == "single" else "split", splitVertical=mode == "vertical"))

    def swap(self, *_):
        def received_a(a, error):
            if error:
                self.cmds.warning(str(error))
                return
            def received_b(b, error):
                if error:
                    self.cmds.warning(str(error))
                    return
                self.adapter.request("viewer.assign", pane=0, id=b.get("id", ""))
                self.adapter.request("viewer.assign", pane=1, id=a.get("id", ""))
            self.adapter.request("viewer.get", pane=1, callback=received_b)
        self.perform(lambda: self.adapter.request("viewer.get", pane=0, callback=received_a))

    def playblast(self, *_):
        def action():
            format_name = self.cmds.optionMenuGrp(self.format, query=True, value=True)
            image = format_name not in ("avi", "webm")
            settings = CaptureSettings(
                format_name="image" if image else format_name,
                compression=format_name if image else None,
                width=self.cmds.intFieldGrp(self.dimensions, query=True, value1=True),
                height=self.cmds.intFieldGrp(self.dimensions, query=True, value2=True),
                quality=self.cmds.intFieldGrp(self.capture_quality, query=True, value1=True),
                percent=self.cmds.intFieldGrp(self.capture_quality, query=True, value2=True),
                padding=self.cmds.intFieldGrp(self.capture_padding, query=True, value1=True),
                show_ornaments=self.cmds.checkBoxGrp(self.capture_ornaments, query=True, value1=True),
                off_screen=self.cmds.checkBoxGrp(self.capture_offscreen, query=True, value1=True),
                include_sound=self.cmds.checkBoxGrp(self.capture_sound, query=True, value1=True))
            self.adapter.capture(settings=settings,
                viewer=self.cmds.optionMenuGrp(self.viewer, query=True, value=True),
                timecode_offset=self.cmds.checkBoxGrp(self.blast_tc, query=True, value1=True),
                callback=lambda result, error: self.cmds.text(self.status, edit=True, label=str(error) if error else "Playblast imported: " + result["name"]))
        self.perform(action)

    def open_temp(self, *_):
        def action():
            path = self.adapter.playblast_directory()
            window = self.cmds.window(title="OpenFrame · Local captures", widthHeight=(560, 280))
            self.cmds.columnLayout(adjustableColumn=True, rowSpacing=6)
            self.cmds.textField(text=path, editable=False)
            listing = self.cmds.textScrollList(numberOfRows=12)
            for entry in sorted(os.scandir(path), key=lambda item: item.name):
                try:
                    if entry.is_dir(follow_symlinks=False):
                        local_path(entry.path, directory=True)
                        label = entry.name + "/  ·  image sequence directory"
                    else:
                        local_path(entry.path)
                        label = "{}  ·  {} bytes".format(entry.name, entry.stat(follow_symlinks=False).st_size)
                    self.cmds.textScrollList(listing, edit=True, append=label)
                except (OSError, ValueError):
                    continue
            self.cmds.text(label="Current Maya session only. External file browsers are not launched.", align="left")
            self.cmds.showWindow(window)
        self.perform(action)

    def clear_temp(self, *_):
        def confirm(paths, preserved):
            scope = self.adapter.playblast_directory()
            names = "\n".join(os.path.relpath(path, scope) for path in paths[:128])
            if len(paths) > 128:
                names += "\n... and {} additional owned paths in this scope".format(len(paths) - 128)
            message = "Folder:\n{}\n\nDelete these {} unused files / empty capture directories created by this Maya session?\n\n{}\n\n{} loaded or changed paths will be preserved. Directories containing untracked files are kept.".format(scope, len(paths), names, len(preserved))
            return self.cmds.confirmDialog(title="Clear OpenFrame captures", message=message,
                                           button=["Delete", "Cancel"], defaultButton="Cancel",
                                           cancelButton="Cancel", dismissString="Cancel") == "Delete"
        def completed(result, error):
            label = str(error) if error else "Cancelled" if result["cancelled"] else "Removed {} unused captures; preserved {} loaded or changed files".format(len(result["removed"]), len(result["preserved"]))
            self.cmds.text(self.status, edit=True, label=label)
        self.perform(lambda: self.adapter.clear_playblasts(confirm=confirm, callback=completed))

    def close(self, *_):
        self.timer.stop()
        if self.adapter:
            self.adapter.stop()
            self.adapter = None


def show():
    global _controller
    if _controller:
        _controller.close()
        if _controller.cmds.window(_controller.window, exists=True):
            _controller.cmds.deleteUI(_controller.window)
    _controller = Window()
    return _controller

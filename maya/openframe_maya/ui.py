"""OpenFrame's Maya window. Maya operations and widgets stay on the main thread."""
import json
import os
import tempfile

from .adapter import Adapter, MayaHost, SyncMapping
from .capture import CaptureSettings
from .client import MAX_TIMEOUT, MIN_TIMEOUT, discover_endpoints, viewer_layout_params
from .paths import local_path

_WINDOW_NAME = "openFrameMayaAdapterWindow"
_controller = globals().get("_controller")  # Keep the live controller across importlib.reload().
_CAPTURE = CaptureSettings()
_CHOICES = {"viewer": ("A", "B", "None"), "format": ("avi", "webm", "png", "jpg", "bmp", "tif"),
            "direction": ("off", "maya_to_openframe", "openframe_to_maya", "bidirectional"),
            "tab": ("Playblast", "Sync")}
_LIMITS = {"width": (4, 8192), "height": (4, 8192), "quality": (0, 100), "percent": (10, 100),
           "padding": (1, 12), "offset": (-1e9, 1e9), "multiplier": (-1000., 1000.),
           "timeout": (MIN_TIMEOUT, MAX_TIMEOUT), "display_offset": (-2147483648, 2147483647)}
_DEFAULTS = {"viewer": "A", "format": "avi", "width": _CAPTURE.width, "height": _CAPTURE.height,
             "quality": _CAPTURE.quality, "percent": _CAPTURE.percent, "padding": _CAPTURE.padding,
             "ornaments": _CAPTURE.show_ornaments, "offscreen": _CAPTURE.off_screen,
             "include_audio": _CAPTURE.include_sound, "timecode": True, "direction": "off",
             "offset": 0., "multiplier": 1., "from_range": False, "timeout": .75, "display_offset": 1,
             "tab": "Playblast", "advanced": False, "geometry": None, "audio": None}
_SYNC_KEYS = {"direction": "direction", "offset": "offset", "multiplier": "multiplier",
              "fromRangeStart": "from_range", "timeoutSeconds": "timeout"}
_AUTO_TIP = ("Automatic connects to the only instance that answers an authenticated status request. "
             "A previous connection is preferred if it still responds. Otherwise, if several respond, choose one. "
             "Background retries stay with the connected instance until you press Connect again.")


def _valid_setting(key, value):
    if key in _CHOICES:
        return value in _CHOICES[key]
    if key in _LIMITS:
        low, high = _LIMITS[key]
        numeric = type(value) in (int, float) if type(_DEFAULTS[key]) is float else type(value) is int
        return (numeric and low <= value <= high and (key != "multiplier" or value != 0)
                and (key not in ("width", "height") or value % 4 == 0))
    if key == "geometry":
        return (isinstance(value, list) and len(value) == 4 and all(type(v) is int for v in value)
                and all(abs(v) <= 100000 for v in value[:2]) and all(0 < v <= 32768 for v in value[2:]))
    if key == "audio":
        return (isinstance(value, dict) and set(value) == {"scene", "node", "uuid"}
                and all(isinstance(v, str) and len(v) <= 4096 and "\0" not in v for v in value.values())
                and bool(value["node"]) and bool(value["uuid"]))
    return type(value) is bool


def load_settings(path):
    values = dict(_DEFAULTS)
    try:
        with open(path, encoding="utf-8") as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            return values
        data = json.loads(raw)
        if not isinstance(data, dict) or type(data.get("version")) is not int or data["version"] != 1:
            return values
        values.update((key, data[key]) for key in values if key in data and _valid_setting(key, data[key]))
    except (OSError, ValueError, RecursionError):
        pass
    return values


def save_settings(path, values):
    """Replace the previous complete snapshot only after a new one has been written."""
    data = {key: values[key] for key in _DEFAULTS if key in values and _valid_setting(key, values[key])}
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=os.path.dirname(path),
                                         prefix=".openframe-maya-", suffix=".tmp", delete=False) as stream:
            temp_path = stream.name
            json.dump(dict(data, version=1), stream, ensure_ascii=False, allow_nan=False)
        os.replace(temp_path, path)
    finally:
        if temp_path and os.path.exists(temp_path):
            os.unlink(temp_path)


def _instance_label(path):
    name = os.path.splitext(os.path.basename(path))[0]
    if name.startswith("endpoint-"):
        name = name[len("endpoint-"):]
    return name if len(name) <= 24 else name[:14] + "…" + name[-8:]


class Window:
    def __init__(self):
        import maya.cmds as cmds
        from maya import OpenMayaUI
        try:
            from PySide6 import QtCore, QtGui, QtWidgets
            import shiboken6 as shiboken
        except ImportError:
            from PySide2 import QtCore, QtGui, QtWidgets
            import shiboken2 as shiboken

        self.cmds, self.widgets, self.qt = cmds, QtWidgets, QtCore.Qt
        self._point = QtCore.QPoint
        self.host = MayaHost()
        self.adapter = None
        self._closed = self._ready = self._dirty = False
        self._audio_signature = None
        self._remote_config = {}
        self.controls = {}
        self.settings_path = os.path.join(cmds.internalVar(userPrefDir=True), "openframe-maya-window.json")
        self.values = load_settings(self.settings_path)
        self.window = cmds.window(_WINDOW_NAME, title="OpenFrame · Maya", widthHeight=(480, 460), sizeable=True)
        form = cmds.formLayout()
        parent = OpenMayaUI.MQtUtil.getCurrentParent()
        self.body = QtWidgets.QWidget(shiboken.wrapInstance(int(parent), QtWidgets.QWidget))
        self.body.setObjectName("openFrameControls")
        control = OpenMayaUI.MQtUtil.addWidgetToMayaLayout(shiboken.getCppPointer(self.body)[0], int(parent))
        cmds.formLayout(form, edit=True, attachForm=[(control, edge, 0) for edge in ("left", "right", "top", "bottom")])

        class WindowEvents(QtCore.QObject):
            def eventFilter(events, widget, event):
                kind = event.type()
                if kind == QtCore.QEvent.Resize and hasattr(widget, "_message"):
                    self._elide(widget)
                if (self._ready and not self._closed and kind in (QtCore.QEvent.Move, QtCore.QEvent.Resize, QtCore.QEvent.Close)
                        and (widget is self.body or widget is self.shell)):
                    if kind != QtCore.QEvent.Close:
                        self._remember_geometry()
                    elif kind == QtCore.QEvent.Close:
                        self._remember_geometry()
                        self.flush()
                return False

        self.events = WindowEvents(self.body)
        self.body.installEventFilter(self.events)
        self.save_timer = QtCore.QTimer()
        self.save_timer.setSingleShot(True)
        self.save_timer.setInterval(300)
        self.save_timer.timeout.connect(self.flush)
        self.timer = QtCore.QTimer()
        self.timer.setInterval(500)
        self.timer.timeout.connect(self.refresh)
        self._build()
        self._restore_controls()
        self.find_endpoint()
        self.refresh_audio_sources()
        QtWidgets.QApplication.instance().aboutToQuit.connect(self.close)
        cmds.showWindow(self.window)
        icon_path = os.path.join(os.path.dirname(__file__), "icons", "openframe-maya.png")
        self.shell.setWindowIcon(QtGui.QIcon(icon_path))
        cmds.scriptJob(uiDeleted=[self.window, self.close], runOnce=True)
        self.body.destroyed.connect(self.close)
        self.shell.installEventFilter(self.events)
        self._restore_geometry()
        self._ready = True
        self.timer.start()

    @property
    def shell(self):
        # Maya can replace its native window while showing the form.
        return self.body.window()

    def _notice(self):
        label = self.widgets.QLabel()
        label._message = ""
        label.setTextFormat(self.qt.PlainText)
        label.setSizePolicy(self.widgets.QSizePolicy.Ignored, self.widgets.QSizePolicy.Fixed)
        label.setMinimumHeight(label.fontMetrics().height() + 2)
        label.installEventFilter(self.events)
        return label

    def _elide(self, label):
        text = label.fontMetrics().elidedText(label._message, self.qt.ElideRight, max(0, label.contentsRect().width()))
        if text != label.text():
            label.setText(text)

    def _message(self, label, text, detail=None):
        label._message = text
        label.setToolTip(detail or text)
        self._elide(label)

    def _button(self, text, callback):
        button = self.widgets.QPushButton(text)
        button.setAutoDefault(False)
        button.clicked.connect(lambda _checked=False: callback())
        return button

    def _row(self, *widgets):
        row = self.widgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        for widget in widgets:
            row.addWidget(widget)
        return row

    def _combo(self):
        combo = self.widgets.QComboBox()
        combo.setSizeAdjustPolicy(self.widgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        combo.setMinimumContentsLength(6)
        combo.setSizePolicy(self.widgets.QSizePolicy.Expanding, self.widgets.QSizePolicy.Fixed)
        return combo

    def _choice(self, key, labels=None):
        combo = self._combo()
        for value, label in zip(_CHOICES[key], labels or _CHOICES[key]):
            combo.addItem(label, value)
        combo.currentIndexChanged.connect(lambda _index: self._changed(key, combo.currentData()))
        self.controls[key] = combo
        return combo

    def _spin(self, key):
        if type(_DEFAULTS[key]) is float:
            box = self.widgets.QDoubleSpinBox()
            box.setDecimals(3 if key == "timeout" else 6)
        else:
            box = self.widgets.QSpinBox()
        box.setRange(*_LIMITS[key])
        box.setKeyboardTracking(False)
        if key in ("width", "height"):
            box.setSingleStep(4)
            box.setToolTip("Maya requires dimensions in multiples of four.")
        if key == "timeout":
            box.setSingleStep(.05)
            box.setSuffix(" s")
        box.valueChanged.connect(lambda value: self._changed(key, value))
        self.controls[key] = box
        return box

    def _check(self, key, label):
        check = self.widgets.QCheckBox(label)
        check.toggled.connect(lambda checked: self._changed(key, checked))
        self.controls[key] = check
        return check

    def _form(self, parent=None):
        form = self.widgets.QFormLayout(parent)
        form.setFieldGrowthPolicy(self.widgets.QFormLayout.AllNonFixedFieldsGrow)
        form.setLabelAlignment(self.qt.AlignLeft | self.qt.AlignVCenter)
        form.setVerticalSpacing(6)
        return form

    def _tab(self, title):
        scroll = self.widgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(self.widgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(self.qt.ScrollBarAlwaysOff)
        page = self.widgets.QWidget()
        layout = self.widgets.QVBoxLayout(page)
        layout.setAlignment(self.qt.AlignTop)
        scroll.setWidget(page)
        self.tabs.addTab(scroll, title)
        return layout

    def _build(self):
        W = self.widgets
        layout = W.QVBoxLayout(self.body)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(6)
        self.endpoint = self._combo()
        self.endpoint.setAccessibleName("OpenFrame instance")
        self.endpoint.currentIndexChanged.connect(self._endpoint_tip)
        self.connect_button = self._button("Connect", self.connect)
        self.disconnect_button = self._button("Disconnect", self.disconnect)
        self.disconnect_button.setEnabled(False)
        layout.addLayout(self._row(self.endpoint, self.connect_button, self.disconnect_button))
        self.status = self._notice()
        self._message(self.status, "Disconnected")
        layout.addWidget(self.status)
        self.tabs = W.QTabWidget()
        layout.addWidget(self.tabs, 1)

        play = self._tab("Playblast")
        form = self._form()
        play.addLayout(form)
        form.addRow("Open in viewer", self._choice("viewer"))
        form.addRow("Output format", self._choice("format"))
        form.addRow("Resolution", self._row(self._spin("width"), W.QLabel("×"), self._spin("height")))
        form.addRow("", self._check("include_audio", "Include audio"))
        self.audio_source = self._combo()
        self.audio_source.currentIndexChanged.connect(self._choose_audio)
        form.addRow("Audio source", self._row(self.audio_source, self._button("Refresh", self.refresh_audio_sources)))
        self.audio_status = self._notice()
        play.addWidget(self.audio_status)
        play.addWidget(self._check("timecode", "Timecode from Maya start"))

        self.advanced = W.QToolButton()
        self.advanced.setText("Advanced")
        self.advanced.setToolButtonStyle(self.qt.ToolButtonTextBesideIcon)
        self.advanced.setCheckable(True)
        self.advanced.toggled.connect(self._toggle_advanced)
        play.addWidget(self.advanced, 0, self.qt.AlignLeft)
        self.advanced_panel = W.QWidget()
        advanced = self._form(self.advanced_panel)
        advanced.setContentsMargins(0, 0, 0, 0)
        advanced.addRow("Quality / scale %", self._row(self._spin("quality"), self._spin("percent")))
        advanced.addRow("Image frame padding", self._spin("padding"))
        advanced.addRow("", self._check("ornaments", "Show ornaments"))
        advanced.addRow("", self._check("offscreen", "Render offscreen"))
        advanced.addRow("Connection timeout", self._spin("timeout"))
        advanced.addRow("", self._button("Reset adapter settings", self.reset_settings))
        play.addWidget(self.advanced_panel)

        sync = self._tab("Sync")
        form = self._form()
        sync.addLayout(form)
        form.addRow("Sync direction", self._choice("direction", ("Off", "Maya → OpenFrame", "OpenFrame → Maya", "Bidirectional")))
        form.addRow("Offset", self._spin("offset"))
        form.addRow("Multiplier", self._spin("multiplier"))
        relative = self._check("from_range", "From range start")
        relative.setToolTip("Map Maya frame 1 to the OpenFrame range start. Apply the offset after multiplication.")
        form.addRow("", relative)
        sync.addLayout(self._row(self._button("Send Maya range", lambda: self.perform(lambda: self.adapter.send_range())),
                                 self._button("Receive OpenFrame range", lambda: self.perform(lambda: self.adapter.receive_range()))))
        form = self._form()
        sync.addLayout(form)
        form.addRow("Display offset", self._spin("display_offset"))
        sync.addLayout(self._row(self._button("Apply display offset", lambda: self.timecode(True)),
                                 self._button("Disable override", lambda: self.timecode(False))))
        sync.addWidget(W.QLabel("Viewer layout"))
        sync.addLayout(self._row(self._button("Single A", lambda: self.layout("single")),
                                 self._button("Single B", lambda: self.layout("single_b")),
                                 self._button("Active", lambda: self.layout("active"))))
        sync.addLayout(self._row(self._button("Horizontal", lambda: self.layout("horizontal")),
                                 self._button("Vertical", lambda: self.layout("vertical")),
                                 self._button("Swap A/B", self.swap)))
        self.tabs.currentChanged.connect(lambda index: self._changed("tab", self.tabs.tabText(index)))

        self.action_status = self._notice()
        self.action_status.hide()
        layout.addWidget(self.action_status)
        self.capture_button = self._button("Create playblast and import", self.playblast)
        self.capture_button.setMinimumHeight(32)
        self.capture_button.setEnabled(False)
        layout.addWidget(self.capture_button)
        self.open_temp_button = self._button("Open Temp Folder", self.open_temp)
        self.clear_temp_button = self._button("Clear Temp Folder", self.clear_temp)
        layout.addLayout(self._row(self.open_temp_button, self.clear_temp_button))

    def _set_control(self, key, value):
        widget = self.controls[key]
        blocked = widget.blockSignals(True)
        try:
            if key in _CHOICES:
                widget.setCurrentIndex(widget.findData(value))
            elif key in _LIMITS:
                widget.setValue(value)
            else:
                widget.setChecked(value)
        finally:
            widget.blockSignals(blocked)

    def _restore_controls(self):
        for key in self.controls:
            self._set_control(key, self.values[key])
        self.tabs.setCurrentIndex(_CHOICES["tab"].index(self.values["tab"]))
        self.advanced.setChecked(self.values["advanced"])
        self._toggle_advanced(self.values["advanced"])

    def _toggle_advanced(self, opened):
        self.advanced_panel.setVisible(opened)
        self.advanced.setArrowType(self.qt.DownArrow if opened else self.qt.RightArrow)
        self._changed("advanced", opened)

    def _restore_geometry(self):
        app = self.widgets.QApplication.instance()
        parent = self.shell.parentWidget()
        center = (parent or self.shell).frameGeometry().center()
        screen = app.screenAt(center) or app.primaryScreen()
        geometry = self.values["geometry"]
        width, height = (480, 460) if geometry is None else geometry[2:]
        if geometry:
            from_point = self._point(geometry[0] + width // 2, geometry[1] + height // 2)
            screen = app.screenAt(from_point) or screen
        available = screen.availableGeometry()
        frame = self.shell.frameGeometry()
        extra_w = max(0, frame.width() - self.shell.width())
        extra_h = max(0, frame.height() - self.shell.height())
        width = min(max(360, width), max(1, available.width() - extra_w))
        height = min(max(240, height), max(1, available.height() - extra_h))
        x, y = geometry[:2] if geometry else (center.x() - width // 2, center.y() - height // 2)
        self.shell.resize(width, height)
        self.shell.move(max(available.left(), min(x, available.right() + 1 - width - extra_w)),
                        max(available.top(), min(y, available.bottom() + 1 - height - extra_h)))

    def _remember_geometry(self):
        if self.shell.isMinimized() or self.shell.isMaximized():
            return
        frame = self.shell.frameGeometry()
        self._store("geometry", [frame.x(), frame.y(), self.shell.width(), self.shell.height()])

    def _store(self, key, value):
        if self._ready and self.values[key] != value:
            self.values[key] = value
            self._dirty = True
            self.save_timer.start()

    def _changed(self, key, value):
        if not self._ready or self._closed:
            return
        if not _valid_setting(key, value):
            self._set_control(key, self.values[key])
            self._feedback("Dimensions must be multiples of four." if key in ("width", "height")
                           else "The sync multiplier must be nonzero.")
            return
        self._store(key, value)
        if key in _SYNC_KEYS.values() and self.adapter:
            self.configure()
        if key in ("include_audio", "format"):
            self.update_audio_status()

    def flush(self):
        self.save_timer.stop()
        if self._dirty:
            try:
                save_settings(self.settings_path, self.values)
                self._dirty = False
            except (OSError, ValueError) as error:
                detail = "Could not save OpenFrame settings: " + str(error)
                if not self._closed:
                    self._feedback(detail)
                self.cmds.warning(detail)

    def reset_settings(self):
        self._ready = False
        try:
            self.values = dict(_DEFAULTS)
            self._restore_controls()
            self.refresh_audio_sources()
            self._restore_geometry()
        finally:
            self._ready = True
        if self.adapter:
            self.configure()
        self._dirty = True
        self.flush()

    def _endpoint_tip(self, *_):
        self.endpoint.setToolTip(self.endpoint.currentData() or _AUTO_TIP)

    def find_endpoint(self):
        selected = self.endpoint.currentData()
        paths = discover_endpoints()
        if [self.endpoint.itemData(i) for i in range(self.endpoint.count())] != [None] + paths:
            self.endpoint.blockSignals(True)
            try:
                self.endpoint.clear()
                self.endpoint.addItem("Automatic", None)
                self.endpoint.setItemData(0, _AUTO_TIP, self.qt.ToolTipRole)
                for path in paths:
                    self.endpoint.addItem(_instance_label(path), path)
                    self.endpoint.setItemData(self.endpoint.count() - 1, path, self.qt.ToolTipRole)
                self.endpoint.setCurrentIndex(paths.index(selected) + 1 if selected in paths else 0)
            finally:
                self.endpoint.blockSignals(False)
            self._endpoint_tip()
        return self.endpoint.currentData()

    def connect(self):
        try:
            path = self.find_endpoint()
            mapping = self._mapping()
            previous = self.adapter.state.get("instance") if self.adapter else None
            self.disconnect()
            self.adapter = Adapter(path, host=self.host, timeout=self.values["timeout"],
                                   pin_instance=True, preferred_instance=previous if path is None else None)
            self.adapter.start(self.values["direction"], mapping)
            self.action_status.hide()
            self.refresh()
        except Exception as error:
            self.disconnect()
            self._message(self.status, "Connection failed", str(error))
            self._feedback(str(error))

    def disconnect(self):
        if self.adapter:
            self.adapter.stop()
            self.adapter = None
        self._remote_config.clear()
        self._message(self.status, "Disconnected")
        self.disconnect_button.setEnabled(False)
        self.capture_button.setEnabled(False)

    def _mapping(self):
        return SyncMapping(self.values["offset"], self.values["multiplier"], self.values["from_range"])

    def configure(self):
        self.perform(lambda: self.adapter.configure(direction=self.values["direction"], mapping=self._mapping(),
                                                   timeout=self.values["timeout"]), require_connected=False)

    def _show_connection(self):
        state = self.adapter.state
        self.disconnect_button.setEnabled(True)
        self.capture_button.setEnabled(state["connected"])
        if state["connected"]:
            path = state["endpoint"]
            frame = state["status"].get("frame", "?")
            self._message(self.status, "Connected · {} · frame {}".format(_instance_label(path), frame), path)
            focus = self.widgets.QApplication.focusWidget()
            for remote, key in _SYNC_KEYS.items():
                value = state["config"][remote]
                if self._remote_config.get(key) == value:
                    continue
                widget = self.controls[key]
                if widget is focus or (focus is not None and widget.isAncestorOf(focus)):
                    continue
                if self.values[key] != value:
                    self._set_control(key, value)
                    self._store(key, value)
                self._remote_config[key] = value
        else:
            messages = {"connecting": "Searching for OpenFrame…", "selection": "Choose an instance, then press Connect.",
                        "waiting": "Waiting for OpenFrame · press Connect to search again.",
                        "error": "Connection failed · retrying"}
            self._message(self.status, messages.get(state["phase"], "Connecting…"), state["error"] or _AUTO_TIP)

    def refresh(self):
        if self._closed:
            return
        if not self.cmds.window(self.window, exists=True):
            self.close()
            return
        if self.adapter:
            self._show_connection()
        if not self.endpoint.view().isVisible():
            self.find_endpoint()
        self.refresh_audio_sources(force=False)

    def _choose_audio(self, index):
        if index < 0 or not self._ready:
            return
        node = self.audio_source.itemData(index)
        if node is None:
            self._store("audio", None)
        else:
            ids = self.cmds.ls(node, uuid=True) or []
            if not ids or self.cmds.nodeType(node) != "audio":
                self.refresh_audio_sources()
                return
            scene = self.cmds.file(query=True, sceneName=True)
            self._store("audio", {"scene": scene, "node": node, "uuid": ids[0]})
        self.refresh_audio_sources()

    def _audio_present(self, saved, scene, names):
        return (saved is not None and saved["scene"] == scene and saved["node"] in names
                and self.cmds.ls(saved["node"], uuid=True) == [saved["uuid"]])

    def refresh_audio_sources(self, force=True):
        scene = self.cmds.file(query=True, sceneName=True)
        names = tuple(sorted(self.cmds.ls(type="audio") or []))
        saved = self.values["audio"]
        present = self._audio_present(saved, scene, names)
        signature = (scene, names, saved, present)
        if signature != self._audio_signature:
            self._audio_signature = signature
            self.audio_source.blockSignals(True)
            try:
                self.audio_source.clear()
                self.audio_source.addItem("Time Slider", None)
                for name in names:
                    label = self.audio_source.fontMetrics().elidedText(name, self.qt.ElideMiddle, 210)
                    self.audio_source.addItem(label, name)
                    self.audio_source.setItemData(self.audio_source.count() - 1, name, self.qt.ToolTipRole)
                if saved and not present:
                    self.audio_source.addItem("Missing audio source")
                    index = self.audio_source.count() - 1
                    self.audio_source.setItemData(index, saved["node"], self.qt.ToolTipRole)
                    self.audio_source.model().item(index).setEnabled(False)
                else:
                    index = self.audio_source.findData(saved["node"]) if saved else 0
                self.audio_source.setCurrentIndex(index)
                self.audio_source.setToolTip(saved["node"] if saved else "Use the sound assigned to Maya's Time Slider.")
            finally:
                self.audio_source.blockSignals(False)
            force = True
        if force:
            self.update_audio_status()

    def selected_sound_node(self):
        saved = self.values["audio"]
        if saved:
            scene = self.cmds.file(query=True, sceneName=True)
            if not self._audio_present(saved, scene, self.cmds.ls(type="audio") or []):
                raise ValueError("Selected audio source is missing in this scene. Choose a source or disable audio.")
            return saved["node"]
        return None

    def update_audio_status(self):
        movie = self.values["format"] in ("avi", "webm")
        enabled = self.values["include_audio"]
        self.controls["include_audio"].setEnabled(movie)
        self.audio_source.setEnabled(movie and enabled)
        if not movie:
            text = "Image sequences are silent."
        elif not enabled:
            text = "Audio disabled for this capture."
        else:
            try:
                sound = self.host.sound_snapshot(self.selected_sound_node())
                if sound is None:
                    text = "Time Slider has no sound. Choose an audio source for this capture."
                elif sound.muted:
                    text = "Muted in Maya · " + sound.node
                else:
                    text = "Audio · {} · offset {:g}".format(sound.node, sound.offset)
            except (OSError, RuntimeError, ValueError) as error:
                text = str(error)
        self._message(self.audio_status, text)

    def _feedback(self, text):
        self._message(self.action_status, text)
        self.action_status.show()

    def perform(self, action, require_connected=True):
        try:
            if not self.adapter or (require_connected and not self.adapter.state["connected"]):
                raise RuntimeError("Connect to OpenFrame first.")
            return action()
        except Exception as error:
            self._feedback(str(error))
            self.cmds.warning("OpenFrame: " + str(error))

    def timecode(self, enable):
        self.perform(lambda: self.adapter.request("timecode.set", enabled=enable,
                                                  offset=self.values["display_offset"] if enable else 0))

    def layout(self, mode):
        self.perform(lambda: self.adapter.request("viewer.set", **viewer_layout_params(mode)))

    def swap(self):
        def received_a(a, error):
            if error:
                self._feedback(str(error))
                return
            def received_b(b, error):
                if error:
                    self._feedback(str(error))
                    return
                self.adapter.request("viewer.assign", pane=0, id=b.get("id", ""))
                self.adapter.request("viewer.assign", pane=1, id=a.get("id", ""))
            self.adapter.request("viewer.get", pane=1, callback=received_b)
        self.perform(lambda: self.adapter.request("viewer.get", pane=0, callback=received_a))

    def playblast(self):
        def action():
            self.refresh_audio_sources()
            v = self.values
            image = v["format"] not in ("avi", "webm")
            settings = CaptureSettings(format_name="image" if image else v["format"],
                                      compression=v["format"] if image else None, width=v["width"], height=v["height"],
                                      quality=v["quality"], percent=v["percent"], padding=v["padding"],
                                      show_ornaments=v["ornaments"], off_screen=v["offscreen"], include_sound=v["include_audio"])
            node = self.selected_sound_node() if settings.include_sound and not image else None
            self.adapter.capture(settings=settings, sound_node=node, viewer=v["viewer"], timecode_offset=v["timecode"],
                                 callback=lambda result, error: self._feedback(str(error) if error else "Playblast imported · " + result["name"]))
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
            self.cmds.text(label="Current Maya session only. External file browsers are not launched.", align="left", wordWrap=True)
            self.cmds.showWindow(window)
        self.perform(action, require_connected=False)

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
            self._feedback(label)
        self.perform(lambda: self.adapter.clear_playblasts(confirm=confirm, callback=completed))

    def close(self, *_):
        if self._closed:
            return
        self._closed = True
        self._ready = False
        self.timer.stop()
        self.flush()
        app = self.widgets.QApplication.instance()
        if app:
            app.aboutToQuit.disconnect(self.close)
        if self.adapter:
            self.adapter.stop()
            self.adapter = None


def show():
    import maya.cmds as cmds
    global _controller
    previous_name = _controller.window if _controller else _WINDOW_NAME
    if _controller:
        _controller.close()
    if cmds.window(previous_name, exists=True):
        cmds.deleteUI(previous_name, window=True)
    _controller = Window()
    return _controller

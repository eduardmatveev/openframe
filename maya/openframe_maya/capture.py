"""Explicit capture values; no saved Maya preference names are assumed."""
import math
from dataclasses import dataclass
from typing import Optional, Tuple

MAX_SEQUENCE_FRAMES = 100000


@dataclass(frozen=True)
class CaptureSettings:
    format_name: str = "avi"
    compression: Optional[str] = None
    width: int = 1920
    height: int = 1080
    percent: int = 100
    quality: int = 100
    padding: int = 4
    show_ornaments: bool = False
    off_screen: bool = True
    include_sound: bool = True

    def __post_init__(self):
        if self.format_name not in ("avi", "webm", "image"):
            raise ValueError("Capture format must be avi, webm or image")
        for value in (self.width, self.height):
            if type(value) is not int or not 4 <= value <= 8192 or value % 4:
                raise ValueError("Dimensions must be multiples of four between 4 and 8192")
        for value, low, high, name in ((self.percent, 10, 100, "percent"),
                                       (self.quality, 0, 100, "quality"),
                                       (self.padding, 1, 12, "padding")):
            if type(value) is not int or not low <= value <= high:
                raise ValueError("{} must be an integer in {}..{}".format(name, low, high))
        for value in (self.show_ornaments, self.off_screen, self.include_sound):
            if type(value) is not bool:
                raise ValueError("Capture switches must be boolean")
        if self.compression is not None and (not isinstance(self.compression, str)
                or not self.compression or len(self.compression) > 128
                or any(ord(char) < 32 for char in self.compression)):
            raise ValueError("Invalid compression name")
        if self.format_name == "image" and self.encoding not in ("png", "jpg", "bmp", "tif"):
            raise ValueError("Image capture supports png, jpg, bmp and tif")

    @property
    def encoding(self):
        if self.compression is not None:
            return self.compression
        return "png" if self.format_name == "image" else "none" if self.format_name == "avi" else None


@dataclass(frozen=True)
class TimelineSound:
    node: str
    path: str
    offset: float
    source_start: float
    source_end: float
    end_time: float
    muted: bool
    displayed: bool

    def __post_init__(self):
        if not isinstance(self.node, str) or not self.node or len(self.node) > 4096 or "\0" in self.node:
            raise ValueError("Invalid timeline sound node")
        for value in (self.offset, self.source_start, self.source_end, self.end_time):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ValueError("Timeline sound timing must be finite")
        if type(self.muted) is not bool or type(self.displayed) is not bool:
            raise ValueError("Timeline sound switches must be boolean")


@dataclass(frozen=True)
class CaptureResult:
    kind: str
    paths: Tuple[str, ...]
    first: float
    last: float
    fps_num: int
    fps_den: int
    sound: Optional[TimelineSound] = None


"""Independent, authenticated localhost integration for OpenFrame and Maya.

Import has no side effects: no socket, callback, scene or preference changes.
"""
from .client import Client, OpenFrameError, discover_endpoints
from .adapter import Adapter, MayaHost, SyncMapping
from .capture import CaptureResult, CaptureSettings, TimelineSound

__all__ = ["Client", "OpenFrameError", "discover_endpoints", "Adapter", "MayaHost", "SyncMapping",
           "CaptureResult", "CaptureSettings", "TimelineSound", "show"]
__version__ = "0.1.0"


def show():
    """Open the Maya integration window. Sync remains disabled until requested."""
    from .ui import show as show_window
    return show_window()

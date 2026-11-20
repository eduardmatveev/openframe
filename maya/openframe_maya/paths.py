"""Filesystem guard: reject remote names before any filesystem operation.

Reparse points/symlinks are rejected, even when their target would be local.
This conservative rule avoids following a remote target just to validate it.
"""
import os
import re
import stat
from pathlib import Path


class PathPolicyError(ValueError):
    pass


def local_path(value, *, output=False, directory=False):
    path = os.fspath(value)
    if not isinstance(path, str) or not path or len(path) > 32000:
        raise PathPolicyError("A nonempty local path is required")
    if any(ord(char) < 32 or ord(char) == 127 for char in path):
        raise PathPolicyError("Control characters in paths are forbidden")
    normalized = path.replace("\\", "/")
    if normalized.startswith(("//", "/??/")) or "://" in normalized:
        raise PathPolicyError("URLs, UNC paths and device namespaces are forbidden")
    drive = bool(re.match(r"^[A-Za-z]:/", normalized))
    if ":" in normalized and (not drive or ":" in normalized[2:]):
        raise PathPolicyError("URLs, drive-relative paths and alternate streams are forbidden")
    parts = normalized.split("/")
    if ".." in parts:
        raise PathPolicyError("Parent traversal is forbidden; use an absolute local path")
    if os.name == "nt":
        for part in parts:
            if part and part != "." and part.endswith((" ", ".")):
                raise PathPolicyError("Ambiguous Windows path component")
            if re.match(r"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)", part, re.I):
                raise PathPolicyError("Windows device names are forbidden")
    absolute = os.path.abspath(path)
    if os.name == "nt":
        return _windows_validate(absolute, output=output, directory=directory)
    current = Path(absolute).anchor
    components = Path(absolute).parts[1:]
    for index, part in enumerate(("",) + components):
        if part:
            current = os.path.join(current, part)
        leaf = index == len(components)
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            if leaf and output:
                return absolute
            raise PathPolicyError("Local path does not exist") from None
        if stat.S_ISLNK(info.st_mode):
            raise PathPolicyError("Symbolic links are forbidden")
        if leaf:
            valid = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
            if not valid:
                raise PathPolicyError("Unexpected local path type")
        elif not stat.S_ISDIR(info.st_mode):
            raise PathPolicyError("Parent is not a directory")
    return absolute


def _windows_validate(absolute, *, output, directory):
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    root, tail = os.path.splitdrive(absolute)
    if not re.match(r"^[A-Za-z]:$", root):
        raise PathPolicyError("Drive-qualified local paths are required")
    kernel.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    kernel.GetDriveTypeW.restype = wintypes.UINT
    if kernel.GetDriveTypeW(root + "\\") not in (2, 3, 5, 6):
        raise PathPolicyError("The drive is unavailable or is not local")
    kernel.QueryDosDeviceW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    kernel.QueryDosDeviceW.restype = wintypes.DWORD
    target = ctypes.create_unicode_buffer(32768)
    if not kernel.QueryDosDeviceW(root, target, len(target)):
        raise PathPolicyError("Cannot establish drive mapping")
    mapping = target.value.lower()
    if mapping.startswith("\\??\\") or "mup" in mapping or "lanmanredirector" in mapping:
        raise PathPolicyError("Substituted and network drives are forbidden")
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL

    class FileInformation(ctypes.Structure):
        _fields_ = [("attributes", wintypes.DWORD), ("created", wintypes.FILETIME),
                    ("accessed", wintypes.FILETIME), ("modified", wintypes.FILETIME),
                    ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                    ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                    ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]
    kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileInformation)]
    kernel.GetFileInformationByHandle.restype = wintypes.BOOL
    parts = [part for part in tail.replace("\\", "/").split("/") if part]
    current = root + "\\"
    for index, part in enumerate([""] + parts):
        if part:
            current = os.path.join(current, part)
        leaf = index == len(parts)
        handle = kernel.CreateFileW(current, 0x80, 7, None, 3, 0x00200000 | 0x02000000, None)
        if handle == ctypes.c_void_p(-1).value:
            error = ctypes.get_last_error()
            if leaf and output and error == 2:
                return absolute
            raise PathPolicyError("Cannot open local path (Windows error {})".format(error))
        try:
            info = FileInformation()
            if not kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise PathPolicyError("Cannot inspect local path")
            if info.attributes & 0x400:
                raise PathPolicyError("Reparse points and symbolic links are forbidden")
            is_directory = bool(info.attributes & 0x10)
            if leaf and is_directory != directory:
                raise PathPolicyError("Unexpected local path type")
            if not leaf and not is_directory:
                raise PathPolicyError("Parent is not a directory")
        finally:
            kernel.CloseHandle(handle)
    return absolute

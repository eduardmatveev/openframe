"""Opt-in import-name shim for independently written OpenFrame compatibility.

Add both OpenFrame/maya and OpenFrame/maya/openframe_compat to sys.path only
when existing pipeline scripts should target OpenFrame. No original client
implementation is included and this module cannot connect to the original app.
"""
from openframe_maya.compatibility import KeyframePro2ClientApi

__all__ = ["KeyframePro2ClientApi"]

# SPDX-License-Identifier: MIT
"""1.11.0 (plan item A4): keeps a test process's window in the foreground.

DLSS-G stops interpolating while its window is not focused ("DLSS-G disabled:
window not focused" in sl.log), and in the full suite another window (the
terminal, the editor) often has the focus when the sample opens. Windows lets a
process take the foreground only in some cases; a synthetic Alt press first is
the usual way to be allowed (SetForegroundWindow's documented restrictions).
"""
import ctypes
from ctypes import wintypes

_user32 = ctypes.WinDLL('user32', use_last_error=True)
_EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_user32.EnumWindows.argtypes = [_EnumProc, wintypes.LPARAM]
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_user32.GetWindow.restype = wintypes.HWND
_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.keybd_event.argtypes = [wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_void_p]
_GW_OWNER, _SW_RESTORE, _VK_MENU, _KEYEVENTF_KEYUP = 4, 9, 0x12, 0x2


def main_window(pid):
    """The process's visible top-level window without an owner, or None."""
    found = []

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd) and not _user32.GetWindow(hwnd, _GW_OWNER):
            found.append(hwnd)
            return False
        return True

    _user32.EnumWindows(_EnumProc(visit), 0)
    return found[0] if found else None


def focus(pid):
    """Brings the process's window to the foreground. True when it has the focus."""
    hwnd = main_window(pid)
    if not hwnd:
        return False
    if _user32.GetForegroundWindow() == hwnd:
        return True
    _user32.keybd_event(_VK_MENU, 0, 0, None)
    _user32.keybd_event(_VK_MENU, 0, _KEYEVENTF_KEYUP, None)
    _user32.ShowWindow(hwnd, _SW_RESTORE)
    _user32.SetForegroundWindow(hwnd)
    return _user32.GetForegroundWindow() == hwnd


def hold(pid, seconds, poll):
    """Sleeps `seconds`, taking the focus back every second while `poll()` is None.
    Returns how many times the focus had to be taken back."""
    import time
    taken = 0
    end = time.monotonic() + seconds
    while time.monotonic() < end and poll() is None:
        hwnd = main_window(pid)
        if hwnd and _user32.GetForegroundWindow() != hwnd:
            focus(pid)
            taken += 1
        time.sleep(1)
    return taken

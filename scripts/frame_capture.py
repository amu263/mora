"""Capture one real ANSI frame from the MORA TUI at a requested console size.

The frame comes from the player itself (--snapshot), so previews and narrow-screen acceptance are
never hand-written. Run it from a normal terminal or a non-interactive shell: it allocates a
private hidden console and detaches from the caller's console if one is attached.
"""
import argparse
import ctypes as c
import subprocess
import time
from ctypes import wintypes as w
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Coord(c.Structure):
    _fields_ = [('X', w.SHORT), ('Y', w.SHORT)]


class Rect(c.Structure):
    _fields_ = [('Left', w.SHORT), ('Top', w.SHORT), ('Right', w.SHORT), ('Bottom', w.SHORT)]


class ScreenBufferInfo(c.Structure):
    _fields_ = [('Size', Coord), ('Cursor', Coord), ('Attributes', w.WORD), ('Window', Rect),
                ('MaximumWindow', Coord)]


kernel32 = c.WinDLL('kernel32', use_last_error=True)
user32 = c.WinDLL('user32', use_last_error=True)
kernel32.GetStdHandle.restype = w.HANDLE
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD,
                                 w.HANDLE]
kernel32.CreateFileW.restype = w.HANDLE
kernel32.SetConsoleWindowInfo.argtypes = [w.HANDLE, w.BOOL, c.POINTER(Rect)]
kernel32.SetConsoleScreenBufferSize.argtypes = [w.HANDLE, Coord]
kernel32.GetConsoleWindow.restype = w.HWND
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.GetConsoleScreenBufferInfo.argtypes = [w.HANDLE, c.POINTER(ScreenBufferInfo)]


class Char(c.Union):
    _fields_ = [('UnicodeChar', w.WCHAR), ('AsciiChar', c.c_char)]


class KeyEvent(c.Structure):
    _fields_ = [('down', w.BOOL), ('repeat', w.WORD), ('vk', w.WORD), ('scan', w.WORD),
                ('char', Char), ('control', w.DWORD)]


class MouseEvent(c.Structure):
    _fields_ = [('position', Coord), ('buttons', w.DWORD), ('control', w.DWORD),
                ('flags', w.DWORD)]


class Event(c.Union):
    _fields_ = [('key', KeyEvent), ('mouse', MouseEvent), ('raw', c.c_byte * 16)]


class Input(c.Structure):
    _fields_ = [('type', w.WORD), ('event', Event)]


kernel32.WriteConsoleInputW.argtypes = [w.HANDLE, c.POINTER(Input), w.DWORD,
                                        c.POINTER(w.DWORD)]


def send_key(conin, vk=0, char='\0'):
    """Inject one key press the same way ui_smoke.py does, through the real console input queue."""
    events = (Input * 2)()
    for index, down in enumerate((True, False)):
        events[index].type = 1
        events[index].event.key = KeyEvent(down, 1, vk, 0, Char(UnicodeChar=char), 0)
    written = w.DWORD()
    if not kernel32.WriteConsoleInputW(conin, events, 2, c.byref(written)):
        raise OSError(c.get_last_error(), 'console key injection failed')
user32.ShowWindow.argtypes = [w.HWND, c.c_int]
invalid = w.HANDLE(-1).value


def main() -> int:
    parser = argparse.ArgumentParser(description='Capture a real MORA ANSI frame.')
    parser.add_argument('out', help='ANSI snapshot path')
    parser.add_argument('--columns', type=int, default=80)
    parser.add_argument('--rows', type=int, default=24)
    parser.add_argument('--seconds', type=int, default=4)
    parser.add_argument('--exe', default=str(ROOT / 'build' / 'mora.exe'))
    parser.add_argument('--pass-through', nargs=argparse.REMAINDER, default=['--demo'])
    parser.add_argument('--command', default=None,
                        help='type this command in the real input box and press Enter')
    parser.add_argument('--command-after', type=float, default=1.0,
                        help='seconds to wait before typing the command')
    parser.add_argument('--enter-after', type=float, default=None,
                        help='press Enter again after this many seconds (open/play the selection)')
    args = parser.parse_args()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Detach from the caller's console: its window is usually larger than the requested size and
    # a console buffer can never be narrower than its window.
    if kernel32.GetConsoleWindow():
        kernel32.FreeConsole()
    allocated = bool(kernel32.AllocConsole())
    window = kernel32.GetConsoleWindow()
    if allocated and window:
        user32.ShowWindow(window, 0)
    conin = kernel32.CreateFileW('CONIN$', 0xC0000000, 3, None, 3, 0, None)
    conout = kernel32.CreateFileW('CONOUT$', 0xC0000000, 3, None, 3, 0, None)
    if conin == invalid or conout == invalid:
        raise OSError(c.get_last_error(), 'capture console unavailable')
    try:
        # Grow the buffer first (it must admit the old window), then shrink the window to the
        # requested size, then trim the buffer to that width.
        kernel32.SetConsoleScreenBufferSize(conout, Coord(240, 120))
        rect = Rect(0, 0, args.columns - 1, args.rows - 1)
        if not kernel32.SetConsoleWindowInfo(conout, True, c.byref(rect)):
            raise OSError(c.get_last_error(), 'console window resize failed')
        kernel32.SetConsoleScreenBufferSize(conout, Coord(args.columns, 120))
        info = ScreenBufferInfo()
        if kernel32.GetConsoleScreenBufferInfo(conout, c.byref(info)):
            observed = (info.Window.Right - info.Window.Left + 1,
                        info.Window.Bottom - info.Window.Top + 1)
            print(f'console window {observed[0]}x{observed[1]}', flush=True)
        out.unlink(missing_ok=True)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = ([args.exe] + args.pass_through +
                   ['--seconds', str(args.seconds), '--snapshot', str(out)])
        with open('CONIN$', 'rb', buffering=0) as stdin_file, \
                open('CONOUT$', 'wb', buffering=0) as stdout_file:
            child = subprocess.Popen(command, stdin=stdin_file, stdout=stdout_file,
                                     stderr=subprocess.DEVNULL, startupinfo=startup)
            print(f'player pid {child.pid}', flush=True)
            try:
                if args.command:
                    time.sleep(args.command_after)
                    for character in args.command:
                        send_key(conin, 0, character)
                    send_key(conin, 0x0D)
                if args.enter_after is not None:
                    time.sleep(args.enter_after)
                    send_key(conin, 0x0D)
                child.wait(timeout=args.seconds + 20)
            finally:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=5)
    finally:
        kernel32.CloseHandle(conin)
        kernel32.CloseHandle(conout)
        if allocated:
            kernel32.FreeConsole()
    text = out.read_text(encoding='utf-8', errors='replace')
    print(f'{out} rows={text.count(chr(10))} columns={args.columns} bytes={len(text)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

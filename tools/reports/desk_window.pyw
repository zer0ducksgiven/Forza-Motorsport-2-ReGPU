"""The reports desk in its own window (WebView2 through pywebview) instead of a browser tab.

  pythonw tools/reports/desk_window.pyw        (the "FM2 Reports Desk" shortcut runs this)

Starts the desk's server (app.py) on a free local port and shows its page; closing the window stops it.
"""
import os
import sys
import threading
from http.server import ThreadingHTTPServer

import webview

# pythonw has no console: the desk's prints (empty logs deleted, errors) go to a log next to its other data.
if sys.stdout is None or sys.stderr is None:
    log_dir = os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'fm2-reports')
    os.makedirs(log_dir, exist_ok=True)
    sys.stdout = sys.stderr = open(os.path.join(log_dir, 'desk.log'), 'a', encoding='utf-8', buffering=1)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app  # noqa: E402


APP_ID = 'zer0ducksgiven.FM2.ReportsDesk'  # the shortcut carries the same ID, so pinned and running are one taskbar button
ICON = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'desk.ico')  # drawn in desk-icon.svg


def taskbar_identity():
    """Our own taskbar entry instead of Python's: the app ID, then the FM2 icon on the window."""
    import ctypes
    ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(APP_ID)


def set_icon(window):
    import ctypes
    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, window.title)
    if not hwnd:
        return
    for size, which in ((16, 0), (32, 1)):  # ICON_SMALL, ICON_BIG
        icon = user32.LoadImageW(None, os.path.abspath(ICON), 1, size, size, 0x10)  # IMAGE_ICON, LR_LOADFROMFILE
        if icon:
            user32.SendMessageW(hwnd, 0x80, which, icon)  # WM_SETICON


def main():
    taskbar_identity()
    server = ThreadingHTTPServer(('127.0.0.1', 0), app.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    webview.settings['OPEN_EXTERNAL_LINKS_IN_BROWSER'] = True  # the GitHub links
    window = webview.create_window('FM2 Reports Desk', f'http://127.0.0.1:{server.server_address[1]}/', width=1500, height=900,
                                   min_size=(760, 480), background_color='#0e1015')
    window.events.shown += lambda: set_icon(window)
    webview.start(private_mode=False)
    server.shutdown()


if __name__ == '__main__':
    main()

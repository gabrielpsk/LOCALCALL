"""Modal de Pré-Apresentação Multiplataforma (Windows & Linux X11): seleção de Tela/Janela, Resolução, FPS e Áudio."""
import ctypes
import platform

from PyQt6.QtCore import QSize, Qt
from PyQt6.QtGui import QGuiApplication, QIcon
from PyQt6.QtWidgets import (QCheckBox, QComboBox, QDialog, QFrame, QGridLayout,
                             QHBoxLayout, QLabel, QListWidget,
                             QListWidgetItem, QPushButton, QTabWidget,
                             QVBoxLayout, QWidget)

_IS_WINDOWS = platform.system() == "Windows"

if _IS_WINDOWS:
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    dwmapi = ctypes.windll.dwmapi

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]

    GWL_EXSTYLE = -20
    WS_EX_TOOLWINDOW = 0x00000080
    DWMWA_CLOAKED = 14


def _list_windows_win32() -> list[tuple[int, str]]:
    """Lista janelas visíveis de aplicativos reais no Windows via EnumWindows."""
    results: list[tuple[int, str]] = []
    shell_wnd = user32.GetShellWindow()

    def _cb(hwnd, _lparam):
        if not hwnd or hwnd == shell_wnd or not user32.IsWindowVisible(hwnd) or user32.IsIconic(hwnd):
            return True
        ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if ex_style & WS_EX_TOOLWINDOW:
            return True
        cloaked = ctypes.c_int(0)
        dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
        if cloaked.value != 0:
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 1:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value.strip()
        if title and title not in ("Program Manager", "Configurações", "Settings", "Windows Input Experience"):
            results.append((int(hwnd), title))
        return True

    cb_func = WNDENUMPROC(_cb)
    user32.EnumWindows(cb_func, 0)
    return results


def _list_windows_x11() -> list[tuple[int, str]]:
    """Lista janelas de aplicativos visíveis no Linux X11 via _NET_CLIENT_LIST (EWMH)."""
    results: list[tuple[int, str]] = []
    try:
        x11 = ctypes.cdll.LoadLibrary("libX11.so.6")
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        x11.XInternAtom.restype = ctypes.c_ulong
        x11.XGetWindowProperty.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
        ]
        x11.XGetWindowProperty.restype = ctypes.c_int
        x11.XFree.argtypes = [ctypes.c_void_p]

        dpy = x11.XOpenDisplay(None)
        if not dpy:
            return results

        try:
            root = x11.XDefaultRootWindow(dpy)
            atom_client_list = x11.XInternAtom(dpy, b"_NET_CLIENT_LIST", 1)
            atom_net_wm_name = x11.XInternAtom(dpy, b"_NET_WM_NAME", 1)
            atom_wm_name = x11.XInternAtom(dpy, b"WM_NAME", 1)
            if not atom_client_list:
                return results

            actual_type = ctypes.c_ulong()
            actual_format = ctypes.c_int()
            nitems = ctypes.c_ulong()
            bytes_after = ctypes.c_ulong()
            prop = ctypes.POINTER(ctypes.c_ubyte)()

            if x11.XGetWindowProperty(
                dpy, root, atom_client_list, 0, 1024, 0, 0,
                ctypes.byref(actual_type), ctypes.byref(actual_format),
                ctypes.byref(nitems), ctypes.byref(bytes_after), ctypes.byref(prop)
            ) != 0 or not prop:
                return results

            count = int(nitems.value)
            win_array = ctypes.cast(prop, ctypes.POINTER(ctypes.c_ulong * count)).contents
            xids = [int(win_array[i]) for i in range(count)]
            x11.XFree(prop)

            for xid in xids:
                if not xid:
                    continue
                title = ""
                for name_atom in (atom_net_wm_name, atom_wm_name):
                    if not name_atom:
                        continue
                    t_type = ctypes.c_ulong()
                    t_fmt = ctypes.c_int()
                    t_nitems = ctypes.c_ulong()
                    t_ba = ctypes.c_ulong()
                    t_prop = ctypes.POINTER(ctypes.c_ubyte)()
                    if x11.XGetWindowProperty(
                        dpy, xid, name_atom, 0, 256, 0, 0,
                        ctypes.byref(t_type), ctypes.byref(t_fmt),
                        ctypes.byref(t_nitems), ctypes.byref(t_ba), ctypes.byref(t_prop)
                    ) == 0 and t_prop:
                        if t_nitems.value > 0:
                            raw = ctypes.string_at(t_prop, t_nitems.value)
                            title = raw.decode("utf-8", errors="ignore").strip()
                        x11.XFree(t_prop)
                        if title:
                            break
                if title:
                    results.append((xid, title))
        finally:
            x11.XCloseDisplay(dpy)
    except Exception:
        pass
    return results


def list_capturable_windows() -> list[tuple[int, str]]:
    """Lista janelas visíveis de aplicativos no sistema operacional atual."""
    if _IS_WINDOWS:
        return _list_windows_win32()
    return _list_windows_x11()


class ScreenShareDialog(QDialog):
    def __init__(self, parent=None, default_share_audio: bool = True):
        super().__init__(parent)
        self.setWindowTitle("Compartilhar Tela")
        self.setMinimumSize(620, 480)

        # Tabs: Aba 1 "Telas" | Aba 2 "Janelas de Aplicativos"
        self.tabs = QTabWidget()

        # --- Aba 1: Monitores com miniaturas ---
        screens_tab = QWidget()
        screens_lay = QVBoxLayout(screens_tab)
        screens_lay.setContentsMargins(10, 10, 10, 10)
        self.screen_list = QListWidget()
        self.screen_list.setViewMode(QListWidget.ViewMode.IconMode)
        self.screen_list.setIconSize(QSize(220, 124))
        self.screen_list.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.screen_list.setSpacing(12)
        self.screen_list.setMovement(QListWidget.Movement.Static)
        screens_lay.addWidget(self.screen_list)

        # --- Aba 2: Janelas de Aplicativos ---
        windows_tab = QWidget()
        windows_lay = QVBoxLayout(windows_tab)
        windows_lay.setContentsMargins(10, 10, 10, 10)
        win_top = QHBoxLayout()
        win_hint = QLabel("Selecione a janela do aplicativo ou jogo:")
        win_hint.setObjectName("hint")
        refresh_btn = QPushButton("Atualizar Lista")
        refresh_btn.clicked.connect(self._populate_windows)
        win_top.addWidget(win_hint, 1)
        win_top.addWidget(refresh_btn)

        self.window_list = QListWidget()
        self.window_list.setIconSize(QSize(160, 90))
        self.window_list.setSpacing(6)
        windows_lay.addLayout(win_top)
        windows_lay.addWidget(self.window_list, 1)

        self.tabs.addTab(screens_tab, "Telas")
        self.tabs.addTab(windows_tab, "Janelas de Aplicativos")

        # --- Painel de Configurações de Qualidade e Áudio Loopback ---
        qual_frame = QFrame()
        qual_frame.setObjectName("qualityPanel")
        qual_grid = QGridLayout(qual_frame)
        qual_grid.setContentsMargins(12, 10, 12, 10)
        qual_grid.setHorizontalSpacing(16)
        qual_grid.setVerticalSpacing(6)

        lbl_qual = QLabel("QUALIDADE DA TRANSMISSÃO (BAIXA LATÊNCIA)")
        lbl_qual.setObjectName("sectionLabel")
        qual_grid.addWidget(lbl_qual, 0, 0, 1, 3)

        self.res_combo = QComboBox()
        self.res_combo.addItem("720p (HD)", 720)
        self.res_combo.addItem("1080p (Full HD)", 1080)
        self.res_combo.currentIndexChanged.connect(self._update_bitrate_label)

        self.fps_combo = QComboBox()
        self.fps_combo.addItem("60 FPS (Fluido)", 60)
        self.fps_combo.addItem("30 FPS (Econômico)", 30)
        self.fps_combo.currentIndexChanged.connect(self._update_bitrate_label)

        self.bitrate_label = QLabel()
        self.bitrate_label.setObjectName("hint")

        audio_api_lbl = "WASAPI Loopback" if _IS_WINDOWS else "PulseAudio Monitor"
        self.share_audio_chk = QCheckBox(
            f"Compartilhar áudio do sistema ({audio_api_lbl} — jogos e vídeos)"
        )
        self.share_audio_chk.setChecked(default_share_audio)

        qual_grid.addWidget(QLabel("Resolução:"), 1, 0)
        qual_grid.addWidget(self.res_combo, 1, 1)
        qual_grid.addWidget(self.bitrate_label, 1, 2)
        qual_grid.addWidget(QLabel("Taxa de Quadros:"), 2, 0)
        qual_grid.addWidget(self.fps_combo, 2, 1)
        qual_grid.addWidget(self.share_audio_chk, 3, 0, 1, 3)

        # --- Botões de ação no rodapé ---
        footer = QHBoxLayout()
        footer.addStretch()
        cancel_btn = QPushButton("Cancelar")
        cancel_btn.clicked.connect(self.reject)
        start_btn = QPushButton("Iniciar Apresentação")
        start_btn.setProperty("primary", True)
        start_btn.clicked.connect(self._on_start)
        footer.addWidget(cancel_btn)
        footer.addWidget(start_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(10)
        root.addWidget(self.tabs, 1)
        root.addWidget(qual_frame)
        root.addLayout(footer)

        self._populate_screens()
        self._populate_windows()
        self._update_bitrate_label()

    def _populate_screens(self):
        self.screen_list.clear()
        screens = QGuiApplication.screens()
        for idx, scr in enumerate(screens):
            geom = scr.geometry()
            aspect = "21:9 Ultrawide" if (geom.width() / max(1, geom.height())) > 2.0 else f"{geom.width()}x{geom.height()}"
            title = f"Tela {idx + 1} ({aspect})"
            item = QListWidgetItem(title)
            item.setData(Qt.ItemDataRole.UserRole, idx)

            pix = scr.grabWindow(0)
            if not pix.isNull():
                thumb = pix.scaled(QSize(220, 124), Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
                item.setIcon(QIcon(thumb))
            self.screen_list.addItem(item)

        if self.screen_list.count() > 0:
            self.screen_list.setCurrentRow(0)

    def _populate_windows(self):
        self.window_list.clear()
        primary = QGuiApplication.primaryScreen()
        for hwnd, title in list_capturable_windows():
            display = title if len(title) <= 68 else title[:65] + "..."
            item = QListWidgetItem(display)
            item.setData(Qt.ItemDataRole.UserRole, hwnd)
            if primary:
                pix = primary.grabWindow(hwnd)
                if not pix.isNull():
                    thumb = pix.scaled(QSize(160, 90), Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
                    item.setIcon(QIcon(thumb))
            self.window_list.addItem(item)

        if self.window_list.count() > 0:
            self.window_list.setCurrentRow(0)

    def _compute_bitrate(self) -> int:
        res = int(self.res_combo.currentData() or 720)
        fps = int(self.fps_combo.currentData() or 60)
        if res >= 1080:
            return 8_000_000 if fps >= 60 else 5_000_000
        return 6_000_000 if fps >= 60 else 4_000_000

    def _update_bitrate_label(self):
        br_mbps = self._compute_bitrate() // 1_000_000
        self.bitrate_label.setText(f"Bitrate alvo: ~{br_mbps} Mbps (proporção original preservada)")

    def _on_start(self):
        if self.tabs.currentIndex() == 1 and self.window_list.currentItem() is None:
            self.tabs.setCurrentIndex(0)
        self.accept()

    def values(self) -> dict:
        max_h = int(self.res_combo.currentData() or 720)
        fps = int(self.fps_combo.currentData() or 60)
        bitrate = self._compute_bitrate()
        share_audio = bool(self.share_audio_chk.isChecked())

        if self.tabs.currentIndex() == 1 and self.window_list.currentItem() is not None:
            item = self.window_list.currentItem()
            hwnd = int(item.data(Qt.ItemDataRole.UserRole) or 0)
            src_name = item.text()
            return {
                "max_height": max_h,
                "fps": fps,
                "bitrate": bitrate,
                "hwnd": hwnd,
                "monitor_index": 0,
                "label": f"{src_name} ({max_h}p {fps} FPS)",
                "share_audio": share_audio,
            }

        item = self.screen_list.currentItem()
        mon_idx = int(item.data(Qt.ItemDataRole.UserRole) or 0) if item else 0
        return {
            "max_height": max_h,
            "fps": fps,
            "bitrate": bitrate,
            "hwnd": 0,
            "monitor_index": mon_idx,
            "label": f"Tela {mon_idx + 1} ({max_h}p {fps} FPS)",
            "share_audio": share_audio,
        }

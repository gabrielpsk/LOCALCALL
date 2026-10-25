"""Ponte ctypes multiplataforma para o motor de compartilhamento de tela em C (video_engine.dll / video_engine.so)."""
import ctypes
import platform
import sys
from pathlib import Path

from PyQt6.QtCore import QObject, pyqtSignal
from PyQt6.QtGui import QImage

DEFAULT_VIDEO_PORT = 45002
MAX_STREAMS = 2
_IS_WINDOWS = platform.system() == "Windows"
_LIB_NAME = "video_engine.dll" if _IS_WINDOWS else "video_engine.so"


def _resolve_app_root() -> Path:
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            return Path(meipass).resolve()
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


_APP_ROOT = _resolve_app_root()
_DLL_PATH = (_APP_ROOT / "video_engine" / _LIB_NAME).resolve()

FRAME_CB_TYPE = ctypes.CFUNCTYPE(
    None,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_ubyte),
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
)


class VideoBridge(QObject):
    frame_received = pyqtSignal(int, QImage)  # stream_id (0=local, 1..2=remoto), QImage

    def __init__(self, parent=None):
        super().__init__(parent)
        self._dll = None
        self._cb_ref = None
        self.active_port = 0
        self.capturing = False
        # Configuração selecionada no ScreenShareDialog
        self.capture_config = {
            "max_height": 720,
            "fps": 60,
            "bitrate": 6_000_000,
            "hwnd": 0,
            "monitor_index": 0,
            "label": "Tela 1 (720p 60 FPS)",
        }
        self._load_dll()

    def _load_dll(self):
        if _DLL_PATH.is_file() and _DLL_PATH.parent.parent == _APP_ROOT:
            try:
                dll = ctypes.CDLL(str(_DLL_PATH))
                dll.video_init.argtypes = [ctypes.c_int]
                dll.video_init.restype = ctypes.c_int

                dll.video_set_key.argtypes = [ctypes.c_char_p, ctypes.c_uint]
                dll.video_set_key.restype = ctypes.c_int

                dll.video_start_capture.argtypes = [ctypes.c_int, ctypes.c_int]
                dll.video_start_capture.restype = ctypes.c_int

                dll.video_start_capture_ex.argtypes = [
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_uint64,
                    ctypes.c_int,
                ]
                dll.video_start_capture_ex.restype = ctypes.c_int

                dll.video_stop_capture.argtypes = []
                dll.video_stop_capture.restype = None

                dll.video_add_target.argtypes = [ctypes.c_char_p, ctypes.c_int]
                dll.video_add_target.restype = None

                dll.video_remove_targets.argtypes = []
                dll.video_remove_targets.restype = None

                dll.video_set_frame_callback.argtypes = [FRAME_CB_TYPE]
                dll.video_set_frame_callback.restype = None

                dll.video_cleanup.argtypes = []
                dll.video_cleanup.restype = None

                self._cb_ref = FRAME_CB_TYPE(self._on_c_frame)
                dll.video_set_frame_callback(self._cb_ref)
                self._dll = dll
            except OSError:
                self._dll = None

    def _on_c_frame(self, stream_id: int, data_ptr, width: int, height: int, stride: int):
        if not data_ptr or width <= 0 or height <= 0:
            return
        try:
            byte_count = height * stride
            raw = ctypes.string_at(data_ptr, byte_count)
            img = QImage(raw, width, height, stride, QImage.Format.Format_ARGB32).copy()
            self.frame_received.emit(stream_id, img)
        except Exception:
            pass

    @property
    def available(self) -> bool:
        return self._dll is not None

    def configure_capture(
        self,
        max_height: int = 720,
        fps: int = 60,
        bitrate: int = 6_000_000,
        hwnd: int = 0,
        monitor_index: int = 0,
        label: str = "",
    ):
        self.capture_config = {
            "max_height": int(max_height),
            "fps": int(fps),
            "bitrate": int(bitrate),
            "hwnd": int(hwnd),
            "monitor_index": int(monitor_index),
            "label": label or f"{max_height}p {fps} FPS",
        }

    def start(self, preferred_port: int = DEFAULT_VIDEO_PORT) -> tuple[bool, str]:
        if not self._dll:
            self.active_port = 0
            return False, f"Vídeo indisponível ({_LIB_NAME} ausente)"

        for offset in range(0, 20, 2):
            port = preferred_port + offset
            if self._dll.video_init(port) == 0:
                self.active_port = port
                return True, f"Vídeo UDP cifrado (porta {port})"

        self.active_port = 0
        return False, f"Falha ao abrir porta de vídeo {preferred_port}"

    def set_key(self, key32: bytes, sender_id: int) -> bool:
        if not self._dll or not self.active_port or len(key32) != 32:
            return False
        return self._dll.video_set_key(key32, int(sender_id)) == 0

    def start_capture(self, fps: int | None = None, quality: int | None = None) -> bool:
        if not self._dll or not self.active_port:
            return False
        cfg = self.capture_config
        use_fps = int(fps) if fps is not None else cfg["fps"]
        rc = self._dll.video_start_capture_ex(
            cfg["max_height"],
            use_fps,
            cfg["bitrate"],
            cfg["hwnd"],
            cfg["monitor_index"],
        )
        if rc == 0:
            self.capturing = True
            return True
        return False

    def stop_capture(self):
        if self._dll and self.capturing:
            self._dll.video_stop_capture()
        self.capturing = False

    def sync_targets(self, endpoints: list[dict], local_name: str):
        if not self._dll or not self.active_port:
            return
        self._dll.video_remove_targets()
        for ep in endpoints:
            if ep.get("name") == local_name:
                continue
            ip = str(ep.get("ip", "")).strip()
            port = int(ep.get("video_port") or DEFAULT_VIDEO_PORT)
            if ip and port > 0 and ip not in ("0.0.0.0", "::"):
                self._dll.video_add_target(ip.encode("ascii", errors="ignore"), port)

    def cleanup(self):
        self.stop_capture()
        if self._dll and self.active_port:
            self._dll.video_cleanup()
        self.active_port = 0

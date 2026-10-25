"""Ponte ctypes multiplataforma para o motor de áudio em C (audio_engine.dll / audio_engine.so)."""
import ctypes
import platform
import sys
from pathlib import Path

DEFAULT_UDP_PORT = 45001
_IS_WINDOWS = platform.system() == "Windows"
_LIB_NAME = "audio_engine.dll" if _IS_WINDOWS else "audio_engine.so"


def _resolve_app_root() -> Path:
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            return Path(meipass).resolve()
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


_APP_ROOT = _resolve_app_root()
_DLL_PATH = (_APP_ROOT / "audio_engine" / _LIB_NAME).resolve()


class AudioBridge:
    def __init__(self):
        self._dll = None
        self.active_port = 0
        self.mic_muted = False
        self.deafened = False
        self.loopback_enabled = False
        # Controles individuais por participante (chave: nome do usuário)
        self.peer_volumes: dict[str, int] = {}
        self.peer_muted: set[str] = set()
        self.peer_loopback_muted: set[str] = set()
        self._name_to_sid: dict[str, int] = {}
        self._load_dll()

    def _load_dll(self):
        if _DLL_PATH.is_file() and _DLL_PATH.parent.parent == _APP_ROOT:
            try:
                dll = ctypes.CDLL(str(_DLL_PATH))
                dll.audio_init.argtypes = [ctypes.c_int]
                dll.audio_init.restype = ctypes.c_int

                dll.audio_set_key.argtypes = [ctypes.c_char_p, ctypes.c_uint]
                dll.audio_set_key.restype = ctypes.c_int

                dll.audio_add_peer.argtypes = [ctypes.c_char_p, ctypes.c_int]
                dll.audio_add_peer.restype = None

                dll.audio_remove_peers.argtypes = []
                dll.audio_remove_peers.restype = None

                dll.audio_set_mic_muted.argtypes = [ctypes.c_int]
                dll.audio_set_mic_muted.restype = None

                dll.audio_set_deafened.argtypes = [ctypes.c_int]
                dll.audio_set_deafened.restype = None

                if hasattr(dll, "audio_set_loopback"):
                    dll.audio_set_loopback.argtypes = [ctypes.c_int]
                    dll.audio_set_loopback.restype = None

                if hasattr(dll, "audio_get_speaking_mask"):
                    dll.audio_get_speaking_mask.argtypes = []
                    dll.audio_get_speaking_mask.restype = ctypes.c_uint

                if hasattr(dll, "audio_set_peer_volume"):
                    dll.audio_set_peer_volume.argtypes = [ctypes.c_uint, ctypes.c_int]
                    dll.audio_set_peer_volume.restype = None

                if hasattr(dll, "audio_set_peer_muted"):
                    dll.audio_set_peer_muted.argtypes = [ctypes.c_uint, ctypes.c_int]
                    dll.audio_set_peer_muted.restype = None

                if hasattr(dll, "audio_set_peer_loopback_muted"):
                    dll.audio_set_peer_loopback_muted.argtypes = [ctypes.c_uint, ctypes.c_int]
                    dll.audio_set_peer_loopback_muted.restype = None

                dll.audio_cleanup.argtypes = []
                dll.audio_cleanup.restype = None

                self._dll = dll
            except OSError:
                self._dll = None

    @property
    def available(self) -> bool:
        return self._dll is not None

    def start(self, preferred_port: int = DEFAULT_UDP_PORT) -> tuple[bool, str]:
        if not self._dll:
            self.active_port = 0
            return False, f"Áudio indisponível ({_LIB_NAME} não encontrada)"

        for offset in range(10):
            port = preferred_port + offset
            rc = self._dll.audio_init(port)
            if rc == 0:
                self.active_port = port
                self._dll.audio_set_mic_muted(1 if self.mic_muted else 0)
                self._dll.audio_set_deafened(1 if self.deafened else 0)
                if hasattr(self._dll, "audio_set_loopback"):
                    self._dll.audio_set_loopback(1 if self.loopback_enabled else 0)
                return True, f"Voz UDP cifrada (porta {port})"

        self.active_port = 0
        return False, f"Falha ao abrir porta UDP {preferred_port}"

    def set_key(self, key32: bytes, sender_id: int) -> bool:
        if not self._dll or not self.active_port or len(key32) != 32:
            return False
        return self._dll.audio_set_key(key32, int(sender_id)) == 0

    def sync_peers(self, endpoints: list[dict], local_name: str):
        self._name_to_sid.clear()
        for ep in endpoints:
            ep_name = ep.get("name")
            sid = ep.get("sender_id")
            if ep_name and sid:
                self._name_to_sid[str(ep_name)] = int(sid)

        if not self._dll or not self.active_port:
            return
        self._dll.audio_remove_peers()
        for ep in endpoints:
            ep_name = str(ep.get("name", ""))
            if ep_name == local_name:
                continue
            ip = str(ep.get("ip", "")).strip()
            port = int(ep.get("udp_port") or DEFAULT_UDP_PORT)
            if ip and port > 0:
                self._dll.audio_add_peer(ip.encode("ascii", errors="ignore"), port)
            sid = int(ep.get("sender_id") or 0)
            if 0 < sid < 16:
                self._apply_peer_state_to_dll(ep_name, sid)

    def _apply_peer_state_to_dll(self, peer_name: str, sid: int):
        if not self._dll or sid <= 0 or sid >= 16:
            return
        vol = self.peer_volumes.get(peer_name, 100)
        is_muted = peer_name in self.peer_muted
        is_loop_muted = peer_name in self.peer_loopback_muted
        if hasattr(self._dll, "audio_set_peer_volume"):
            self._dll.audio_set_peer_volume(sid, int(vol))
        if hasattr(self._dll, "audio_set_peer_muted"):
            self._dll.audio_set_peer_muted(sid, 1 if is_muted else 0)
        if hasattr(self._dll, "audio_set_peer_loopback_muted"):
            self._dll.audio_set_peer_loopback_muted(sid, 1 if is_loop_muted else 0)

    def set_peer_volume(self, peer_name: str, volume_pct: int, sender_id: int | None = None):
        """Define o volume individual (0% a 150%) de um participante."""
        vol = max(0, min(150, int(volume_pct)))
        self.peer_volumes[peer_name] = vol
        sid = sender_id if sender_id is not None else self._name_to_sid.get(peer_name, 0)
        if self._dll and 0 < sid < 16 and hasattr(self._dll, "audio_set_peer_volume"):
            self._dll.audio_set_peer_volume(int(sid), vol)

    def get_peer_volume(self, peer_name: str) -> int:
        return self.peer_volumes.get(peer_name, 100)

    def set_peer_muted(self, peer_name: str, muted: bool, sender_id: int | None = None):
        """Muta/desmuta localmente a voz de um participante específico (Mutar para Mim)."""
        if muted:
            self.peer_muted.add(peer_name)
        else:
            self.peer_muted.discard(peer_name)
        sid = sender_id if sender_id is not None else self._name_to_sid.get(peer_name, 0)
        if self._dll and 0 < sid < 16 and hasattr(self._dll, "audio_set_peer_muted"):
            self._dll.audio_set_peer_muted(int(sid), 1 if muted else 0)

    def is_peer_muted(self, peer_name: str) -> bool:
        return peer_name in self.peer_muted

    def set_peer_loopback_muted(self, peer_name: str, muted: bool, sender_id: int | None = None):
        """Muta/desmuta localmente apenas o áudio da tela (loopback) daquele participante."""
        if muted:
            self.peer_loopback_muted.add(peer_name)
        else:
            self.peer_loopback_muted.discard(peer_name)
        sid = sender_id if sender_id is not None else self._name_to_sid.get(peer_name, 0)
        if self._dll and 0 < sid < 16 and hasattr(self._dll, "audio_set_peer_loopback_muted"):
            self._dll.audio_set_peer_loopback_muted(int(sid), 1 if muted else 0)

    def is_peer_loopback_muted(self, peer_name: str) -> bool:
        return peer_name in self.peer_loopback_muted

    def set_mic_muted(self, muted: bool):
        self.mic_muted = bool(muted)
        if self._dll:
            self._dll.audio_set_mic_muted(1 if self.mic_muted else 0)

    def set_deafened(self, deafened: bool):
        self.deafened = bool(deafened)
        if self._dll:
            self._dll.audio_set_deafened(1 if self.deafened else 0)

    def set_loopback(self, enabled: bool):
        self.loopback_enabled = bool(enabled)
        if self._dll and hasattr(self._dll, "audio_set_loopback"):
            self._dll.audio_set_loopback(1 if self.loopback_enabled else 0)

    def get_speaking_mask(self) -> int:
        if not self._dll or not self.active_port or not hasattr(self._dll, "audio_get_speaking_mask"):
            return 0
        return int(self._dll.audio_get_speaking_mask())

    def cleanup(self):
        self.loopback_enabled = False
        self.peer_volumes.clear()
        self.peer_muted.clear()
        self.peer_loopback_muted.clear()
        self._name_to_sid.clear()
        if self._dll and self.active_port:
            if hasattr(self._dll, "audio_set_loopback"):
                self._dll.audio_set_loopback(0)
            self._dll.audio_cleanup()
        self.active_port = 0

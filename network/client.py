"""Sessão do cliente: prova de conhecimento da senha (HMAC-SHA256) e transporte AES-256-GCM."""
import base64

from PyQt6.QtCore import QObject, pyqtSignal
from PyQt6.QtNetwork import QAbstractSocket, QTcpSocket

from .audio_bridge import DEFAULT_UDP_PORT
from .crypto import CHALLENGE_LEN, SALT_LEN, ReplayWindow, RoomKeys
from .media import run_async
from .protocol import (FrameReader, clean_ip, pack_encrypted,
                       pack_handshake)
from .video_bridge import DEFAULT_VIDEO_PORT


class ClientSession(QObject):
    applied = pyqtSignal(dict)
    peers_changed = pyqtSignal(list)
    endpoints_changed = pyqtSignal(list)
    streams_changed = pyqtSignal(list)
    authenticated = pyqtSignal(object, int)  # (RoomKeys, sender_id)
    host_disconnected = pyqtSignal()
    kicked = pyqtSignal(str)
    server_muted = pyqtSignal()
    status = pyqtSignal(str)
    media_ready = pyqtSignal()

    def __init__(
        self,
        store,
        name: str,
        host: str,
        port: int,
        password: str,
        udp_port: int = DEFAULT_UDP_PORT,
        video_port: int = DEFAULT_VIDEO_PORT,
        parent=None,
    ):
        super().__init__(parent)
        clean_host = host.strip()
        clean_port = port
        if ":" in clean_host and not clean_host.startswith("["):
            parts = clean_host.split(":")
            if len(parts) == 2 and parts[1].isdigit():
                clean_host = parts[0]
                clean_port = int(parts[1])

        self.store, self.name, self.host, self.port = store, name, clean_host, clean_port
        self.password = password
        self.udp_port = udp_port
        self.video_port = video_port

        self.keys: RoomKeys | None = None
        self.sender_id: int = 0
        self._tx_seq: int = 0
        self.replay = ReplayWindow()
        self._reader = FrameReader()
        self._handshake_done = False

        self._rejected = False
        self._requested = set()
        self._sock = QTcpSocket(self)
        self._sock.connected.connect(self._on_connected)
        self._sock.readyRead.connect(self._on_ready)
        self._sock.disconnected.connect(self._on_disconnected)
        self._sock.errorOccurred.connect(
            lambda _e: self.status.emit(f"Erro de conexão: {self._sock.errorString()}")
            if not self._rejected else None
        )

    def start(self) -> bool:
        self.status.emit(f"Conectando a {self.host}:{self.port}...")
        self._sock.connectToHost(self.host, self.port)
        return True

    def stop(self):
        try:
            self._sock.readyRead.disconnect()
            self._sock.disconnected.disconnect()
            self._sock.connected.disconnect()
            self._sock.errorOccurred.disconnect()
        except Exception:
            pass
        self._sock.abort()

    def send(self, req: dict) -> bool:
        if (
            self._sock.state() != QAbstractSocket.SocketState.ConnectedState
            or not self._handshake_done
            or not self.keys
            or not self.sender_id
        ):
            return False
        self._tx_seq += 1
        pkt = pack_encrypted(self.keys, self.sender_id, self._tx_seq, req)
        self._sock.write(pkt)
        return True

    def _on_connected(self):
        self.status.emit("Autenticando com criptografia ponta a ponta...")

    def _on_ready(self):
        if not self._handshake_done:
            for msg in self._reader.feed_and_read(self._sock, self.keys, self.replay, allow_handshake=True):
                t = msg.get("t")
                if t == "hello_challenge":
                    self._handle_challenge(msg)
                elif t == "auth_fail":
                    self._rejected = True
                    self.status.emit("Falha na autenticação (senha incorreta ou sala bloqueada)")
                    self._sock.disconnectFromHost()
                    return
                elif t == "auth_ok":
                    self.sender_id = int(msg.get("sender_id") or 0)
                    self._handshake_done = True
                    self.status.emit(f"Conectado e cifrado (AES-256-GCM) em {self.host}:{self.port}")
                    if self.keys and self.sender_id:
                        self.authenticated.emit(self.keys, self.sender_id)
                    break
            if not self._handshake_done:
                return

        # Já autenticado: processa apenas frames cifrados do Host (sender_id = 1)
        for msg in self._reader.feed_and_read(
            self._sock,
            self.keys,
            self.replay,
            allow_handshake=False,
            expected_sender_id=1,
        ):
            t = msg.get("t")
            if t == "peers":
                endpoints = msg.get("endpoints", [])
                peer_addr = clean_ip(self._sock.peerAddress()) if self._sock else ""
                fallback_host = peer_addr or self.host
                host_name = (msg.get("names") or [""])[0]

                for ep in endpoints:
                    # O Host tem sender_id = 1 ou seu nome coincide com o primeiro da lista
                    is_host = ep.get("sender_id") == 1 or ep.get("name") == host_name
                    ep_ip = str(ep.get("ip", "")).strip()

                    if is_host:
                        if fallback_host:
                            ep["ip"] = fallback_host
                    elif not ep_ip or ep_ip in ("0.0.0.0", "::", "::1", "localhost"):
                        if fallback_host and fallback_host not in ("127.0.0.1", "::1", "localhost"):
                            ep["ip"] = fallback_host
                    elif ep_ip.startswith("127.") and fallback_host and not fallback_host.startswith("127."):
                        ep["ip"] = fallback_host

                self.peers_changed.emit(msg["names"])
                self.endpoints_changed.emit(endpoints)
            elif t == "streams":
                self.streams_changed.emit(msg.get("active", []))
            elif t == "kicked":
                self._rejected = True
                reason = msg.get("text", "Você foi expulso da sala pelo Host")
                self.status.emit(reason)
                self.kicked.emit(reason)
                self._sock.disconnectFromHost()
                return
            elif t == "server_mute":
                if msg.get("target") == self.name:
                    self.server_muted.emit()
            elif t == "error":
                self._rejected = True
                self.status.emit(msg.get("text", "Conexão recusada"))
            elif t == "notice":
                self.status.emit(msg.get("text", ""))
            elif t == "media":
                self._save_media(msg.get("data"))
            else:
                self.store.apply(msg)
                self.applied.emit(msg)
                self._request_missing()

    def _handle_challenge(self, msg: dict):
        try:
            salt = base64.b64decode(msg.get("salt", ""))
            challenge = base64.b64decode(msg.get("challenge", ""))
        except Exception:
            self._sock.abort()
            return
        if len(salt) != SALT_LEN or len(challenge) != CHALLENGE_LEN:
            self._sock.abort()
            return

        # Deriva chaves com Argon2id uma única vez ao entrar na sala
        self.keys = RoomKeys(self.password, salt)
        proof = self.keys.compute_handshake_proof(challenge, self.name, self.udp_port, self.video_port)
        self._sock.write(pack_handshake({
            "t": "hello_auth",
            "name": self.name,
            "udp_port": self.udp_port,
            "video_port": self.video_port,
            "proof": base64.b64encode(proof).decode("ascii"),
        }))

    def _request_missing(self):
        for name in self.store.missing_media():
            if name not in self._requested:
                self._requested.add(name)
                self.send({"t": "media_get", "name": name})

    def _save_media(self, data):
        if not isinstance(data, str):
            return
        run_async(lambda: self.store.save_media(base64.b64decode(data)),
                  lambda name: self.media_ready.emit() if name else None)

    def _on_disconnected(self):
        was_connected = self._handshake_done
        self._handshake_done = False
        self._requested.clear()
        self.peers_changed.emit([])
        self.endpoints_changed.emit([])
        self.streams_changed.emit([])
        if not self._rejected:
            if was_connected:
                self.status.emit("Host desconectado — conexão encerrada")
                self.host_disconnected.emit()
            else:
                self.status.emit("Desconectado")

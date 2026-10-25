"""Sessão do host: handshake desafio-resposta, anti-bruteforce, AES-256-GCM e rate limit."""
import base64
import secrets
import time

from PyQt6.QtCore import QObject, QTimer, pyqtSignal
from PyQt6.QtNetwork import QHostAddress, QTcpServer

from database.media import MAX_BYTES, valid_name
from .audio_bridge import DEFAULT_UDP_PORT
from .crypto import CHALLENGE_LEN, SALT_LEN, ReplayWindow, RoomKeys
from .media import run_async
from .protocol import (FrameReader, find_radmin_ip, pack_encrypted,
                       pack_handshake)
from .video_bridge import DEFAULT_VIDEO_PORT, MAX_STREAMS

MAX_CONNECTIONS = 6          # até 4 amigos + margem para handshakes
HANDSHAKE_TIMEOUT_MS = 5000  # 5 segundos para autenticar
MAX_FAILS_PER_MIN = 10
LOCKOUT_SECONDS = 60.0
MAX_BACKOFF_SECONDS = 300.0
RATE_LIMIT_PER_SEC = 25


def _clean_ip(addr: QHostAddress) -> str:
    ip4 = addr.toIPv4Address()
    if ip4:
        return QHostAddress(ip4).toString()
    s = addr.toString()
    return s[7:] if s.startswith("::ffff:") else s


class HostSession(QObject):
    applied = pyqtSignal(dict)            # evento aplicado ao banco
    peers_changed = pyqtSignal(list)      # lista de nomes
    endpoints_changed = pyqtSignal(list)  # lista de dicts {name, ip, udp_port, video_port, sender_id}
    streams_changed = pyqtSignal(list)    # lista de nomes transmitindo tela (máx 2)
    status = pyqtSignal(str)

    def __init__(
        self,
        store,
        name: str,
        port: int,
        password: str,
        udp_port: int = DEFAULT_UDP_PORT,
        video_port: int = DEFAULT_VIDEO_PORT,
        parent=None,
    ):
        super().__init__(parent)
        self.store, self.name, self.port = store, name, port
        self.udp_port = udp_port
        self.video_port = video_port
        self.sender_id = 1
        self._tx_seq = 0

        # Deriva chaves uma única vez na criação da sala
        self.salt = secrets.token_bytes(SALT_LEN)
        self.keys = RoomKeys(password, self.salt)
        self.replay = ReplayWindow()

        self._peers = {}                  # sock -> {name, ip, udp_port, video_port, sender_id, reader, rate}
        self._pending = {}                # sock -> {challenge, reader, timer, ip}
        self._next_sender_id = 2
        self._streamers: list[str] = []

        # Estado anti-bruteforce
        self._ip_backoff: dict[str, list[float]] = {}  # ip -> [fail_count, blocked_until_ts]
        self._global_fails: list[float] = []           # timestamps de falhas recentes
        self._lockout_until = 0.0

        self._server = QTcpServer(self)
        self._server.newConnection.connect(self._on_connection)

    def start(self) -> bool:
        radmin_ip = find_radmin_ip()
        # Escuta na interface Radmin quando detectada ou permite loopback+LAN avisando o usuário
        bind_addr = QHostAddress(QHostAddress.SpecialAddress.Any)
        if not self._server.listen(bind_addr, self.port):
            self.status.emit(f"Falha ao abrir porta {self.port}: {self._server.errorString()}")
            return False

        if radmin_ip:
            self.status.emit(f"Hospedando cifrado ({radmin_ip}:{self.port})")
        else:
            self.status.emit(f"Hospedando cifrado na porta {self.port} (escutando em 0.0.0.0)")

        self.peers_changed.emit(self.peers())
        self.endpoints_changed.emit([])
        self.streams_changed.emit(self._streamers)
        return True

    def stop(self):
        for sock in list(self._pending):
            sock.abort()
        for sock in list(self._peers):
            sock.disconnectFromHost()
        self._server.close()

    def peers(self):
        return [self.name] + [info["name"] for info in self._peers.values()]

    def send(self, req: dict) -> bool:
        self._process(self.name, req)
        return True

    def _next_seq(self) -> int:
        self._tx_seq += 1
        return self._tx_seq

    def _send_encrypted(self, sock, msg: dict):
        pkt = pack_encrypted(self.keys, self.sender_id, self._next_seq(), msg)
        sock.write(pkt)

    def _record_auth_failure(self, ip: str):
        now = time.monotonic()
        st = self._ip_backoff.get(ip, [0, 0.0])
        fails = int(st[0]) + 1
        delay = min(MAX_BACKOFF_SECONDS, float(1 << min(fails - 1, 9)))
        self._ip_backoff[ip] = [fails, now + delay]

        self._global_fails = [t for t in self._global_fails if now - t < 60.0]
        self._global_fails.append(now)
        if len(self._global_fails) >= MAX_FAILS_PER_MIN:
            self._lockout_until = now + LOCKOUT_SECONDS
            self.status.emit("Alerta de segurança: 10 falhas de senha/min. Novos handshakes bloqueados por 60s.")

    def _reject_uniform(self, sock, ip: str, record_fail: bool = True):
        """Resposta uniforme em falhas sem distinguir o motivo."""
        if record_fail:
            self._record_auth_failure(ip)
        try:
            sock.write(pack_handshake({"t": "auth_fail", "text": "Falha na autenticação"}))
            sock.flush()
        except Exception:
            pass
        sock.disconnectFromHost()

    def _on_connection(self):
        sock = self._server.nextPendingConnection()
        if not sock:
            return
        now = time.monotonic()
        ip = _clean_ip(sock.peerAddress())

        # Limite de conexões simultâneas
        if len(self._peers) + len(self._pending) >= MAX_CONNECTIONS:
            sock.abort()
            sock.deleteLater()
            return

        # Verifica bloqueio global ou backoff do IP
        if now < self._lockout_until:
            self._reject_uniform(sock, ip, record_fail=False)
            sock.deleteLater()
            return

        ip_st = self._ip_backoff.get(ip)
        if ip_st and now < ip_st[1]:
            self._reject_uniform(sock, ip, record_fail=False)
            sock.deleteLater()
            return

        challenge = secrets.token_bytes(CHALLENGE_LEN)
        timer = QTimer(self)
        timer.setSingleShot(True)
        timer.timeout.connect(lambda s=sock: self._on_handshake_timeout(s))
        timer.start(HANDSHAKE_TIMEOUT_MS)

        self._pending[sock] = {
            "challenge": challenge,
            "reader": FrameReader(),
            "timer": timer,
            "ip": ip,
        }
        sock.readyRead.connect(lambda s=sock: self._on_ready(s))
        sock.disconnected.connect(lambda s=sock: self._on_gone(s))

        # Envia salt da sala + desafio aleatório
        sock.write(pack_handshake({
            "t": "hello_challenge",
            "salt": base64.b64encode(self.salt).decode("ascii"),
            "challenge": base64.b64encode(challenge).decode("ascii"),
        }))

    def _on_handshake_timeout(self, sock):
        pend = self._pending.pop(sock, None)
        if pend:
            self._reject_uniform(sock, pend["ip"], record_fail=True)

    def _on_ready(self, sock):
        if sock in self._pending:
            pend = self._pending[sock]
            for msg in pend["reader"].feed_and_read(sock, None, None, allow_handshake=True):
                if msg.get("t") == "hello_auth":
                    self._authenticate_peer(sock, pend, msg)
                    return
                else:
                    self._pending.pop(sock, None)
                    pend["timer"].stop()
                    self._reject_uniform(sock, pend["ip"], record_fail=True)
                    return
            return

        info = self._peers.get(sock)
        if not info:
            return

        # Rate limit por conexão
        now = time.monotonic()
        rate = info["rate"]
        if now - rate[0] >= 1.0:
            rate[0], rate[1] = now, 0

        for msg in info["reader"].feed_and_read(
            sock,
            self.keys,
            self.replay,
            allow_handshake=False,
            expected_sender_id=info["sender_id"],
        ):
            rate[1] += 1
            if rate[1] > RATE_LIMIT_PER_SEC:
                continue
            if msg.get("t") == "media_get":
                self._serve_media(sock, msg.get("name"))
            else:
                # O host define o author pela conexão autenticada (ignora qualquer 'author' vindo do cliente)
                self._process(info["name"], msg)

    def _authenticate_peer(self, sock, pend: dict, msg: dict):
        pend["timer"].stop()
        self._pending.pop(sock, None)
        ip = pend["ip"]

        name = str(msg.get("name", "")).strip()
        udp_port = int(msg.get("udp_port") or DEFAULT_UDP_PORT)
        video_port = int(msg.get("video_port") or DEFAULT_VIDEO_PORT)
        proof_b64 = msg.get("proof", "")

        try:
            proof = base64.b64decode(proof_b64) if isinstance(proof_b64, str) else b""
        except Exception:
            proof = b""

        # Sempre executa a verificação HMAC em tempo constante antes de checar nome duplicado
        proof_ok = self.keys.verify_handshake_proof(proof, pend["challenge"], name, udp_port, video_port)
        name_ok = bool(name) and len(name) <= 32 and (name not in self.peers()) and (len(self._peers) < 4)

        if not (proof_ok and name_ok):
            self._reject_uniform(sock, ip, record_fail=not proof_ok)
            return

        # Sucesso: zera backoff do IP
        self._ip_backoff.pop(ip, None)
        assigned_id = self._next_sender_id
        self._next_sender_id += 1

        self._peers[sock] = {
            "name": name,
            "ip": ip,
            "udp_port": udp_port,
            "video_port": video_port,
            "sender_id": assigned_id,
            "reader": FrameReader(),
            "rate": [time.monotonic(), 0],
        }

        self._send_encrypted(sock, {"t": "auth_ok", "sender_id": assigned_id})
        self._send_encrypted(sock, {"t": "snapshot", **self.store.snapshot()})
        self._send_peers()
        self._send_streams()
        self.status.emit(f"{name} entrou (autenticado)")

    def _on_gone(self, sock):
        pend = self._pending.pop(sock, None)
        if pend:
            pend["timer"].stop()
        info = self._peers.pop(sock, None)
        sock.deleteLater()
        if info:
            self.replay.reset_sender(info["sender_id"])
            name = info["name"]
            if name in self._streamers:
                self._streamers.remove(name)
                self._send_streams()
            self._send_peers()
            self.status.emit(f"{name} saiu")

    def _send_peers(self):
        names = self.peers()
        self.peers_changed.emit(names)
        client_eps = [
            {
                "name": p["name"],
                "ip": p["ip"],
                "udp_port": p["udp_port"],
                "video_port": p["video_port"],
                "sender_id": p["sender_id"],
            }
            for p in self._peers.values()
        ]
        self.endpoints_changed.emit(client_eps)

        for sock in self._peers:
            host_ip = _clean_ip(sock.localAddress())
            eps = [{
                "name": self.name,
                "ip": host_ip,
                "udp_port": self.udp_port,
                "video_port": self.video_port,
                "sender_id": self.sender_id,
            }] + client_eps
            self._send_encrypted(sock, {"t": "peers", "names": names, "endpoints": eps})

    def _send_streams(self):
        active = list(self._streamers)
        self.streams_changed.emit(active)
        self._to_all({"t": "streams", "active": active})

    def _to_all(self, msg: dict):
        for sock in list(self._peers):
            self._send_encrypted(sock, msg)

    def _commit(self, ev: dict):
        self.store.apply(ev)
        self.applied.emit(ev)
        self._to_all(ev)

    def _notice(self, author: str, text: str):
        if author == self.name:
            self.status.emit(text)
            return
        for sock, info in self._peers.items():
            if info["name"] == author:
                self._send_encrypted(sock, {"t": "notice", "text": text})

    def _serve_media(self, sock, name):
        if not valid_name(name) or not self.store.has_media(name):
            return
        path = self.store.media_path(name)

        def done(data):
            if data and sock in self._peers:
                self._send_encrypted(sock, {"t": "media", "name": name, "data": data})

        run_async(lambda: base64.b64encode(path.read_bytes()).decode("ascii"), done)

    def _ingest(self, author: str, req: dict, img):
        data = img.get("data") if isinstance(img, dict) else None
        clean = {k: v for k, v in req.items() if k not in ("img", "author")}

        def work():
            if not isinstance(data, str) or len(data) > MAX_BYTES * 4 // 3 + 8:
                return None
            return self.store.save_media(base64.b64decode(data))

        def done(name):
            if name is None:
                self._notice(author, "Imagem inválida ou maior que 10 MB")
                return
            clean["image"] = name
            self._process(author, clean)

        run_async(work, done)

    def _process(self, author: str, req: dict):
        """Autor é sempre determinado pela sessão autenticada."""
        if req.get("img"):
            self._ingest(author, req, req["img"])
            return
        s, t, now = self.store, req.get("t"), time.time()
        text = str(req.get("text", "")).strip()
        image = req.get("image")
        if not (valid_name(image) and s.has_media(image)):
            image = None

        if t == "stream_start":
            if author in self._streamers:
                return
            if len(self._streamers) >= MAX_STREAMS:
                self._notice(author, "Limite atingido: já existem 2 transmissões de tela ativas.")
                return
            self._streamers.append(author)
            self._send_streams()
            self.status.emit(f"{author} iniciou compartilhamento de tela")
        elif t == "stream_stop":
            if author in self._streamers:
                self._streamers.remove(author)
                self._send_streams()
                self.status.emit(f"{author} encerrou o compartilhamento de tela")
        elif t == "kick":
            if author != self.name:
                self._notice(author, "Apenas o Host pode expulsar participantes.")
                return
            target = str(req.get("target", "")).strip()
            if not target or target == self.name:
                return
            target_sock = None
            target_info = None
            for sock, info in list(self._peers.items()):
                if info["name"] == target or info["ip"] == target:
                    target_sock = sock
                    target_info = info
                    break
            if target_sock and target_info:
                kicked_name = target_info["name"]
                try:
                    self._send_encrypted(
                        target_sock,
                        {"t": "kicked", "text": "Você foi expulso da sala pelo Host"},
                    )
                    target_sock.flush()
                except Exception:
                    pass
                self._peers.pop(target_sock, None)
                self.replay.reset_sender(target_info["sender_id"])
                if kicked_name in self._streamers:
                    self._streamers.remove(kicked_name)
                self._send_streams()
                self._send_peers()
                target_sock.disconnectFromHost()
                self.status.emit(f"{kicked_name} foi expulso da sala")
        elif t == "server_mute":
            if author != self.name:
                self._notice(author, "Apenas o Host pode silenciar participantes para todos.")
                return
            target = str(req.get("target", "")).strip()
            if not target or target == self.name:
                return
            for sock, info in list(self._peers.items()):
                if info["name"] == target or info["ip"] == target:
                    muted_name = info["name"]
                    self._send_encrypted(sock, {"t": "server_mute", "target": muted_name})
                    self.status.emit(f"{muted_name} foi silenciado para todos")
                    break
        elif t == "chat" and (text or image):
            reply_info = None
            raw_reply = req.get("reply_to")
            if isinstance(raw_reply, dict) and raw_reply.get("id") is not None:
                rid = int(raw_reply["id"])
                orig = s.get("chat", rid)
                r_author = orig["author"] if orig else str(raw_reply.get("author", "")).strip()
                r_text = (orig["text"] or ("[Imagem]" if orig.get("image") else "")) if orig else str(raw_reply.get("text", "")).strip()
                reply_info = {"id": rid, "author": r_author[:32], "text": r_text[:120]}
            self._commit({
                "t": "chat",
                "id": s.next_id("chat"),
                "author": author,
                "text": text,
                "ts": now,
                "image": image,
                "reply_to": reply_info,
                "reply_to_id": reply_info["id"] if reply_info else None,
                "reply_author": reply_info["author"] if reply_info else None,
                "reply_text": reply_info["text"] if reply_info else None,
            })
        elif t == "chat_del":
            msg_id = req.get("id")
            orig = s.get("chat", msg_id)
            if orig:
                if author == orig["author"] or author == self.name:
                    self._commit({"t": "chat_del", "id": msg_id})
                else:
                    self._notice(author, "Sem permissão para apagar esta mensagem")
        elif t == "task_add" and text:
            self._commit({"t": "task", "id": s.next_id("tasks"), "text": text,
                          "done": 0, "author": author, "image": image})
        elif t == "task_image" and image:
            task = s.get("tasks", req.get("id"))
            if task:
                task["image"] = image
                self._commit({"t": "task", **task})
        elif t == "task_toggle":
            task = s.get("tasks", req.get("id"))
            if task:
                task["done"] = 0 if task["done"] else 1
                self._commit({"t": "task", **task})
        elif t == "task_del" and s.get("tasks", req.get("id")):
            self._commit({"t": "task_del", "id": req["id"]})
        elif t == "note_save":
            note_id = req.get("id") or s.next_id("notes")
            self._commit({"t": "note", "id": note_id, "title": (req.get("title") or "Sem título")[:300],
                          "body": req.get("body", ""), "author": author, "ts": now})
        elif t == "note_del" and s.get("notes", req.get("id")):
            self._commit({"t": "note_del", "id": req["id"]})

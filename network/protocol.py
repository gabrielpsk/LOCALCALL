"""Protocolo TCP binário cifrado (AES-256-GCM) e validação estrita de mensagens."""
import json
import struct

from PyQt6.QtNetwork import QNetworkInterface, QAbstractSocket

from .crypto import NONCE_LEN, TAG_LEN, ReplayWindow, RoomKeys

DEFAULT_PORT = 45000

FRAME_HANDSHAKE = 0x01
FRAME_DATA      = 0x02
FRAME_MEDIA     = 0x03

MAX_HANDSHAKE_BYTES = 4 * 1024
MAX_DATA_BYTES      = 64 * 1024          # 64 KB para chat/tarefas/notas/controle
MAX_MEDIA_BYTES     = 16 * 1024 * 1024   # ~10 MB em base64 + metadados

ALLOWED_TYPES = {
    "hello_challenge", "hello_auth", "auth_ok", "auth_fail",
    "snapshot", "peers", "streams", "notice", "error",
    "chat", "chat_del",
    "task", "task_add", "task_image", "task_toggle", "task_del",
    "note", "note_save", "note_del",
    "media_get", "media",
    "stream_start", "stream_stop",
    "kick", "kicked", "server_mute",
}


def is_cgnat_ip(ip: str) -> bool:
    """Verifica se o IP pertence à faixa Carrier-Grade NAT (RFC 6598: 100.64.0.0/10)."""
    if not ip or not ip.startswith("100."):
        return False
    parts = ip.split(".")
    if len(parts) >= 2 and parts[1].isdigit():
        val = int(parts[1])
        return 64 <= val <= 127
    return False


def is_private_ip(ip: str) -> bool:
    """Verifica se o IP pertence a faixas privadas / locais / CGNAT / VPN."""
    if not ip:
        return True
    if ip.startswith(("10.", "192.168.", "127.", "169.254.", "26.")):
        return True
    if ip.startswith("172."):
        parts = ip.split(".")
        if len(parts) >= 2 and parts[1].isdigit():
            val = int(parts[1])
            if 16 <= val <= 31:
                return True
    if is_cgnat_ip(ip):
        return True
    return False


def list_network_ips(include_public: bool = True) -> list[tuple[str, str]]:
    """
    Lista IPs IPv4 ordenados por prioridade de rota e velocidade:
    1. Rede Local LAN (Ethernet / Wi-Fi - velocidade nativa máxima, latência < 1ms)
    2. IP Público Direto / UPnP (Conexão direta pela internet sem VPN)
    3. VPN existente se houver (Radmin VPN, ZeroTier, Tailscale, tun/tap)
    Retorna lista de tuplas (ip, descricao_amigavel).
    """
    lan_ips: list[tuple[str, str]] = []
    zerotier_ips: list[tuple[str, str]] = []
    vpn_ips: list[tuple[str, str]] = []
    radmin_ips: list[tuple[str, str]] = []

    for iface in QNetworkInterface.allInterfaces():
        flags = iface.flags()
        if (
            not (flags & QNetworkInterface.InterfaceFlag.IsUp)
            or not (flags & QNetworkInterface.InterfaceFlag.IsRunning)
            or bool(flags & QNetworkInterface.InterfaceFlag.IsLoopBack)
        ):
            continue
        ifname = (iface.name() or iface.humanReadableName() or "net").strip()
        ifname_low = ifname.lower()

        for entry in iface.addressEntries():
            ip_obj = entry.ip()
            if ip_obj.protocol() != QAbstractSocket.NetworkLayerProtocol.IPv4Protocol:
                continue
            s = ip_obj.toString()
            if not s or s.startswith("127.") or s.startswith("169.254."):
                continue

            if s.startswith("26."):
                radmin_ips.append((s, f"{s} (Radmin VPN)"))
            elif ifname_low.startswith("zt"):
                zerotier_ips.append((s, f"{s} (ZeroTier: {ifname})"))
            elif ifname_low.startswith(("tun", "tap", "wg", "tailscale", "ppp", "ham")):
                vpn_ips.append((s, f"{s} (VPN: {ifname})"))
            else:
                lan_ips.append((s, f"{s} (Rede Local: {ifname})"))

    public_ips: list[tuple[str, str]] = []
    if include_public:
        try:
            from .upnp import get_public_ip
            pub = get_public_ip(timeout=1.0)
            if pub and not any(pub == item[0] for item in (lan_ips + radmin_ips + zerotier_ips + vpn_ips)):
                public_ips.append((pub, f"{pub} (IP Público / UPnP Direto)"))
        except Exception:
            pass

    return lan_ips + public_ips + vpn_ips + zerotier_ips + radmin_ips


def find_primary_lan_ip() -> str | None:
    """Retorna o IP da interface local LAN preferencial (192.168.x.x ou 10.x.x.x)."""
    candidates = list_network_ips(include_public=False)
    for ip, _ in candidates:
        if ip.startswith(("192.168.", "10.")) or (ip.startswith("172.") and is_private_ip(ip)):
            return ip
    return candidates[0][0] if candidates else None


def find_preferred_ip() -> str | None:
    """Retorna o melhor IP padrão: LAN > IP Público > VPN."""
    candidates = list_network_ips(include_public=True)
    return candidates[0][0] if candidates else None


def find_radmin_ip() -> str | None:
    """Compatibilidade retroativa: retorna o melhor IP disponível (LAN > Público > VPN)."""
    return find_preferred_ip()


def clean_ip(addr) -> str:
    """Extrai string IPv4 limpa (remove prefixo IPv4-mapped ::ffff: e trata QHostAddress)."""
    if addr is None:
        return ""
    if hasattr(addr, "toIPv4Address"):
        res = addr.toIPv4Address()
        if isinstance(res, tuple):
            val, ok = res
            if ok:
                from PyQt6.QtNetwork import QHostAddress
                return QHostAddress(val).toString()
        elif isinstance(res, int) and res:
            from PyQt6.QtNetwork import QHostAddress
            return QHostAddress(res).toString()
    s = addr.toString() if hasattr(addr, "toString") else str(addr)
    return s[7:] if s.startswith("::ffff:") else s


def best_matching_ip(peer_ip: str) -> str | None:
    """Encontra o IP de interface local mais adequado para comunicar com peer_ip."""
    if not peer_ip:
        return None
    candidates = [ip for ip, _ in list_network_ips(include_public=False)]
    if peer_ip.startswith("26."):
        for c in candidates:
            if c.startswith("26."):
                return c
    parts = peer_ip.split(".")
    if len(parts) == 4:
        pfx24 = ".".join(parts[:3]) + "."
        for c in candidates:
            if c.startswith(pfx24):
                return c
        pfx16 = ".".join(parts[:2]) + "."
        for c in candidates:
            if c.startswith(pfx16):
                return c
    return candidates[0] if candidates else None


def validate_message(msg: dict) -> bool:
    """Valida tipos e tamanhos dos campos do dicionário JSON recebido."""
    if not isinstance(msg, dict):
        return False
    t = msg.get("t")
    if not isinstance(t, str) or t not in ALLOWED_TYPES:
        return False

    if "text" in msg and (not isinstance(msg["text"], str) or len(msg["text"]) > 8000):
        return False
    if "title" in msg and (not isinstance(msg["title"], str) or len(msg["title"]) > 300):
        return False
    if "body" in msg and (not isinstance(msg["body"], str) or len(msg["body"]) > 32000):
        return False
    if "name" in msg and (not isinstance(msg["name"], str) or len(msg["name"]) > 64):
        return False
    if "target" in msg and (not isinstance(msg["target"], str) or len(msg["target"]) > 64):
        return False
    if "id" in msg and msg["id"] is not None and not isinstance(msg["id"], int):
        return False
    if "img" in msg:
        img = msg["img"]
        if not isinstance(img, dict) or not isinstance(img.get("data"), str):
            return False
        if len(img["data"]) > MAX_MEDIA_BYTES:
            return False
    return True


def pack_handshake(msg: dict) -> bytes:
    """Enquadra mensagem inicial de handshake (desafio/prova): [len:4B][0x01][json]."""
    raw = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    body = struct.pack("!B", FRAME_HANDSHAKE) + raw
    return struct.pack("!I", len(body)) + body


def pack_encrypted(keys: RoomKeys, sender_id: int, seq: int, msg: dict) -> bytes:
    """Enquadra mensagem cifrada: [len:4B][frame_type:1B][nonce:12B][ciphertext+tag]."""
    raw = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    has_media = (
        msg.get("t") in ("media", "snapshot")
        or bool(msg.get("img"))
        or len(raw) > (MAX_DATA_BYTES - 1024)
    )
    ftype = FRAME_MEDIA if has_media else FRAME_DATA
    nonce, ct = keys.encrypt_tcp(sender_id, seq, raw, frame_type=ftype)
    body = struct.pack("!B", ftype) + nonce + ct
    return struct.pack("!I", len(body)) + body


class FrameReader:
    """
    Leitor de frames TCP por conexão:
    Verifica o limite de tamanho ANTES de alocar/acumular o frame na memória.
    """

    def __init__(self):
        self._buf = bytearray()

    def feed_and_read(
        self,
        sock,
        keys: RoomKeys | None,
        replay: ReplayWindow | None,
        allow_handshake: bool = False,
        expected_sender_id: int | None = None,
    ):
        avail = sock.bytesAvailable()
        if avail > 0:
            # Lê em blocos limitados para nunca alocar pacotes gigantes de uma vez
            chunk = bytes(sock.read(min(avail, MAX_MEDIA_BYTES + 4096)))
            self._buf.extend(chunk)

        while len(self._buf) >= 5:
            body_len = struct.unpack("!I", self._buf[:4])[0]
            ftype = self._buf[4]

            # Valida tamanho máximo ANTES de aguardar/alocar o restante do frame
            if ftype == FRAME_HANDSHAKE:
                limit = MAX_HANDSHAKE_BYTES if allow_handshake else 0
            elif ftype == FRAME_DATA:
                limit = MAX_DATA_BYTES
            elif ftype == FRAME_MEDIA:
                limit = MAX_MEDIA_BYTES
            else:
                limit = 0

            if body_len <= 1 or body_len > limit:
                self._buf.clear()
                sock.abort()
                return

            if len(self._buf) < 4 + body_len:
                break

            frame = bytes(self._buf[5 : 4 + body_len])
            del self._buf[: 4 + body_len]

            if ftype == FRAME_HANDSHAKE:
                try:
                    obj = json.loads(frame.decode("utf-8"))
                except Exception:
                    sock.abort()
                    return
                if validate_message(obj):
                    yield obj
                else:
                    sock.abort()
                    return
            else:
                if keys is None or len(frame) < NONCE_LEN + TAG_LEN:
                    continue
                nonce = frame[:NONCE_LEN]
                ct = frame[NONCE_LEN:]
                res = keys.decrypt_tcp(nonce, ct, frame_type=ftype)
                if res is None:
                    # Pacote com tag GCM inválida: descartado em silêncio
                    continue
                sender_id, seq, pt = res
                if expected_sender_id is not None and sender_id != expected_sender_id:
                    continue
                if replay is not None and not replay.accept(sender_id, seq):
                    # Pacote repetido ou antigo (replay): descartado em silêncio
                    continue
                try:
                    obj = json.loads(pt.decode("utf-8"))
                except Exception:
                    continue
                if validate_message(obj):
                    yield obj

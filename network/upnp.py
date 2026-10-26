"""Módulo de conectividade direta P2P: UPnP IGD (Internet Gateway Device) nativo e consulta STUN.

Permite que o Host abra automaticamente portas no roteador (TCP 45000, UDP 45001, UDP 45002)
e descubra seu IP público sem qualquer servidor intermediário de mídia, eliminando a
necessidade de VPNs de terceiros (como Radmin VPN).
"""
import atexit
import ipaddress
import logging
import re
import secrets
import socket
import struct
import threading
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

logger = logging.getLogger("LocalCall.UPnP")

STUN_SERVERS = (
    ("stun.l.google.com", 19302),
    ("stun1.l.google.com", 19302),
    ("stun.cloudflare.com", 3478),
)

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900

UPNP_SERVICE_TYPES = (
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:2",
)

_cached_public_ip: str | None = None
_cached_ip_lock = threading.Lock()


def is_valid_public_ipv4(ip_str: str) -> bool:
    """Verifica se a string é um IPv4 público válido (não privado, não loopback, não CGNAT)."""
    try:
        ip = ipaddress.IPv4Address(ip_str)
        if (
            ip.is_private
            or ip.is_loopback
            or ip.is_multicast
            or ip.is_unspecified
            or ip.is_reserved
            or ip.is_link_local
        ):
            return False
        # Verifica CGNAT RFC 6598 (100.64.0.0/10)
        cgnat_net = ipaddress.IPv4Network("100.64.0.0/10")
        if ip in cgnat_net:
            return False
        return True
    except Exception:
        return False


def is_cgnat_ipv4(ip_str: str) -> bool:
    """Verifica se o IP está no bloco CGNAT RFC 6598 (100.64.0.0/10)."""
    try:
        ip = ipaddress.IPv4Address(ip_str)
        return ip in ipaddress.IPv4Network("100.64.0.0/10")
    except Exception:
        return False


def _is_safe_ssdp_url(url: str) -> bool:
    """Hardening contra SSRF: valida se a URL SSDP aponta exclusivamente para a rede local privada."""
    try:
        parsed = urllib.parse.urlparse(url)
        if parsed.scheme not in ("http", "https"):
            return False
        host = (parsed.hostname or "").lower()
        if not host:
            return False
        # Bloqueia vetores de SSRF contra serviços de nuvem ou loopback
        if host in ("169.254.169.254", "metadata.google.internal", "localhost", "127.0.0.1"):
            return False
        try:
            ip = ipaddress.ip_address(host)
            if ip.is_private and not ip.is_loopback and not ip.is_link_local:
                return True
            return False
        except ValueError:
            return host.endswith(".local") or "." not in host
    except Exception:
        return False


def get_public_ip(timeout: float = 2.0) -> str | None:
    """Descobre o IP público externo através de uma consulta UDP STUN simples (RFC 5389).

    Nenhum dado pessoal, senha ou mídia trafega no STUN; apenas um datagrama padrão
    de 20 bytes para reflexão de endereço IP.
    """
    global _cached_public_ip
    with _cached_ip_lock:
        if _cached_public_ip:
            return _cached_public_ip

    for host, port in STUN_SERVERS:
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(timeout)
            # STUN Binding Request (RFC 5389): Type=0x0001, Length=0, Magic Cookie=0x2112A442
            # Hardening: Transaction ID criptograficamente aleatório de 12 bytes contra spoofing
            tx_id = secrets.token_bytes(12)
            req = struct.pack("!HHI12s", 0x0001, 0x0000, 0x2112A442, tx_id)
            sock.sendto(req, (host, port))
            resp, _ = sock.recvfrom(2048)

            if len(resp) >= 20:
                msg_type, msg_len, cookie = struct.unpack("!HHI", resp[:8])
                resp_tx_id = resp[8:20]
                # 0x0101 = Binding Response com validação estrita da transação
                if msg_type == 0x0101 and cookie == 0x2112A442 and resp_tx_id == tx_id:
                    pos = 20
                    limit = min(len(resp), 20 + msg_len)
                    while pos + 4 <= limit:
                        attr_type, attr_len = struct.unpack("!HH", resp[pos : pos + 4])
                        val_pos = pos + 4
                        if attr_type == 0x0020 and attr_len >= 8:  # XOR-MAPPED-ADDRESS
                            _, family, xport = struct.unpack("!BBH", resp[val_pos : val_pos + 4])
                            if family == 0x01:  # IPv4
                                xip = struct.unpack("!I", resp[val_pos + 4 : val_pos + 8])[0]
                                ip_int = xip ^ 0x2112A442
                                ip_str = socket.inet_ntoa(struct.pack("!I", ip_int))
                                if is_valid_public_ipv4(ip_str):
                                    with _cached_ip_lock:
                                        _cached_public_ip = ip_str
                                    return ip_str
                        elif attr_type == 0x0001 and attr_len >= 8:  # MAPPED-ADDRESS
                            _, family, mport = struct.unpack("!BBH", resp[val_pos : val_pos + 4])
                            if family == 0x01:
                                ip_str = socket.inet_ntoa(resp[val_pos + 4 : val_pos + 8])
                                if is_valid_public_ipv4(ip_str):
                                    with _cached_ip_lock:
                                        _cached_public_ip = ip_str
                                    return ip_str
                        pos += 4 + attr_len
        except Exception:
            continue
        finally:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass

    return None


class UPnPManager:
    """Gerenciador UPnP IGD nativo em Python (com fallback para Windows NATUPnP COM).

    Abre e remove automaticamente os mapeamentos de porta no roteador do usuário
    para chamadas de áudio, vídeo e controle direto sem VPN.
    """

    def __init__(self):
        self.control_url: str | None = None
        self.service_type: str | None = None
        self.router_url: str | None = None
        self._mapped_ports: list[tuple[int, str]] = []  # [(port, "TCP"), (port, "UDP")]
        self._lock = threading.Lock()
        self._discovered = False
        atexit.register(self.close_all_ports)

    def discover(self, timeout: float = 2.0) -> bool:
        """Descobre o dispositivo IGD (roteador) na rede local via SSDP."""
        with self._lock:
            if self._discovered and self.control_url and self.service_type:
                return True

        # Cria socket UDP para envio de SSDP M-SEARCH
        search_targets = (
            "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
            "urn:schemas-upnp-org:service:WANIPConnection:1",
            "urn:schemas-upnp-org:service:WANPPPConnection:1",
            "upnp:rootdevice",
            "ssdp:all",
        )

        location_urls: set[str] = set()

        for target in search_targets:
            sock = None
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
                sock.settimeout(timeout)
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)

                msg = (
                    f"M-SEARCH * HTTP/1.1\r\n"
                    f"HOST: {SSDP_ADDR}:{SSDP_PORT}\r\n"
                    f'MAN: "ssdp:discover"\r\n'
                    f"MX: 2\r\n"
                    f"ST: {target}\r\n\r\n"
                ).encode("ascii")

                sock.sendto(msg, (SSDP_ADDR, SSDP_PORT))

                while True:
                    try:
                        data, _ = sock.recvfrom(4096)
                        text = data.decode("utf-8", errors="ignore")
                        match = re.search(r"(?i)location:\s*(http[^\r\n]+)", text)
                        if match:
                            location_urls.add(match.group(1).strip())
                    except socket.timeout:
                        break
                    except Exception:
                        break
            except Exception:
                pass
            finally:
                if sock:
                    try:
                        sock.close()
                    except Exception:
                        pass
            if location_urls:
                break

        # Examina os XMLs de localização encontrados
        for loc in location_urls:
            if self._parse_igd_description(loc, timeout=timeout):
                with self._lock:
                    self._discovered = True
                return True

        return False

    def _parse_igd_description(self, url: str, timeout: float = 2.0) -> bool:
        """Baixa o XML de descrição do roteador e localiza o controlURL de WANIP/PPP."""
        if not _is_safe_ssdp_url(url):
            return False
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "LocalCall/1.1"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                xml_data = resp.read(65536)  # Hardening: Limite estrito de 64 KB contra DoS de memória

            root = ET.fromstring(xml_data)
            # Remove namespaces XML para facilitar busca
            for elem in root.iter():
                if "}" in elem.tag:
                    elem.tag = elem.tag.split("}", 1)[1]

            for service in root.iter("service"):
                st_elem = service.find("serviceType")
                cu_elem = service.find("controlURL")
                if st_elem is not None and cu_elem is not None and st_elem.text and cu_elem.text:
                    st_text = st_elem.text.strip()
                    cu_text = cu_elem.text.strip()
                    if st_text in UPNP_SERVICE_TYPES:
                        full_control_url = urllib.parse.urljoin(url, cu_text)
                        with self._lock:
                            self.control_url = full_control_url
                            self.service_type = st_text
                            self.router_url = url
                        return True
        except Exception:
            pass
        return False

    def _soap_request(self, action: str, arguments: dict[str, str], timeout: float = 3.0) -> ET.Element | None:
        """Envia uma chamada SOAP UPnP para o controlURL do roteador."""
        if not self.control_url or not self.service_type or not _is_safe_ssdp_url(self.control_url):
            return None

        body_args = "".join(f"<{k}>{v}</{k}>" for k, v in arguments.items())
        soap_body = (
            f'<?xml version="1.0"?>\r\n'
            f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            f's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\r\n'
            f"<s:Body>\r\n"
            f'<u:{action} xmlns:u="{self.service_type}">\r\n'
            f"{body_args}\r\n"
            f"</u:{action}>\r\n"
            f"</s:Body>\r\n"
            f"</s:Envelope>"
        ).encode("utf-8")

        headers = {
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPAction": f'"{self.service_type}#{action}"',
            "User-Agent": "LocalCall/1.1",
        }

        try:
            req = urllib.request.Request(self.control_url, data=soap_body, headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                xml_resp = resp.read(65536)  # Hardening: Limite estrito de 64 KB
                root = ET.fromstring(xml_resp)
                for elem in root.iter():
                    if "}" in elem.tag:
                        elem.tag = elem.tag.split("}", 1)[1]
                return root
        except Exception:
            return None

    def add_port_mapping(
        self,
        external_port: int,
        internal_port: int,
        protocol: str,
        internal_ip: str,
        description: str = "LocalCall",
    ) -> bool:
        """Adiciona um mapeamento de porta no roteador via UPnP SOAP ou COM nativo."""
        protocol = protocol.upper()

        # 1. Tenta via UPnP SOAP direto
        if self.control_url and self.service_type:
            args = {
                "NewRemoteHost": "",
                "NewExternalPort": str(external_port),
                "NewProtocol": protocol,
                "NewInternalPort": str(internal_port),
                "NewInternalClient": internal_ip,
                "NewEnabled": "1",
                "NewPortMappingDescription": description,
                "NewLeaseDuration": "0",
            }
            res = self._soap_request("AddPortMapping", args)
            if res is not None:
                with self._lock:
                    if (external_port, protocol) not in self._mapped_ports:
                        self._mapped_ports.append((external_port, protocol))
                return True

        # 2. Tenta via Windows COM NATUPnP caso SOAP direto não tenha respondido
        try:
            import comtypes.client
            nat = comtypes.client.CreateObject("HNetCfg.NATUPnP")
            maps = nat.StaticPortMappingCollection
            if maps is not None:
                maps.Add(external_port, protocol, internal_port, internal_ip, True, description)
                with self._lock:
                    if (external_port, protocol) not in self._mapped_ports:
                        self._mapped_ports.append((external_port, protocol))
                return True
        except Exception:
            pass

        return False

    def delete_port_mapping(self, external_port: int, protocol: str) -> bool:
        """Remove o mapeamento de porta do roteador."""
        protocol = protocol.upper()

        # 1. Tenta via UPnP SOAP direto
        if self.control_url and self.service_type:
            args = {
                "NewRemoteHost": "",
                "NewExternalPort": str(external_port),
                "NewProtocol": protocol,
            }
            res = self._soap_request("DeletePortMapping", args)
            if res is not None:
                return True

        # 2. Tenta via Windows COM NATUPnP
        try:
            import comtypes.client
            nat = comtypes.client.CreateObject("HNetCfg.NATUPnP")
            maps = nat.StaticPortMappingCollection
            if maps is not None:
                maps.Remove(external_port, protocol)
                return True
        except Exception:
            pass

        return False

    def get_external_ip_upnp(self) -> str | None:
        """Obtém o IP externo retornado pelo roteador via UPnP GetExternalIPAddress."""
        if self.control_url and self.service_type:
            res = self._soap_request("GetExternalIPAddress", {})
            if res is not None:
                ip_elem = res.find(".//NewExternalIPAddress")
                if ip_elem is not None and ip_elem.text:
                    ip = ip_elem.text.strip()
                    try:
                        parsed = ipaddress.IPv4Address(ip)
                        if not parsed.is_unspecified and not parsed.is_loopback:
                            return str(parsed)
                    except Exception:
                        pass
        return None

    def open_room_ports(
        self,
        internal_ip: str,
        tcp_port: int = 45000,
        udp_audio_port: int = 45001,
        udp_video_port: int = 45002,
    ) -> tuple[bool, str]:
        """Abre automaticamente todas as portas necessárias da sala no roteador do Host."""
        # Tenta descoberta UPnP IGD
        self.discover(timeout=1.5)

        ports = [
            (tcp_port, tcp_port, "TCP", "LocalCall Control"),
            (udp_audio_port, udp_audio_port, "UDP", "LocalCall Audio"),
            (udp_video_port, udp_video_port, "UDP", "LocalCall Video"),
        ]

        success_count = 0
        for ext_p, int_p, proto, desc in ports:
            if self.add_port_mapping(ext_p, int_p, proto, internal_ip, desc):
                success_count += 1

        router_wan_ip = self.get_external_ip_upnp()
        stun_ip = get_public_ip(timeout=1.5)
        ext_ip = stun_ip or (router_wan_ip if router_wan_ip and is_valid_public_ipv4(router_wan_ip) else None)

        # Detecção de CGNAT (Carrier-Grade NAT / RFC 6598 100.64.0.0/10 ou Duplo NAT)
        is_cgnat = False
        if router_wan_ip and is_cgnat_ipv4(router_wan_ip):
            is_cgnat = True
        elif router_wan_ip and stun_ip and router_wan_ip != stun_ip and not is_valid_public_ipv4(router_wan_ip):
            is_cgnat = True

        if is_cgnat:
            msg = f"Aviso CGNAT: Modem em NAT de operadora ({router_wan_ip or '100.64.x.x'}). Amigos fora da rede local precisam de ZeroTier/Radmin."
            return (success_count > 0), msg

        if success_count > 0:
            msg = f"UPnP Ativo: {success_count}/3 portas mapeadas no roteador"
            if ext_ip:
                msg += f" (IP Público: {ext_ip})"
            return True, msg

        if ext_ip:
            return False, f"UPnP desativado no roteador. IP Público STUN: {ext_ip}. Para amigos externos, ative UPnP no modem ou use ZeroTier/Radmin."

        return False, "UPnP e consulta externa indisponíveis no momento"

    def close_all_ports(self):
        """Remove todos os mapeamentos criados por esta sessão."""
        with self._lock:
            ports = list(self._mapped_ports)
            self._mapped_ports.clear()

        for port, proto in ports:
            try:
                self.delete_port_mapping(port, proto)
            except Exception:
                pass


_global_upnp: UPnPManager | None = None
_global_upnp_lock = threading.Lock()


def get_upnp_manager() -> UPnPManager:
    """Retorna o gerenciador singleton UPnP para a aplicação."""
    global _global_upnp
    with _global_upnp_lock:
        if _global_upnp is None:
            _global_upnp = UPnPManager()
        return _global_upnp

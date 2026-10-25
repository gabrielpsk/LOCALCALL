"""Regras de arquivos de imagem: tipo real, tamanho máximo e nome por hash."""
import hashlib
import re

MAX_BYTES = 10 * 1024 * 1024          # limite por imagem
NAME_RE = re.compile(r"^[0-9a-f]{16}\.(png|jpg|gif)$")


def sniff_ext(data: bytes):
    """Detecta o tipo pelos bytes iniciais (não confia na extensão do arquivo)."""
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "png"
    if data.startswith(b"\xff\xd8\xff"):
        return "jpg"
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return "gif"
    return None


def make_name(data: bytes, ext: str) -> str:
    return f"{hashlib.sha1(data).hexdigest()[:16]}.{ext}"


def valid_name(name) -> bool:
    """Só aceita nomes gerados por make_name (evita caminhos arbitrários)."""
    return isinstance(name, str) and bool(NAME_RE.match(name))

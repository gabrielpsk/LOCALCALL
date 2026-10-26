"""Camada de criptografia E2EE: Argon2id, HKDF-SHA256, AES-256-GCM e janela anti-replay."""
import hashlib
import hmac
import secrets
import struct

from argon2.low_level import Type, hash_secret_raw
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

SALT_LEN = 16
CHALLENGE_LEN = 32
NONCE_LEN = 12
TAG_LEN = 16
REPLAY_WINDOW = 64

# Lista enxuta de palavras em português para geração de passphrase forte (6 palavras)
_WORDS = (
    "acaso", "agudo", "alvor", "amplo", "andar", "apelo", "aroma", "asilo",
    "atlas", "atomo", "autor", "aviso", "balsa", "barco", "basto", "beijo",
    "berco", "bloco", "borda", "brasa", "bravo", "brejo", "brisa", "bucha",
    "cabo", "caixa", "calma", "campo", " canto", "carro", "carta", "casco",
    "cedro", "celta", "censo", "cerco", "chave", "ciclo", "cinza", "claro",
    "clima", "clube", "cobra", "coffe", "colar", "conde", "coral", "corda",
    "coroa", "corpo", "costa", "couro", "cravo", "creme", "crise", "cume",
    "curso", "curva", "dardo", "delta", "dente", "dique", "disco", "dogma",
    "drama", "duelo", "duna", "ebano", "elite", "época", "esqui", "etapa",
    "exame", "exito", "facho", "faixa", "farol", "fenda", "ferro", "feudo",
    "fibra", "ficha", "filme", "firma", "flora", "fluxo", "fogo", "folha",
    "fonte", "forca", "forma", "forno", "forte", "forum", "fosso", "frase",
    "freio", "frota", "fruto", "fuga", "fundo", "furia", "galho", "garoa",
    "garra", "geada", "gelo", "gema", "germe", "gesto", "globo", "golfe",
    "golpe", "gorro", "grade", "grama", "grito", "grupo", "gruta", "harpa",
    "haste", "heroi", "hino", "horto", "icone", "ideia", "idolo", "ilha",
    "imune", "indio", "infra", "invio", "irado", "isca", "istmo", "janta",
    "jarra", "jaula", "jegue", "jeito", "jogar", "joia", "joven", "juizo",
    "junco", "junta", "justo", "lagoa", "laico", "lanca", "lapso", "larva",
    "laser", "laudo", "lavra", "lebre", "leito", "lenda", "lente", "leque",
    "leste", "letra", "liame", "lider", "lilas", "limbo", "linha", "lirio",
    "lista", "livro", "lombo", "longe", "lotus", "louro", "lugar", "lunar",
    "luzir", "macro", "afia", "magia", "malha", "manto", " (mar)", "marco",
    "marte", "massa", "matiz", "mecha", "medo", "meigo", "melao", "mente",
    "merito", "metal", "metro", "miolo", "mirar", "mito", "moeda", "moita",
    "molde", "monte", "moral", "mural", "museu", "musgo", "natal", "navio",
    "nevoa", "nicho", "ninfa", "nivel", "nobre", "noite", "norma", "norte",
    "nuvem", "oasis", "obito", "ocaso", "oeste", "olhar", "ombro", "onda",
    "opaco", "opera", "ordem", "orgao", "orla", "ostro", "ouro", "outro",
    "pacto", "paiol", "palco", "palma", "pampa", "panda", "papel", "parco",
    "pardo", "parte", "passo", "pasto", "patio", "pausa", "pauta", "pedra",
    "peito", "pente", "perla", "perna", "perto", "piano", "pilar", "pinho",
    "pista", "placa", "plano", "plebe", "pleno", "pluma", "pobre", "poema",
    "poeta", "polar", "polen", "polpa", "polvo", "pomar", "ponte", "ponto",
    "porao", "porta", "porto", "posse", "poste", "pouso", "prado", "prata",
    "prato", "praia", "prazo", "prece", "preco", "prego", "presa", "prosa",
    "prova", "pulso", "punho", "quadro", "quase", "queda", "quilo", "quota",
    "radar", "radio", "raiar", "ramo", "rampa", "rastro", "razao", "reino",
    "relva", "remo", "renda", "resto", "feixe", "rigor", "ritmo", "rival",
    "rocha", "roda", "rolha", "rombo", "ronda", "rosto", "rota", "rubro",
    "ruido", "ruina", "rumo", "rural", "sabio", "sabor", "safra", "salao",
    "salto", "samba", "santo", "saude", "seara", "seiva", "selva", "idosa",
    "senha", "senso", "serra", "servo", "setor", "sexto", "sigla", "signo",
    "silvo", "sinal", "sinos", "sirio", "sitio", "sobra", "socio", "solar",
    "solo", "sombra", "sonho", " sopro", "sorte", "suave", "sulco", "super",
    "surdo", "tábua", "taça", "talco", "talho", "tampa", "tango", "tarde",
    "tarja", "taxis", "tecla", "teia", "telha", "ritual", "tempo", "tenda",
    "tenis", "tenso", "termo", "terra", "tese", "tigre", "tinta", "tipo",
    "tiro", "tocha", "toldo", "tombo", "tonel", "topaz", "toque", "torre",
    "torso", "traje", "trama", "trapo", "trato", "trave", "trevo", "tribo",
    "trigo", "trilho", "trono", "tropa", "trova", "truta", "tubo", "tufao",
    "tumba", "tunel", "turba", "turma", "turno", "tutor", "ulna", "ultra",
    "uniao", "unico", "urano", "urbe", "urso", "usina", "usual", "uteis",
    "vazio", "velho", "venda", "vento", "verao", "verbo", "verde", "verso",
    "vulto", "video", "vidro", "vigia", "vigor", "vinho", "viola", "virus",
    "visao", "visor", "vista", "vital", "viuvo", "vocal", "vogal", "volta",
    "voraz", "voto", "vulgo", "xale", "xampu", "xerox", "zebra", "zinco",
)


def generate_strong_password() -> str:
    """Gera senha forte com 6 palavras aleatórias + sufixo numérico (~28+ chars)."""
    clean_words = [w.strip(" ()") for w in _WORDS if len(w.strip(" ()")) >= 4]
    chosen = [secrets.choice(clean_words) for _ in range(6)]
    num = secrets.randbelow(900) + 100
    return "-".join(chosen) + f"-{num}"


def is_password_strong(password: str) -> tuple[bool, str]:
    """Verifica se a senha da sala possui o comprimento mínimo (mínimo 4 caracteres)."""
    pw = password.strip()
    if len(pw) < 4:
        return False, "Senha muito curta (digite ao menos 4 caracteres)."
    if len(pw) < 8:
        return True, "Senha simples aceita (E2EE: Argon2id + AES-256-GCM)"
    return True, "Senha forte (E2EE: Argon2id + AES-256-GCM)"


def _hkdf_expand(master_key: bytes, info: bytes) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=None,
        info=info,
    ).derive(master_key)


class ReplayWindow:
    """Janela deslizante de 64 pacotes por remetente para impedir replay."""

    def __init__(self):
        self._state: dict[int, list[int]] = {}  # sender_id -> [max_seq, bitmap_64]

    def accept(self, sender_id: int, seq: int) -> bool:
        if seq <= 0:
            return False
        st = self._state.get(sender_id)
        if st is None:
            self._state[sender_id] = [seq, 1]
            return True
        max_seq, mask = st
        if seq > max_seq:
            shift = seq - max_seq
            if shift >= REPLAY_WINDOW:
                mask = 1
            else:
                mask = ((mask << shift) | 1) & ((1 << REPLAY_WINDOW) - 1)
            st[0], st[1] = seq, mask
            return True
        diff = max_seq - seq
        if diff >= REPLAY_WINDOW:
            return False
        bit = 1 << diff
        if mask & bit:
            return False
        st[1] = mask | bit
        return True

    def reset_sender(self, sender_id: int):
        self._state.pop(sender_id, None)


class RoomKeys:
    """Deriva chave mestra com Argon2id (~0.5s) e 4 subchaves independentes via HKDF-SHA256."""

    def __init__(self, password: str, salt: bytes, time_cost: int = 3, memory_cost: int = 131072):
        if len(salt) != SALT_LEN:
            raise ValueError("Salt inválido")
        self.salt = salt
        master = hash_secret_raw(
            secret=password.encode("utf-8"),
            salt=salt,
            time_cost=time_cost,
            memory_cost=memory_cost,  # 128 MB
            parallelism=2,
            hash_len=32,
            type=Type.ID,
        )
        self.tcp_key = _hkdf_expand(master, b"call-app-v1-tcp")
        self.audio_key = _hkdf_expand(master, b"call-app-v1-audio")
        self.video_key = _hkdf_expand(master, b"call-app-v1-video")
        self.auth_key = _hkdf_expand(master, b"call-app-v1-handshake")
        self._aes_tcp = AESGCM(self.tcp_key)

    def compute_handshake_proof(self, challenge: bytes, client_name: str, udp_port: int, video_port: int) -> bytes:
        msg = challenge + b"|" + client_name.encode("utf-8") + f"|{udp_port}|{video_port}".encode("ascii")
        return hmac.new(self.auth_key, msg, hashlib.sha256).digest()

    def verify_handshake_proof(self, proof: bytes, challenge: bytes, client_name: str, udp_port: int, video_port: int) -> bool:
        expected = self.compute_handshake_proof(challenge, client_name, udp_port, video_port)
        return hmac.compare_digest(expected, proof)

    def encrypt_tcp(self, sender_id: int, seq: int, plaintext: bytes, frame_type: int = 1) -> tuple[bytes, bytes]:
        """Retorna (nonce_12B, ciphertext_with_tag). Header [frame_type:1B][nonce:12B] vai como AAD."""
        nonce = struct.pack("!IQ", sender_id & 0xFFFFFFFF, seq & 0xFFFFFFFFFFFFFFFF)
        aad = struct.pack("!B", frame_type) + nonce
        ct = self._aes_tcp.encrypt(nonce, plaintext, aad)
        return nonce, ct

    def decrypt_tcp(self, nonce: bytes, ciphertext: bytes, frame_type: int = 1) -> tuple[int, int, bytes] | None:
        """Valida tag AES-256-GCM e retorna (sender_id, seq, plaintext) ou None em falha."""
        if len(nonce) != NONCE_LEN or len(ciphertext) < TAG_LEN:
            return None
        sender_id, seq = struct.unpack("!IQ", nonce)
        aad = struct.pack("!B", frame_type) + nonce
        try:
            pt = self._aes_tcp.decrypt(nonce, ciphertext, aad)
            return sender_id, seq, pt
        except InvalidTag:
            return None

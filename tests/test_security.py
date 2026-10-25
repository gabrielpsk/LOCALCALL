"""Testes de segurança E2EE (Fase 4):
1. Cifrar e decifrar (Argon2id + HKDF + AES-256-GCM)
2. Pacote adulterado (ciphertext ou AAD/nonce) é rejeitado
3. Replay (pacote duplicado ou antigo fora da janela) é rejeitado
4. Senha errada não conecta no handshake desafio-resposta (HMAC-SHA256 + backoff)
"""
import os
import secrets
import sys
import unittest

os.environ["QT_QPA_PLATFORM"] = "offscreen"
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from PyQt6.QtWidgets import QApplication
from database.store import Store
from network.crypto import ReplayWindow, RoomKeys, SALT_LEN, generate_strong_password
from network.host import HostSession
from network.protocol import FRAME_DATA


class TestSecurityE2EE(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.salt = secrets.token_bytes(SALT_LEN)
        cls.password = generate_strong_password()
        cls.keys = RoomKeys(cls.password, cls.salt, time_cost=2, memory_cost=65536)

    def test_1_encrypt_and_decrypt(self):
        """(1) Cifrar e decifrar restaura exatamente o plaintext e metadados do nonce."""
        msg = b'{"t": "chat", "text": "mensagem secreta ponta a ponta"}'
        nonce, ct = self.keys.encrypt_tcp(sender_id=2, seq=10, plaintext=msg, frame_type=FRAME_DATA)
        res = self.keys.decrypt_tcp(nonce, ct, frame_type=FRAME_DATA)
        self.assertIsNotNone(res)
        sid, seq, pt = res
        self.assertEqual(sid, 2)
        self.assertEqual(seq, 10)
        self.assertEqual(pt, msg)

    def test_2_tampered_packet_rejected(self):
        """(2) Qualquer alteração no ciphertext, tag ou AAD (nonce/tipo) é rejeitada pela tag GCM."""
        msg = b'{"t": "chat", "text": "integro"}'
        nonce, ct = self.keys.encrypt_tcp(sender_id=1, seq=1, plaintext=msg, frame_type=FRAME_DATA)

        # Adultera 1 bit no ciphertext
        bad_ct = bytearray(ct)
        bad_ct[0] ^= 0x01
        self.assertIsNone(self.keys.decrypt_tcp(nonce, bytes(bad_ct), frame_type=FRAME_DATA))

        # Adultera 1 bit na tag GCM
        bad_tag = bytearray(ct)
        bad_tag[-1] ^= 0x01
        self.assertIsNone(self.keys.decrypt_tcp(nonce, bytes(bad_tag), frame_type=FRAME_DATA))

        # Adultera frame_type no AAD
        self.assertIsNone(self.keys.decrypt_tcp(nonce, ct, frame_type=0x03))

    def test_3_replay_rejected(self):
        """(3) Pacotes repetidos ou muito antigos são rejeitados pela janela deslizante."""
        rw = ReplayWindow()
        self.assertTrue(rw.accept(sender_id=3, seq=1))
        self.assertFalse(rw.accept(sender_id=3, seq=1))  # Replay imediato rejeitado

        # Avança janela até seq=100; pacotes fora de ordem recentes passam uma vez, antigos falham
        self.assertTrue(rw.accept(sender_id=3, seq=100))
        self.assertTrue(rw.accept(sender_id=3, seq=95))
        self.assertFalse(rw.accept(sender_id=3, seq=95))  # Replay de pacote fora de ordem
        self.assertFalse(rw.accept(sender_id=3, seq=20))  # Muito antigo (fora da janela de 64)

    def test_4_wrong_password_rejected(self):
        """(4) Senha errada gera prova HMAC inválida e é recusada pelo Host com backoff."""
        challenge = secrets.token_bytes(32)
        wrong_keys = RoomKeys("senha-totalmente-errada-123456", self.salt, time_cost=2, memory_cost=65536)

        bad_proof = wrong_keys.compute_handshake_proof(challenge, "Invasor", 45001, 45002)
        good_proof = self.keys.compute_handshake_proof(challenge, "Amigo", 45001, 45002)

        self.assertFalse(self.keys.verify_handshake_proof(bad_proof, challenge, "Invasor", 45001, 45002))
        self.assertTrue(self.keys.verify_handshake_proof(good_proof, challenge, "Amigo", 45001, 45002))

        # Verifica backoff exponencial e bloqueio global no HostSession
        store = Store("test_sec_host")
        host = HostSession(store, "Host", 45990, self.password)
        for _ in range(10):
            host._record_auth_failure("26.1.2.3")
        self.assertGreater(host._lockout_until, 0.0)

    def test_5_peer_audio_controls_and_host_moderation(self):
        """(5) Controles de áudio por peer (volume 0-150%, mute local, mute loopback) e moderação do Host."""
        from network.audio_bridge import AudioBridge
        from network.protocol import validate_message

        self.assertTrue(validate_message({"t": "kick", "target": "Amigo"}))
        self.assertTrue(validate_message({"t": "server_mute", "target": "Amigo"}))
        self.assertTrue(validate_message({"t": "kicked", "text": "Você foi expulso da sala pelo Host"}))
        self.assertFalse(validate_message({"t": "kick", "target": "x" * 100}))

        ab = AudioBridge()
        ab.set_peer_volume("Amigo", 135, sender_id=2)
        self.assertEqual(ab.get_peer_volume("Amigo"), 135)
        ab.set_peer_volume("Amigo", 200, sender_id=2)
        self.assertEqual(ab.get_peer_volume("Amigo"), 150)  # Clamped to 150%
        ab.set_peer_muted("Amigo", True, sender_id=2)
        self.assertTrue(ab.is_peer_muted("Amigo"))
        ab.set_peer_loopback_muted("Amigo", True, sender_id=2)
        self.assertTrue(ab.is_peer_loopback_muted("Amigo"))
        ab.cleanup()


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Notificações sonoras sutis sintetizadas em memória (WAV PCM) multiplataforma (Windows e Linux)."""
import io
import math
import shutil
import struct
import subprocess
import wave

try:
    import winsound
except ImportError:
    winsound = None  # type: ignore[assignment]


def _synth_chime(freqs: list[tuple[float, float]], volume: float = 0.14, sample_rate: int = 24000) -> bytes:
    """Gera um WAV PCM 16-bit mono suave com envelope de ataque/decay exponencial."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)

        frames = bytearray()
        for freq, duration_s in freqs:
            n_samples = int(sample_rate * duration_s)
            for i in range(n_samples):
                t = i / sample_rate
                # Envelope suave para evitar cliques
                attack = min(1.0, i / max(1, int(sample_rate * 0.008)))
                decay = math.exp(-4.5 * (t / max(0.01, duration_s)))
                sample = math.sin(2.0 * math.pi * freq * t) * volume * attack * decay
                val = max(-32767, min(32767, int(sample * 32767)))
                frames.extend(struct.pack("<h", val))
        wf.writeframes(bytes(frames))
    return buf.getvalue()


_CHAT_WAV = _synth_chime([(587.33, 0.075), (880.0, 0.11)], volume=0.12)
_POMODORO_WAV = _synth_chime([(523.25, 0.10), (659.25, 0.10), (783.99, 0.18)], volume=0.16)


def _play_wav_bytes(wav_data: bytes):
    if winsound is not None:
        try:
            winsound.PlaySound(wav_data, winsound.SND_MEMORY | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
        except Exception:
            pass
        return

    # Linux (ALSA aplay via stdin sem bloquear a UI)
    aplay = shutil.which("aplay")
    if aplay:
        try:
            proc = subprocess.Popen(
                [aplay, "-q", "-"],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if proc.stdin:
                proc.stdin.write(wav_data)
                proc.stdin.close()
        except Exception:
            pass


def play_chat_ping():
    """Toque curto e discreto para nova mensagem com janela em segundo plano."""
    _play_wav_bytes(_CHAT_WAV)


def play_pomodoro_chime():
    """Acorde suave indicando fim de ciclo do Pomodoro."""
    _play_wav_bytes(_POMODORO_WAV)

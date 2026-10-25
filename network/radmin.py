"""Integração de rede e detecção automática de IP (Radmin VPN no Windows / ZeroTier, VPN tun0 e LAN no Linux)."""
import os
import platform
import subprocess
from pathlib import Path

from .protocol import find_radmin_ip, list_network_ips

_IS_WINDOWS = platform.system() == "Windows"

if _IS_WINDOWS:
    import winreg
else:
    winreg = None  # type: ignore[assignment]

RADMIN_PROCESS_NAMES = ("Radmin_VPN.exe", "RvDetect.exe", "RvControlSvc.exe")

STANDARD_RADMIN_PATHS = (
    Path(r"C:\Program Files (x86)\Radmin VPN\Radmin_VPN.exe"),
    Path(r"C:\Program Files\Radmin VPN\Radmin_VPN.exe"),
)

REGISTRY_KEYS = (
    (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Famatech\RadminVPN"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Famatech\RadminVPN"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Famatech\RadminVPN"),
    )
    if _IS_WINDOWS and winreg is not None
    else ()
)


def is_radmin_running() -> bool:
    """Verifica se o cliente ou serviço do Radmin VPN está em execução no Windows."""
    if not _IS_WINDOWS:
        return False
    try:
        out = subprocess.check_output(
            ["tasklist", "/NH", "/FO", "CSV"],
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000),
            text=True,
            timeout=3,
            errors="ignore",
        )
        lower_out = out.lower()
        return any(proc.lower() in lower_out for proc in RADMIN_PROCESS_NAMES)
    except Exception:
        return False


def find_radmin_executable() -> Path | None:
    """Procura o executável Radmin_VPN.exe nos caminhos padrão ou no Registro do Windows."""
    if not _IS_WINDOWS or winreg is None:
        return None
    for candidate in STANDARD_RADMIN_PATHS:
        if candidate.is_file():
            return candidate

    for root, subkey in REGISTRY_KEYS:
        try:
            with winreg.OpenKey(root, subkey) as key:
                for val_name in ("InstallDir", "Path", "InstallPath", ""):
                    try:
                        val, _ = winreg.QueryValueEx(key, val_name)
                        if isinstance(val, str) and val.strip():
                            base = Path(val.strip().strip('"'))
                            exe = base if base.suffix.lower() == ".exe" else base / "Radmin_VPN.exe"
                            if exe.is_file():
                                return exe
                    except OSError:
                        continue
        except OSError:
            continue
    return None


def ensure_radmin_launched() -> tuple[bool, str]:
    """
    No Windows: verifica se o Radmin VPN está rodando e tenta iniciá-lo automaticamente.
    No Linux: detecta interfaces ZeroTier (zt...), VPN (tun0/wg0) ou LAN (eth0/wlan0).
    """
    if not _IS_WINDOWS:
        ips = list_network_ips()
        if ips:
            return True, f"Interface ativa: {ips[0][1]}"
        return True, "Pronto para conexão (ZeroTier / VPN / LAN)"

    ip = find_radmin_ip()
    if is_radmin_running():
        if ip:
            return True, f"Radmin VPN ativo ({ip})"
        return True, "Radmin VPN em execução (aguardando rede)"

    exe = find_radmin_executable()
    if exe:
        try:
            subprocess.Popen(
                [str(exe)],
                cwd=str(exe.parent),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "DETACHED_PROCESS", 0x00000008),
            )
            return True, "Radmin VPN iniciado automaticamente"
        except OSError:
            try:
                os.startfile(str(exe))  # type: ignore[attr-defined]
                return True, "Radmin VPN iniciado automaticamente"
            except OSError:
                pass

    if ip:
        return True, f"Interface detectada ({ip})"
    return (
        False,
        "Radmin VPN não detectado em execução. Você pode usar Radmin VPN, ZeroTier ou rede local (LAN).",
    )


def detect_radmin_ip() -> str | None:
    """Retorna o IP virtual preferencial (Radmin 26.x.x.x, ZeroTier zt..., tun0 ou LAN)."""
    return find_radmin_ip()

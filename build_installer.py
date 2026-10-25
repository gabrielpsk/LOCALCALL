"""Script para gerar o arquivo unico 'Instalador_LocalCall.exe'.

1. Compacta a pasta dist/LocalCall em payload.zip.
2. Empacota installer.py + payload.zip em um unico executavel standalone (onefile, windowed, icone).
3. Move Instalador_LocalCall.exe para a raiz do projeto para facil acesso.
"""
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST_APP = ROOT / "dist" / "LocalCall"
PAYLOAD_ZIP = ROOT / "payload.zip"
INSTALLER_PY = ROOT / "installer.py"


def make_payload():
    if not (DIST_APP / "LocalCall.exe").is_file():
        print(f"[ERRO] Executavel base {DIST_APP / 'LocalCall.exe'} nao encontrado.")
        sys.exit(1)

    print("[1/3] Criando payload.zip a partir de dist/LocalCall...")
    if PAYLOAD_ZIP.exists():
        PAYLOAD_ZIP.unlink()

    with zipfile.ZipFile(PAYLOAD_ZIP, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for file in DIST_APP.rglob("*"):
            if file.is_file():
                rel = file.relative_to(DIST_APP)
                zf.write(file, rel)
    print(f"      payload.zip criado com sucesso ({PAYLOAD_ZIP.stat().st_size // (1024*1024)} MB).")


def build_installer_exe():
    print("[2/3] Gerando Instalador_LocalCall.exe com PyInstaller (onefile, windowed, icone)...")
    cmd = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onefile",
        "--windowed",
        "--name",
        "Instalador_LocalCall",
        "--icon",
        "icon.ico",
        "--add-data",
        f"{PAYLOAD_ZIP};.",
        "--add-data",
        "ui/icon.png;ui",
        str(INSTALLER_PY),
    ]
    subprocess.run(cmd, check=True, cwd=str(ROOT))

    out_exe = ROOT / "dist" / "Instalador_LocalCall.exe"
    target_root_exe = ROOT / "Instalador_LocalCall.exe"
    if out_exe.is_file():
        shutil.copy2(str(out_exe), str(target_root_exe))
        print(f"[3/3] Instalador final copiado para: {target_root_exe}")

    # Remove o payload.zip temporario
    if PAYLOAD_ZIP.exists():
        PAYLOAD_ZIP.unlink()

    print("\n============================================================")
    print("  INSTALADOR CRIADO COM SUCESSO!")
    print(f"  Arquivo: {target_root_exe}")
    print("  - Interface clara limpa e profissional")
    print("  - Opcao de atalho na Area de Trabalho: LocalCall.lnk")
    print("  - Instalacao em %LocalAppData%\\Programs\\LocalCall sem pedir admin")
    print("  - O usuario pode apagar o Instalador_LocalCall.exe apos instalar!")
    print("============================================================")


if __name__ == "__main__":
    make_payload()
    build_installer_exe()

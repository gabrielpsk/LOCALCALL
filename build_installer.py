"""Script para gerar os executáveis e instaladores oficiais da versão 1.1.0:

1. Compacta a pasta dist/LocalCall em payload.zip.
2. Gera o pacote portátil LocalCall-Portable-v1.1.0.zip.
3. Empacota installer.py + payload.zip em LocalCall-Setup-v1.1.0.exe (e Instalador_LocalCall.exe).
4. Copia os binários finais para SITE APP CALL - v2/downloads.
5. Atualiza a instalação do sistema em %LocalAppData%\\Programs\\LocalCall para teste imediato.
"""
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

VERSION = "1.1.0"
ROOT = Path(__file__).resolve().parent
DIST_APP = ROOT / "dist" / "LocalCall"
PAYLOAD_ZIP = ROOT / "payload.zip"
INSTALLER_PY = ROOT / "installer.py"
SITE_DOWNLOADS = Path(r"C:\Users\gabri\OneDrive\Área de Trabalho\SITE APP CALL - v2\downloads")
SYSTEM_INSTALL_DIR = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "LocalCall"


def make_payload():
    if not (DIST_APP / "LocalCall.exe").is_file():
        print(f"[ERRO] Executavel base {DIST_APP / 'LocalCall.exe'} nao encontrado.")
        sys.exit(1)

    print("[1/5] Criando payload.zip a partir de dist/LocalCall...")
    if PAYLOAD_ZIP.exists():
        PAYLOAD_ZIP.unlink()

    with zipfile.ZipFile(PAYLOAD_ZIP, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for file in DIST_APP.rglob("*"):
            if file.is_file():
                rel = file.relative_to(DIST_APP)
                zf.write(file, rel)
    print(f"      payload.zip criado com sucesso ({PAYLOAD_ZIP.stat().st_size // (1024*1024)} MB).")


def make_portable_zip():
    print(f"[2/5] Gerando pacote portatil LocalCall-Portable-v{VERSION}.zip...")
    out_zip = ROOT / f"LocalCall-Portable-v{VERSION}.zip"
    if out_zip.exists():
        out_zip.unlink()

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for file in DIST_APP.rglob("*"):
            if file.is_file():
                rel = file.relative_to(DIST_APP)
                zf.write(file, rel)
    print(f"      LocalCall-Portable-v{VERSION}.zip criado ({out_zip.stat().st_size // (1024*1024)} MB).")
    return out_zip


def build_installer_exe():
    print(f"[3/5] Gerando LocalCall-Setup-v{VERSION}.exe com PyInstaller (onefile, windowed, icone)...")
    cmd = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onefile",
        "--windowed",
        "--name",
        f"LocalCall-Setup-v{VERSION}",
        "--icon",
        "icon.ico",
        "--add-data",
        f"{PAYLOAD_ZIP};.",
        "--add-data",
        "ui/icon.png;ui",
        str(INSTALLER_PY),
    ]
    subprocess.run(cmd, check=True, cwd=str(ROOT))

    out_exe = ROOT / "dist" / f"LocalCall-Setup-v{VERSION}.exe"
    target_setup = ROOT / f"LocalCall-Setup-v{VERSION}.exe"
    target_compat = ROOT / "Instalador_LocalCall.exe"

    if out_exe.is_file():
        shutil.copy2(str(out_exe), str(target_setup))
        shutil.copy2(str(out_exe), str(target_compat))
        print(f"      Instalador oficial copiado para: {target_setup}")

    # Remove o payload.zip temporario
    if PAYLOAD_ZIP.exists():
        PAYLOAD_ZIP.unlink()

    return target_setup


def update_site_downloads(setup_exe: Path, portable_zip: Path):
    print(f"[4/5] Copiando arquivos para o site: {SITE_DOWNLOADS}...")
    if SITE_DOWNLOADS.is_dir():
        if setup_exe.is_file():
            shutil.copy2(str(setup_exe), str(SITE_DOWNLOADS / setup_exe.name))
            print(f"      Copiado: {SITE_DOWNLOADS / setup_exe.name}")
        if portable_zip.is_file():
            shutil.copy2(str(portable_zip), str(SITE_DOWNLOADS / portable_zip.name))
            print(f"      Copiado: {SITE_DOWNLOADS / portable_zip.name}")
    else:
        print(f"      [AVISO] Pasta {SITE_DOWNLOADS} nao encontrada.")


def update_system_installation():
    print(f"[5/5] Atualizando binarios instalados no PC: {SYSTEM_INSTALL_DIR}...")
    if not SYSTEM_INSTALL_DIR.is_dir():
        print(f"      Pasta {SYSTEM_INSTALL_DIR} nao existe; pulando atualizacao local.")
        return

    try:
        # Copia todos os arquivos atualizados de dist/LocalCall para a pasta instalada
        for item in DIST_APP.iterdir():
            dest = SYSTEM_INSTALL_DIR / item.name
            if item.is_dir():
                if dest.exists():
                    shutil.rmtree(dest, ignore_errors=True)
                shutil.copytree(item, dest)
            else:
                shutil.copy2(item, dest)
        print("      Binarios locais do sistema atualizados com sucesso para v1.1.0!")
    except Exception as e:
        print(f"      [AVISO] Nao foi possivel atualizar pasta local automaticamente ({e}).")


if __name__ == "__main__":
    make_payload()
    portable_zip = make_portable_zip()
    setup_exe = build_installer_exe()
    update_site_downloads(setup_exe, portable_zip)
    update_system_installation()

    print("\n============================================================")
    print(f"  BUILD OFICIAL LOCALCALL v{VERSION} CONCLUIDA COM SUCESSO!")
    print(f"  Instalador:  LocalCall-Setup-v{VERSION}.exe")
    print(f"  Portatil:    LocalCall-Portable-v{VERSION}.zip")
    print(f"  Site v2:     {SITE_DOWNLOADS}")
    print(f"  Sistema:     {SYSTEM_INSTALL_DIR}")
    print("============================================================")

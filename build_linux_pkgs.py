import os
import sys
import tarfile
import io
import time

SOURCE_DIR = r"c:\Users\gabri\OneDrive\Área de Trabalho\App de call"
OUTPUT_DIR = r"C:\Users\gabri\OneDrive\Área de Trabalho\SITE APP CALL\downloads"

os.makedirs(OUTPUT_DIR, exist_ok=True)

# 1. Gerar localcall-v1.0.0-linux-x86_64.tar.gz
tar_path = os.path.join(OUTPUT_DIR, "localcall-v1.0.0-linux-x86_64.tar.gz")
print(f"Gerando {tar_path}...")

files_to_include = [
    ("main.py", "main.py"),
    ("requirements.txt", "requirements.txt"),
    ("README.md", "README.md"),
    ("build_linux.sh", "build_linux.sh"),
    ("audio_engine/audio_engine_linux.c", "audio_engine/audio_engine_linux.c"),
    ("audio_engine/audio_engine.c", "audio_engine/audio_engine.c"),
    ("video_engine/video_engine_linux.c", "video_engine/video_engine_linux.c"),
    ("video_engine/video_engine.c", "video_engine/video_engine.c"),
    ("network/protocol.py", "network/protocol.py"),
    ("network/crypto.py", "network/crypto.py"),
    ("network/audio_bridge.py", "network/audio_bridge.py"),
    ("network/video_bridge.py", "network/video_bridge.py"),
    ("database/store.py", "database/store.py"),
    ("database/media.py", "database/media.py"),
    ("ui/main_window.py", "ui/main_window.py"),
    ("ui/style.qss", "ui/style.qss"),
    ("ui/icon.png", "ui/icon.png"),
]

# Copia também o install.sh para o tarball
install_sh_src = os.path.join(OUTPUT_DIR, "install.sh")

with tarfile.open(tar_path, "w:gz") as tar:
    for rel_src, rel_dst in files_to_include:
        full_src = os.path.join(SOURCE_DIR, rel_src)
        if os.path.exists(full_src):
            info = tar.gettarinfo(full_src, arcname=f"localcall-v1.0.0/{rel_dst}")
            # Permissões unix
            if rel_dst.endswith(".sh") or rel_dst == "main.py":
                info.mode = 0o755
            else:
                info.mode = 0o644
            info.uname = "root"
            info.gname = "root"
            with open(full_src, "rb") as f:
                tar.addfile(info, f)
        else:
            print(f"Aviso: arquivo não encontrado {full_src}")

    if os.path.exists(install_sh_src):
        info = tar.gettarinfo(install_sh_src, arcname="localcall-v1.0.0/install.sh")
        info.mode = 0o755
        info.uname = "root"
        info.gname = "root"
        with open(install_sh_src, "rb") as f:
            tar.addfile(info, f)

print(f"Tarball gerado: {os.path.getsize(tar_path)} bytes")

# 2. Gerar localcall_1.0.0_amd64.deb
# O formato .deb é um arquivo ar no padrão GNU:
# !<arch>\n
# debian-binary
# control.tar.gz
# data.tar.gz
deb_path = os.path.join(OUTPUT_DIR, "localcall_1.0.0_amd64.deb")
print(f"Gerando pacote Debian {deb_path}...")

# 2.1 debian-binary
debian_binary_bytes = b"2.0\n"

# 2.2 control.tar.gz
control_content = """Package: localcall
Version: 1.0.0
Section: net
Priority: optional
Architecture: amd64
Depends: python3, python3-pyqt6, python3-cryptography, python3-argon2, libpulse0, libasound2, libx11-6, libxext6, libssl3 | libssl1.1
Maintainer: LocalCall Team <contact@localcall.org>
Description: Chamadas de voz, telas a 60 FPS e foco em rede local (E2EE)
 Aplicativo desktop seguro, de baixissima latencia e sem intermediarios centralizados.
 Porta padrao oficial: 45000 TCP (sinalizacao), 45001 UDP (audio), 45002 UDP (video).
"""

postinst_content = """#!/bin/sh
set -e
if [ -x /usr/bin/update-desktop-database ]; then
    update-desktop-database /usr/share/applications || true
fi
exit 0
"""

control_tar_io = io.BytesIO()
with tarfile.open(fileobj=control_tar_io, mode="w:gz") as ctar:
    # control file
    cdata = control_content.encode("utf-8")
    tinfo = tarfile.TarInfo(name="./control")
    tinfo.size = len(cdata)
    tinfo.mtime = int(time.time())
    tinfo.mode = 0o644
    tinfo.uname = "root"
    tinfo.gname = "root"
    ctar.addfile(tinfo, io.BytesIO(cdata))

    # postinst
    pdata = postinst_content.encode("utf-8")
    pinfo = tarfile.TarInfo(name="./postinst")
    pinfo.size = len(pdata)
    pinfo.mtime = int(time.time())
    pinfo.mode = 0o755
    pinfo.uname = "root"
    pinfo.gname = "root"
    ctar.addfile(pinfo, io.BytesIO(pdata))

control_tar_bytes = control_tar_io.getvalue()

# 2.3 data.tar.gz
data_tar_io = io.BytesIO()
with tarfile.open(fileobj=data_tar_io, mode="w:gz") as dtar:
    # Cria diretórios
    dirs = [
        "./opt",
        "./opt/localcall",
        "./opt/localcall/audio_engine",
        "./opt/localcall/video_engine",
        "./opt/localcall/network",
        "./opt/localcall/database",
        "./opt/localcall/ui",
        "./usr",
        "./usr/bin",
        "./usr/share",
        "./usr/share/applications",
        "./usr/share/icons",
        "./usr/share/icons/hicolor",
        "./usr/share/icons/hicolor/512x512",
        "./usr/share/icons/hicolor/512x512/apps",
    ]
    for d in dirs:
        dinfo = tarfile.TarInfo(name=d)
        dinfo.type = tarfile.DIRTYPE
        dinfo.mode = 0o755
        dinfo.mtime = int(time.time())
        dinfo.uname = "root"
        dinfo.gname = "root"
        dtar.addfile(dinfo)

    # Copia arquivos do app para /opt/localcall/
    for rel_src, rel_dst in files_to_include:
        full_src = os.path.join(SOURCE_DIR, rel_src)
        if os.path.exists(full_src):
            info = dtar.gettarinfo(full_src, arcname=f"./opt/localcall/{rel_dst}")
            if rel_dst.endswith(".sh") or rel_dst == "main.py":
                info.mode = 0o755
            else:
                info.mode = 0o644
            info.uname = "root"
            info.gname = "root"
            with open(full_src, "rb") as f:
                dtar.addfile(info, f)

    # Copia o ícone também para o tema padrão de ícones do sistema
    icon_src = os.path.join(SOURCE_DIR, "ui/icon.png")
    if os.path.exists(icon_src):
        iinfo = dtar.gettarinfo(icon_src, arcname="./usr/share/icons/hicolor/512x512/apps/localcall.png")
        iinfo.mode = 0o644
        iinfo.uname = "root"
        iinfo.gname = "root"
        with open(icon_src, "rb") as f:
            dtar.addfile(iinfo, f)

    # /usr/bin/localcall
    launcher_sh = """#!/bin/sh
cd /opt/localcall
exec python3 /opt/localcall/main.py "$@"
"""
    ldata = launcher_sh.encode("utf-8")
    linfo = tarfile.TarInfo(name="./usr/bin/localcall")
    linfo.size = len(ldata)
    linfo.mode = 0o755
    linfo.mtime = int(time.time())
    linfo.uname = "root"
    linfo.gname = "root"
    dtar.addfile(linfo, io.BytesIO(ldata))

    # /usr/share/applications/localcall.desktop
    desktop_entry = """[Desktop Entry]
Name=LocalCall
Comment=Chamadas de Voz, Telas a 60 FPS e Foco E2EE em Rede Local
Exec=/usr/bin/localcall
Icon=/opt/localcall/ui/icon.png
Terminal=false
Type=Application
Categories=Network;InstantMessaging;AudioVideo;
Keywords=call;voice;audio;screenshare;e2ee;
"""
    dkdata = desktop_entry.encode("utf-8")
    dkinfo = tarfile.TarInfo(name="./usr/share/applications/localcall.desktop")
    dkinfo.size = len(dkdata)
    dkinfo.mode = 0o644
    dkinfo.mtime = int(time.time())
    dkinfo.uname = "root"
    dkinfo.gname = "root"
    dtar.addfile(dkinfo, io.BytesIO(dkdata))

data_tar_bytes = data_tar_io.getvalue()

# 2.4 Empacotar formato AR (.deb)
# Header AR: !<arch>\n
# Cada membro tem um cabeçalho de 60 bytes:
# Nome (16 bytes), Timestamp (12 bytes), Owner (6 bytes), Group (6 bytes), Mode (8 bytes), Size (10 bytes), End mark (`\n) (2 bytes)
def make_ar_header(name, size):
    name_field = name.ljust(16)
    time_field = str(int(time.time())).ljust(12)
    owner_field = "0".ljust(6)
    group_field = "0".ljust(6)
    mode_field = "100644".ljust(8)
    size_field = str(size).ljust(10)
    end_field = "`\n"
    header = (name_field + time_field + owner_field + group_field + mode_field + size_field + end_field).encode("ascii")
    if len(header) != 60:
        raise ValueError(f"Cabeçalho inválido: {len(header)} bytes")
    return header

with open(deb_path, "wb") as deb_f:
    deb_f.write(b"!<arch>\n")

    # 1. debian-binary
    deb_f.write(make_ar_header("debian-binary", len(debian_binary_bytes)))
    deb_f.write(debian_binary_bytes)
    if len(debian_binary_bytes) % 2 != 0:
        deb_f.write(b"\n")

    # 2. control.tar.gz
    deb_f.write(make_ar_header("control.tar.gz", len(control_tar_bytes)))
    deb_f.write(control_tar_bytes)
    if len(control_tar_bytes) % 2 != 0:
        deb_f.write(b"\n")

    # 3. data.tar.gz
    deb_f.write(make_ar_header("data.tar.gz", len(data_tar_bytes)))
    deb_f.write(data_tar_bytes)
    if len(data_tar_bytes) % 2 != 0:
        deb_f.write(b"\n")

print(f"Pacote Debian gerado com sucesso: {deb_path} ({os.path.getsize(deb_path)} bytes)")

#!/usr/bin/env bash
# ==============================================================================
# Script de Instalação, Compilação e Empacotamento para Linux (Debian/Ubuntu/Kali)
# - Arquitetura alvo: x86-64 genérico (-march=x86-64 -mtune=generic) para rodar
#   leve inclusive em hardware antigo (ex: ThinkPad Intel Core 2 Duo).
# - Uso:
#   chmod +x build_linux.sh
#   ./build_linux.sh            # Instala dependências e compila .so
#   ./build_linux.sh --deb      # Gera pacote instalável .deb (rodar com 'appdecall')
#   ./build_linux.sh --binary   # Gera binário portátil único em dist/AppDeCall
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "[1/4] Instalando pacotes de sistema necessários (Kali / Debian / Ubuntu)..."
sudo apt-get update
sudo apt-get install -y \
    libpulse-dev \
    libasound2-dev \
    libx11-dev \
    libxext-dev \
    libssl-dev \
    gcc \
    make \
    python3 \
    python3-pip \
    python3-venv \
    python3-pyqt6 \
    python3-cryptography \
    python3-argon2 \
    alsa-utils

echo "[2/4] Verificando bibliotecas Python (PyQt6, cryptography, argon2-cffi)..."
python3 -m pip install -r requirements.txt --break-system-packages 2>/dev/null || \
    python3 -m pip install -r requirements.txt || true

echo "[3/4] Compilando motor de áudio C (audio_engine/audio_engine.so)..."
gcc -O2 -march=x86-64 -mtune=generic \
    -Wall -Wextra -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -shared -o audio_engine/audio_engine.so audio_engine/audio_engine_linux.c \
    -lpulse-simple -lpulse -lasound -lcrypto -lpthread

echo "[4/4] Compilando motor de vídeo X11/XShm C (video_engine/video_engine.so)..."
gcc -O2 -march=x86-64 -mtune=generic \
    -Wall -Wextra -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -shared -o video_engine/video_engine.so video_engine/video_engine_linux.c \
    -lX11 -lXext -lcrypto -lpthread

echo ""
echo "=============================================================================="
echo " Compilação concluída com sucesso!"
echo " - audio_engine/audio_engine.so"
echo " - video_engine/video_engine.so"
echo " Para iniciar o aplicativo agora: python3 main.py"
echo "=============================================================================="

# Opção 1: Gerar pacote .deb nativo para Debian/Ubuntu/Kali Linux
if [[ "$1" == "--deb" ]]; then
    echo ""
    echo "[*] Construindo pacote Debian (dist/appdecall_1.0.0_amd64.deb)..."
    PKG_ROOT="$SCRIPT_DIR/build/deb_pkg"
    rm -rf "$PKG_ROOT"
    mkdir -p "$PKG_ROOT/DEBIAN"
    mkdir -p "$PKG_ROOT/opt/appdecall/audio_engine"
    mkdir -p "$PKG_ROOT/opt/appdecall/video_engine"
    mkdir -p "$PKG_ROOT/opt/appdecall/database"
    mkdir -p "$PKG_ROOT/opt/appdecall/network"
    mkdir -p "$PKG_ROOT/opt/appdecall/ui"
    mkdir -p "$PKG_ROOT/usr/bin"
    mkdir -p "$PKG_ROOT/usr/share/applications"
    mkdir -p "$SCRIPT_DIR/dist"

    cp main.py requirements.txt "$PKG_ROOT/opt/appdecall/"
    cp audio_engine/audio_engine.so "$PKG_ROOT/opt/appdecall/audio_engine/"
    cp video_engine/video_engine.so "$PKG_ROOT/opt/appdecall/video_engine/"
    cp database/*.py "$PKG_ROOT/opt/appdecall/database/"
    cp network/*.py "$PKG_ROOT/opt/appdecall/network/"
    cp ui/*.py ui/style.qss ui/icon.png "$PKG_ROOT/opt/appdecall/ui/"

    cat > "$PKG_ROOT/usr/bin/appdecall" << 'EOF'
#!/usr/bin/env bash
exec python3 /opt/appdecall/main.py "$@"
EOF
    chmod 755 "$PKG_ROOT/usr/bin/appdecall"

    cat > "$PKG_ROOT/usr/share/applications/appdecall.desktop" << 'EOF'
[Desktop Entry]
Name=AppDeCall
Comment=Chamadas de Voz, Compartilhamento de Tela e Produtividade E2EE
Exec=/usr/bin/appdecall
Icon=/opt/appdecall/ui/icon.png
Terminal=false
Type=Application
Categories=Network;InstantMessaging;AudioVideo;
EOF

    cat > "$PKG_ROOT/DEBIAN/control" << 'EOF'
Package: appdecall
Version: 1.0.0
Section: net
Priority: optional
Architecture: amd64
Depends: python3, python3-pyqt6, python3-cryptography, python3-argon2, libpulse0, libasound2, libx11-6, libxext6, libssl3 | libssl1.1
Maintainer: AppDeCall Team <dev@appdecall.local>
Description: Aplicativo desktop P2P de chamadas de voz, tela e produtividade E2EE
 Interoperavel entre Windows e Linux com criptografia ponta a ponta (AES-256-GCM + Argon2id).
EOF

    dpkg-deb --build "$PKG_ROOT" "$SCRIPT_DIR/dist/appdecall_1.0.0_amd64.deb"
    echo "[+] Pacote criado em: dist/appdecall_1.0.0_amd64.deb"
    echo "    Instale com: sudo dpkg -i dist/appdecall_1.0.0_amd64.deb"
    echo "    Execute com 1 comando: appdecall"
fi

# Opção 2: Gerar binário portátil standalone via PyInstaller
if [[ "$1" == "--binary" ]]; then
    echo ""
    echo "[*] Gerando binário portátil standalone com PyInstaller..."
    python3 -m pip install pyinstaller --break-system-packages 2>/dev/null || \
        python3 -m pip install pyinstaller
    python3 -m PyInstaller --noconfirm --clean --onefile --windowed \
        --name "AppDeCall" \
        --add-data "ui/style.qss:ui" \
        --add-data "ui/icon.png:ui" \
        --add-binary "audio_engine/audio_engine.so:audio_engine" \
        --add-binary "video_engine/video_engine.so:video_engine" \
        main.py
    echo "[+] Binário portátil gerado em: dist/AppDeCall"
    echo "    Execute com: ./dist/AppDeCall"
fi

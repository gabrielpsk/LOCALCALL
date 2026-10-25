@echo off
chcp 65001 >nul
echo ============================================================
echo   Empacotador Portatil - App de Call (PyQt6 + C Engines)
echo ============================================================
echo.

REM 1. Compila as DLLs em C caso o GCC esteja disponivel no PATH
where gcc >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo [1/3] Compilando motores nativos em C ^(audio_engine.dll e video_engine.dll^)...
    pushd audio_engine
    call build.bat
    popd
    pushd video_engine
    call build.bat
    popd
) else (
    echo [1/3] GCC nao encontrado no PATH; usando DLLs pre-compiladas existentes.
)

if not exist "audio_engine\audio_engine.dll" (
    echo [ERRO] audio_engine\audio_engine.dll nao encontrada!
    exit /b 1
)
if not exist "video_engine\video_engine.dll" (
    echo [ERRO] video_engine\video_engine.dll nao encontrada!
    exit /b 1
)

REM 2. Verifica / instala PyInstaller
echo [2/3] Verificando PyInstaller...
python -m PyInstaller --version >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Instalando PyInstaller...
    python -m pip install pyinstaller
)

REM 3. Gera a pasta portatil dist\LocalCall com --noconsole (--windowed) e icone
echo [3/3] Gerando executavel portatil em dist\LocalCall...
python -m PyInstaller --noconfirm --clean --onedir --windowed ^
    --name "LocalCall" ^
    --icon "icon.ico" ^
    --add-data "ui/style.qss;ui" ^
    --add-data "ui/icon.png;ui" ^
    --add-binary "audio_engine/audio_engine.dll;audio_engine" ^
    --add-binary "video_engine/video_engine.dll;video_engine" ^
    --hidden-import "argon2" ^
    --hidden-import "cryptography" ^
    main.py

if %ERRORLEVEL% NEQ 0 (
    echo [ERRO] Falha durante o empacotamento com PyInstaller.
    exit /b 1
)

REM Garante copia direta na raiz da pasta portatil alem de _internal
if not exist "dist\LocalCall\audio_engine" mkdir "dist\LocalCall\audio_engine"
if not exist "dist\LocalCall\video_engine" mkdir "dist\LocalCall\video_engine"
if not exist "dist\LocalCall\ui" mkdir "dist\LocalCall\ui"
if not exist "dist\LocalCall\data" mkdir "dist\LocalCall\data"
copy /Y "audio_engine\audio_engine.dll" "dist\LocalCall\audio_engine\audio_engine.dll" >nul
copy /Y "video_engine\video_engine.dll" "dist\LocalCall\video_engine\video_engine.dll" >nul
copy /Y "ui\style.qss" "dist\LocalCall\ui\style.qss" >nul
copy /Y "ui\icon.png" "dist\LocalCall\ui\icon.png" >nul
copy /Y "icon.ico" "dist\LocalCall\icon.ico" >nul

REM 4. Gera o Instalador executavel unico standalone
echo.
echo [4/4] Gerando instalador unico (Instalador_LocalCall.exe)...
python build_installer.py

echo.
echo ============================================================
echo   CONCLUIDO COM SUCESSO!
echo   Pasta portatil:     dist\LocalCall\
echo   Instalador Unico:   Instalador_LocalCall.exe
echo   Basta enviar o arquivo "Instalador_LocalCall.exe" para seus
echo   amigos. Eles executam, criam atalho e podem apagar o instalador!
============================================================

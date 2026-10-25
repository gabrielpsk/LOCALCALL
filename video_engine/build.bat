@echo off
cd /d "%~dp0"
where gcc >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Compilando video_engine.dll com GCC + Hardening -march=x86-64...
    gcc -O2 -march=x86-64 -mtune=generic -Wall -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,--dynamicbase,--nxcompat -shared -o video_engine.dll video_engine.c -lmfplat -lmfuuid -ld3d11 -ldxgi -lws2_32 -lgdi32 -lole32 -luuid -lbcrypt -lssp
    exit /b %ERRORLEVEL%
)
where cl >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Compilando video_engine.dll com MSVC + Media Foundation H.264 + Hardening...
    cl /O2 /GS /DYNAMICBASE /NXCOMPAT /LD video_engine.c mfplat.lib mfuuid.lib d3d11.lib dxgi.lib ws2_32.lib gdi32.lib ole32.lib uuid.lib bcrypt.lib /Fe:video_engine.dll
    exit /b %ERRORLEVEL%
)
echo Erro: Nenhum compilador C (gcc ou cl) encontrado no PATH.
exit /b 1

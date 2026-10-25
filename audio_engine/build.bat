@echo off
cd /d "%~dp0"
where gcc >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Compilando audio_engine.dll com GCC + Hardening -march=x86-64...
    gcc -O2 -march=x86-64 -mtune=generic -Wall -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,--dynamicbase,--nxcompat -shared -o audio_engine.dll audio_engine.c -lws2_32 -lwinmm -lole32 -luuid -lbcrypt -lssp
    exit /b %ERRORLEVEL%
)
where cl >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Compilando audio_engine.dll com MSVC + Hardening...
    cl /O2 /GS /DYNAMICBASE /NXCOMPAT /LD audio_engine.c ws2_32.lib winmm.lib ole32.lib uuid.lib bcrypt.lib /Fe:audio_engine.dll
    exit /b %ERRORLEVEL%
)
echo Erro: Nenhum compilador C (gcc ou cl) encontrado no PATH.
exit /b 1

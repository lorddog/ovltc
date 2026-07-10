@echo off
setlocal

set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist %VCVARS% (
    echo [ERROR] vcvarsall.bat not found: %VCVARS%
    exit /b 1
)
call %VCVARS% x64
if errorlevel 1 exit /b 1

set SRC=src
set ZLIB=src\zlib
set OUT=build

if not exist %OUT% mkdir %OUT%

cl.exe /nologo /O2 /W3 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE ^
    /I %SRC% /I %ZLIB% ^
    /LD ^
    %SRC%\ovl_wcx.c %SRC%\ovl_format.c %SRC%\ovl_oodle.c ^
    %ZLIB%\inflate.c %ZLIB%\inftrees.c %ZLIB%\inffast.c %ZLIB%\zutil.c %ZLIB%\crc32.c %ZLIB%\adler32.c ^
    /Fo%OUT%\ /Fe%OUT%\ovl_wcx.wcx64 ^
    /link /DEF:%SRC%\ovl_wcx.def /MACHINE:X64 kernel32.lib

if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

copy /y pluginst.inf %OUT%\pluginst.inf >nul

echo.
echo [OK] Plugin built: %OUT%\ovl_wcx.wcx64
endlocal

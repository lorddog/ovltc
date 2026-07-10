@echo off
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
call %VCVARS% x64
if errorlevel 1 exit /b 1

if not exist build mkdir build

cl.exe /nologo /O2 /W3 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
    /I src ^
    src\test_harness.c ^
    /Fobuild\ /Febuild\test_harness.exe

if errorlevel 1 (
    echo [ERROR] test_harness build failed.
    exit /b 1
)
echo [OK] build\test_harness.exe
endlocal

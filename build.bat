@echo off
REM Builds Ultralay (quicksearch.exe + qshelper.exe + AffHook.dll).
REM Requires: VS 2022 Build Tools (x64 native toolchain).
REM Self-contained: builds + runs from this folder only.
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at the VS BuildTools path
    exit /b 1
)
call "%VCVARS%" >nul

set "CEF=sdk\cef"
set "WRAPPER=thirdparty\libcef_dll_wrapper.lib"
if not exist "%WRAPPER%" (
    echo [ERROR] thirdparty\libcef_dll_wrapper.lib missing
    exit /b 1
)

REM outputs land in portable\ (the app folder)
if not exist portable mkdir portable
if not exist portable\obj mkdir portable\obj

set "CFLAGS=/nologo /std:c++20 /O2 /MT /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /GR- /D_HAS_EXCEPTIONS=0 /wd4244 /wd4267 /wd4838 /I %CEF%"
set "LIBS=%WRAPPER% %CEF%\Release\libcef.lib gdiplus.lib shlwapi.lib psapi.lib user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib ws2_32.lib rpcrt4.lib crypt32.lib wintrust.lib delayimp.lib"

cl %CFLAGS% src\subproc.cpp /Fe:portable\qshelper.exe /Fo:portable\obj\ /link %LIBS% /DELAYLOAD:libcef.dll /SUBSYSTEM:WINDOWS /PDBALTPATH:quicksearch.pdb
if errorlevel 1 (
    echo [ERROR] subproc build failed
    exit /b 1
)

cl %CFLAGS% src\main.cpp /Fe:portable\quicksearch.exe /Fo:portable\obj\ /link %LIBS% /DELAYLOAD:libcef.dll /SUBSYSTEM:WINDOWS /STACK:0x800000 /PDBALTPATH:quicksearch.pdb
if errorlevel 1 (
    echo [ERROR] main build failed
    exit /b 1
)

REM AffHook.dll — native loader (APC entry + native in-target sweep).
REM Plain kernel32/user32/ole32 links only — no .NET SDK headers required.
cl /nologo /O2 /MT /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /EHsc /LD src\affhook.cpp /Fe:portable\AffHook.dll /Fo:portable\obj\affhook.obj /link user32.lib kernel32.lib ole32.lib /PDBALTPATH:quicksearch.pdb
if errorlevel 1 (
    echo [ERROR] affhook build failed
    exit /b 1
)

REM AffManaged.dll — managed WDA_NONE sweeper run inside the target's CLR.
where csc >nul 2>nul
if not errorlevel 1 (
    csc /nologo /target:library /optimize /out:portable\AffManaged.dll src\affmanaged.cs
    if errorlevel 1 echo [WARN] AffManaged.dll build failed - loader falls back to self-sweep
) else (
    echo [WARN] csc not found - AffManaged.dll not built
)

REM CEF runtime files must sit next to the exes.
copy /y %CEF%\Release\libcef.dll portable\ >nul
copy /y %CEF%\Release\chrome_elf.dll portable\ >nul
copy /y %CEF%\Release\v8_context_snapshot.bin portable\ >nul
copy /y %CEF%\Resources\icudtl.dat portable\ >nul
copy /y %CEF%\Resources\chrome_100_percent.pak portable\ >nul
copy /y %CEF%\Resources\chrome_200_percent.pak portable\ >nul
copy /y %CEF%\Resources\resources.pak portable\ >nul
xcopy /y /s /i %CEF%\Resources\locales portable\locales >nul

echo.
echo Built: %cd%\portable\quicksearch.exe  (+ qshelper.exe, AffHook.dll, CEF runtime)
endlocal

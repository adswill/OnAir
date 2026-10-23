#!/usr/bin/env python3
"""Builds OnAir-Laptop-Setup.cmd and OnAir-Laptop-Uninstall.cmd (CRLF) from src/*.ps1.

The .cmd is a batch header plus the PowerShell script as plain text after a marker line. The batch part cuts the
script out of itself, writes it to %TEMP% and runs it. Plain text (not base64) so the coordinator can replace the
__RUNNER_TOKEN__ placeholder with sed right before handing the file over:
    sed "s/__RUNNER_TOKEN__/THE_TOKEN/" OnAir-Laptop-Setup.cmd > out.cmd   (keep CRLF; sed leaves it alone)
"""
import pathlib, sys

here = pathlib.Path(__file__).resolve().parent

HEADER = r'''@echo off
rem {title}
rem Double-click this file. It asks Windows for permission once, then does everything by itself.
setlocal DisableDelayedExpansion
title {title}
set "ONAIR_SELF=%~f0"
set "ONAIR_DIR=%~dp0"
set "ONAIR_TOKEN=%~1"
echo.
echo   {title}
echo   Starting... a Windows permission question will appear: please click Yes.
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:ONAIR_SELF);$m='#'+'#PS-START#'+'#';$i=$t.LastIndexOf($m);if($i -lt 0){Write-Host 'The file is damaged (script part missing). Download it again.';exit 9};$s=$t.Substring($i+$m.Length);$p=Join-Path $env:TEMP '{ps}';[IO.File]::WriteAllText($p,$s,(New-Object Text.UTF8Encoding $false));& $p;exit 0"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo   Something went wrong ^(code %RC%^). Press any key to close.
  pause >nul
)
exit /b %RC%
##PS-START##
'''

def build(name, title, psname, src):
    body = (here / 'src' / src).read_text(encoding='ascii').replace('\r\n', '\n')
    head = HEADER.replace('{title}', title).replace('{ps}', psname)
    text = (head + body).replace('\r\n', '\n').replace('\n', '\r\n')
    (here / name).write_bytes(text.encode('ascii'))
    print('wrote', name, len(text), 'bytes')

build('OnAir-Laptop-Setup.cmd', 'OnAir laptop worker setup', 'OnAir-Laptop-Setup.ps1', 'setup.ps1')
build('OnAir-Laptop-Uninstall.cmd', 'OnAir laptop worker uninstall', 'OnAir-Laptop-Uninstall.ps1', 'uninstall.ps1')

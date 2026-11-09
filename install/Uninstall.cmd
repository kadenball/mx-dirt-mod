@echo off
powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install\Uninstall.ps1" %*


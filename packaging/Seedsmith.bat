@echo off
title Seedsmith
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
python\python.exe serve.py
if errorlevel 1 (
  echo.
  echo Seedsmith could not start. The message above says why.
  pause
)

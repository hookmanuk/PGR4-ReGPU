@echo off
rem Starts the PGR4 port from the repository root (game\ holds the extracted disc,
rem out\tu2 the title update). F12 in the window captures a frame. Logs go to out\play.log.
cd /d "%~dp0"
if not exist out mkdir out
set PGR4_TITLE_UPDATE_DIR=out\tu2
build\rel\pgr4.exe game > out\play.log 2>&1

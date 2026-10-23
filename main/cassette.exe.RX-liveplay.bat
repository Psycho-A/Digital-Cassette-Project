@echo off
pushd "%~dp0"

if "%~1"=="" set test_file=test_music.pcm.wav
if not "%~1"=="" set "test_file=%~1"
del /f /q "%test_file%.bin*" 2> nul

call cassette.exe rx - - -input_backend=winmm -output_backend=winmm

pause

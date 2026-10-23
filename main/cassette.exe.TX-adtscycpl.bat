@echo off

if "%~1"=="" set test_file=test_music.pcm
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.wav" 2> nul

call cassette.exe tx "%test_file%" "%test_file%.wav" -input_backend=wav -output_backend=wav
call cassette.exe rx "%test_file%.wav" - -input_backend=wav -output_backend=winmm

pause

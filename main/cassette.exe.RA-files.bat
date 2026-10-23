@echo off

if "%~1"=="" set test_file=test_music.pcm
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%_*.wav" 2> nul

call cassette.exe rx_analog "%test_file%" "%test_file%_rxa.wav" -input_backend=wav -output_backend=wav

pause

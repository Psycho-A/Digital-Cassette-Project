@echo off

if "%~1"=="" set test_file=test_music.pcm
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%_*.wav" 2> nul

cassette.exe tx_analog "%test_file%" - -input_backend=wav -output_backend=wav | ^
cassette.exe rx_analog - "%test_file%-loop.wav" -input_backend=wav -output_backend=wav

pause

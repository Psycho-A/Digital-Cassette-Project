// -------------------------------
// Digital Cassette Project - main
// -------------------------------
#pragma once
#include <stdio.h>
#include <string.h>
#include "config.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "fdkaac_dll.h"  // AAC-энкодер
#include "faad2_dll.h"   // AAC-декодер
#endif

/* настройки и функции */
AppConfig cfg;  // глобальный конфиг
int tx_run(const char* in_name, const char* out_name);
int rx_run(const char* in_name, const char* out_name);
int analog_run(const char *in_name, const char *out_name, int fsm_mode);

/* выполнение программы */
int main(int argc, char** argv)
{
    int res = 1; char log_fn[256];
    char *exe_fn = strstr(argv[0],"\\") ? strrchr(argv[0],'\\') +1 : argv[0];

    /* твики для Win32 */
    #ifdef _WIN32
        // заголовок программы в консоли...
        SetConsoleTitle("Digital Cassette Project");
        // включить бинарный режим для pipe
        _setmode(_fileno(stdin),  _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    #endif

    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s tx input.file output.wav [options]\n", exe_fn);
        fprintf(stderr, "  %s rx input.wav output.file [options]\n", exe_fn);
        fprintf(stderr, "  %s tx_analog input.wav output.wav [options]\n", exe_fn);
        fprintf(stderr, "  %s rx_analog input.wav output.wav [options]\n", exe_fn);
        fprintf(stderr, "\n");
        fprintf(stderr, "Options (override config params):\n");
        fprintf(stderr, "  -input_backend=<name>  : input audio source (wav, winmm)\n");
        fprintf(stderr, "  -output_backend=<name> : output audio target (wav, winmm)\n");
        fprintf(stderr, "  -winmm_source=<name>   : input WinMM device (index or name)\n");
        fprintf(stderr, "  -winmm_target=<name>   : output WinMM device (index or name)\n");
        fprintf(stderr, "  -frame_capacity=<size> : frame byte capacity (-1 = auto)\n");
        fprintf(stderr, "  -record_level=<value>  : record level for TX mode (dB value)\n");
        fprintf(stderr, "  -stream_class=<number> : class for StdIn input (10 = AAC)\n");
        fprintf(stderr, "  -enable_dsp=<0|1>      : forces DSP processing (on or off)\n");
        fprintf(stderr, "\n"); system("pause"); return 1;
    }

    /* читаем настройки */
    read_config(&cfg, "config.ini", argc, argv);

    /* включение или выключение логирования */
    sprintf(log_fn, "%s.log", argv[0]);  // "program.exe.log"
    log_fp = g_logging ? fopen(log_fn, "wb") : fopen("NUL", "wb");

    /* загружаем DLL для кодера и декодера */
    if (load_fdkaac_dll() != 0) fprintf(stderr, "Warning: libFdkAac.dll not loaded, AAC encoding disabled.\n\n");
    if (load_faad2_dll() != 0)  fprintf(stderr, "Warning: libFaad2.dll not loaded, AAC decoding disabled.\n\n");

    /* запускаем выбранный режим */
    if (strcmp(argv[1], "tx") == 0)             res = tx_run(argv[2], argv[3]);
    else if (strcmp(argv[1], "rx") == 0)        res = rx_run(argv[2], argv[3]);
    else if (strcmp(argv[1], "tx_analog") == 0) res = analog_run(argv[2], argv[3], 0);
    else if (strcmp(argv[1], "rx_analog") == 0) res = analog_run(argv[2], argv[3], 1);
    else fprintf(stderr, "Unknown mode!\n");

    /* выгружаем DLL кодера и декодера */
    if (free_fdkaac_dll() != 0) fprintf(stderr, "Warning: libFdkAac.dll not unloaded, probably memory issue!\n");
    if (free_faad2_dll() != 0)  fprintf(stderr, "Warning: libFaad2.dll not unloaded, probably memory issue!\n");
    if (g_logging && log_fp) fclose(log_fp);  // закрыть лог-файл

    return res;
}

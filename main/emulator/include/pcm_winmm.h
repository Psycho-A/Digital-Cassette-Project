/* pcm_winmm.h */
/* WinMM: classic Win32 WaveOut backend
 *
 * Compatible with Windows 98 / Windows 10 OS
 * Use textual C-file including for easy OS checks.
 * Won't be included in pcm_io.c on Unix systems!
 */
#ifndef PCM_WINMM_H
#define PCM_WINMM_H

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include "pcm_io.h"

#pragma comment(lib, "winmm.lib")

/* Контекст WinMM (общий для входа и выхода) */
typedef struct WinmmCtx WinmmCtx;

struct WinmmCtx {
    int is_input;                  // 1: waveIn, 0: waveOut
    int sample_rate;
    int channels;
    PcmFormat format;              // выходной формат для sink, входной для source (но мы будем получать от системы)
    int bits;                      // битность на сэмпл (реальная)
    uint32_t frame_size;           // байт на фрейм (1 сэмпл)
    int32_t  num_buffers;          // количество буферов (2-4)
    int32_t  buffer_frames;        // размер буфера в фреймах (2048 или 1024 сэмплов)
    uint32_t buffer_bytes;         // размер выделенных буферов в байтах (max_frames * frame_size)
    uint8_t *raw_buffer;           // временный буфер для чтения сырых байт (выделяется при открытии)
    int32_t  timeout_ms;           // таймаут ожидания чтения (0 = INFINITE)
    int32_t  extra_buffers;        // число чанков для сглаживания неровностей потока (размер ring_buffer + задержка)

    /* дескрипторы и буферы */
    union {
        HWAVEIN hwi;
        HWAVEOUT hwo;
    } handle;

    WAVEHDR *headers;              // массив заголовков
    uint8_t **buffers;             // указатели на данные каждого буфера
    int *buffer_status;            // 0 = свободен, 1 = в драйвере
    HANDLE hEvent;                 // событие освобождения буфера
    CRITICAL_SECTION cs;           // защита buffer_status и кольцевого буфера

    /* кольцевой буфер для обмена данными с основным потоком */
    uint8_t *ring_buffer;
    uint32_t ring_size;            // в байтах
    volatile uint32_t ring_read;   // индекс чтения
    volatile uint32_t ring_write;  // индекс записи

    /* функции для выхода */
    PcmPackFn pack_sample;         // функция упаковки одного сэмпла

    /* флаг завершения */
    volatile int running;
};

/* Функции для создания источника (вход) */
int winmm_source_open(PcmSource *src, const AppConfig *cfg, const char *path);
uint32_t winmm_source_read_chunk(void *vctx, uint32_t max_frames);
int winmm_source_close(void *vctx);

/* Функции для создания приёмника (выход) */
int winmm_sink_open(PcmSink *sink, const AppConfig *cfg, const char *path);
void winmm_sink_write_sample(void *vctx, int32_t left, int32_t right);
void winmm_wait_for_data(WinmmCtx *ctx); // проиграть остатки в буфере
int winmm_sink_close(void *vctx);

/* Вспомогательная функция для выбора устройства по имени/индексу */
int open_winmm_device(int is_input, const AppConfig *cfg,
                      WAVEFORMATEX *wfx, HWAVEIN *phwi, HWAVEOUT *phwo,
                      DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD dwFlags);

#endif
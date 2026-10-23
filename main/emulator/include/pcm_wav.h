/* pcm_wav.h */
#ifndef PCM_WAV_H
#define PCM_WAV_H

/* PCM WAV/StdIO backend (C89-friendly)
 *
 * We support writing PCM with configurable bit-depth (8/16/24/32/float).
 * The implementation selects an efficient per-sample writer once at open time,
 * so hot-path sample writes do not use runtime branching on format.
 *
 * Types:
 *  - WavCtx: context used for writing (contains FILE* internally)
 *  - WavUnpackFn: unpack source samples into int32 format (stereo)
 *  - WavDownmixFn: downmix all chunk samples into mono (from cfg)
 *  - WavPackFn: pack output samples into target format (cfg/device)
 */
#include "pcm_io.h"

/* Контекст WAV/потока */
typedef struct WavCtx WavCtx;

struct WavCtx {
    FILE *fp;
    int sample_rate;
    int channels;
    PcmFormat format;       // условные идентификаторы формата
    int bits;               // реальное число бит, расчёт из format
    int is_raw;             // 1 = сырой PCM (без заголовка), 0 = обычный WAV

    /* поля для чтения */
    uint32_t data_size;     // полный размер данных в файле
    uint32_t data_offset;   // смещение до данных
    uint32_t data_pos;      // текущая позиция в данных
    uint32_t data_seconds;  // длительность потока (рассчитываемая)

    /* поля для цикла чтения (используются в PcmSource) */
    uint32_t frame_size;    // размер одного фрейма в байтах (channels * bytes_per_sample)
    uint32_t max_frames;    // максимальное количество фреймов в блоке (2048 или 1024 сэмплов)
    uint8_t *chunk_buf;     // временный буфер для чтения сырых байт (выделяется при открытии)
    uint32_t chunk_size;    // размер выделенного chunk_buf в байтах (max_frames * frame_size)

    /* поля для записи сэмплов */
    PcmPackFn pack_sample;  // упаковка 1 сэмпла (формат выбирается при открытии)
    uint32_t bytes_written; // количество записанных байт в "data"-блоке
};


/* ---------- Базовые функции для чтения ---------- */
/* Инициализировать PcmSource для чтения из WAV-файла или stdin.
   - открывает файл или поток для чтения;
   - заполняет поля "src" и внутренний контекст;
   - для обычного файла читается заголовок, для stdin - параметры def_*. */
int wav_source_open(PcmSource *src, const AppConfig *cfg, const char *path);

/* Прочитать максимум max_frames фреймов сырых данных в буфер.
   - Возвращает количество реально прочитанных фреймов. */
uint32_t wav_source_read_chunk(void *vctx, uint32_t max_frames);

/* Закрыть входной файл/поток. */
void wav_source_close(void *vctx);


/* ---------- Базовые функции для записи ---------- */
/* Инициализировать PcmSink для записи в WAV-файл или stdout.
   - открывает файл или поток для записи;
   - заполняет поля "sink" и внутренний контекст;
   - для файла создаётся пустой RIFF-заголовок, для stdout - сырой вывод. */
int wav_sink_open(PcmSink *sink, const AppConfig *cfg, const char *path);

/* Записать один стерео-сэмпл (значения уже приведены к целевому диапазону). */
void wav_sink_write_sample(void *vctx, int32_t left, int32_t right);

/* Закрыть выходной файл/поток, при необходимости дописать заголовок. */
int wav_sink_close(void *vctx);


#endif
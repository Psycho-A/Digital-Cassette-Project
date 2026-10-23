/* pcm_io.h */
#ifndef PCM_H
#define PCM_H

/* Universal PCM source/sink interface
 *
 * Types:
 *  - PcmFormat: bit depth (8/16/24/32)
 *  - PcmSource: signal source (file/stdin/device)
 *  - PcmSink:   signal output (file/stdout/device)
 */
#include "stdint_compat.h"
#include "config.h"

typedef enum {
    PCM_S8  = 8,
    PCM_S16 = 16,
    PCM_S24 = 24,
    PCM_S32 = 32,
    PCM_F32 = 64  // условность, не биты
} PcmFormat;

typedef struct PcmSource {
    void *ctx;
    /* Читает сырые PCM-байты из источника, возвращает количество прочитанных байт */
    uint32_t (*read_chunk)(void *ctx, uint32_t max_frames);
    void (*close)(void *ctx);

    /* Реальный формат источника (заполняется бэкендом при открытии) */
    int sample_rate, channels, format;  // например, 44100 / 2 / 8,16,24,32
    uint32_t duration_sec;  // длительность в секундах (для метаданных)

    /* Общий временный буфер для чтения сырых байт (chunk) */
    uint8_t *chunk_buf; uint32_t chunk_frames;  // указатель, число сэмплов

} PcmSource;

typedef struct PcmSink {
    void *ctx;
    /* Записывает один стереосэмпл (int32) */
    void (*write_sample)(void *ctx, int32_t left, int32_t right);
    void (*close)(void *ctx);

    /* Реальный формат приёмника (заполняется бэкендом при открытии) */
    int sample_rate, channels, format;
    int src_rate; PcmFormat src_format;  // формат источника (для конверсии сэмплов)
    uint32_t chunk_frames;  // кол-во сэмплов для выделения буферов (общее)

} PcmSink;


/* Открыть источник PCM на основе cfg->input.pcm_backend */
int open_source(PcmSource *src, const AppConfig *cfg, const char *path);

/* Открыть приёмник PCM на основе cfg->output.pcm_backend */
int open_sink(PcmSink *sink, const AppConfig *cfg, const char *path);


/* ---------- Функции для выбора unpack/pack мини-функций (из pcm_helpers) ---------- */
typedef void (*PcmUnpackFn)(const uint8_t *src, int32_t *l, int32_t *r, uint32_t frames);
typedef void (*PcmDownmixFn)(int32_t *l, int32_t *r, uint32_t frames);
typedef uint32_t (*PcmPackFn)(int32_t l, int32_t r, uint8_t *dst);

/* Выбрать функцию распаковки по контексту (формату и каналам) */
PcmUnpackFn pcm_select_unpack_fn(int channels, PcmFormat fmt);

/* Выбрать функцию микширования в моно (опционально) */
PcmDownmixFn pcm_select_downmix_fn(int channels, int mode);

/* Выбрать функцию упаковки сэмплов (по входному и выходному формату) */
PcmPackFn pcm_select_pack_fn(PcmFormat in_fmt, PcmFormat out_fmt);


/* Конвертер амплитуд из дБ в PCM-сэмплы (для пред-расчётов) */
int32_t amp_from_db(double db, PcmFormat pcm_bits);

#endif
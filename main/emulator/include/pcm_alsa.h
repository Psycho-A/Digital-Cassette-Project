/* pcm_alsa.h */
/* ALSA: Linux Low-level backend
 *
 * TODO! Unimplemented yet.
 */
#ifndef PCM_ALSA_H
#define PCM_ALSA_H

#include "pcm_io.h"

/* Контекст ALSA (общий для входа и выхода) */
typedef struct AlsaCtx AlsaCtx;

struct AlsaCtx {
    // пока пусто...
};

/* Функции для создания источника (вход) */
int alsa_source_open(PcmSource *src, const AppConfig *cfg, const char *path);
static uint32_t alsa_source_read_chunk(void *vctx, void *buffer, uint32_t max_frames);
static int alsa_source_close(void *vctx);

/* Функции для создания приёмника (выход) */
int alsa_sink_open(PcmSink *sink, const AppConfig *cfg, const char *path);
static void alsa_sink_write_sample(void *vctx, int32_t left, int32_t right);
static int alsa_sink_close(void *vctx);


#endif
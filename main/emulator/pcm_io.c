/* pcm_io.c */
#include <string.h>
#include "pcm_io.h"
#include "pcm_helpers.h"

/* PCM backends:
   ------------- */
#include "pcm_wav.h"

#ifdef   _WIN32
#include "pcm_winmm.c"
#else
// (alsa, portaudio...)
#endif


/* Открыть источник PCM-сигнала (на чтение) */
int open_source(PcmSource *src, const AppConfig *cfg, const char *path)
{
    /* входные настройки */
    src->sample_rate = cfg->input.def_rate;
    src->channels = cfg->input.def_channels;
    src->format = cfg->input.def_format;  // условный формат
    src->duration_sec = 0;  // для метаданных (рассчитываемая)

    /* определим кол-во сэмплов в буфере чтения (на основе кодека) */
    src->chunk_frames = cfg->stream.chunk_samples;  // -1 = выбор по AAC-профилю

    if (!strcasecmp(cfg->input.pcm_backend, "wav")) return wav_source_open(src, cfg, path);
    else if (!strcasecmp(cfg->input.pcm_backend, "winmm")) return winmm_source_open(src, cfg, path);
    else {
        /* TODO: другие бэкенды в будущем, сейчас вывести ошибку */
        fprintf(stderr, "Unknown PCM input backend: %s.\n", cfg->input.pcm_backend);
    }
    return -1;
}

/* Открыть приёмник PCM-сигнала (на запись) */
int open_sink(PcmSink *sink, const AppConfig *cfg, const char *path)
{
    /* выходные настройки */
    sink->sample_rate = cfg->output.sample_rate;
    sink->channels = cfg->output.pcm_channels;
    sink->format = cfg->output.pcm_format;  // условный формат
    sink->chunk_frames = cfg->stream.chunk_samples;  // сэмплов в буфере чтения

    /* исправить частоту: ре-семплирования нет */
    if (sink->src_rate >= 8000 && !cfg->output.force_rate) {
        sink->sample_rate = sink->src_rate;  // форсим исходную
    }
    /* исправить формат источника, если не задан */
    if (!is_valid_format(sink->src_format, 0)) {
        sink->src_format = (PcmFormat)cfg->output.pcm_format;  // приравниваем к выходному
    }

    /* открыть функцию выбранного выходного бэкенда */
    if (!strcasecmp(cfg->output.pcm_backend, "wav")) return wav_sink_open(sink, cfg, path);
    else if (!strcasecmp(cfg->output.pcm_backend, "winmm")) return winmm_sink_open(sink, cfg, path);
    else {
        /* TODO: другие бэкенды в будущем, сейчас вывести ошибку */
        fprintf(stderr, "Unknown PCM output backend: %s.\n", cfg->output.pcm_backend);
    }
    return -1;
}


/* --- Функции выбора unpack/downmix/pack (из pcm_helpers.h) --- */

/* select analog RX/TX unpacker once */
PcmUnpackFn pcm_select_unpack_fn(int channels, PcmFormat fmt)
{
    PcmUnpackFn fn = NULL;
    switch (fmt) {
        case PCM_S8:  fn = (channels == 1) ? pcm_unpack_s8_mono_to_s32  : pcm_unpack_s8_dual_to_s32;  break;
        case PCM_S16: fn = (channels == 1) ? pcm_unpack_s16_mono_to_s32 : pcm_unpack_s16_dual_to_s32; break;
        case PCM_S24: fn = (channels == 1) ? pcm_unpack_s24_mono_to_s32 : pcm_unpack_s24_dual_to_s32; break;
        case PCM_S32: fn = (channels == 1) ? pcm_unpack_s32_mono_to_s32 : pcm_unpack_s32_dual_to_s32; break;
        case PCM_F32: fn = (channels == 1) ? pcm_unpack_f32_mono_to_s32 : pcm_unpack_f32_dual_to_s32; break;
        default: fprintf(stderr, "Unsupported PCM input bits: %d.\n", fmt);
    }
    return fn;
}

/* select analog RX/TX mono-downmix mode */
PcmDownmixFn pcm_select_downmix_fn(int channels, int mode)
{
    PcmDownmixFn fn = pcm_downmix_none;
    switch (mode) {
        case 1:  fn = (channels > 1) ? pcm_downmix_half : fn; break;
        case 2:  fn = (channels > 1) ? pcm_downmix_peak : fn; break;
        default: break;
    }
    return fn;
}

/* таблица выбора формата упаковки */
typedef struct {
    PcmFormat in_fmt, out_fmt;
    PcmPackFn fn;
} PackEntry;

static const PackEntry pack_table[] = {
    {PCM_S8,  PCM_S8,  pcm_pack_s8_to_s8  },
    {PCM_S8,  PCM_S16, pcm_pack_s8_to_s16 },
    {PCM_S8,  PCM_S24, pcm_pack_s8_to_s24 },
    {PCM_S8,  PCM_S32, pcm_pack_s8_to_s32 },
    {PCM_S8,  PCM_F32, pcm_pack_s8_to_f32 },

    {PCM_S16, PCM_S8,  pcm_pack_s16_to_s8 },
    {PCM_S16, PCM_S16, pcm_pack_s16_to_s16},
    {PCM_S16, PCM_S24, pcm_pack_s16_to_s24},
    {PCM_S16, PCM_S32, pcm_pack_s16_to_s32},
    {PCM_S16, PCM_F32, pcm_pack_s16_to_f32},

    {PCM_S24, PCM_S8,  pcm_pack_s24_to_s8 },
    {PCM_S24, PCM_S16, pcm_pack_s24_to_s16},
    {PCM_S24, PCM_S24, pcm_pack_s24_to_s24},
    {PCM_S24, PCM_S32, pcm_pack_s24_to_s32},
    {PCM_S24, PCM_F32, pcm_pack_s24_to_f32},

    {PCM_S32, PCM_S8,  pcm_pack_s32_to_s8 },
    {PCM_S32, PCM_S16, pcm_pack_s32_to_s16},
    {PCM_S32, PCM_S24, pcm_pack_s32_to_s24},
    {PCM_S32, PCM_S32, pcm_pack_s32_to_s32},
    {PCM_S32, PCM_F32, pcm_pack_s32_to_f32},

    {PCM_F32, PCM_S8,  pcm_pack_f32_to_s8 },
    {PCM_F32, PCM_S16, pcm_pack_f32_to_s16},
    {PCM_F32, PCM_S24, pcm_pack_f32_to_s24},
    {PCM_F32, PCM_S32, pcm_pack_f32_to_s32},
    {PCM_F32, PCM_F32, pcm_pack_f32_to_f32},
};

PcmPackFn pcm_select_pack_fn(PcmFormat in_fmt, PcmFormat out_fmt)
{
    size_t i;
    for (i = 0; i < sizeof(pack_table)/sizeof(pack_table[0]); ++i) {
        if (pack_table[i].in_fmt == in_fmt && pack_table[i].out_fmt == out_fmt) {
            return pack_table[i].fn;
        }
    }
    return NULL;
}


/* Конвертер амплитуд из дБ в PCM-сэмплы */
int32_t amp_from_db(double db, PcmFormat pcm_bits)
{
    double lin;
    float lin_f;
    int32_t int_f;

    /* clamp input value */
    if (db > 0.0) db = 0.0;
    if (db < -90.0) db = -90.0;

    lin = pow(10.0, db / 20.0);

    switch (pcm_bits) {
    case 8:  return (int32_t)(lin * 127.0);
    case 16: return (int32_t)(lin * 32767.0);
    case 24: return (int32_t)(lin * 8388607.0);
    case 32: return (int32_t)(lin * 2147483647.0);
    case 64: // 32-bit float (биты как int32_t)
        lin_f = (float)lin;
        memcpy(&int_f, &lin_f, 4);
        return int_f;
    default:
        /* unreachable by design */
        return 0;
    }
}

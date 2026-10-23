/* pcm_wav.c */
#include <stdlib.h>
#include <string.h>
#include "pcm_wav.h"


/* Helper: запись в little-endian 32-bit */
static void write_u32_le(FILE* fp, uint32_t v)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
    fwrite(buf, 1, 4, fp);
}

/* Helper: запись в little-endian 16-bit */
static void write_u16_le(FILE* fp, uint16_t v)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    fwrite(buf, 1, 2, fp);
}


/* ---------- Источник (WAV-вход) ---------- */
int wav_source_open(PcmSource *src, const AppConfig *cfg, const char *path)
{
    WavCtx *ctx = calloc(1, sizeof(WavCtx));

    uint8_t  riff[12];
    int      chunk_count = 0;
    char     chunk_id[5] = {0};
    uint32_t chunk_size;
    int      fmt_found = 0;
    int      data_found = 0;
    int      max_chunks = 32;  // защита от бесконечного цикла

    if (!ctx || !path) return -1;
    memset(ctx, 0, sizeof(*ctx));

    /* умолчания */
    ctx->is_raw = 0;
    ctx->sample_rate = src->sample_rate;
    ctx->channels = src->channels;
    ctx->format = src->format;
    ctx->bits = src->format > 32 ? 32 : src->format;
    ctx->max_frames = src->chunk_frames;
    ctx->data_size = ctx->data_pos = 0;
    ctx->data_seconds = 0;

    /* выбираем источник */
    if (!strcmp(path, "-")) {
        /* поток stdin */
        ctx->is_raw = 1;
        ctx->fp = stdin;
        ctx->data_size = 0x7FFFFFFF;  // max 2 Gb

    } else {
        /* обычный файл */
        ctx->fp = fopen(path, "rb");
        if (!ctx->fp) goto fail;

        /* читаем RIFF-заголовок */
        if (fread(riff, 1, 12, ctx->fp) != 12) goto fail;
        if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) goto fail;

        /* сканируем все RIFF-чанки */
        while (chunk_count++ < max_chunks) {
            if (fread(chunk_id, 1, 4, ctx->fp) != 4) goto fail;
            if (fread(&chunk_size, 4, 1, ctx->fp) != 1) goto fail;

            /* найден "fmt"-чанк */
            if (memcmp(chunk_id, "fmt ", 4) == 0) {
                uint8_t  fmt_buf[16];
                uint16_t audio_fmt;

                if (chunk_size < 16) goto fail;
                if (fread(fmt_buf, 1, 16, ctx->fp) != 16) goto fail;
                if (chunk_size > 16) {
                    /* пропустить оставшуюся часть чанка, если есть */
                    if (fseek(ctx->fp, chunk_size - 16, SEEK_CUR) != 0) goto fail;
                }

                /* читаем поля формата */
                audio_fmt        = fmt_buf[0]  | (fmt_buf[1]  << 8);
                ctx->channels    = fmt_buf[2]  | (fmt_buf[3]  << 8);
                ctx->sample_rate = fmt_buf[4]  | (fmt_buf[5]  << 8) | (fmt_buf[6] << 16) | (fmt_buf[7] << 24);
                ctx->bits        = fmt_buf[14] | (fmt_buf[15] << 8);

                if (audio_fmt == 1) { // PCM
                    switch (ctx->bits) {
                    case 8:  ctx->format = PCM_S8;  break;
                    case 16: ctx->format = PCM_S16; break;
                    case 24: ctx->format = PCM_S24; break;
                    case 32: ctx->format = PCM_S32; break;
                    default: goto fail;
                    }
                } else if (audio_fmt == 3) { // IEEE float
                    ctx->format = PCM_F32;
                } else {
                    fprintf(stderr, "Unsupported WAV format ID: %u.\n", audio_fmt);
                    goto fail;
                }
                fmt_found = 1;
            }

            /* найден "data"-чанк */
            else if (memcmp(chunk_id, "data", 4) == 0) {
                ctx->data_size = chunk_size;
                ctx->data_offset = (uint32_t)ftell(ctx->fp);
                data_found = 1; break;
            }
            else {
                /* пропускаем неизвестный чанк */
                if (fseek(ctx->fp, chunk_size, SEEK_CUR) != 0) goto fail;
            }
        }
        if (!fmt_found || !data_found) goto fail;

        /* поставить позицию чтения */
        fseek(ctx->fp, ctx->data_offset, SEEK_SET);

        /* рассчитать длительность потока */
        ctx->data_seconds = ctx->data_size / 
            (ctx->sample_rate * ctx->channels * (ctx->bits / 8));
    }

    /* считаем размер блока для чтения фреймов */
    ctx->frame_size = ctx->channels * (ctx->bits / 8);
    ctx->chunk_size = ctx->max_frames * ctx->frame_size;

    /* выделяем внутренний буфер для чтения (под один блок) */
    ctx->chunk_buf = calloc(1, ctx->chunk_size);
    if (!ctx->chunk_buf) goto fail;

    /* заполнить PcmSource */
    src->ctx = ctx;
    src->read_chunk = wav_source_read_chunk;
    src->close = wav_source_close;

    /* заполнить глобальные параметры */
    src->sample_rate = ctx->sample_rate;
    src->channels = ctx->channels;
    src->format = ctx->format;  // условный формат
    src->duration_sec = ctx->data_seconds;  // длительность
    src->chunk_buf = ctx->chunk_buf;  // указатель на буфер

    return 0;

fail:
    wav_source_close(ctx);
    return -1;
}


/* ---------- Чтение сырых байт из блоков ---------- */
uint32_t wav_source_read_chunk(void *vctx, uint32_t max_frames)
{
    WavCtx *ctx = (WavCtx*)vctx;
    size_t to_copy = 0;  // прочитано raw байт
    uint32_t bytes_left = ctx->data_size - ctx->data_pos;
    uint32_t to_read = bytes_left < ctx->chunk_size ? bytes_left : ctx->chunk_size;

    if (to_read == 0) return 0;
    to_copy = fread(ctx->chunk_buf, 1, to_read, ctx->fp);
    ctx->data_pos += (uint32_t)to_copy;

    /* возвращаем количество фреймов */
    return (uint32_t)(to_copy / ctx->frame_size);
}


/* ---------- Закрытие WAV-потока ---------- */
void wav_source_close(void *vctx)
{
    WavCtx *ctx = (WavCtx*)vctx;
    if (!ctx) return;

    if (ctx->chunk_buf) free(ctx->chunk_buf);
    if (ctx->fp && !ctx->is_raw) fclose(ctx->fp);
    free(ctx);
}




/* ---------- Приёмник (WAV-выход) ---------- */
int wav_sink_open(PcmSink *sink, const AppConfig *cfg, const char *path)
{
    WavCtx *ctx = calloc(1, sizeof(WavCtx));
    if (!ctx || !path) return -1;

    /* умолчания */
    ctx->sample_rate = sink->sample_rate;
    ctx->channels = sink->channels;
    ctx->format = sink->format;
    ctx->bits = sink->format > 32 ? 32 : sink->format;
    ctx->bytes_written = 0;

    /* выбираем источник */
    if (!strcmp(path, "-")) {
        /* поток stdout */
        ctx->is_raw = 1;
        ctx->fp = stdout;
    } else {
        /* обычный файл */
        ctx->is_raw = 0;
        ctx->fp = fopen(path, "wb");
        if (!ctx->fp) goto fail;
    }

    /* записываем пустой RIFF-заголовок (только для файлов) */
    if (!ctx->is_raw) {
        int bytes_per_sample = ctx->bits / 8;
        int audio_format = (ctx->format == PCM_F32) ? 3 : 1;

        fwrite("RIFF", 1, 4, ctx->fp);
        write_u32_le(ctx->fp, 36);  // размер riff (патчится позже)
        fwrite("WAVE", 1, 4, ctx->fp);
        fwrite("fmt ", 1, 4, ctx->fp);
        write_u32_le(ctx->fp, 16);  // размер fmt-куска (дефолтный)
        write_u16_le(ctx->fp, (uint16_t)audio_format);
        write_u16_le(ctx->fp, (uint16_t)ctx->channels);
        write_u32_le(ctx->fp, (uint32_t)ctx->sample_rate);
        write_u32_le(ctx->fp, (uint32_t)(ctx->sample_rate * ctx->channels * bytes_per_sample));  // битрейт
        write_u16_le(ctx->fp, (uint16_t)(ctx->channels * bytes_per_sample));  // чередование блоков
        write_u16_le(ctx->fp, (uint16_t)ctx->bits);
        fwrite("data", 1, 4, ctx->fp);
        write_u32_le(ctx->fp, 0);  // размер данных (патчится позже)
    }

    /* выбираем функцию упаковки сэмплов */
    ctx->pack_sample = pcm_select_pack_fn(sink->src_format, ctx->format);
    if (!ctx->pack_sample) goto fail;

    /* заполнить PcmSink */
    sink->ctx = ctx;
    sink->write_sample = wav_sink_write_sample;
    sink->close = wav_sink_close;

    /* заполнить глобальные параметры */
    sink->sample_rate = ctx->sample_rate;
    sink->channels = ctx->channels;
    sink->format = ctx->format;  // условный формат

    return 0;

fail:
    wav_sink_close(ctx);
    return -1;
}


/* ---------- Запись парных PCM-сэмплов ---------- */
void wav_sink_write_sample(void *vctx, int32_t left, int32_t right)
{
    WavCtx *ctx = (WavCtx*)vctx;
    uint8_t buf[8];  // максимум для 32-бит стерео
    uint32_t bytes = ctx->pack_sample(left, right, buf);

    bytes *= ctx->channels;  // умножаем на 2 для стерео
    fwrite(buf, 1, bytes, ctx->fp);
    ctx->bytes_written += bytes;
}


/* ---------- Закрытие WAV-потока + патч заголовка ---------- */
int wav_sink_close(void *vctx)
{
    WavCtx *ctx = (WavCtx*)vctx;
    int rc = 0; if (!ctx) return -1;

    if (ctx->fp) {
        if (!ctx->is_raw) {
            /* патчим WAV-заголовок */
            uint32_t riff_size = 36 + ctx->bytes_written;

            if (fseek(ctx->fp, 4, SEEK_SET) != 0) rc = -1;
            write_u32_le(ctx->fp, riff_size);  // размер riff

            if (fseek(ctx->fp, 40, SEEK_SET) != 0) rc = -1;
            write_u32_le(ctx->fp, ctx->bytes_written);  // размер данных

            fclose(ctx->fp);

        } else {
            /* очищаем stdout */
            fflush(ctx->fp);
        }
    }

    free(ctx); return rc;
}

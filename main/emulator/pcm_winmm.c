/* pcm_winmm.c */
/* WinMM / WaveOut backend */
#include <stdlib.h>
#include <string.h>
#include "pcm_winmm.h"


/* Небольшой хелпер для показа всех устройств */
void winmm_list_devices(int is_input)
{
    UINT i, num;
    if (is_input == 1) {
        WAVEINCAPS wic;
        fprintf(stderr, "Available WaveIn devices (inputs):\n");
        num = waveInGetNumDevs();
        for (i = 0; i < num; i++) {
            waveInGetDevCaps(i, &wic, sizeof(wic));
            fprintf(stderr, "  %u: %s\n", i, wic.szPname);
        }
        fprintf(stderr, "\n");
    } else {
        WAVEOUTCAPS woc;
        fprintf(stderr, "Available WaveOut devices (speakers):\n");
        num = waveOutGetNumDevs();
        for (i = 0; i < num; i++) {
            waveOutGetDevCaps(i, &woc, sizeof(woc));
            fprintf(stderr, "  %u: %s\n", i, woc.szPname);
        }
        fprintf(stderr, "\n");
    }
}

/* Вспомогательная функция: выбор устройства по имени или индексу */
static int open_winmm_device(int is_input, const AppConfig *cfg, WAVEFORMATEX *wfx, 
    HWAVEIN *phwi, HWAVEOUT *phwo, DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD dwFlags)
{
    UINT i, num;
    MMRESULT res;
    WAVEINCAPS wic;
    WAVEOUTCAPS woc;
    UINT dev_id = WAVE_MAPPER;  // устройство по умолчанию

    char *dev_spec = is_input ? cfg->winmm.source_device : cfg->winmm.sink_device;
    int   is_index = 1;  // по умолчанию считаем, что это индекс

    if (dev_spec && dev_spec[0] != '\0') {
        /* проверяем, состоит ли строка только из цифр */
        char *p; for (p = dev_spec; *p; ++p) 
        if (*p < '0' || *p > '9') { is_index = 0; break; }

        if (is_index) {
            /* конвертируем в число */
            dev_id = atoi(dev_spec);
        } else {
            /* или ищем по началу имени */
            num = is_input ? waveInGetNumDevs() : waveOutGetNumDevs();
            for (i = 0; i < num; ++i) {
                if (is_input) {
                    waveInGetDevCaps(i, &wic, sizeof(wic));
                    if (!strncasecmp(wic.szPname, dev_spec, strlen(dev_spec))) {
                        dev_id = i; break;
                    }
                } else {
                    waveOutGetDevCaps(i, &woc, sizeof(woc));
                    if (!strncasecmp(woc.szPname, dev_spec, strlen(dev_spec))) {
                        dev_id = i; break;
                    }
                }
            }
        }
    }

    /* открываем с коллбэком, который передаст вызывающая функция */
    res = is_input ? 
        waveInOpen(phwi, dev_id, wfx, dwCallback, dwInstance, dwFlags) : 
        waveOutOpen(phwo, dev_id, wfx, dwCallback, dwInstance, dwFlags);

    /* 0 = успех, устройство доступно */
    return (res == MMSYSERR_NOERROR) ? 0 : -1;
}


/* Helper: возвращает кол-во байт данных в кольцевом буфере */
static inline uint32_t ring_used(const WinmmCtx *ctx) {
    if (ctx->ring_write >= ctx->ring_read)
        return ctx->ring_write - ctx->ring_read;
    else
        return ctx->ring_size - (ctx->ring_read - ctx->ring_write);
}

/* Helper: возвращает кол-во свободных байт в кольцевом буфере */
static inline uint32_t ring_free(const WinmmCtx *ctx) {
    return ctx->ring_size - ring_used(ctx) /*- 1*/;
}

/* Helper: копирует данные из кольцевого буфера во внешний и обновляет ring_read */
static inline void ring_read_copy(WinmmCtx *ctx, uint8_t *dst, uint32_t bytes) {
    if (ctx->ring_read + bytes <= ctx->ring_size) {
        memcpy(dst, ctx->ring_buffer + ctx->ring_read, bytes);
        ctx->ring_read += bytes;
    } else {
        uint32_t first = ctx->ring_size - ctx->ring_read;
        memcpy(dst, ctx->ring_buffer + ctx->ring_read, first);
        memcpy(dst + first, ctx->ring_buffer, bytes - first);
        ctx->ring_read = bytes - first;
    }
}

/* Helper: записывает данные в кольцевой буфер и обновляет ring_write (предполагается, что места достаточно) */
static inline void ring_write_copy(WinmmCtx *ctx, const uint8_t *src, uint32_t bytes) {
    if (ctx->ring_write + bytes <= ctx->ring_size) {
        memcpy(ctx->ring_buffer + ctx->ring_write, src, bytes);
        ctx->ring_write += bytes;
    } else {
        uint32_t first = ctx->ring_size - ctx->ring_write;
        memcpy(ctx->ring_buffer + ctx->ring_write, src, first);
        memcpy(ctx->ring_buffer, src + first, bytes - first);
        ctx->ring_write = bytes - first;
    }
}


/* Коллбэк для waveIn */
static void CALLBACK wave_in_cb(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                                DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    WinmmCtx *ctx = (WinmmCtx*)dwInstance;

    if (uMsg == WIM_DATA) {
        int i, idx = -1;
        WAVEHDR *hdr = (WAVEHDR*)dwParam1;

        /* находим индекс заголовка */
        EnterCriticalSection(&ctx->cs);
        for (i = 0; i < ctx->num_buffers; i++) {
            if (&ctx->headers[i] == hdr) {
                idx = i; break;
            }
        }
        if (idx >= 0) {
            /* копируем данные в кольцевой буфер */
            uint8_t *src = ctx->buffers[idx];
            uint32_t bytes = hdr->dwBytesRecorded;
            if (bytes <= ring_free(ctx)) {
                /* запись в кольцо (с защитой от переполнения) */
                ring_write_copy(ctx, src, bytes);
            }   // иначе теряем данные (можно увеличить кольцо)

            /* возвращаем буфер драйверу (если работаем) */
            if (ctx->running) {
                waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
            }
        }
        LeaveCriticalSection(&ctx->cs);
        SetEvent(ctx->hEvent);  // сигнализируем об освобождении буфера
    }
}

/* Коллбэк для waveOut */
static void CALLBACK wave_out_cb(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
                                 DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    WinmmCtx *ctx = (WinmmCtx*)dwInstance;

    if (uMsg == WOM_DONE) {
        int i, idx = -1;
        WAVEHDR *hdr = (WAVEHDR*)dwParam1;

        EnterCriticalSection(&ctx->cs);
        for (i = 0; i < ctx->num_buffers; i++) {
            if (&ctx->headers[i] == hdr) {
                idx = i; break;
            }
        }
        if (idx >= 0) {
            ctx->buffer_status[idx] = 0;  // свободен
        }
        LeaveCriticalSection(&ctx->cs);
        SetEvent(ctx->hEvent);
    }
}




/* Открытие источника */
int winmm_source_open(PcmSource *src, const AppConfig *cfg, const char *path)
{
    WinmmCtx *ctx = calloc(1, sizeof(WinmmCtx));
    int i; WAVEFORMATEX wfx; MMRESULT res;
    if (!ctx) return -1; (void)path;  // не используется

    /* читаем настройки */
    ctx->is_input = 1;
    ctx->sample_rate = src->sample_rate;
    ctx->channels = src->channels;
    ctx->format = src->format;  // входной формат (обычно 32)
    ctx->bits = src->format > 32 ? 32 : src->format;  // битность (фикс)
    ctx->timeout_ms = cfg->stream.timeout_ms;  // таймаут ожидания данных
    ctx->num_buffers = cfg->winmm.buffer_count;
    ctx->buffer_frames = src->chunk_frames;
    ctx->extra_buffers = cfg->winmm.source_delay;  // фикс потери сэмплов при ABR
    if (ctx->timeout_ms <= 0) ctx->timeout_ms = INFINITE;
    if (ctx->num_buffers < 2) ctx->num_buffers = 2;
    if (ctx->num_buffers > 8) ctx->num_buffers = 8;
    if (ctx->extra_buffers < 2) ctx->extra_buffers = 2;

    /* рассчитываем размеры буфера */
    ctx->frame_size = ctx->channels * (ctx->bits / 8);
    ctx->buffer_bytes = ctx->buffer_frames * ctx->frame_size;

    /* событие и критическая секция */
    ctx->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&ctx->cs);
    ctx->running = 1;

    /* выделяем заголовки и буферы для драйвера */
    ctx->headers = (WAVEHDR*)calloc(ctx->num_buffers, sizeof(WAVEHDR));
    ctx->buffers = (uint8_t**)calloc(ctx->num_buffers, sizeof(uint8_t*));
    ctx->buffer_status = (int*)calloc(ctx->num_buffers, sizeof(int));
    if (!ctx->headers || !ctx->buffers || !ctx->buffer_status) goto fail;

    for (i = 0; i < ctx->num_buffers; i++) {
        ctx->buffers[i] = (uint8_t*)malloc(ctx->buffer_bytes);
        if (!ctx->buffers[i]) goto fail;
        ctx->headers[i].lpData = (LPSTR)ctx->buffers[i];
        ctx->headers[i].dwBufferLength = ctx->buffer_bytes;
    }

    /* буфер для чтения */
    ctx->raw_buffer = calloc(1, ctx->buffer_bytes);
    if (!ctx->raw_buffer) goto fail;

    /* кольцевой буфер (множитель помогает от пропусков) */
    ctx->ring_size = ctx->buffer_bytes * ctx->num_buffers * ctx->extra_buffers;  // 2-8
    ctx->ring_buffer = (uint8_t*)malloc(ctx->ring_size);
    if (!ctx->ring_buffer) goto fail;
    ctx->ring_read = ctx->ring_write = 0;

    /* входной формат (int или float) */
    wfx.wFormatTag = (ctx->format == PCM_F32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)ctx->channels;
    wfx.nSamplesPerSec = (DWORD)ctx->sample_rate;
    wfx.wBitsPerSample = (WORD)ctx->bits;
    wfx.nBlockAlign = (WORD)ctx->frame_size;
    wfx.nAvgBytesPerSec = ctx->sample_rate * wfx.nBlockAlign;
    wfx.cbSize = 0;

    /* открываем устройство ввода */
    res = open_winmm_device(1, cfg, &wfx, &ctx->handle.hwi, NULL, (DWORD_PTR)wave_in_cb, (DWORD_PTR)ctx, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        fprintf(stderr, "WinMM: waveInOpen failed [%d]\n", res); goto fail;
    }

    /* подготавливаем буферы и передаём драйверу */
    for (i = 0; i < ctx->num_buffers; i++) {
        waveInPrepareHeader(ctx->handle.hwi, &ctx->headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(ctx->handle.hwi, &ctx->headers[i], sizeof(WAVEHDR));
    }
    waveInStart(ctx->handle.hwi);

    /* заполняем PcmSource */
    src->ctx = ctx;
    src->read_chunk = winmm_source_read_chunk;
    src->close = winmm_source_close;
    src->sample_rate = ctx->sample_rate;
    src->channels = ctx->channels;
    src->format = ctx->format;
    src->chunk_buf = ctx->raw_buffer;  // указатель на буфер

    /* вывести список устройств (если включено) */
    if (cfg->winmm.list_devices) winmm_list_devices(ctx->is_input);

    return 0;

fail:
    winmm_source_close(ctx);
    return -1;
}


/* Чтение из источника (для PcmSource) */
static uint32_t winmm_source_read_chunk(void *vctx, uint32_t max_frames)
{
    WinmmCtx *ctx = (WinmmCtx*)vctx;
    uint32_t bytes_needed = max_frames * ctx->frame_size;
    uint32_t bytes_avail, to_copy; DWORD waitResult;

    while (1) {
        /* прочитать доступные данные */
        EnterCriticalSection(&ctx->cs);
        bytes_avail = ring_used(ctx);

        /* данных достаточно, или завершение */
        if (bytes_avail >= bytes_needed || !ctx->running) {
            LeaveCriticalSection(&ctx->cs); break;
        }
        /* недостаточно данных, ждём события */
        LeaveCriticalSection(&ctx->cs);
        waitResult = WaitForSingleObject(ctx->hEvent, ctx->timeout_ms);
        if (waitResult == WAIT_TIMEOUT) return 0;  // таймаут: нет данных
    }
    if (!ctx->running) return 0;

    EnterCriticalSection(&ctx->cs);
    to_copy = (bytes_needed < bytes_avail) ? bytes_needed : bytes_avail;
    ring_read_copy(ctx, ctx->raw_buffer, to_copy);
    LeaveCriticalSection(&ctx->cs);

    return to_copy / ctx->frame_size;
}


/* Закрытие источника */
static int winmm_source_close(void *vctx)
{
    WinmmCtx *ctx = (WinmmCtx*)vctx;
    int i; if (!ctx) return 0;

    ctx->running = 0;
    if (ctx->handle.hwi) {
        waveInStop(ctx->handle.hwi);
        waveInReset(ctx->handle.hwi); Sleep(20);
        for (i = 0; i < ctx->num_buffers; ++i) {
            if (ctx->headers[i].dwFlags & WHDR_DONE) 
                waveInUnprepareHeader(ctx->handle.hwi, &ctx->headers[i], sizeof(WAVEHDR));
        }
        waveInClose(ctx->handle.hwi);
        ctx->handle.hwi = NULL;
    }
    if (ctx->hEvent) CloseHandle(ctx->hEvent);
    DeleteCriticalSection(&ctx->cs);

    if (ctx->buffers) {
        for (i = 0; i < ctx->num_buffers; ++i) free(ctx->buffers[i]);
        free(ctx->buffers);
    }
    if (ctx->headers) free(ctx->headers);
    if (ctx->raw_buffer) free(ctx->raw_buffer);
    if (ctx->buffer_status) free((void*)ctx->buffer_status);
    if (ctx->ring_buffer) free(ctx->ring_buffer);

    free(ctx); return 0;
}




/* Открытие приёмника */
int winmm_sink_open(PcmSink *sink, const AppConfig *cfg, const char *path)
{
    WinmmCtx *ctx = calloc(1, sizeof(WinmmCtx));
    int i; WAVEFORMATEX wfx; MMRESULT res;
    if (!ctx) return -1; (void)path;  // не используется

    /* читаем настройки */
    ctx->is_input = 0;
    ctx->sample_rate = sink->sample_rate;
    ctx->channels = sink->channels;
    ctx->format = sink->format;  // выходной формат
    ctx->bits = sink->format > 32 ? 32 : sink->format;  // битность (фикс)
    ctx->timeout_ms = cfg->stream.timeout_ms;  // таймаут ожидания данных
    ctx->num_buffers = cfg->winmm.buffer_count;
    ctx->buffer_frames = sink->chunk_frames * cfg->winmm.buffer_mul;  // для live-потока
    ctx->extra_buffers = cfg->winmm.sink_delay;  // фикс "раскачки" для live-потока
    if (ctx->timeout_ms <= 0) ctx->timeout_ms = INFINITE;
    if (ctx->num_buffers < 2) ctx->num_buffers = 2;
    if (ctx->num_buffers > 8) ctx->num_buffers = 8;
    if (ctx->extra_buffers < 2) ctx->extra_buffers = 2;

    /* рассчитываем размеры буфера */
    ctx->frame_size = ctx->channels * (ctx->bits / 8);
    ctx->buffer_bytes = ctx->buffer_frames * ctx->frame_size;

    /* инициализация синхронизации */
    ctx->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&ctx->cs);
    ctx->running = 1;

    /* выделение буферов и заголовков */
    ctx->headers = (WAVEHDR*)calloc(ctx->num_buffers, sizeof(WAVEHDR));
    ctx->buffers = (uint8_t**)calloc(ctx->num_buffers, sizeof(uint8_t*));
    ctx->buffer_status = (int*)calloc(ctx->num_buffers, sizeof(int));
    if (!ctx->headers || !ctx->buffers || !ctx->buffer_status) goto fail;

    for (i = 0; i < ctx->num_buffers; i++) {
        ctx->buffers[i] = (uint8_t*)malloc(ctx->buffer_bytes);
        if (!ctx->buffers[i]) goto fail;
        ctx->headers[i].lpData = (LPSTR)ctx->buffers[i];
        ctx->headers[i].dwBufferLength = ctx->buffer_bytes;
    }

    /* кольцевой буфер (достаточно большой) */
    ctx->ring_size = ctx->buffer_bytes * ctx->num_buffers * ctx->extra_buffers;  // запас
    ctx->ring_buffer = (uint8_t*)malloc(ctx->ring_size);
    ctx->ring_read = ctx->ring_write = 0;
    if (!ctx->ring_buffer) goto fail;

    /* выходной формат */
    wfx.wFormatTag = (ctx->format == PCM_F32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wfx.nChannels = ctx->channels;
    wfx.nSamplesPerSec = ctx->sample_rate;
    wfx.wBitsPerSample = ctx->bits;
    wfx.nBlockAlign = ctx->channels * (ctx->bits / 8);
    wfx.nAvgBytesPerSec = ctx->sample_rate * wfx.nBlockAlign;
    wfx.cbSize = 0;

    /* открываем устройство вывода */
    res = open_winmm_device(0, cfg, &wfx, NULL, &ctx->handle.hwo, (DWORD_PTR)wave_out_cb, (DWORD_PTR)ctx, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        fprintf(stderr, "WinMM: waveOutOpen failed [%d]\n", res); goto fail;
    }

    /* подготовка заголовков (но не отправляем их сразу) */
    for (i = 0; i < ctx->num_buffers; i++) {
        waveOutPrepareHeader(ctx->handle.hwo, &ctx->headers[i], sizeof(WAVEHDR));
    }

    /* выбираем функцию упаковки сэмплов */
    ctx->pack_sample = pcm_select_pack_fn(sink->src_format, ctx->format);
    if (!ctx->pack_sample) goto fail;

    /* заполняем PcmSink */
    sink->ctx = ctx;
    sink->write_sample = winmm_sink_write_sample;
    sink->close = winmm_sink_close;
    sink->sample_rate = ctx->sample_rate;
    sink->channels = ctx->channels;
    sink->format = ctx->format;

    /* вывести список устройств (если включено) */
    if (cfg->winmm.list_devices) winmm_list_devices(ctx->is_input);

    /* принудительный "прогрев" (фиксит раскачку при ABR-AAC) */
    if (cfg->winmm.sink_delay > 0) {
        for (i = 0; i < (int)sink->chunk_frames * ctx->extra_buffers; i++) 
        sink->write_sample(sink->ctx, 0, 0);  // запись тишины
    }
    return 0;

fail:
    winmm_sink_close(ctx);
    return -1;
}


/* Запись в приёмник (для PcmSink) */
static void winmm_sink_write_sample(void *vctx, int32_t left, int32_t right)
{
    WinmmCtx *ctx = (WinmmCtx*)vctx;
    int i, free_idx = -1;
    uint8_t buf[8], *dst;
    uint32_t bytes_per_channel = ctx->pack_sample(left, right, buf);
    uint32_t total_bytes = ctx->channels * bytes_per_channel;

    EnterCriticalSection(&ctx->cs);

    /* помещаем упакованные байты в кольцевой буфер */
    if (total_bytes <= ring_free(ctx)) {
        ring_write_copy(ctx, buf, total_bytes);
    }  // иначе теряем (можно увеличить кольцо)

    /* проверим, есть ли свободный буфер и данные в кольце */
    while (1) {
        /* ищем свободный буфер */
        for (i = 0; i < ctx->num_buffers; i++) {
            if (ctx->buffer_status[i] == 0) {
                free_idx = i; break;
            }
        }
        if (free_idx == -1) {
            /* все буферы заняты, ждём освобождения */
            LeaveCriticalSection(&ctx->cs);
            WaitForSingleObject(ctx->hEvent, INFINITE);
            EnterCriticalSection(&ctx->cs);
            continue;  // после пробуждения снова проверяем
        }

        /* недостаточно данных для заполнения, выходим */
        if (ring_used(ctx) < ctx->buffer_bytes) break;

        /* данных достаточно, копируем из кольца в буфер */
        dst = ctx->buffers[free_idx];
        ring_read_copy(ctx, dst, ctx->buffer_bytes);

        /* отправляем буфер */
        ctx->headers[free_idx].dwBufferLength = ctx->buffer_bytes;
        waveOutWrite(ctx->handle.hwo, &ctx->headers[free_idx], sizeof(WAVEHDR));
        ctx->buffer_status[free_idx] = 1;
        //break;  // выходим из цикла, отправили один буфер
    }

    LeaveCriticalSection(&ctx->cs);
}


/* Отправляет все остатки данных из кольцевого буфера */
static void winmm_wait_for_data(WinmmCtx *ctx)
{
    uint32_t avail, bytes_in_ring;
    int i, busy, free_idx = -1;

    /* вычисляем доступные байты в кольце */
    EnterCriticalSection(&ctx->cs);
    avail = ring_used(ctx);
    if (avail == 0) {
        LeaveCriticalSection(&ctx->cs); return;
    }
    /* ищем свободный буфер */
    for (i = 0; i < ctx->num_buffers; i++) {
        if (ctx->buffer_status[i] == 0) {
            free_idx = i; break;
        }
    }
    /* выводим остаток на устройство */
    if (free_idx >= 0) {
        uint8_t *dst = ctx->buffers[free_idx];
        ring_read_copy(ctx, dst, avail);
        ctx->headers[free_idx].dwBufferLength = avail;
        waveOutWrite(ctx->handle.hwo, &ctx->headers[free_idx], sizeof(WAVEHDR));
        ctx->buffer_status[free_idx] = 1;
    }
    LeaveCriticalSection(&ctx->cs);

    /* ждём, пока все данные не будут проиграны */
    while (1) {
        /* вычисляем число байт в кольце */
        EnterCriticalSection(&ctx->cs);
        bytes_in_ring = ring_used(ctx);

        /* считаем занятые буферы */
        busy = 0;
        for (i = 0; i < ctx->num_buffers; i++) {
            if (ctx->buffer_status[i]) busy++;
        }
        LeaveCriticalSection(&ctx->cs);

        /* всё отправлено и воспроизведено! */
        if (bytes_in_ring == 0 && busy == 0) break;

        /* ожидаем событие освобождения буфера */
        WaitForSingleObject(ctx->hEvent, ctx->timeout_ms);
    }
}


/* Закрытие приёмника */
static int winmm_sink_close(void *vctx)
{
    WinmmCtx *ctx = (WinmmCtx*)vctx;
    int i; if (!ctx) return -1;

    /* останавливаем устройство и освобождаем ресурсы */
    ctx->running = 0;
    if (ctx->handle.hwo) {
        winmm_wait_for_data(ctx);  // обрабатываем остатки данных
        waveOutReset(ctx->handle.hwo);
        for (i = 0; i < ctx->num_buffers; i++) {
            waveOutUnprepareHeader(ctx->handle.hwo, &ctx->headers[i], sizeof(WAVEHDR));
        }
        waveOutClose(ctx->handle.hwo);
    }
    DeleteCriticalSection(&ctx->cs);
    CloseHandle(ctx->hEvent);
    if (ctx->buffers) {
        for (i = 0; i < ctx->num_buffers; i++) {
            free(ctx->buffers[i]);
        }
        free(ctx->buffers);
    }
    if (ctx->headers) free(ctx->headers);
    if (ctx->buffer_status) free(ctx->buffer_status);
    if (ctx->ring_buffer) free(ctx->ring_buffer);

    free(ctx); return 0;
}

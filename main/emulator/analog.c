/* Analog.c (C89-friendly)
 * -----------------------
 * Direct analog sound transfer with DSP processing.
 * RX and TX modes (listening/recording) compatible!
 * Input mode (FSM) is controlled by input argument.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "pcm_io.h"
#include "dsp_host.h"
#include "control.h"

/* ---------- main ---------- */
int analog_run(const char *in_name, const char *out_name, int fsm_mode)
{
    int rc = 0;
    PcmSource src = {0};
    PcmSink sink = {0};

    /* processing */
    PcmUnpackFn unpack_samples;
    PcmDownmixFn downmix_process;
    AudioDspCtx dsp = {0};

    /* chunks, frames */
    uint32_t i, n;
    uint32_t chunk_num = 0;
    uint32_t chunk_bytes = 2;
    uint32_t chunk_size_ms;
    int32_t  hk_stat = 0;  // хот-кеи
    int32_t  is_stream = 0;

    /* unpacked samples buffers */
    int32_t *lbuf = NULL;
    int32_t *rbuf = NULL;

    /* fade on enter RX mode */
    fade_q15 fade;
    uint32_t fade_samples = 0;  // total

    /* mode-depended processing */
    int32_t fadein_ms = fsm_mode ? cfg.analog_rx.fadein_ms : cfg.analog_tx.fadein_ms;
    int32_t downmix_mode = fsm_mode ? cfg.analog_rx.downmix_mode : cfg.analog_tx.downmix_mode;
    char* mode_pref = fsm_mode ? "RX" : "TX";  // 0 = ANALOG_TX

    /* ms timer for benchmark */
    double elapsed_ms;
    clock_t time_start, time_end;


    /* --- открыть источник сигнала --- */
    if (open_source(&src, &cfg, in_name) != 0) {
        fprintf(stderr, "%s_ANALOG: cannot open input %s source.\n", mode_pref, cfg.input.pcm_backend);
        rc = 1; goto cleanup;
    } else {
        // режим потока: нажатие паузы не блокирует цикл чтения
        if (!strcmp(in_name, "-") || strcasecmp(cfg.input.pcm_backend, "wav")) is_stream = 1;
    }

    /* --- открыть приёмник сигнала --- */
    sink.src_rate = src.sample_rate;  // сообщить исходную частоту
    sink.src_format = (PcmFormat)32;  // сообщить формат обработки (32-bit)
    if (open_sink(&sink, &cfg, out_name) != 0) {
        fprintf(stderr, "%s_ANALOG: cannot open output %s target.\n", mode_pref, cfg.output.pcm_backend);
        rc = 1; goto cleanup;
    }


    /* если выход моно, микшировать каналы */
    if (sink.channels < 2 && downmix_mode < 1) downmix_mode = 1;

    /* --- выбрать функции преобразования --- */
    unpack_samples  = pcm_select_unpack_fn(src.channels, src.format);
    downmix_process = pcm_select_downmix_fn(src.channels, downmix_mode);

    if (!unpack_samples) {
        fprintf(stderr, "%s_ANALOG: unsupported PCM input format.\n", mode_pref);
        rc = 1; goto cleanup;
    }


    /* посчитаем размеры буфера чтения в байтах и мс */
    chunk_bytes = src.chunk_frames * src.channels * (src.format > 32 ? 4 : src.format / 8);
    chunk_size_ms = (src.chunk_frames * 1000) / src.sample_rate;

    /* буферы для распакованных сэмплов (int32 stereo) */
    lbuf = (int32_t*)malloc(src.chunk_frames * sizeof(int32_t));
    rbuf = (int32_t*)malloc(src.chunk_frames * sizeof(int32_t));
    if (!lbuf || !rbuf) {
        fprintf(stderr, "%s_ANALOG: buffers out of memory.\n", mode_pref);
        rc = 1; goto cleanup;
    }


    /* --- подсказки по управлению --- */
    fprintf(stderr, "Keyboard Controls:\n");
    fprintf(stderr, "- Use [p] or [space] to PAUSE %s stream\n",   fsm_mode ? "playing" : "recording");
    fprintf(stderr, "- Use [s] or [escape] to STOP %s stream\n\n", fsm_mode ? "playing" : "recording");

    /* --- информация пользователю --- */
    fprintf(stderr, "Input FSM mode:      %s\n", fsm_mode ? "RX (playback)" : "TX (record)");
    fprintf(stderr, "Input PCM backend:   %s <- %i Hz, %i ch, %i-bit\n", cfg.input.pcm_backend, src.sample_rate, src.channels, src.format);
    fprintf(stderr, "Output PCM backend:  %s -> %i Hz, %i ch, %i-bit\n", cfg.output.pcm_backend, sink.sample_rate, sink.channels, sink.format);
    fprintf(stderr, "Read chunk params:   %i ms, %i frames, %i bytes\n", chunk_size_ms, src.chunk_frames, chunk_bytes);
    fprintf(stderr, "\n");


    /* --- инициализация DSP --- */
    dsp.fsm_mode = fsm_mode;  // TX/RX
    dsp_init(&dsp, &cfg, sink.sample_rate);

    /* --- инициализация fade-in --- */
    fade_samples = (fadein_ms * src.sample_rate) / 1000;
    fadein_init(&fade, fade_samples);


    /* --- основной цикл --- */
    time_start = clock();  // бенчмарк
    fprintf(stderr, "\n");

    while ((n = src.read_chunk(src.ctx, src.chunk_frames)) > 0) {
        chunk_num++; // update counter

        /* проверка нажатия хот-кеев */
        hk_stat = check_hotkeys(is_stream);
        if (hk_stat == 1) break;     // отправлен стоп: выходим
        if (hk_stat == 2) continue;  // пауза в потоке: пропуск цикла

        /* вывести прогресс в консоль */
        fprintf(stderr, "Processing PCM chunk #%u...\r", chunk_num);

        /* распаковка сырых данных в int32 */
        unpack_samples(src.chunk_buf, lbuf, rbuf, n);

        /* обработка DSP-модулями */
        dsp_process(&dsp, lbuf, rbuf, n);

        /* downmix в моно, если задано */
        downmix_process(lbuf, rbuf, n);

        /* fade-in первые N мс, если задано */
        fadein_process(&fade, lbuf, rbuf, n);

        /* запись сэмплов в целевом формате */
        for (i = 0; i < n; ++i)
            sink.write_sample(sink.ctx, lbuf[i], rbuf[i]);
    }


    fprintf(stderr, "\n");
    time_end = clock(); // остановить таймер
    elapsed_ms = (double)(time_end - time_start) / CLOCKS_PER_SEC;
    dsp_reset(&dsp);

    fprintf(stderr, "\n%s_ANALOG done: %s\n", mode_pref, !strcmp(out_name, "-") ? "<stream>" : out_name);
    fprintf(stderr, "Total elapsed time: %.3f seconds\n", elapsed_ms);

cleanup:
    if (lbuf) free(lbuf);
    if (rbuf) free(rbuf);
    if (!rc)  dsp_free(&dsp);
    if (src.ctx)  src.close(src.ctx);
    if (sink.ctx) sink.close(sink.ctx);

    return rc;
}

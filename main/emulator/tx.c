/* TX.c */
/* ------------------------------
 * TX-режим (запись с модуляцией)
 * ------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "config.h"
#include "crc16.h"
#include "frame.h"
#include "modulation.h"
#include "pcm_io.h"
#include "metadata.h"
#include "control.h"
#include "tx_adts.c"

#ifdef   _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

/* Прототипы функций */
typedef struct {
    AskParams ask;               // m-ASK params (from modulation.h)
    AudioDspCtx dsp_out;         // DSP chain for pre-emphasis EQ

    uint8_t *bitbuf_L;           // the whole left frame bits buffer
    uint8_t *bitbuf_R;           // the whole right frame bits buffer
    uint32_t frame_bits_total;   // max frame size with header and payload
    uint32_t capacity_bits;      // max frame size with header and sync
    uint8_t  bits_per_symbol;    // copied from ofdm, just for logging
    uint8_t  payload_previous;   // just for console log formatting

} TxBitCtx;

typedef struct {
// input type
    FILE *file;
    enum {
      TX_INPUT_FILE,         // binary file
      TX_INPUT_PCM,          // PcmSource for AAC encoding
      TX_INPUT_MEM           // cached metadata
    } type;

// memory backend
    uint8_t *mem;            // selected payload
    size_t   mem_size;
    uint32_t mem_pos;

// meta helpers
    char    *user_file;      // only for post-check
    char    *kv_template;    // malloc'ed text string
    uint8_t *file_mem;       // malloc'ed file buffer
    size_t   file_size;      // malloc'ed buffer size
    int      max_frames;     // cuts data, 0=infinite

// aac-encoding
    AudioEncCtx aenc;        // encoder context (from tx_adts.c)

} TxInput;

typedef enum {
    TX_READ_OK = 0,
    TX_READ_EOF,
    TX_READ_NO_DATA

} TxReadStatus;

typedef struct {
    AdtsReader  adts;           // выравнивание AAC-фреймов по payload (из tx_adts.c)
    int         is_stream;      // маркер stdin/WinMM-потока
    uint32_t    timeout_ms;     // таймаут восстановления
    uint32_t    stream_bytes;   // всего полученных байт (включая файлы)
    uint8_t     payload_class;  // класс, если читаем из stdin-потока
    const char *file_ext;       // для определения типа данных

} StreamState;

/* TODO: refactor
typedef struct {
    PcmSink sink;
    TxInput input;
    TxBitCtx bit;
    OfdmParams ofdm;
    AskParams ask;
    StreamState sstat;
    MetadataCtx meta;

} TxModulation;
*/


/* Основные функции записи фрейма */
int tx_emit_payload_frame(PcmSink*, TxInput*, StreamState*, OfdmParams*, MetadataCtx*, int, FrameDesc*, FrameDesc*, uint8_t*, uint8_t*, TxBitCtx*, int);
void tx_try_emit_meta_frame(PcmSink*, TxInput*, StreamState*, OfdmParams*, MetadataCtx*, int, FrameDesc*, FrameDesc*, uint8_t*, uint8_t*, TxBitCtx*, int);
TxReadStatus tx_read_input_data(TxInput*, StreamState*, int, uint8_t*, uint8_t*, FrameDesc*, FrameDesc*);

/* Хелперы основных функций */
int read_mem_input(TxInput*, size_t);
size_t tx_accumulate_bytes(FILE*, StreamState*, uint8_t*, size_t);
int wait_for_input(FILE*, int);  // таймаут ожидания данных (кросс-платформа)

/* Хелперы для управления метаданными */
int payload_class_from_type(StreamState*);
char *build_kv_string(const char*, ...);
char *read_user_file(char*, TxInput*);
int patch_metadata_field(TxInput*, const uint8_t*, size_t, const void*, size_t, const uint8_t*, size_t, const void*, size_t);
int patch_mem_data(uint8_t*, size_t, const uint8_t*, size_t, const void*, size_t);

/* Вывод всей статистики обработки на экран */
void tx_log_frame_status(FrameDesc*, TxBitCtx*, StreamState*);


/* TX main process */
int tx_run(const char* in_name, const char* out_name)
{
    /* channel */
    FILE *in = NULL;
    PcmSink sink = {0};
    TxInput input = {0};
    TxBitCtx tctx = {0};
    OfdmParams ofdm = {0};  // параметры модуляции
    FrameDesc desc = {0};  // фрейм левого канала
    FrameDesc desc_r = {0};  // фрейм правого канала
    StreamState sstat = {0};  // состояние потока
    MetadataCtx meta = {0};  // политика метаданных

    int rc = 0; // ok
    int is_stream = 0;
    int aac_encoding = 0;
    int adts_input = 0;
    int32_t carrier_freq = 0;
    uint32_t step = 0;

    uint8_t *payload_buf = NULL;
    uint8_t *payload_buf_r = NULL;

    uint16_t file_crc = 0xFFFF;
    uint32_t file_size = 0;
    char    *file_ext = strrchr(in_name, '.');

    /* frame sync borders */
    uint32_t num_lead_symbols = 0;

    /* frame size for bitrate */
    uint32_t frame_symbols = 0;
    uint32_t pilot_symbols = 0;

    /* ms timer for benchmark */
    double  elapsed_ms;
    clock_t time_start, time_end;


    /* open input and output
       --------------------- */
    if (!strcmp(in_name, "-")) {
        in = stdin;
        is_stream = 1;  // работаем как с потоком
    } else {
        in = fopen(in_name, "rb"); if (!in) {
            fprintf(stderr, "Cannot open input file.\n");
            rc = 1; goto cleanup;
        }
    }
    /* open target from selected PCM backend */
    sink.src_format = (PcmFormat)32;  // сообщить формат обработки (32-bit)
    if (open_sink(&sink, &cfg, out_name) != 0) {
        fprintf(stderr, "Cannot open output %s target.\n", cfg.output.pcm_backend);
        rc = 1; goto cleanup;
    }

    /* enable reading from PcmSource and FDK-AAC */
    if (cfg.tx.aac_encoding) {
        if (strcasecmp(cfg.input.pcm_backend, "wav")) {
            // аудио-бэкенды: работаем с потоком
            aac_encoding = is_stream = 1;  // флаги для проверок
        } else {
            // file/stdin: проверим расширение/класс потока
            if (file_ext && !strcasecmp(file_ext, ".wav")) aac_encoding = 1;
            if (file_ext && !strcasecmp(file_ext, ".pcm")) aac_encoding = 1;
            if (is_stream && cfg.stream.payload_class == PAYLOAD_AUDIO +AUDIO_PCM) aac_encoding = 1;
        }
    }


    /* input params */
    input.file = in;
    input.type = TX_INPUT_FILE;
    input.max_frames = 0;
    input.kv_template = NULL;
    input.user_file = NULL;

    /* stream/file/stdin */
    sstat.is_stream = is_stream;
    sstat.file_ext = file_ext;
    sstat.payload_class = cfg.stream.payload_class;
    sstat.timeout_ms = cfg.stream.timeout_ms;
    sstat.stream_bytes = 0;  // всего прочитано

    /* meta-frames policy and fields */
    meta.insert_gaps = cfg.meta.insert_gaps;
    meta.insert_frames = cfg.meta.insert_frames;
    meta.max_frames = cfg.meta.max_frames;
    meta.track_number = meta.total_tracks = -1;
    meta.time_stamp = meta.duration = -1;
    meta.year_id = meta.genre_id = -1;
    meta.record_time = time(NULL);  // время старта записи

    /* meta-frames info window tuning */
    meta.ui_font_file = cfg.meta.ui_font_file;  // ttf-файл шрифта
    meta.ui_font_name = cfg.meta.ui_font_name;  // название шрифта
    meta.ui_font_size = cfg.meta.ui_font_size;  // размер шрифта
    meta.ui_font_bold = cfg.meta.ui_font_bold;  // жирность шрифта


    /* modulation params
       ----------------- */
    #define acfg tctx.ask  // профиль

    /* fill default ofdm values */
    ofdm.qam_order = (uint16_t)cfg.ofdm.qam_order;
    ofdm.symbols_per_frame = (uint8_t)cfg.ofdm.symbols_per_frame;
    ofdm.low_freq = (uint32_t)cfg.ofdm.data_low_freq;
    ofdm.high_freq = (uint32_t)cfg.ofdm.data_high_freq;
    ofdm.sample_rate = (uint32_t)sink.sample_rate;
    ofdm.fft_size = (uint16_t)cfg.ofdm.fft_size;
    ofdm.frame_stereo = (sink.channels >= 2) ? cfg.tx.stereo_mode : 0;
    ofdm.bits_per_symbol = bits_per_symbol_from_qam(cfg.ofdm.qam_order);
    ofdm.active_carriers = calc_active_carriers(&ofdm);
    ofdm.custom_bytes = 0; desc.ofdm_encoding = 1; // при OFDM

    /* override some params on forced m-ASK */
    if (cfg.ofdm.prefer_ask) {
        // отключаем sync_word, MSB/LSB-кодирование payload и OFDM-ёмкость
        SERVICE_BITS = HEADER_BITS + CRC_BITS; desc.ofdm_encoding = 0;
        if (cfg.ask.frame_bytes >= PAYLOAD_MIN) ofdm.custom_bytes = cfg.ask.frame_bytes;
    }


    /* frame params
       ------------ */
    /* compute capacity for bit buffers */
    step = lcm(ofdm.bits_per_symbol * 16, 8);  // мин. общее кратное байту и 16 символам
    tctx.frame_bits_total = ofdm.active_carriers * ofdm.symbols_per_frame * ofdm.bits_per_symbol;  // OFDM-версия
    tctx.frame_bits_total = ofdm.custom_bytes ? SERVICE_BITS + ofdm.custom_bytes * 8 : tctx.frame_bits_total;  // ASK
    tctx.frame_bits_total = (tctx.frame_bits_total + step-1) / step * step;  // округление вверх до $step
    tctx.bits_per_symbol  = ofdm.bits_per_symbol;  // копия для логов

    /* compute and fill required fields */
    desc.sync.preamble_pattern = cfg.tx.sync_pattern;  // 01010101 (не используется)...
    desc.sync.preamble_len_bits = (cfg.ask.preamble_symbols + cfg.ask.training_symbols) * ofdm.bits_per_symbol;  // ASK-версия
    desc.hdr.symbols_bits = cfg.ofdm.prefer_ask ? ofdm.bits_per_symbol : ofdm.symbols_per_frame;  // OFDM-символов или бит на ASK-символ
    desc.hdr.symbols_bits |= (cfg.tx.gray_mapping && ofdm.bits_per_symbol > 1) ? 0x10 : 0x00;  // включить код Грея для символов (улучшение чтения)
    desc.payload_bytes = (uint16_t)calc_payload_bytes(&ofdm);  // макс. размер полезных данных
    desc.payload_extra = 0;  // общий размер блока стерео-пары, включая хвост метаданных (для ADTS)


    /* m-ASK/PAM definitions
       --------------------- */
    acfg.min_amp = amp_from_db(cfg.ask.low_level_db, 32);
    acfg.max_amp = amp_from_db(cfg.ask.high_level_db, 32);
    acfg.sync_amp = acfg.max_amp;  // всегда равна макс. бит-уровню
    acfg.cycle_samples = cfg.ask.cycle_samples;  // задаёт частоту (вместе с sample_rate)
    acfg.symbol_bits = ofdm.bits_per_symbol;  // бит на символ (1,2,4...)
    acfg.variable_len = cfg.tx.variable_len;  // обрезка длины фреймов по большему из пары

    /* preamble, header, pilot-pulses */
    acfg.preamble_symbols = cfg.ask.preamble_symbols;  // кол-во начальных НЧ-маркеров в преамбуле
    acfg.training_symbols = cfg.ask.training_symbols;  // символы после НЧ-маркеров для вычисления порогов
    acfg.pilot_interval = cfg.ask.pilot_interval;  // интервал чередования синхро-импульса в символах
    acfg.extra_symbols = SERVICE_BITS - (SERVICE_BITS + acfg.symbol_bits-1) / acfg.symbol_bits;  // доп. символы от 2-ASK заголовка (при 4+ASK)

    /* preamble training symbols thresholds */
    acfg.sync_train_max = acfg.preamble_symbols + acfg.training_symbols * 0/4;  // начало столбика с максимальным битом
    acfg.sync_train_min = acfg.preamble_symbols + acfg.training_symbols * 2/4;  // начало столбика с нулевым битом (TODO: 1/4 для 4-th)
    acfg.sync_train_lv2 = acfg.preamble_symbols + acfg.training_symbols * 2/4;  // начало столбика с верхним средним порогом (для 4+ASK)
    acfg.sync_train_lv1 = acfg.preamble_symbols + acfg.training_symbols * 3/4;  // начало столбика с нижним средним порогом (для 4+ASK)

    /* compute m-ASK bits level amplitudes */
    acfg.num_levels = ofdm.qam_order;  // битовых уровней (2,4,16...)
    carrier_freq = cfg.ask.carrier_freq;  // рассчитана в config.h
    ask_calc_thresh_levels(&acfg, acfg.min_amp, acfg.max_amp, acfg.amp_levels, acfg.thresh_levels, 32);

    /* next compute each-level sine period templates */
    if (ask_build_wave_table(&acfg) != 0) {
        fprintf(stderr, "OOM acfg.wave_table.\n"); rc=1; goto cleanup;
    }


    /* allocate bit and payload buffers */
    tctx.capacity_bits = desc.sync.preamble_len_bits + tctx.frame_bits_total;
    tctx.bitbuf_L = (uint8_t*)calloc(1, tctx.capacity_bits);  // аллоцируются как байты!
    tctx.bitbuf_R = (uint8_t*)calloc(1, tctx.capacity_bits);
    if (!tctx.bitbuf_L || !tctx.bitbuf_R) { fprintf(stderr, "OOM bitbuf.\n"); rc=1; goto cleanup; }

    payload_buf = (uint8_t*)calloc(1, desc.payload_bytes);
    payload_buf_r = (uint8_t*)calloc(1, desc.payload_bytes);
    if (!payload_buf || !payload_buf_r) { fprintf(stderr, "OOM payload_buf.\n"); rc=1; goto cleanup; }


    /* streaming/AAC-reader init
       ------------------------- */
    sstat.adts.pending[0] = 0;
    sstat.adts.pending_len = 0;
    sstat.adts.stream_eof = 0;
    sstat.adts.stream_time = 0;

    /* check direct ADTS mode (for info) */
    if (file_ext && !strcasecmp(file_ext, ".aac")) adts_input = 1;
    if (is_stream && sstat.payload_class == PAYLOAD_AUDIO +AUDIO_AAC) adts_input = 1;
    if (acfg.variable_len == -1) {
        acfg.variable_len = aac_encoding || adts_input ? 1 : 0;  // enabled for ADTS
    }

    /* compute target bitrate */
    frame_symbols = tctx.frame_bits_total / acfg.symbol_bits + acfg.extra_symbols;  // символов в теле 1 фрейма (header + payload)
    pilot_symbols = (frame_symbols + acfg.pilot_interval-1) / acfg.pilot_interval;  // пилотов в 1 фрейме (для битрейта и re-sync)
    calc_stream_bitrate(&ofdm, &acfg, sink.sample_rate, tctx.capacity_bits, desc.payload_bytes, pilot_symbols);


    /* initialize AAC-encoder (FDK-AAC)
       -------------------------------- */
    if (aac_encoding) {
        // инициализация AAC-кодера и PcmSource...
        input.aenc.in_file = in_name;  // скопировать имя файла
        if (aac_encoder_create(&input.aenc, payload_bitrate) == 0) {
            fprintf(stderr, "\n");

            if (meta.insert_gaps && cfg.meta.metadata_window) {
                // Вывод метаданных в отдельное окно (GUI как в RX)
                if (strcasecmp(cfg.output.pcm_backend, "wav"))  // -input?
                    metadata_window_init(&meta, "WAITING FOR RECORD...");
            }
        } else {
            aac_encoding = 0;  // отключаем флаг
            fprintf(stderr, "AAC encoder init failed, falling back to binary mode.\n\n");
        }
    }


    /* preparing to run
       ---------------- */
    /* show keyboard hints */
    fprintf(stderr, "Keyboard Controls:\n");
    fprintf(stderr, "- Use [p] or [space] to PAUSE encoding stream\n");
    fprintf(stderr, "- Use [s] or [escape] to STOP encoding stream\n\n");

    /* show session details */
    fprintf(stderr, "TX input source:    %s (%i ms timeout)\n", is_stream ? "Stream" : "File", cfg.stream.timeout_ms);
    fprintf(stderr, "Output backend:     %s -> %i Hz, %i channels, %i-bit\n", cfg.output.pcm_backend, sink.sample_rate, sink.channels, sink.format);
    fprintf(stderr, "Operating mode:     %s (%s)\n", aac_encoding || adts_input ? "Record audio" : "Binary data", 
                                                     aac_encoding ? "AAC-encoding" : (adts_input ? "ADTS-reading" : "direct modulation"));
    if (cfg.ofdm.prefer_ask) {
    fprintf(stderr, "Modulation:         %i-ASK, %i bits/symbol, 1 cycles/symbol\n", acfg.num_levels, acfg.symbol_bits);
    fprintf(stderr, "Frequency Range:    carrier = %i, sync = %i, bw = %i Hz\n", carrier_freq, carrier_freq / 2, carrier_freq * 2);
    fprintf(stderr, "Frame Levels:       max = %.1f dB, min = %.1f dB (range: %.0f dB)\n", cfg.ask.high_level_db, cfg.ask.low_level_db, cfg.ask.high_level_db-cfg.ask.low_level_db);
    } else {  // OFDM-mode specs
    fprintf(stderr, "Modulation:         %i-QAM, %i bits/symbol, %i symbols/frame\n", ofdm.qam_order, ofdm.bits_per_symbol, ofdm.symbols_per_frame);
    fprintf(stderr, "Frequency Range:    %i...%i Hz, service: %i Hz\n", ofdm.low_freq, ofdm.high_freq, cfg.service.max_freq);
    fprintf(stderr, "OFDM Sub-carriers:  max = %i (FFT: %i bins)\n", ofdm.active_carriers, ofdm.fft_size);
    }
    fprintf(stderr, "Frame Capacity:     raw = %i bytes, data = %i bytes, %s\n", tctx.capacity_bits/8, desc.payload_bytes, ofdm.frame_stereo ? "2-ch" : "1-ch");
    fprintf(stderr, "Preamble symbols:   marker = %d, training = %d (%i bytes total)\n", cfg.ask.preamble_symbols, acfg.training_symbols, desc.sync.preamble_len_bits/8);
    fprintf(stderr, "Header length:      sync = %u, main = %u, crc = %u+%u+%u (%u bytes)\n", cfg.ofdm.prefer_ask ? 0 : SYNC_BITS/8, HEADER_BITS/8, 2,2,2, SERVICE_BITS/8);
    fprintf(stderr, "Pilot symbols:      interleaving = %d, in-frame total = %d\n", acfg.pilot_interval, pilot_symbols);
    fprintf(stderr, "Target bitrate:     raw = %d bit/s, useful = %d bit/s\n", total_bitrate, payload_bitrate);
    fprintf(stderr, "Pre-emphasis EQ:    %s, record-level: %.1f dB, bias-profile: %i\n", cfg.tx.preemphasis_eq ? "on" : "off", cfg.tx.eq.preamp_db, cfg.tx.eq_bias_profile);
    fprintf(stderr, "AAC encoder:        type: %s, initialized: %s, mode: DLL\n%s", cfg.tx.aac_encoding ? "FDK-AAC" : "none", aac_encoding ? "yes" : "no");


    /* init DSP for pre-emphasis EQ
       ---------------------------- */
    if (cfg.tx.preemphasis_eq) {
        fprintf(stderr, "\n"); tctx.dsp_out.fsm_mode = TXMOD_PRE;
        dsp_init(&tctx.dsp_out, &cfg, sink.sample_rate);  // runs in 'ask_write_frame'
    }


    /* main stage: build frames
       ------------------------ */
    time_start = clock();  // бенчмарк

    /* write lead-in zero-level symbols */
    num_lead_symbols = acfg.preamble_symbols * 4;  // >= 16x min_amp
    if (cfg.tx.use_lead_symbols) {
        ask_write_lead_symbols(&sink, &tctx.dsp_out, &acfg, num_lead_symbols, 0);
    }

    /* ==== TODO: build and write CALIB frames ==== */
    // ...
    // Iterate per 10-20 frame series set:
    // 3 dB stepping down from -6 to -21 dB, with
    // 1 kHz stepping from 12 to 4 kHz (at 88.2-96 kHz).
    // Then, write $reset_frames * 2 number silence.
    // Use build_kv_string() to write params info.
    // ...

    /* ==== build and write TOC frame ==== */
    {
        time_t file_time = time(NULL);
        uint16_t crc_acc = 0xFFFF; char *p;
        size_t rr; uint8_t file[1024];

        if (is_stream) {
            /* stream default name */
            sprintf(file, "stream.bin");

        } else {
            /* determine file size */
            fseek(in, 0, SEEK_END);
            file_size = (uint32_t)ftell(in);
            fseek(in, 0, SEEK_SET);

            /* compute CRC16 over file, streaming */
            while ((rr = fread(file, 1, sizeof(file), in)) > 0) {
                crc_acc = crc16_update(crc_acc, file, (int)rr);
            }
            file_crc = crc_acc;
            /* rewind for normal reading of payloads */
            fseek(in, 0, SEEK_SET);

            /* remove full path from file name */
            sprintf(file, strstr(in_name,"\\") ? strrchr(in_name,'\\') +1 : in_name);
            sprintf(file, strstr(file,"/") ? strrchr(file,'/') +1 : file);
        }

        if (adts_input || aac_encoding) {
            /* swap file extension for AAC-in */
            if (p = strrchr(file,'.')) *p = '\0';  // strip old
            strcat(file, ".aac");  // append AAC one
        }

        input.type = TX_INPUT_MEM;
        desc.hdr.payload_class = PAYLOAD_TOC;
        input.max_frames = 0;  // unlimited
        ofdm.high_freq = cfg.service.max_freq;  // low-freq zone

        input.kv_template = build_kv_string(
            cfg.service.toc_template, 
            file, file_size, file_time, file_crc
        );
        input.user_file = read_user_file(
            cfg.service.toc_file, &input
        );
        if (!input.kv_template && !input.user_file) {
            fprintf(stderr, "Failed to build TOC data.\n");
            rc = 1; goto cleanup;
        }
        tx_emit_payload_frame(
            &sink, &input, &sstat, &ofdm, &meta, 0, // always mono
            &desc, &desc_r, payload_buf, payload_buf_r,
            &tctx, cfg.service.repeat_count
        );
    }

    /* ==== prepare META frames data ==== */
    if (meta.insert_gaps || meta.insert_frames) {
        /* заполняем KV-шаблон своими значениями */
        input.kv_template = build_kv_string(
            cfg.meta.tag_template, // шаблон
            cfg.meta.tag_number,   cfg.meta.tag_tracks, 0,
            cfg.meta.tag_duration, meta.record_time,
            cfg.meta.tag_year,     cfg.meta.tag_artist,
            cfg.meta.tag_title,    cfg.meta.tag_album,
            cfg.meta.tag_genre,    cfg.meta.tag_comment
        );
        input.user_file = read_user_file(
            cfg.meta.user_file, &input
        );
        if (!input.kv_template && !input.user_file) {
            fprintf(stderr, "\nFailed to build metadata.\n");
            meta.insert_frames = 0;  // skips emit in data cycle
        }

        /* Заполняем поля (для GAP-версии) */
        meta.track_number = (uint8_t)cfg.meta.tag_number;
        meta.total_tracks = (uint8_t)cfg.meta.tag_tracks;
        meta.time_stamp   = 0; // будет обновляться динамически
        meta.duration     = (uint16_t)cfg.meta.tag_duration;
        meta.record_time  = (uint32_t)meta.record_time;
        meta.genre_id     = (uint8_t)find_genre_id(cfg.meta.tag_genre);  // ID из текста
        meta.year_id      = (uint8_t)(cfg.meta.tag_year -1900);

        /* Текстовые поля и их длины (для GAP-версии) */
        strncpy(meta.artist_text,  cfg.meta.tag_artist,  255); meta.artist_len  = strlen(meta.artist_text);
        strncpy(meta.title_text,   cfg.meta.tag_title,   255); meta.title_len   = strlen(meta.title_text);
        strncpy(meta.album_text,   cfg.meta.tag_album,   255); meta.album_len   = strlen(meta.album_text);
        strncpy(meta.comment_text, cfg.meta.tag_comment, 255); meta.comment_len = strlen(meta.comment_text);
    }

    /* ==== build and write DATA frames ==== */
    {
        /* обычный бинарный режим */
        input.type = TX_INPUT_FILE;
        desc.hdr.payload_class = payload_class_from_type(&sstat);
        input.max_frames = 0;  // always unlimited
        ofdm.high_freq = cfg.ofdm.data_high_freq;  // full-freq range

        /* режим записи PCM в AAC */
        if (aac_encoding) {
            input.type = TX_INPUT_PCM;
            desc.hdr.payload_class = PAYLOAD_AUDIO +AUDIO_AAC;
            // обновим длительность потока, если получена
            if (input.aenc.src.duration_sec > 0) {
                meta.duration = (uint16_t)input.aenc.src.duration_sec;
                meta.insert_frames = 0;  // не вставлять META-фреймы
            }
        } else if (file_size && payload_bitrate > 0) {
            // обновим длительность по размеру и примерному битрейту
            if (desc.hdr.payload_class/10*10 == PAYLOAD_AUDIO) {
                meta.duration = (uint16_t)(file_size / payload_bitrate * 8);
                meta.insert_frames = 0;  // аналогично aac_encoding = 1
            }
        }
        /* запуск генерации фреймов */
        tx_emit_payload_frame(
            &sink, &input, &sstat, &ofdm, &meta, ofdm.frame_stereo,
            &desc, &desc_r, payload_buf, payload_buf_r,
            &tctx, 1  // 1 = don't repeat frame
        );
    }

    /* ==== build and write EOF frame ==== */
    {
        /* KV-payload with file_size */
        desc.hdr.payload_class = PAYLOAD_EOF;
        input.type = TX_INPUT_MEM;
        input.max_frames = 1;   // always cut!
        input.user_file = NULL; // always key-value...
        ofdm.high_freq = cfg.service.max_freq;  // low-freq zone

        if (is_stream) 
        file_size = sstat.stream_bytes;

        input.kv_template = build_kv_string(
            cfg.service.eof_template,
            is_stream ? 1 : 0,
            file_size, file_crc
        );
        if (!input.kv_template) {
            fprintf(stderr, "\nFailed to build EOF data.\n");
            rc = 1; goto cleanup;
        }
        tx_emit_payload_frame(
            &sink, &input, &sstat, &ofdm, &meta, 0, // always mono
            &desc, &desc_r, payload_buf, payload_buf_r,
            &tctx, cfg.service.repeat_count
        );
    }

    /* write lead-out zero-level symbols */
    if (cfg.tx.use_lead_symbols) {
        ask_write_lead_symbols(&sink, &tctx.dsp_out, &acfg, num_lead_symbols, 1);
    }


    /* --- show final info, patch header and exit --- */
    time_end = clock(); // остановить таймер
    elapsed_ms = (double)(time_end - time_start) / CLOCKS_PER_SEC;
    dsp_reset(&tctx.dsp_out);
    fprintf(stderr, "\n\nTX finished: %s.\n", strcmp(out_name, "-") ? out_name : "<stream>");
    fprintf(stderr, "Total elapsed time: %.3f seconds\n", elapsed_ms);


cleanup:
    fflush(stderr);
    if (payload_buf) free(payload_buf);
    if (payload_buf_r) free(payload_buf_r);
    if (tctx.bitbuf_L) free(tctx.bitbuf_L);
    if (tctx.bitbuf_R) free(tctx.bitbuf_R);
    if (&tctx.dsp_out) dsp_free(&tctx.dsp_out);
    if (in && !is_stream) fclose(in);
    if (sink.ctx) sink.close(sink.ctx);
    if (input.kv_template) free(input.kv_template);
    if (input.file_mem) free(input.file_mem);
    if (aac_encoding) aac_encoder_free(&input.aenc);
    if (acfg.wave_table) { // двухмерный sine-буфер
        for (step = 0; step < acfg.num_levels + 2; ++step) 
        free(acfg.wave_table[step]); free(acfg.wave_table);
    }

    return rc;  // error code
}


/* -------------------------------------------------------------------
 tx_emit_payload_frame
   Unified emitter: reads from FILE (in) using tx_read_input_data()
   OR from memory buffer (mem, mem_size). Emits frames using external
   ask_write_frame() helper. Returns 0 on success, -1 on failure.
--------------------------------------------------------------------*/
static int tx_emit_payload_frame(
    PcmSink *sink, TxInput *in, StreamState *st, OfdmParams *ofdm, MetadataCtx *meta, int frame_stereo,
    FrameDesc *desc, FrameDesc *desc_r, uint8_t *payload_buf, uint8_t *payload_buf_r, TxBitCtx *tctx, int repeat_count)
{
    TxReadStatus stat;
    int r = 0, hk_stat = 0;
    uint32_t bitbuf_len = tctx->capacity_bits;
    if (repeat_count < 1) repeat_count = 1;

    if (desc->ofdm_encoding) {
        ofdm->active_carriers = calc_active_carriers(ofdm);
        desc->payload_bytes = calc_payload_bytes(ofdm);
    }
    if (desc->payload_bytes <= 0) return -1;
    if (desc_r) memcpy(desc_r, desc, sizeof(*desc));

    /* prepare MEM INPUT once */
    if (in->type == TX_INPUT_MEM) {
        if (read_mem_input(in, desc->payload_bytes) != 0) {
            fprintf(stderr, "Failed to build memory payload.\n"); return -1;
        }
    }

    for (r = 0; r < repeat_count; ++r) {
        /* сбросить фреймовые метрики */
        desc->hdr.frame_number = 0;
        desc->hdr.payload_used = desc_r->hdr.payload_used = 0;
        desc->payload_extra = desc_r->payload_extra = 0;
        if (in->type == TX_INPUT_MEM) in->mem_pos = 0;

        while (1) {
            /* вставка META-фреймов с заданным интервалом */
            if (in->type != TX_INPUT_MEM) {
                tx_try_emit_meta_frame(
                    sink, in, st, ofdm, meta, frame_stereo,
                    desc, desc_r, payload_buf, payload_buf_r,
                    tctx, 1);  // никогда не повторять
            }

            /* чтение байтов из файла, потока или памяти */
            stat = tx_read_input_data(in, st, frame_stereo, payload_buf, payload_buf_r, desc, desc_r);
            if (stat == TX_READ_NO_DATA) break;

            /* проверка нажатия горячих клавиш */
            hk_stat = check_hotkeys(st->is_stream);
            if (hk_stat == 1) return 0;  // отправлен стоп: выходим
            if (hk_stat == 2) continue;  // пауза в потоке: пропуск цикла

            /* build LEFT frame */
            desc->hdr.frame_number += 1;  // обновить счётчик фреймов
            // записываем метаданные в свободный хвост фрейма...
            if (meta->insert_gaps) serialize_metadata(desc, meta, st->adts.stream_time, payload_buf);
            // формируем битовый буфер для записи из данных фрейма
            if (build_frame_data(desc, payload_buf, tctx->bitbuf_L, bitbuf_len) == 0) {
                tx_log_frame_status(desc, tctx, st);  // логируем инфо фрейма

                /* build RIGHT frame */
                if (desc_r->hdr.payload_used > 0) {
                    desc->hdr.frame_number += 1;  // +1 для стерео
                    desc_r->hdr.frame_number = desc->hdr.frame_number;
                    if (meta->insert_gaps) serialize_metadata(desc_r, meta, st->adts.stream_time, payload_buf_r);
                    if (build_frame_data(desc_r, payload_buf_r, tctx->bitbuf_R, bitbuf_len) == 0) {
                        tx_log_frame_status(desc_r, tctx, st);  // логируем правый фрейм
                    } else {
                        fprintf(stderr, "Right frame build failed (number: %u).\n", desc_r->hdr.frame_number); return -1;
                    }
                }
                /* emit frame pair in m-ASK stereo or mono [if forced or R-channel EOF] */
                if (ask_write_frame(sink, &tctx->dsp_out, desc, desc_r, &tctx->ask, tctx->bitbuf_L, tctx->bitbuf_R, bitbuf_len) != 0) {
                    fprintf(stderr, "Writing frames failed (numbers: %u, %u).\n", desc->hdr.frame_number, desc_r->hdr.frame_number); return -1;
                }
            } else {
                fprintf(stderr, "Frame data build failed (number: %u).\n", desc->hdr.frame_number); return -1;
            }

            if (stat == TX_READ_EOF) break;
        }

        /* DATA stream doesn't support repeat */
        if (in->type != TX_INPUT_MEM) break;
    }

    return 0;
}

/* Функция для вставки метаданных в поток */
static void tx_try_emit_meta_frame(
    PcmSink *sink, TxInput *input, StreamState *st, OfdmParams *ofdm, MetadataCtx *meta, int frame_stereo,
    FrameDesc *desc, FrameDesc *desc_r, uint8_t *payload_buf, uint8_t *payload_buf_r, TxBitCtx *tctx, int repeat_count)
{
    /* форсим первый META перед началом DATA */
    if (desc->hdr.frame_number == 0 || meta->insert_frames 
    && (desc->hdr.frame_number % meta->insert_frames) == 0)
    {
        /* наследуем исходные структуры в META */
        TxInput in_meta; StreamState st_meta;
        FrameDesc desc_meta, desc_meta_r; /* TxBitCtx bitctx_meta; */
        memcpy(&in_meta, input, sizeof(*input)); memcpy(&st_meta, st, sizeof(*st));
        memcpy(&desc_meta, desc, sizeof(*desc)); memcpy(&desc_meta_r, desc_r, sizeof(*desc_r));

        /* переключаемся во временный META-контекст */
        desc_meta.hdr.payload_class = PAYLOAD_META;
        in_meta.type = TX_INPUT_MEM;
        in_meta.max_frames = meta->max_frames;

        /* вставляем текущую временную метку */
        {
            /* задаём шаблоны поиска */
            char    kv_buf[16] = { 0 };
            uint8_t pat_ts_kv[] = "TimeStamp=";
            uint8_t pat_ts_bin[] = { 'T','S','T','M' };

            /* получаем промежуток времени с начала старта */
            uint32_t stamp = (uint32_t)(time(NULL) - meta->record_time);

            /* если найден таймер из ADTS-потока, используем его */
            if (st->adts.stream_time > 0) stamp = st->adts.stream_time / 1000000;  // в мкс

            /* задаём длины значения и патчим память */
            sprintf(kv_buf, "%010u", (unsigned)stamp);
            patch_metadata_field(&in_meta, pat_ts_kv, sizeof(pat_ts_kv)-1, kv_buf, 10, pat_ts_bin, sizeof(pat_ts_bin), &stamp, sizeof(stamp));
        }

        if (tx_emit_payload_frame(
            sink, &in_meta, &st_meta, ofdm, meta, frame_stereo,
            &desc_meta, &desc_meta_r, payload_buf, payload_buf_r,
            tctx, repeat_count
        ) != 0) {
            /* META-фрейм сломался (только информируем) */
            fprintf(stderr, "\nFailed to build metadata frame.\n");
        }

        if (st->is_stream && desc->hdr.frame_number == 0) {
            meta->insert_frames = 0; // для стрима исключаем потерю данных
        }
    }
}


/* Хелпер для построения KV-метаданных */
static char *build_kv_string(const char *fmt, ...)
{
    va_list ap;
    int len; char *buf;
    char tmp[4096];  // буфер форматирования

    va_start(ap, fmt);
    len = vsprintf(tmp, fmt, ap);  // формируем строку
    va_end(ap);
    if (len <= 0 || len >= (int)sizeof(tmp))
        return NULL;

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) return NULL;

    memcpy(buf, tmp, len + 1);  // копируем из tmp-буфера
    return buf;  // ВАЖНО: владелец — вызывающий
}

/* Хелпер загрузки мета-файлов в память */
static char *read_user_file(char *user_file, TxInput *in)
{
    FILE *file; long size;
    if (!user_file) return NULL;

    file = fopen(user_file, "rb");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) { fclose(file); return NULL; }

    /* очистка предыдущего */
    if (in->file_mem) {
        free(in->file_mem);
        in->file_mem = NULL;
        in->file_size = 0;
    }
    in->file_mem = (uint8_t*)malloc((size_t)size);
    if (!in->file_mem) { fclose(file); return NULL; }

    in->user_file = user_file;  // post-checks only
    in->file_size = fread(in->file_mem, 1, (size_t)size, file);

    fclose(file); return in->user_file;
}


/* Helper чтения метаданных в память, назначение:
   - выбрать источник (kv_template vs user_file)
   - собрать один линейный MEM payload
   - ограничить по max_frames * payload_bytes
*/
static int read_mem_input(TxInput *in, size_t payload_bytes)
{
    size_t max_bytes = 0;

    /* Приоритет: file > KV */
    if (in->file_mem && in->file_size > 0) {
        in->mem = in->file_mem;
        in->mem_size = in->file_size;
    }
    else if (in->kv_template) {
        in->mem = (uint8_t*)in->kv_template;
        in->mem_size = strlen(in->kv_template);
    }
    else {
        return -1;
    }
    if (in->max_frames > 0) {
        max_bytes = payload_bytes * (size_t)in->max_frames;
        if (in->mem_size > max_bytes) in->mem_size = max_bytes;
    }

    in->mem_pos = 0; return 0;
}

/* Хелперы для патча metadata-полей */
static int patch_metadata_field(TxInput *in,
    const uint8_t *pat_txt, size_t pat_txt_len, const void *val_txt, size_t val_txt_len,
    const uint8_t *pat_bin, size_t pat_bin_len, const void *val_bin, size_t val_bin_len)
{
    if (!in || !in->kv_template) return -1;

    if (in->file_mem && in->file_size > 0) {
        if (!pat_bin || !val_bin) return -1;
        return patch_mem_data(in->file_mem, in->file_size, pat_bin, pat_bin_len, val_bin, val_bin_len);
    } else {
        if (!pat_txt || !val_txt) return -1;
        return patch_mem_data((uint8_t*)in->kv_template, strlen(in->kv_template), pat_txt, pat_txt_len, val_txt, val_txt_len);
    }
}

static int patch_mem_data(uint8_t *mem_data, size_t mem_size, const uint8_t *pattern, size_t pat_len, const void *data, size_t data_len)
{
    size_t i;
    if (!mem_data || !pattern || !data) return -1;
    if (pat_len == 0 || mem_size < pat_len + data_len) return -1;

    for (i = 0; i <= mem_size - pat_len; ++i) {
        if (memcmp(mem_data + i, pattern, pat_len) == 0) {
            /* проверка, что данные влезают */
            if (i + pat_len + data_len > mem_size) return -1;

            memcpy(mem_data + i + pat_len, data, data_len);
            return 0;
        }
    }
    return -1;
}


/* Сборщик данных файла/stdin по пачкам $payload_bytes
   в буфер для выдачи DATA-фрейма в выходной поток/WAV,
   + таймаут для детекции потери приёма входных данных
*/
static TxReadStatus tx_read_input_data(
    TxInput *in, StreamState *st, int frame_stereo, uint8_t *buf_l, uint8_t *buf_r, FrameDesc *desc, FrameDesc *desc_r)
{
    /* для удобства */
    uint16_t *acc_l = &desc->hdr.payload_used;
    uint16_t *acc_r = &desc_r->hdr.payload_used;
    uint32_t *total = &st->stream_bytes;
    uint16_t  max_bytes = desc->payload_bytes;
    int is_adts = (desc->hdr.payload_class == PAYLOAD_AUDIO +AUDIO_AAC);
    *acc_l = *acc_r = desc->payload_extra = desc_r->payload_extra = 0;  // force

    /* ЛЕВЫЙ канал/фрейм */
    if (in->type == TX_INPUT_FILE) {
        *acc_l = !is_adts ? 
        tx_accumulate_bytes(in->file, st, buf_l, max_bytes) :  // бинарные файлы
        tx_accumulate_adts(in->file, &st->adts, buf_l, max_bytes, total);  // готовые AAC
    } else if (in->type == TX_INPUT_PCM) {
        *acc_l =  // чтение PCM-потока с кодированием в AAC
        tx_accumulate_encoded_adts(&in->aenc, &st->adts, buf_l, max_bytes, total);
    } else {
        *acc_l = max_bytes;  // кэшированные метаданные (mem input)
        if (in->mem_pos + *acc_l > in->mem_size) *acc_l = in->mem_size - in->mem_pos;
        memcpy(buf_l, in->mem + in->mem_pos, *acc_l); in->mem_pos += *acc_l;
    }
    if (*acc_l < max_bytes) {
        memset(buf_l + *acc_l, 0, max_bytes - *acc_l);  // заполнить остаток нулями
        if (!(*acc_l)) return TX_READ_NO_DATA;
        if (!is_adts || st->adts.stream_eof) {
            return TX_READ_EOF;  // EOF, если payload неполный или закончился поток
        }
    }

    /* ПРАВЫЙ канал/фрейм */
    if (frame_stereo && desc_r) {
        if (in->type == TX_INPUT_FILE) {
            *acc_r = !is_adts ? 
            tx_accumulate_bytes(in->file, st, buf_r, max_bytes) : 
            tx_accumulate_adts(in->file, &st->adts, buf_r, max_bytes, total);
        } else if (in->type == TX_INPUT_PCM) {
            *acc_r = tx_accumulate_encoded_adts(&in->aenc, &st->adts, buf_r, max_bytes, total);
        } else {
            *acc_r = max_bytes;
            if (in->mem_pos + *acc_r > in->mem_size) *acc_r = in->mem_size - in->mem_pos;
            memcpy(buf_r, in->mem + in->mem_pos, *acc_r); in->mem_pos += *acc_r;
        }
        if (*acc_r < max_bytes) {
            memset(buf_r + *acc_r, 0, max_bytes - *acc_r);
            if (!(*acc_r)) {  // нет правого канала, скопировать левый
                memcpy(buf_r, buf_l, *acc_l); *acc_r = *acc_l;
            }
        }
    }

    /* Определим, есть ли место под метаданные */
    if (frame_stereo && is_adts) {  // TODO: пока только для ADTS...
        if (*acc_r >= *acc_l) desc->payload_extra = desc_r->payload_extra = *acc_r;
        if (*acc_l >= *acc_r) desc->payload_extra = desc_r->payload_extra = *acc_l;
    }

    return TX_READ_OK;
}

/* Только для файлов и stdin! */
static size_t tx_accumulate_bytes(FILE *in, StreamState *st, uint8_t *buf, size_t max_bytes)
{
    uint32_t ready = 0;
    size_t acc = 0, want, got;
    if (!in || !st) return 0;

    while (acc < max_bytes) {
        want = max_bytes - acc;
        got = fread(buf + acc, 1, want, in);
        acc += got; st->stream_bytes += got;

        if (st->is_stream) {
            if (got <= 0) {
                // ждём данных или таймаута
                ready = wait_for_input(in, (int)st->timeout_ms);
                if (ready <= 0) break;  // 0 = таймаут, -1 = ошибка
            }
        } else
            break;  // для файлов просто выходим
    }
    return acc;
}

/* кросс-платформенная версия таймера ожидания */
int wait_for_input(FILE *stream, int timeout_ms)
{
    #ifdef _WIN32
    // Windows: WaitForSingleObject на HANDLE stdin
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (WaitForSingleObject(h, (DWORD)timeout_ms) == WAIT_OBJECT_0) return 1;
    return 0;

    #else
    // Unix/POSIX: poll на дескрипторе
    struct pollfd pfd;
    pfd.fd = fileno(stream);
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeout_ms);

    #endif
}


/* Определение payload_class по типу входного файла */
static int payload_class_from_type(StreamState *st)
{
    const char *file_ext = st->file_ext;
    if (st->is_stream) return st->payload_class;
    else if (!file_ext) goto unknown_file_ext;

    if (!strcasecmp(file_ext, ".aac"))  st->is_stream = 1;  // no-meta
    if (!strcasecmp(file_ext, ".aac"))  return PAYLOAD_AUDIO +AUDIO_AAC;
    if (!strcasecmp(file_ext, ".opus")) return PAYLOAD_AUDIO +AUDIO_OPUS;
    if (!strcasecmp(file_ext, ".ogg"))  return PAYLOAD_AUDIO +AUDIO_OGG;
    if (!strcasecmp(file_ext, ".mp3"))  return PAYLOAD_AUDIO +AUDIO_MP3;
    if (!strcasecmp(file_ext, ".m4a"))  return PAYLOAD_AUDIO +AUDIO_MP4;
    if (!strcasecmp(file_ext, ".mpc"))  return PAYLOAD_AUDIO +AUDIO_MPC;
    if (!strcasecmp(file_ext, ".wav"))  return PAYLOAD_AUDIO +AUDIO_PCM;

    if (!strcasecmp(file_ext, ".png"))  return PAYLOAD_IMAGE +IMAGE_PNG;
    if (!strcasecmp(file_ext, ".jpg"))  return PAYLOAD_IMAGE +IMAGE_JPEG;
    if (!strcasecmp(file_ext, ".jpeg")) return PAYLOAD_IMAGE +IMAGE_JPEG;
    if (!strcasecmp(file_ext, ".bmp"))  return PAYLOAD_IMAGE +IMAGE_BMP;
    if (!strcasecmp(file_ext, ".gif"))  return PAYLOAD_IMAGE +IMAGE_GIF;
    if (!strcasecmp(file_ext, ".webp")) return PAYLOAD_IMAGE +IMAGE_WEBP;
    if (!strcasecmp(file_ext, ".ico"))  return PAYLOAD_IMAGE +IMAGE_ICO;
    if (!strcasecmp(file_ext, ".tga"))  return PAYLOAD_IMAGE +IMAGE_TGA;

    if (!strcasecmp(file_ext, ".ts"))   return PAYLOAD_VIDEO +VIDEO_TS;
    if (!strcasecmp(file_ext, ".mp4"))  return PAYLOAD_VIDEO +VIDEO_MP4;
    if (!strcasecmp(file_ext, ".avi"))  return PAYLOAD_VIDEO +VIDEO_AVI;
    if (!strcasecmp(file_ext, ".mkv"))  return PAYLOAD_VIDEO +VIDEO_MKV;
    if (!strcasecmp(file_ext, ".webm")) return PAYLOAD_VIDEO +VIDEO_WEBM;
    if (!strcasecmp(file_ext, ".mpg"))  return PAYLOAD_VIDEO +VIDEO_MPG;
    if (!strcasecmp(file_ext, ".vob"))  return PAYLOAD_VIDEO +VIDEO_MPG;
    if (!strcasecmp(file_ext, ".wmv"))  return PAYLOAD_VIDEO +VIDEO_WMV;

    unknown_file_ext:  // неизвестное расширение
    return PAYLOAD_DATA +DATA_GENERIC;
}

/* Вывод в консоль сводки по текущему фрейму */
static void inline tx_log_frame_status(FrameDesc* desc, TxBitCtx* tctx, StreamState* st)
{
    /* выбрать тип фрейма */
    size_t data_offset = st->stream_bytes;
    const char *payload_class = "unknown";
    const char *payload_class_str[] = {
        "data", "audio", "image", "video", "toc", "meta", "eof", "calib"
    };
    const char *frame_action = (desc->hdr.payload_class == 10) ? "Encoding" : "Building"; // AAC
    if (desc->hdr.payload_class <= 79) payload_class = payload_class_str[desc->hdr.payload_class/10];
    if (desc->hdr.payload_class >= 40) data_offset = desc->hdr.frame_number * desc->hdr.payload_used;
    if (desc->hdr.payload_class != tctx->payload_previous) fprintf(stderr, "\n"); // line-feed

    /* вывести лог фрейма */
    fprintf(stderr, 
        "\r%s frame #%u (t=%s, d=%u, bs=%u, gm=%u, o=%u, c=%04X+%04X)    ",
        frame_action,
        desc->hdr.frame_number,
        payload_class,
        desc->hdr.payload_used,
        desc->hdr.symbols_bits & 0x0F,
        desc->hdr.symbols_bits & 0x10 ? 1 : 0,
        data_offset,
        desc->crc_msb,
        desc->crc_lsb );

    tctx->payload_previous = desc->hdr.payload_class;
}

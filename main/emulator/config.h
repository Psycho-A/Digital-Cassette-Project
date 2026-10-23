/* config.h */
/* -----------
 * Important:
 * ALL modes and module params MUST be filled here,
 * and [ALL] value normalizations must be performed here.
 * The main code has NO internal value caps and defaults!
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ini.h"
#include "stdint_compat.h"
#include "dsp_config.h"


/* cvar list
   --------- */
typedef struct {
    /* input */
    struct {
        int def_rate;
        int def_channels;
        int def_format;
        char *pcm_backend;
    } input;

    /* output */
    struct {
        int sample_rate;
        int pcm_channels;
        int pcm_format;
        char *pcm_backend;
        int force_rate;
    } output;

    /* ofdm defs */
    struct {
        int qam_order;
        int symbols_per_frame;
        int data_low_freq;
        int data_high_freq;
        int fft_size;
        int prefer_ask;
    } ofdm;

    /* m-ask/pam defs */
    struct {
    // tx-mod...
        int frame_bytes;
        int carrier_freq;
        double low_level_db;
        double high_level_db;
        int cycle_samples;
    // common...
        int preamble_symbols;
        int training_symbols;
        int pilot_interval;
    // rx-demod...
        int agc_percent;
        int agc_smoothing;
        int amp_bad_zone;
        int correct_dynamics;
        int correct_nonlinear;
        int measure_dynamics;
        int workarounds;
    } ask;

    /* TX-modulation */
    struct {
        int aac_encoding;
        int stereo_mode;
        int variable_len;
        int sync_pattern;
        int use_lead_symbols;
        int gray_mapping;
        int preemphasis_eq;
    // pre-emphasis EQ (DSP)
        DspEqConfig eq;
        int eq_bias_profile;
    } tx;

    /* TX-encoder (AAC) */
    struct {
        int format;
        int bitrate;
        int hard_cbr;
        int channels;
        int hq_mode;
    } encoder;

    /* RX-modulation */
    struct {
        int aac_decoding;
        int core_policy;
        int crc_lsb_policy;
        int fill_missing;
        int agc_enabled;
        int reset_state_frames;
        int invert_phase;
        int aac_postproc;
        int preprocess_eq;
    // pre-processing EQ
        DspEqConfig eq;
        double eq_preamp_db;
        double eq_rumble_filter_db;
        int    eq_adaptive_isi;
        int    eq_adaptive_bias;
        int    eq_bias_profile;
        double eq_bias_deadzone;
        double eq_isi_filter_q;
        double eq_isi_upside_pos;
        double eq_isi_deadzone;
        double eq_gain_alpha;
        double eq_gain_max_step_db;
        int    eq_preamp_fine;
        int    eq_dump_samples;
    } rx;

    /* service frames */
    struct {
        int max_freq;
        int repeat_count;
        char *toc_template;
        char *toc_file;
        char *eof_template;
    } service;

    /* metadata frames */
    struct {
        int insert_gaps;
        int insert_frames;
        int max_frames;
        char *user_file;
        char *tag_template;
        int   tag_number;
        int   tag_tracks;
        int   tag_duration;
        int   tag_year;
        char *tag_artist;
        char *tag_title;
        char *tag_album;
        char *tag_genre;
        char *tag_comment;
    // Win32 GUI window
        int metadata_window;
        char *ui_font_file;
        char *ui_font_name;
        int   ui_font_size;
        int   ui_font_bold;
    } meta;

    /* stream backends */
    struct {
        int timeout_ms;
        int chunk_samples;
        int payload_class;
        int fadeout_ms;
    } stream;

    /* WinMM audio-backend */
    struct {
        char *source_device;
        int source_delay;
        char *sink_device;
        int sink_delay;
        int buffer_count;
        int buffer_mul;
        int list_devices;
    } winmm;

    /* RX-analog (DSP) */
    struct {
        int fadein_ms;
        int enable_dsp;
        int tape_correction;
        int downmix_mode;
    } analog_rx;

    /* TX-analog (DSP) */
    struct {
        int fadein_ms;
        int enable_dsp;
        int tape_correction;
        int downmix_mode;
    } analog_tx;

    /* DSP module configs (TX/RX) */
    DspDenoiseConfig dechirp_ad, denoise_rx, denoise_tx;
    DspWidthConfig width_ad, width_rx;
    DspEqConfig eq_ad, eq_rx, eq_tx;
    DspAgcConfig agc_ad, agc_rx, agc_tx;
    DspLimiterConfig limiter_ad, limiter_rx, limiter_tx;

} AppConfig;


/* global settings array */
extern AppConfig cfg;     // visible to all functions
int g_logging; FILE *log_fp;  // fprintf logs, output


/* helper: validate PCM bits */
static int is_valid_format(int fmt, int warn)
{
    switch (fmt) {
        case 8:
        case 16:
        case 24:
        case 32:
        case 64:  // float fmt, not bits
            return 1;
        default:
            if (warn > 0) {
                fprintf(stderr, "Unsupported PCM %s bits: %d.\n",
                (warn == 1) ? "input" : "output", fmt );
            }
            return 0;
    }
}

/* helper: parse EQ filter string */
static void parse_eq_band(char *section, char *str, int *freq, double *gain, double *Q, int *order)
{
    int orig_order = 0;
    *freq = *order = 0; *gain = *Q = 0;
    if (!str || !*str) return;
    sscanf(str, "%d, %lf, %lf, %d", freq, gain, Q, order);

    // простейшие пределы (опционально)
    if (*freq < 10) *freq = 10; if (*freq > 20000) *freq = 20000;
    if (*gain < -40.0) *gain = -40.0; if (*gain > 40.0) *gain = 40.0;
    if (*Q <= 0.0) *Q = default_filter_q;  // не задан в строке
    if (*Q < 0.05) *Q = 0.05; if (*Q > 10.0) *Q = 10.0;
    if (*order < 2) *order = 2; if (*order > 16) *order = 16;

    // округление порядка до степени двойки
    if (*order & (*order -1)) {  // не степень двойки!
        int low = 2, high = 0; orig_order = *order;
        while (low * 2 <= *order) low <<= 1; high = low << 1;
        *order = (*order - low < high - *order) ? low : high;
        fprintf(stderr, "%s: 'low_shelf_order' value %d is not power-of-two, rounding to %d.\n", section, orig_order, *order);
    }
}


/* config loader
   ------------- */
static int read_config(AppConfig *cfg, const char *path, int argc, char** argv)
{
    int cfg_loaded = cfg_load(path);
    if (cfg_loaded == 0)
        fprintf(stderr, "Reading '%s'...\n", path); else 
        fprintf(stderr, "No %s, using defaults.\n", path);

    /* debug logs */
    g_logging = cfg_read_bool("stream", "debug_logging", 1);

    /* audio channel */
    cfg->input.def_rate = cfg_read_int("input", "pcm_rate", 44100, 8000,192000);
    cfg->input.def_channels = cfg_read_int("input", "pcm_channels", 2, 1,2);
    cfg->input.def_format = cfg_read_even("input", "pcm_bits", 16, 8,64);
    cfg->input.pcm_backend = cfg_get_string("input", "audio_backend", "wav");    // wav, winmm

    cfg->output.sample_rate = cfg_read_int("output", "pcm_rate", 44100, 8000,192000);
    cfg->output.pcm_channels = cfg_read_int("output", "pcm_channels", 2, 1,2);
    cfg->output.pcm_format = cfg_read_even("output", "pcm_bits", 16, 8,64);
    cfg->output.pcm_backend = cfg_get_string("output", "audio_backend", "wav");  // wav, winmm
    cfg->output.force_rate = cfg_read_bool("output", "force_rate", 0);

    /* modulation */
    cfg->ofdm.qam_order = cfg_read_pow2("ofdm", "qam_order", 4, 2,256);                                            // уровни/фазы (4 уровня для M-ASK)
    cfg->ofdm.symbols_per_frame = cfg_read_pow2("ofdm", "symbols_per_frame", 8, 2,16);                             // OFDM-символы (не M-ASK периоды)
    cfg->ofdm.data_high_freq = cfg_read_int("ofdm", "high_freq_hz", 11000, 2000, cfg->output.sample_rate/2-2000);  // F последней активной поднесущей
    cfg->ofdm.data_low_freq = cfg_read_int("ofdm", "low_freq_hz", 1000, 500, cfg->ofdm.data_high_freq-2000);       // F нижней SC (мин. ширина 2 кГц)
    cfg->ofdm.fft_size = cfg_read_pow2("ofdm", "fft_size", 1024, 256,4096);                                        // определяет шаг поднесущих (на Fs)
    cfg->ofdm.prefer_ask = cfg_read_bool("ofdm", "prefer_ask", 1);                                                 // отключает MSB/LSB-кодирование, sync-word и OFDM-расчёт payload

    cfg->ask.preamble_symbols = cfg_read_even("m-ask", "preamble_symbols", 4, 2,100);     // кол-во символов в НЧ-маркере преамбулы (мин. 2 для стойкости к ISI)
    cfg->ask.training_symbols = cfg_read_even("m-ask", "training_symbols", 16, 16, 200);  // доп. символы преамбулы для тренировки битовых порогов (пара min/max)
    cfg->ask.pilot_interval = cfg_read_even("m-ask", "pilot_interval", 32, 16, 200);      // через сколько символов с данными вставлять 1 синхро-импульс

    /* TX modulation */
    cfg->tx.aac_encoding = cfg_read_bool("tx_modulate", "encode_audio", 1);           // encode input audio with FDK-AAC (PcmSource input)
    cfg->tx.stereo_mode = cfg_read_bool("tx_modulate", "stereo_mode", 1);             // записывать 2 параллельных фрейма по-канально
    cfg->tx.variable_len = cfg_read_int("tx_modulate", "variable_length", -1, -1,1);  // обрезать фреймы по payload_used вместо capacity_bits (-1 = вкл. для AAC)
    cfg->tx.sync_pattern = cfg_read_int("tx_modulate", "preamble_pattern", 0, 0,1);   // паттерн преамбулы (напр. PREAMBLE_PATTERN_ALT) - не используется
    cfg->tx.use_lead_symbols = cfg_read_bool("tx_modulate", "use_lead_symbols", 1);   // записывать серию lead-in/lead-out периодов вокруг файла (улучшение синхронизации)
    cfg->tx.gray_mapping = cfg_read_bool("tx_modulate", "gray_mapping", 0);           // использовать код Грея для оптимизации битовых ошибок (перемежение символов)
    cfg->tx.preemphasis_eq = cfg_read_bool("tx_modulate", "pre-emphasis_eq", 0);      // эквалайзер и gain для предварительной коррекции сигнала (ниже)

    /* TX (ASK-specific) */
    cfg->ask.frame_bytes = cfg_read_int("tx_modulate_ask", "frame_bytes", -1, -1,4096);                                    // переопределяет ёмкость из OFDM (-1 = расчёт по AAC-фреймам)
    cfg->ask.carrier_freq = cfg_read_int("tx_modulate_ask", "carrier_freq", -1, -1,16000);                                 // переопределяет несущую частоту (и изменяет output.sample_rate)
    cfg->ask.high_level_db = cfg_read_float("tx_modulate_ask", "high_level_db", -6.0, -45.0, -3.0);                        // min -3 dB from max level (0 dB)
    cfg->ask.low_level_db = cfg_read_float("tx_modulate_ask", "low_level_db",  -15.0, -48.0, cfg->ask.high_level_db-3.0);  // min -3 dB from high level
    cfg->ask.cycle_samples = cfg_read_even("tx_modulate_ask", "samples_per_cycle", 4, 4,20);                               // 4 = 1/2 Fs, 6 = 1/3 Fs и т.д. (+при carrier_freq влияет на sample_rate)

    /* TX pre-emphasis EQ */
    cfg->tx.eq.enabled = cfg->tx.preemphasis_eq ? 1 : 0;  // требуется для DSP-хоста
    cfg->tx.eq.preamp_db = cfg_read_float("tx_preemph_eq", "record_level", -12.0, -20.0,0.0);              // ослабление уровня записи (до допустимого MOL ленты)
    cfg->tx.eq.low_shelf_gain_db  = cfg_read_float("tx_preemph_eq", "rumble_preamp_db",   9.0, 0.0,12.0);  // предусиление полосы ниже 400 Гц (защита НЧ от грязи)
    cfg->tx.eq.high_shelf_gain_db = cfg_read_float("tx_preemph_eq", "treble_preamp_db",  12.0, 0.0,20.0);  // предусиление полосы выше 19 кГц (защита ВЧ от спада в шум)
    cfg->tx.eq.band_peak_gain_db  = cfg_read_float("tx_preemph_eq", "bias_correction_db", 0.0, 0.0, 0.0);  // коррекция bias-кривой ленты (заглушка - см. нормализацию)
    cfg->tx.eq_bias_profile = cfg_read_int("tx_preemph_eq", "bias_correction", 0, 0,3);                    // профиль bias-кривой ленты по её Type (0 = нет, 3 = запись Cr как Fe)

    /* TX audio-encoder */
    cfg->encoder.format = cfg_read_int("tx_audiocoder", "codec_format", -1, -1,2);     // -1 = авто, 0 = LC-AAC, 1 = HE-AACv1, 2 = HE-AACv2
    cfg->encoder.bitrate = cfg_read_int("tx_audiocoder", "bitrate_kbps", -1, -1,256);  // -1 = авто-подсчёт из параметров модуляции
    cfg->encoder.channels = cfg_read_int("tx_audiocoder", "audio_channels", 2, 1,2);   // 1 = микшировать в моно (для низких битрейтов)
    cfg->encoder.hard_cbr = cfg_read_bool("tx_audiocoder", "use_hard_cbr", 0);         // ограничить пиковый битрейт целевым
    cfg->encoder.hq_mode = cfg_read_bool("tx_audiocoder", "high_quality", 1);          // улучшение "afterburner" (медленнее, но чётче ВЧ)

    /* RX demodulation */
    cfg->rx.aac_decoding = cfg_read_bool("rx_demod", "decode_audio", 1);                      // decode AAC-audio with FAAD2 (PcmSink output)
    cfg->rx.core_policy = cfg_read_int("rx_demod", "core_policy", 0, 0,3);                    // pass/null/repeat/skip
    cfg->rx.crc_lsb_policy = cfg_read_int("rx_demod", "crc_lsb_policy", 0, 0,3);              // то же, но только если MSB валидны
    cfg->rx.fill_missing = cfg_read_bool("rx_demod", "fill_missing", 1);                      // вставлять пропущенные DATA-фреймы (если не прочитались)
    cfg->rx.agc_enabled = cfg_read_bool("rx_demod", "enable_agc", 1);                         // АРУ для подстройки порогов (по уровню преамбулы)
    cfg->rx.reset_state_frames = cfg_read_int("rx_demod", "reset_state_frames", 20, 0,1000);  // через сколько битых фреймов сбросить состояния АРУ и EQ
    cfg->rx.invert_phase = cfg_read_int("rx_demod", "invert_phase", 0, -1,1);                 // детектировать по нисходящему zero-cross (-1 = авто-обнаружение)
    cfg->rx.aac_postproc = cfg_read_bool("rx_demod", "enhance_aac", 0);                       // in AAC decoding, enable DSP chain before PcmSink output
    cfg->rx.preprocess_eq = cfg_read_bool("rx_demod", "preprocess_eq", 0);                    // эквалайзер для предварительной коррекции АЧХ и ISI (ниже)

    /* RX (ASK-specific) */
    cfg->ask.agc_percent = cfg_read_int("rx_demod_ask", "agc_percent", -1, -1,150);           // ручная поправка gain_mul в АРУ (отладка измерений при ошибках, -1 = авто)
    cfg->ask.agc_smoothing = cfg_read_int("rx_demod_ask", "agc_smoothing", -1, -1,100);       // процентный вес среднего gain_mul к текущему (-1 = авто по состоянию канала)
    cfg->ask.amp_bad_zone = cfg_read_int("rx_demod_ask", "level_bad_zone", 0, 0,25);          // процентный отступ от нестабильных краёв порога символа (в дрейф-коррекции)
    cfg->ask.correct_dynamics = cfg_read_int("rx_demod_ask", "correct_dynamics", 0, 0,100);   // коррекция порогов уровней в % с учётом сжатия ДД внутри данных (0 = нет)
    cfg->ask.correct_nonlinear = cfg_read_bool("rx_demod_ask", "correct_mid_levels", 0);      // коррекция нелинейности промежуточных уровней 2 и 3 (только для 4-ASK)
    cfg->ask.measure_dynamics = cfg_read_bool("rx_demod_ask", "measure_dynamics", 1);         // замер динамического диапазона записи в % от эталонного (по трен. символам)
    cfg->ask.workarounds = cfg_read_bool("rx_demod_ask", "temp_workarounds", 1);              // временные "костыли" и хаки улучшений при разных параметрах сигнала

    /* RX de-emphasis/ISI-adaptive EQ */
    cfg->rx.eq_preamp_db = cfg_read_float("rx_demod_eq", "level_preamp", 3.0, -6.0,20.0);                // восстановление уровня сигнала (после MOL ленты)
    cfg->rx.eq_adaptive_bias = cfg_read_int("rx_demod_eq", "adaptive_bias", 1, 0,2);                     // адаптивная коррекция bias-кривой под текущую ленту (по sync_amp/max_amp_meas)
    cfg->rx.eq_adaptive_isi = cfg_read_bool("rx_demod_eq", "adaptive_dering", 1);                        // адаптивное подавление ISI-звона (по замерам training-символов, 2/3 = bias-режим)
    cfg->rx.eq_rumble_filter_db = cfg_read_float("rx_demod_eq", "rumble_filter_db", -9.0, -20.0,0.0);    // сила подавления НЧ-предыскажения low-shelf фильтром (F=315 Hz, Q=0.7071)
    cfg->rx.eq_bias_profile = cfg_read_int("rx_demod_eq", "bias_tape_profile", 0, 0,4);                  // de-bias: профиль bias-кривой ленты по её Type (0 = нет, 3 = запись Cr как Fe)
    cfg->rx.eq_bias_deadzone = cfg_read_float("rx_demod_eq", "bias_deadzone", 0.45, -1.0,1.00);          // de-bias: толерантность коррекции к изменениям АЧХ ("мёртвая зона", -1 = авто)
    cfg->rx.eq_isi_filter_q = cfg_read_float("rx_demod_eq", "dering_filter_q", 0.30, 0.05,1.00);         // de-ring: добротность peak-фильтра (не рекомендуется шире 0.25-0.50)
    cfg->rx.eq_isi_upside_pos = cfg_read_float("rx_demod_eq", "dering_sideband_pos", 1.50, 1.0,1.85);    // de-ring: позиция peak-фильтра, 1.5 = центр ВБП (16537 Hz при несущей 11025)
    cfg->rx.eq_isi_deadzone = cfg_read_float("rx_demod_eq", "dering_deadzone", 0.10, -1.0,1.00);         // de-ring: толерантность к звону при коррекции (для плохих трактов, -1 = авто)
    cfg->rx.eq_gain_max_step_db = cfg_read_float("rx_demod_eq", "gain_max_step_db", 3.0, 1.0,12.0);      // максимальный лимит при применении начального усиления bias/ISI (в авто-режиме)
    cfg->rx.eq_gain_alpha = cfg_read_float("rx_demod_eq", "auto_gain_smooth", 0.25, 0.0,0.99);           // весовой коэффициент среднего при сглаживании gains (0 = без сглаживания)
    cfg->rx.eq_preamp_fine = cfg_read_int("rx_demod_eq", "accurate_preamp", 1, 0,1);                     // режим предусиления после фильтров: упрощённый и точный (с учётом biquad-кривых)
    cfg->rx.eq_dump_samples = cfg_read_bool("rx_demod_eq", "dump_raw_samples", 0);                       // дамп обработанных эквалайзером сэмплов в raw PCM-файл (raw_samples.pcm)

    /* service frames setup */
    cfg->service.max_freq = cfg_read_int("service", "max_freq_hz", 5000, 2000,cfg->ofdm.data_high_freq);  // не выше data_high_freq, переопределяет размер payload_bytes
    cfg->service.repeat_count = cfg_read_int("service", "repeat_count", 5, 2,10);                         // сколько раз повторить TOC/EOF-фрейм (мин. 2 для корректного сброса сессии)
    cfg->service.toc_template = cfg_get_string("service", "toc_kv_template", "NAME=%s\nSIZE=%u\nTIME=%u\nCRC=%04X\n");
    cfg->service.toc_file = cfg_get_string("service", "toc_user_file", NULL);
    cfg->service.eof_template = cfg_get_string("service", "eof_kv_template", "EOF=1\nSTREAM=%i\nSIZE=%u\nCRC=%04X\n");

    /* metadata setup and fields */
    cfg->meta.insert_gaps = cfg_read_bool("metadata", "insert_gaps", 1);                   // вставка в пустые зазоры в конце фреймов
    cfg->meta.insert_frames = cfg_read_int("metadata", "insert_frames", 50, 0,1000);       // вставка отдельных фреймов, число = интервал
    cfg->meta.max_frames = cfg_read_int("metadata", "max_frames", 1, 0,100);               // макс. фреймов за раз, 0 = неограниченно
    cfg->meta.user_file = cfg_get_string("metadata", "meta_user_file", NULL);              // например, ID3-blob
    cfg->meta.tag_template = cfg_get_string("metadata", "kv_tag_template", "TrackNumber=%02u\nTotalTracks=%02u\nTimeStamp=%010u\nDuration=%010u\nRecordTime=%010u\nYear=%u\nArtist=%s\nTitle=%s\nAlbum=%s\nGenre=%s\nComment=%s\n");

    cfg->meta.tag_number = cfg_read_int("metadata", "kv_tag_number", 1, 1,255);
    cfg->meta.tag_tracks = cfg_read_int("metadata", "kv_tag_tracks", 1, 1,255);
    cfg->meta.tag_duration = cfg_read_int("metadata", "kv_tag_duration", 1800, 0,16777215);  // в секундах
    cfg->meta.tag_year = cfg_read_int("metadata", "kv_tag_year", 2026, 1900,2155);           // 1900 + uint8_t max
    cfg->meta.tag_artist = cfg_get_string("metadata", "kv_tag_artist", "TX Artist");
    cfg->meta.tag_title = cfg_get_string("metadata", "kv_tag_title", "TX Title");
    cfg->meta.tag_album = cfg_get_string("metadata", "kv_tag_album", "TX Album");
    cfg->meta.tag_genre = cfg_get_string("metadata", "kv_tag_genre", "TX Genre");            // конвертируется в табличный ID
    cfg->meta.tag_comment = cfg_get_string("metadata", "kv_tag_comment", "TX Comment");

    cfg->meta.metadata_window = cfg_read_bool("metadata_ui", "enable_window", 1);            // вывод метаданных в отдельное окно (в потоковом режиме)
    cfg->meta.ui_font_file = cfg_get_string("metadata_ui", "window_font_file", NULL);        // кастомный файл TTF-шрифта для GUI-окна
    cfg->meta.ui_font_name = cfg_get_string("metadata_ui", "window_font_name", "Consolas");  // имя гарнитуры шрифта (кастомного или системного - нужно)
    cfg->meta.ui_font_size = cfg_read_int("metadata_ui", "window_font_size", 14, 8,72);      // размер шрифта в окне
    cfg->meta.ui_font_bold = cfg_read_int("metadata_ui", "window_font_bold", 5, 0,9);        // жирность шрифта (нормализуется в CreateFont() как x100)

    /*--------------------*\
    |  PCM Audio Backends  |
    \*--------------------*/
    {
        /* Input stream mode (generic params) */
        cfg->stream.timeout_ms = cfg_read_int("stream", "timeout_ms", 500, -1,15000);      // таймаут потери входного потока (в мс)
        cfg->stream.chunk_samples = cfg_read_int("stream", "chunk_samples", -1, -1,8192);  // переопределить число сэмплов в буфере чтения (иначе по AAC-профилю)
        cfg->stream.payload_class = cfg_read_int("stream", "payload_class", 10, 0,79);     // PAYLOAD_AUDIO + AUDIO_AAC
        cfg->stream.fadeout_ms = cfg_read_int("stream", "fadeout_ms", 500, 0,3000);        // затухание при переключении режимов (пока не реализовано)

        /* WinMM (waveIn/waveOut : Win2000-Win10 only) */
        cfg->winmm.source_device = cfg_get_string("pcm_winmm", "source_device", 0);         // числовой индекс или часть имени устройства ввода (-1/0 = "default")
        cfg->winmm.sink_device = cfg_get_string("pcm_winmm", "sink_device", 0);             // числовой индекс или часть имени устройства вывода (-1/0 = "default")
        cfg->winmm.source_delay = cfg_read_even("pcm_winmm", "source_buffering", 4, 2,10);  // множитель размера для кольцевого буфера (фикс пропусков при 1 потоке)
        cfg->winmm.sink_delay = cfg_read_even("pcm_winmm", "sink_buffering", 4, 0,10);      // кол-во чанков для начальной задержки вывода (фикс раскачки при 1 потоке)
        cfg->winmm.buffer_count = cfg_read_int("pcm_winmm", "buffer_count", 4, 2,8);        // количество буферов (2-8), для сквозного тракта нужно не меньше 4
        cfg->winmm.buffer_mul = cfg_read_int("pcm_winmm", "buffer_multiplier", 1, 1,4);     // множитель кол-ва фреймов буфера (сглаживание кликов, рост задержки)
        cfg->winmm.list_devices = cfg_read_bool("pcm_winmm", "list_devices", 0);            // выводит список доступных устройств (индекс + название)
    }

    /*------------------------------------*\
    |  ANALOG & DIGITAL RX/TX DSP modules  |
    \*------------------------------------*/
    {
        char *config_dsp = "config_dsp.ini"; // same dir
        int cfg_dsp_loaded = cfg_load(config_dsp);
        if (cfg_dsp_loaded == 0)
            fprintf(stderr, "Reading '%s'...\n", config_dsp); else 
            fprintf(stderr, "No %s, using defaults.\n", config_dsp);

        /* analog mode with DSP (RX/TX) */
        cfg->analog_rx.fadein_ms = cfg_read_int("analog.rx", "fadein_ms", 500, 0,3000);
        cfg->analog_tx.fadein_ms = cfg_read_int("analog.tx", "fadein_ms", 250, 0,3000);
        cfg->analog_rx.enable_dsp = cfg_read_bool("analog.rx", "enable_dsp", 1);
        cfg->analog_tx.enable_dsp = cfg_read_bool("analog.tx", "enable_dsp", 1);
        cfg->analog_rx.downmix_mode = cfg_read_int("analog.rx", "downmix_to_mono", 0, 0,2);
        cfg->analog_tx.downmix_mode = cfg_read_int("analog.tx", "downmix_to_mono", 0, 0,2);
        cfg->analog_rx.tape_correction = cfg_read_int("analog.rx", "tape_correction", 0, 0,5);  // профили коррекции ленты (см. ниже)
        cfg->analog_tx.tape_correction = cfg_read_int("analog.tx", "tape_correction", 0, 0,5);  // аналогично для TX

        /* DSP DeNoise/DeChirp+Compander (RX/TX) */
        cfg->dechirp_ad.enabled = cfg_read_bool("dsp_dechirp.ad", "enabled", 0);
        cfg->denoise_rx.enabled = cfg_read_bool("dsp_denoise.rx", "enabled", 0);
        cfg->denoise_tx.enabled = cfg_read_bool("dsp_denoise.tx", "enabled", 0);
        cfg->dechirp_ad.cutoff_freq_hz = cfg_read_int("dsp_dechirp.ad", "lower_freq_hz", 7000, 0,20000);
        cfg->denoise_rx.cutoff_freq_hz = cfg_read_int("dsp_denoise.rx", "lower_freq_hz", 8000, 0,20000);
        cfg->denoise_tx.cutoff_freq_hz = cfg_read_int("dsp_denoise.tx", "lower_freq_hz", 8000, 0,20000);
        cfg->dechirp_ad.reduction_level_db = cfg_read_float("dsp_dechirp.ad", "reduction_level_db",  -3.0, -24.0, 0.0);
        cfg->denoise_rx.reduction_level_db = cfg_read_float("dsp_denoise.rx", "reduction_level_db", -20.0, -60.0,60.0);
        cfg->denoise_tx.reduction_level_db = cfg_read_float("dsp_denoise.tx", "reduction_level_db", -20.0, -60.0,60.0);  // inverted in compander_mode
        cfg->dechirp_ad.noise_threshold_db = cfg_read_float("dsp_dechirp.ad", "chirp_threshold_db", -32.0, -48.0,-3.0);
        cfg->denoise_rx.noise_threshold_db = cfg_read_float("dsp_denoise.rx", "noise_threshold_db", -48.0, -80.0,-6.0);
        cfg->denoise_tx.noise_threshold_db = cfg_read_float("dsp_denoise.tx", "noise_threshold_db", -48.0, -80.0,-6.0);
        cfg->dechirp_ad.attack_ms = cfg_read_float("dsp_dechirp.ad", "attack_ms", 2.0, 0.0,200.0);
        cfg->denoise_rx.attack_ms = cfg_read_float("dsp_denoise.rx", "attack_ms", 5.0, 0.0,500.0);
        cfg->denoise_tx.attack_ms = cfg_read_float("dsp_denoise.tx", "attack_ms", 5.0, 0.0,500.0);
        cfg->dechirp_ad.release_ms = cfg_read_float("dsp_dechirp.ad", "release_ms", 50.0, 0.0, 500.0);
        cfg->denoise_rx.release_ms = cfg_read_float("dsp_denoise.rx", "release_ms", 50.0, 0.0,1000.0);
        cfg->denoise_tx.release_ms = cfg_read_float("dsp_denoise.tx", "release_ms", 50.0, 0.0,1000.0);
        cfg->dechirp_ad.knee_width_db = cfg_read_float("dsp_dechirp.ad", "knee_width_db", 6.0, 0.0,20.0);
        cfg->denoise_rx.knee_width_db = cfg_read_float("dsp_denoise.rx", "knee_width_db", 6.0, 0.0,20.0);
        cfg->denoise_tx.knee_width_db = cfg_read_float("dsp_denoise.tx", "knee_width_db", 6.0, 0.0,20.0);
        cfg->dechirp_ad.enhance_peaks_db = cfg_read_float("dsp_dechirp.ad", "enhance_peaks_db", 3.0,   0.0,12.0);
        cfg->denoise_rx.enhance_peaks_db = cfg_read_float("dsp_denoise.rx", "enhance_peaks_db", 3.0, -20.0,20.0);
        cfg->denoise_tx.enhance_peaks_db = cfg_read_float("dsp_denoise.tx", "enhance_peaks_db", 3.0, -20.0,20.0);  // inverted in compander_mode
        cfg->dechirp_ad.peaks_floor_db = cfg_read_float("dsp_dechirp.ad", "peaks_floor_db", -24.0, cfg->denoise_rx.noise_threshold_db+cfg->denoise_rx.knee_width_db, -3.0);  // safe gap
        cfg->denoise_rx.peaks_floor_db = cfg_read_float("dsp_denoise.rx", "peaks_floor_db", -20.0, cfg->denoise_rx.noise_threshold_db+cfg->denoise_rx.knee_width_db, -3.0);
        cfg->denoise_tx.peaks_floor_db = cfg_read_float("dsp_denoise.tx", "peaks_floor_db", -20.0, cfg->denoise_tx.noise_threshold_db+cfg->denoise_tx.knee_width_db, -3.0);
        cfg->denoise_rx.filter_q = cfg_read_float("dsp_denoise.rx", "filter_q", default_filter_q, 0.05,2.0);
        cfg->denoise_tx.filter_q = cfg_read_float("dsp_denoise.tx", "filter_q", default_filter_q, 0.05,2.0);
        cfg->denoise_rx.filter_order = cfg_read_pow2("dsp_denoise.rx", "filter_order", 4, 2,16);
        cfg->denoise_tx.filter_order = cfg_read_pow2("dsp_denoise.tx", "filter_order", 4, 2,16);
        cfg->denoise_rx.rms_window_cycles = cfg_read_int("dsp_denoise.rx", "rms_window_cycles", 4, 0, cfg->denoise_rx.cutoff_freq_hz/2+4);  // max 500 ms for cutoff_freq (0 = use attack_ms/4)
        cfg->denoise_tx.rms_window_cycles = cfg_read_int("dsp_denoise.tx", "rms_window_cycles", 4, 0, cfg->denoise_tx.cutoff_freq_hz/2+4);
        cfg->denoise_rx.rumble_cutoff_hz = cfg_read_int("dsp_denoise.rx", "rumble_cutoff_hz", 30, 5,5000);
        cfg->denoise_tx.rumble_cutoff_hz = cfg_read_int("dsp_denoise.tx", "rumble_cutoff_hz", 30, 5,5000);  // unused in compander
        cfg->denoise_rx.rumble_filter_order = cfg_read_pow2("dsp_denoise.rx", "rumble_filter_order", 4, 0,16);
        cfg->denoise_tx.rumble_filter_order = cfg_read_pow2("dsp_denoise.tx", "rumble_filter_order", 0, 0,16);  // unused in compander
        cfg->denoise_rx.linked_stereo = cfg_read_bool("dsp_denoise.rx", "linked_stereo", 0);  // 1.5x speed-up
        cfg->denoise_tx.linked_stereo = cfg_read_bool("dsp_denoise.tx", "linked_stereo", 0);
        cfg->denoise_rx.compander_mode = cfg_read_bool("dsp_denoise.rx", "compander_mode", 0);
        cfg->denoise_tx.compander_mode = cfg_read_bool("dsp_denoise.tx", "compander_mode", 0);  // inverts reduction and enhance
        cfg->dechirp_ad.filter_q = default_filter_q;  // forced in aac-decoder mode
        cfg->dechirp_ad.filter_order = 2;             // forced in aac-decoder mode
        cfg->dechirp_ad.rms_window_cycles = 8;        // forced in aac-decoder mode
        cfg->dechirp_ad.rumble_cutoff_hz = 30;        // forced in aac-decoder mode
        cfg->dechirp_ad.rumble_filter_order = 0;      // forced in aac-decoder mode
        cfg->dechirp_ad.linked_stereo = 1;            // forced in aac-decoder mode
        cfg->dechirp_ad.compander_mode = 0;           // forced in aac-decoder mode

        /* DSP Stereo (width, RX-only) */
        cfg->width_ad.enabled = cfg_read_bool("dsp_width.ad", "enabled", 1);
        cfg->width_rx.enabled = cfg_read_bool("dsp_width.rx", "enabled", 0);
        cfg->width_ad.width_percent = cfg_read_int("dsp_width.ad", "width_percent", 115, 0,300);
        cfg->width_rx.width_percent = cfg_read_int("dsp_width.rx", "width_percent", 125, 0,300);
        cfg->width_rx.hpf_enable = cfg_read_bool("dsp_width.rx", "hpf_enable", 0);
        cfg->width_rx.hpf_cutoff_hz = cfg_read_float("dsp_width.rx", "hpf_cutoff_hz", 125, 20,10000);
        cfg->width_ad.lpf_enable = cfg_read_bool("dsp_width.ad", "lpf_enable", 1);
        cfg->width_rx.lpf_enable = cfg_read_bool("dsp_width.rx", "lpf_enable", 0);
        cfg->width_ad.lpf_cutoff_hz = cfg_read_float("dsp_width.ad", "lpf_cutoff_hz", 12000, 200,20000);
        cfg->width_rx.lpf_cutoff_hz = cfg_read_float("dsp_width.rx", "lpf_cutoff_hz", 12500, 200,20000);
        cfg->width_ad.hpf_enable = 0;                 // forced in aac-decoder mode
        cfg->width_ad.hpf_cutoff_hz = 100;            // forced in aac-decoder mode

        /* DSP EQ (tembre) */
        cfg->eq_ad.enabled = cfg_read_bool("dsp_eq.ad", "enabled", 1);
        cfg->eq_rx.enabled = cfg_read_bool("dsp_eq.rx", "enabled", 1);
        cfg->eq_tx.enabled = cfg_read_bool("dsp_eq.tx", "enabled", 0);
        cfg->eq_ad.preamp_db = cfg_read_float("dsp_eq.ad", "preamp_db", 0.0, -10.0,10.0);
        cfg->eq_rx.preamp_db = cfg_read_float("dsp_eq.rx", "preamp_db", 0.0, -20.0,20.0);
        cfg->eq_tx.preamp_db = cfg_read_float("dsp_eq.tx", "preamp_db", 0.0, -20.0,20.0);
        cfg->eq_ad.low_shelf  = cfg_get_string("dsp_eq.ad", "low_shelf",  "40, 3.0, 0.7071, 2");
        cfg->eq_rx.low_shelf  = cfg_get_string("dsp_eq.rx", "low_shelf",  "40, 6.0, 0.7071, 2");
        cfg->eq_tx.low_shelf  = cfg_get_string("dsp_eq.tx", "low_shelf",  "40, 6.0, 0.7071, 2");
        cfg->eq_ad.band_peak  = cfg_get_string("dsp_eq.ad", "band_peak",  "2000, 0.0, 1.00, 2");
        cfg->eq_rx.band_peak  = cfg_get_string("dsp_eq.rx", "band_peak",  "2000, 0.0, 1.00, 2");
        cfg->eq_tx.band_peak  = cfg_get_string("dsp_eq.tx", "band_peak",  "2000, 0.0, 1.00, 2");
        cfg->eq_ad.high_shelf = cfg_get_string("dsp_eq.ad", "high_shelf", "13500, 6.0, 0.7071, 2");
        cfg->eq_rx.high_shelf = cfg_get_string("dsp_eq.rx", "high_shelf", "12500, 6.0, 0.7071, 2");
        cfg->eq_tx.high_shelf = cfg_get_string("dsp_eq.tx", "high_shelf", "12500, 6.0, 0.7071, 2");
        parse_eq_band("dsp_eq.ad", cfg->eq_ad.low_shelf,  &cfg->eq_ad.low_shelf_freq_hz,  &cfg->eq_ad.low_shelf_gain_db,  &cfg->eq_ad.low_shelf_q,  &cfg->eq_ad.low_shelf_order);
        parse_eq_band("dsp_eq.rx", cfg->eq_rx.low_shelf,  &cfg->eq_rx.low_shelf_freq_hz,  &cfg->eq_rx.low_shelf_gain_db,  &cfg->eq_rx.low_shelf_q,  &cfg->eq_rx.low_shelf_order);
        parse_eq_band("dsp_eq.tx", cfg->eq_tx.low_shelf,  &cfg->eq_tx.low_shelf_freq_hz,  &cfg->eq_tx.low_shelf_gain_db,  &cfg->eq_tx.low_shelf_q,  &cfg->eq_tx.low_shelf_order);
        parse_eq_band("dsp_eq.ad", cfg->eq_ad.band_peak,  &cfg->eq_ad.band_peak_freq_hz,  &cfg->eq_ad.band_peak_gain_db,  &cfg->eq_ad.band_peak_q,  &cfg->eq_ad.band_peak_order);
        parse_eq_band("dsp_eq.rx", cfg->eq_rx.band_peak,  &cfg->eq_rx.band_peak_freq_hz,  &cfg->eq_rx.band_peak_gain_db,  &cfg->eq_rx.band_peak_q,  &cfg->eq_rx.band_peak_order);
        parse_eq_band("dsp_eq.tx", cfg->eq_tx.band_peak,  &cfg->eq_tx.band_peak_freq_hz,  &cfg->eq_tx.band_peak_gain_db,  &cfg->eq_tx.band_peak_q,  &cfg->eq_tx.band_peak_order);
        parse_eq_band("dsp_eq.ad", cfg->eq_ad.high_shelf, &cfg->eq_ad.high_shelf_freq_hz, &cfg->eq_ad.high_shelf_gain_db, &cfg->eq_ad.high_shelf_q, &cfg->eq_ad.high_shelf_order);
        parse_eq_band("dsp_eq.rx", cfg->eq_rx.high_shelf, &cfg->eq_rx.high_shelf_freq_hz, &cfg->eq_rx.high_shelf_gain_db, &cfg->eq_rx.high_shelf_q, &cfg->eq_rx.high_shelf_order);
        parse_eq_band("dsp_eq.tx", cfg->eq_tx.high_shelf, &cfg->eq_tx.high_shelf_freq_hz, &cfg->eq_tx.high_shelf_gain_db, &cfg->eq_tx.high_shelf_q, &cfg->eq_tx.high_shelf_order);
        cfg->eq_ad.low_shelf_q = cfg->eq_ad.high_shelf_q = default_filter_q;  // forced in aac-decoder mode
        cfg->eq_ad.low_shelf_order = cfg->eq_ad.high_shelf_order = 2;         // forced in aac-decoder mode

        /* DSP AGC (loudness) */
        cfg->agc_ad.enabled = cfg_read_bool("dsp_agc.ad", "enabled", 1);
        cfg->agc_rx.enabled = cfg_read_bool("dsp_agc.rx", "enabled", 1);
        cfg->agc_tx.enabled = cfg_read_bool("dsp_agc.tx", "enabled", 1);
        cfg->agc_ad.initial_gain_db = cfg_read_float("dsp_agc.ad", "initial_gain_db", -6.0, -80.0,0.0);
        cfg->agc_rx.initial_gain_db = cfg_read_float("dsp_agc.rx", "initial_gain_db", -6.0, -80.0,0.0);
        cfg->agc_tx.initial_gain_db = cfg_read_float("dsp_agc.tx", "initial_gain_db", -6.0, -80.0,0.0);
        cfg->agc_ad.target_rms_db = cfg_read_float("dsp_agc.ad", "target_rms_db", -16.0, -40.0,0.0);
        cfg->agc_rx.target_rms_db = cfg_read_float("dsp_agc.rx", "target_rms_db", -16.0, -40.0,0.0);
        cfg->agc_tx.target_rms_db = cfg_read_float("dsp_agc.tx", "target_rms_db", -16.0, -40.0,0.0);
        cfg->agc_ad.max_gain_db = cfg_read_float("dsp_agc.ad", "max_gain_db", 6.0, 0.0,40.0);
        cfg->agc_rx.max_gain_db = cfg_read_float("dsp_agc.rx", "max_gain_db", 6.0, 0.0,40.0);
        cfg->agc_tx.max_gain_db = cfg_read_float("dsp_agc.tx", "max_gain_db", 6.0, 0.0,40.0);
        cfg->agc_ad.gain_attack_ms = cfg_read_float("dsp_agc.ad", "gain_attack_ms", 1000, 0.0,5000);
        cfg->agc_rx.gain_attack_ms = cfg_read_float("dsp_agc.rx", "gain_attack_ms", 1000, 0.0,5000);
        cfg->agc_tx.gain_attack_ms = cfg_read_float("dsp_agc.tx", "gain_attack_ms", 1000, 0.0,5000);
        cfg->agc_ad.gain_release_ms = cfg_read_float("dsp_agc.ad", "gain_release_ms", 3000, 0.0,10000);
        cfg->agc_rx.gain_release_ms = cfg_read_float("dsp_agc.rx", "gain_release_ms", 3000, 0.0,10000);
        cfg->agc_tx.gain_release_ms = cfg_read_float("dsp_agc.tx", "gain_release_ms", 3000, 0.0,10000);
        cfg->agc_rx.rms_window_ms = cfg_read_float("dsp_agc.rx", "rms_window_ms", 500, 1.0,1000);
        cfg->agc_tx.rms_window_ms = cfg_read_float("dsp_agc.tx", "rms_window_ms", 500, 1.0,1000);
        cfg->agc_rx.silence_threshold_db = cfg_read_float("dsp_agc.rx", "silence_threshold_db", -48.0, -80.0,-6.0);
        cfg->agc_tx.silence_threshold_db = cfg_read_float("dsp_agc.tx", "silence_threshold_db", -48.0, -80.0,-6.0);
        cfg->agc_ad.reset_after_s = cfg_read_float("dsp_agc.ad", "reset_after_s", 3.0, 1.0,15.0);
        cfg->agc_rx.reset_after_s = cfg_read_float("dsp_agc.rx", "reset_after_s", 3.0, 1.0,15.0);
        cfg->agc_tx.reset_after_s = cfg_read_float("dsp_agc.tx", "reset_after_s", 3.0, 1.0,15.0);
        cfg->agc_ad.rms_window_ms = 500;              // forced in aac-decoder mode
        cfg->agc_ad.silence_threshold_db = -48.0;     // forced in aac-decoder mode

        /* DSP Limiter (realtime) */
        cfg->limiter_ad.enabled = cfg_read_bool("dsp_limiter.ad", "enabled", 0);
        cfg->limiter_rx.enabled = cfg_read_bool("dsp_limiter.rx", "enabled", 1);
        cfg->limiter_tx.enabled = cfg_read_bool("dsp_limiter.tx", "enabled", 1);
        cfg->limiter_ad.threshold_db = cfg_read_float("dsp_limiter.ad", "threshold_db", -0.2, -20.0,0.0);
        cfg->limiter_rx.threshold_db = cfg_read_float("dsp_limiter.rx", "threshold_db", -0.2, -40.0,0.0);
        cfg->limiter_tx.threshold_db = cfg_read_float("dsp_limiter.tx", "threshold_db", -1.0, -40.0,0.0);
        cfg->limiter_ad.hold_ms = cfg_read_float("dsp_limiter.ad", "hold_ms", 20.0, 0.0,100.0);  // hysteresis
        cfg->limiter_rx.hold_ms = cfg_read_float("dsp_limiter.rx", "hold_ms", 10.0, 0.0,100.0);
        cfg->limiter_tx.hold_ms = cfg_read_float("dsp_limiter.tx", "hold_ms", 10.0, 0.0,100.0);
        cfg->limiter_ad.release_ms = cfg_read_float("dsp_limiter.ad", "release_ms", 50.0, 0.0,500.0);
        cfg->limiter_rx.release_ms = cfg_read_float("dsp_limiter.rx", "release_ms", 50.0, 0.0,500.0);
        cfg->limiter_tx.release_ms = cfg_read_float("dsp_limiter.tx", "release_ms", 50.0, 0.0,500.0);
        cfg->limiter_rx.knee_width_db = cfg_read_float("dsp_limiter.rx", "knee_width_db", 0.0, -10.0,10.0);
        cfg->limiter_tx.knee_width_db = cfg_read_float("dsp_limiter.tx", "knee_width_db", 0.0, -10.0,10.0);
        cfg->limiter_rx.preamp_db = cfg_read_float("dsp_limiter.rx", "preamp_db", 0.0, -40.0,40.0);  // for compression
        cfg->limiter_tx.preamp_db = cfg_read_float("dsp_limiter.tx", "preamp_db", 0.0, -40.0,40.0);
        cfg->limiter_rx.linked_stereo = cfg_read_bool("dsp_limiter.rx", "linked_stereo", 0);  // ~1.3x speed-up
        cfg->limiter_tx.linked_stereo = cfg_read_bool("dsp_limiter.tx", "linked_stereo", 0);
        cfg->limiter_ad.knee_width_db = 0;            // forced in aac-decoder mode
        cfg->limiter_ad.preamp_db = 0.0;              // forced in aac-decoder mode
        cfg->limiter_ad.linked_stereo = 1;            // forced in aac-decoder mode
    }


    /* override some params with command-line
       -------------------------------------- */
    if (argc > 4) {
        int i; char* key, *value, *eq;
        for (i = 4; i < argc; i++) {
            if (!(eq = strchr(argv[i],'='))) continue;
            *eq = '\0'; key = argv[i]; value = eq + 1;
            if (value[0] != '\0') {
                /* список и присвоение переопределений (можно расширять) */
                if (!strcasecmp(key, "-input_backend"))  cfg->input.pcm_backend = value;
                if (!strcasecmp(key, "-output_backend")) cfg->output.pcm_backend = value;
                if (!strcasecmp(key, "-winmm_source"))   cfg->winmm.source_device = value;
                if (!strcasecmp(key, "-winmm_target"))   cfg->winmm.sink_device = value;
                if (!strcasecmp(key, "-stream_class"))   cfg->stream.payload_class = atoi(value);
                if (!strcasecmp(key, "-frame_capacity")) cfg->ask.frame_bytes = atoi(value);
                if (!strcasecmp(key, "-record_level"))   cfg->tx.eq.preamp_db = atof(value);
                if (!strcasecmp(key, "-record_level"))   cfg->tx.preemphasis_eq = 1; // включить EQ
                if (!strcasecmp(key, "-enable_dsp")) {   // DSP-обработка аудио
                    cfg->rx.aac_postproc = cfg->analog_rx.enable_dsp = cfg->analog_tx.enable_dsp = atoi(value);
                }
                fprintf(stderr, "Forcing '%s' -> %s\n", key +1, value);  // log
            }
        }
    }


    /* check and normalize values
       -------------------------- */
    /* исправить значения битовых форматов */
    if (!is_valid_format(cfg->input.def_format, 1)) cfg->input.def_format = 16;
    if (!is_valid_format(cfg->output.pcm_format, 2)) cfg->output.pcm_format = 16;

    /* определим выходную ЧД в TX при кастомной carrier_freq */
    if (cfg->ask.carrier_freq >= 4000 && strstr(argv[1], "x")) {
        int c; // выставляем новую ЧД, если достаточна по результату
        if (cfg->ask.carrier_freq * cfg->ask.cycle_samples >= 40000) {
            cfg->output.sample_rate = cfg->ask.carrier_freq * cfg->ask.cycle_samples;
        } else for (c = 4; c <= 20; c += 2) {
            // подбираем число сэмплов, пока ЧД не станет хотя бы 40 кГц
            cfg->ask.cycle_samples = c; cfg->output.sample_rate = cfg->ask.carrier_freq * c;
            if (cfg->output.sample_rate >= 40000) break;  // $c подходит
        }
    } else if (cfg->ask.carrier_freq < 4000) {
        // или посчитаем несущую частоту на основе ЧД (по умолчанию)
        cfg->ask.carrier_freq = cfg->output.sample_rate / cfg->ask.cycle_samples;
    }

    /* посчитаем авто-размер фрейма (при -1) на основе AAC-формата */
    if (cfg->ofdm.prefer_ask && cfg->ask.frame_bytes < 16) {
        double num_adts_frames = 44100.0 / 2048;  // 2048 сэмплов = 1-2 ADTS-фрейма при ЧД потока 44100 Гц
        double total_bitrate = cfg->ask.carrier_freq * log2(cfg->ofdm.qam_order) * (cfg->tx.stereo_mode +1);
        int actual_bitrate = (int)(total_bitrate * 0.92) / 1000 * 1000;  // обычно ~92% (грубо) + округление до 1/1000
        int adts_frame_size = (int)(actual_bitrate / num_adts_frames) / 8;  // размер ADTS-фрейма в байтах
        /* вычислим максимальный объём ASK-фрейма, взяв 2 полных ADTS-фрейма */
        cfg->ask.frame_bytes = 2 * (adts_frame_size + adts_frame_size / 10 + 1);  // запас по ABR-битрейту 10%
    }

    /* в стерео-режиме округляем фреймы до чётных */
    if (cfg->tx.stereo_mode && cfg->output.pcm_channels == 2) {
        cfg->meta.max_frames = (cfg->meta.max_frames +1) & ~1;
        cfg->meta.insert_frames = (cfg->meta.insert_frames +1) & ~1;
    }

    /* авто-размер сэмплов в буфере чтения (если мал или -1) */
    if (cfg->stream.chunk_samples < 256) {
        /* для HE-AAC кол-во всегда равно 2048, для LC - 1024 сэмпла */
        if (cfg->tx.aac_encoding && cfg->encoder.format == 0) cfg->stream.chunk_samples = 1024;
        else cfg->stream.chunk_samples = 2048;  // умолчание для HE-AAC
    }

    /* сброс шрифта meta-окна, если задано пустое значение */
    if (cfg->meta.ui_font_name[0] == '\0') {
        cfg->meta.ui_font_name = "Consolas";  // моноширинный шрифт
    }
    /* нормализовать жирность шрифта по x100 (для CreateFont) */
    if (cfg->meta.ui_font_bold > 0) cfg->meta.ui_font_bold *= 100;  // 0-900

    /* инвертировать усиления в компандерном режиме denoise */
    if (cfg->denoise_tx.compander_mode == 1) {
        if (cfg->denoise_tx.reduction_level_db < 0.0) cfg->denoise_tx.reduction_level_db *= -1.0;
        if (cfg->denoise_tx.enhance_peaks_db > 0.0) cfg->denoise_tx.enhance_peaks_db *= -1.0;
    }
    if (cfg->denoise_rx.compander_mode == 1) { // запретить неверные знаки при RX
        if (cfg->denoise_rx.reduction_level_db > 0.0) cfg->denoise_rx.reduction_level_db *= -1.0;
        if (cfg->denoise_rx.enhance_peaks_db < 0.0) cfg->denoise_rx.enhance_peaks_db *= -1.0;
    }

    /* обрезать знак и ширину knee в limiter, если -/> 1.0 */
    if (cfg->limiter_rx.knee_width_db < 0.0) cfg->limiter_rx.knee_width_db *= -1.0;
    if (cfg->limiter_rx.knee_width_db + cfg->limiter_rx.threshold_db > 0.0) {
        cfg->limiter_rx.knee_width_db = cfg->limiter_rx.threshold_db * -1.0;
        fprintf(stderr, "limiter_rx: knee_width_db is clamped to %.3f.\n", cfg->limiter_rx.knee_width_db);
    }
    if (cfg->limiter_tx.knee_width_db < 0.0) cfg->limiter_tx.knee_width_db *= -1.0;
    if (cfg->limiter_tx.knee_width_db + cfg->limiter_tx.threshold_db > 0.0) {
        cfg->limiter_tx.knee_width_db = cfg->limiter_tx.threshold_db * -1.0;
        fprintf(stderr, "limiter_tx: knee_width_db is clamped to %.3f.\n", cfg->limiter_tx.knee_width_db);
    }


    /* set TX/RX tape-correction profiles
       ---------------------------------- */
    /* зададим pre-emphasis профили для TX-эквалайзера */
    if (cfg->tx.preemphasis_eq == 1) {
        // зафиксируем константы пред-коррекции рокота и ВЧ-спада
        cfg->tx.eq.low_shelf_freq_hz  =   315;  // rumble lower freq (Dolby)
        cfg->tx.eq.high_shelf_freq_hz = 19000;  // treble falloff freq (average)
        cfg->tx.eq.band_peak_freq_hz  = 14000;  // bias curves center freq
        cfg->tx.eq.low_shelf_q = cfg->tx.eq.high_shelf_q = cfg->tx.eq.band_peak_q = default_filter_q;
        cfg->tx.eq.low_shelf_order = cfg->tx.eq.high_shelf_order = cfg->tx.eq.band_peak_order = 2;

        // профили bias-коррекции ленты выше 3 кГц (peak as shelf)
        switch (cfg->tx.eq_bias_profile) {
            case 1:  cfg->tx.eq.band_peak_gain_db =  9.0; cfg->tx.eq.band_peak_q = 0.43; break;
            case 2:  cfg->tx.eq.band_peak_gain_db = -3.0; cfg->tx.eq.band_peak_q = 0.10; break;
            case 3:  cfg->tx.eq.band_peak_gain_db = -4.5; cfg->tx.eq.band_peak_q = 0.10; break;
            case 4:  cfg->tx.eq.band_peak_gain_db = -3.0; cfg->tx.eq.band_peak_q = 0.10; break;
            default: cfg->tx.eq.band_peak_gain_db =  0.0; break;
        }
        // исправим перегрузку уровня (с нормировкой на несущую и Q)
        if (cfg->tx.eq.band_peak_gain_db != 0.0) {
            int peak_freq_shift = abs(cfg->tx.eq.band_peak_freq_hz - cfg->ask.carrier_freq);  // выше и ниже пика
            double carrier_freq_ratio = (double)(cfg->tx.eq.band_peak_freq_hz - peak_freq_shift) / cfg->tx.eq.band_peak_freq_hz;
            double carrier_gain_ratio = carrier_freq_ratio + (1.0 - carrier_freq_ratio) * (cfg->tx.eq.band_peak_q / default_filter_q);
            cfg->tx.eq.preamp_db -= cfg->tx.eq.band_peak_gain_db * carrier_gain_ratio;  // вычтем из уже заданного
        }
    }

    /* зададим профили коррекции ленты для Analog DSP */
    if (cfg->analog_rx.tape_correction > 0) {  // чтение (RX)
        // зафиксируем важные константы shelf-фильтров
        cfg->eq_rx.enabled = 1;  // форсим эквалайзер
        cfg->eq_rx.low_shelf_freq_hz  = 63;    // подъём завала на НЧ
        cfg->eq_rx.high_shelf_freq_hz = 9000;  // bias-коррекция на ВЧ
        cfg->eq_rx.low_shelf_q = cfg->eq_rx.high_shelf_q = default_filter_q;
        cfg->eq_rx.low_shelf_order = cfg->eq_rx.high_shelf_order = 2;

        // профили bias-коррекции ленты ниже 100 Гц и выше 2 кГц:
        switch (cfg->analog_rx.tape_correction) { default:
            case 1: cfg->eq_rx.low_shelf_gain_db =  6.0; cfg->eq_rx.high_shelf_gain_db = 12.0; cfg->eq_rx.high_shelf_q = 0.50; break;  // Тип-I,  обычная запись
            case 2: cfg->eq_rx.low_shelf_gain_db = 20.0; cfg->eq_rx.high_shelf_gain_db = 20.0; cfg->eq_rx.high_shelf_q = 0.43; break;  // Тип-I,  дешёвый тракт
            case 3: cfg->eq_rx.low_shelf_gain_db =  6.0; cfg->eq_rx.high_shelf_gain_db = -6.0; cfg->eq_rx.high_shelf_q = 0.15; break;  // Тип-II, обычная запись
            case 4: cfg->eq_rx.low_shelf_gain_db = 20.0; cfg->eq_rx.high_shelf_gain_db = 15.0; cfg->eq_rx.high_shelf_q = 0.43; break;  // Тип-II, дешёвый тракт
            case 5: cfg->eq_rx.low_shelf_gain_db =  6.0; cfg->eq_rx.high_shelf_gain_db = -9.0; cfg->eq_rx.high_shelf_q = 0.15; break;  // Тип-II, запись как Тип-I
        }
        // скорректируем также некоторые значения шумоподавления:
        cfg->denoise_rx.compander_mode = 0;  // отключим компандерный режим (неприемлем при коррекции готового результата)
        cfg->denoise_rx.reduction_level_db -= cfg->eq_rx.high_shelf_gain_db / 2;  // компенсация поднятия ВЧ (на частоте фильтра ~половина усиления)
        cfg->denoise_rx.enhance_peaks_db += cfg->eq_rx.high_shelf_gain_db / 4;  // компенсация слишком заваленных ВЧ-пиков (четверть усиления работает хорошо)
        cfg->denoise_rx.peaks_floor_db -= cfg->eq_rx.high_shelf_gain_db / 2;  // сдвиг нижней границы заваленных ВЧ-пиков (чтобы вытащить их из шумов)
        if (cfg->denoise_rx.noise_threshold_db < -48.0) cfg->denoise_rx.noise_threshold_db = -48.0;  // минимальная шумовая полка для ленты (-48 дБ)
    }
    if (cfg->analog_tx.tape_correction > 0) {  // запись (TX)
        // аналогичные профили и параметры...
        cfg->eq_tx.enabled = 1;
        cfg->eq_tx.low_shelf_freq_hz  = 63;
        cfg->eq_tx.high_shelf_freq_hz = 9000;
        cfg->eq_tx.low_shelf_q = cfg->eq_tx.high_shelf_q = default_filter_q;
        cfg->eq_tx.low_shelf_order = cfg->eq_tx.high_shelf_order = 2;

        switch (cfg->analog_tx.tape_correction) { default:
            case 1: cfg->eq_tx.low_shelf_gain_db =  6.0; cfg->eq_tx.high_shelf_gain_db = 12.0; cfg->eq_tx.high_shelf_q = 0.50; break;  // Тип-I,  обычная запись
            case 2: cfg->eq_tx.low_shelf_gain_db = 20.0; cfg->eq_tx.high_shelf_gain_db = 20.0; cfg->eq_tx.high_shelf_q = 0.43; break;  // Тип-I,  дешёвый тракт
            case 3: cfg->eq_tx.low_shelf_gain_db =  6.0; cfg->eq_tx.high_shelf_gain_db = -6.0; cfg->eq_tx.high_shelf_q = 0.15; break;  // Тип-II, обычная запись
            case 4: cfg->eq_tx.low_shelf_gain_db = 20.0; cfg->eq_tx.high_shelf_gain_db = 15.0; cfg->eq_tx.high_shelf_q = 0.43; break;  // Тип-II, дешёвый тракт
            case 5: cfg->eq_tx.low_shelf_gain_db =  6.0; cfg->eq_tx.high_shelf_gain_db = -9.0; cfg->eq_tx.high_shelf_q = 0.15; break;  // Тип-II, запись как Тип-I
        }
        // TODO: нужна ли коррекция Denoise (компандирование ВЧ)?
    }

    fprintf(stderr, "\n");
    return 0;
}

#endif
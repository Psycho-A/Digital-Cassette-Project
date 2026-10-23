/* RX.c */
/* -------------------------------
 * RX-режим (демодуляция и чтение)
 * -------------------------------
 * rx.c - self-contained RX (M-ASK version), C89 / VS6-friendly
 * Relies on: frame.h, crc16.h, pcm_io.h, ini.h (project files).
 *
 * Features:
 *  - read PCM samples via PcmSource backend;
 *  - demodulate PCM samples into RAW bitstream;
 *  - adapt to current frame params (carrier freq, payload size);
 *  - deserialize frames (unpacks bitstream into payload bytes);
 *  - pending frame payload buffers to write (for stereo);
 *  - sort frames and apply CRC policy for skipping, filling etc.;
 *  - write DATA chunks to output file and META to special file;
 *  - summary and diagnostic logs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <string.h>
#include "config.h"
#include "crc16.h"
#include "frame.h"
#include "modulation.h"
#include "pcm_io.h"
#include "metadata.h"
#include "control.h"
#include "rx_adts.c"

/* Параметры обработки */
typedef struct {
    // Модуляция
    AskDemodCtx ask;               // дескриптор m-ASK демодуляции (из modulation.c)
    MetadataCtx meta;              // дескриптор интерфейса метаданных (из metadata.c)
    AudioDecCtx adec;              // дескриптор аудио/AAC-декодеров (из rx_adts.c)

    // Параметры обработки
    int32_t  agc_enabled;          // включение авто-подстройки усиления (из конфига)
    int32_t  invert_phase;         // детекция периода по нисходящим переходам (из конфига)
    int32_t  reset_frames;         // сколько фреймов можно потерять до сброса состояний

    // Размеры преамбулы и фрейма
    uint16_t payload_bytes;        // максимальная ёмкость данных (копия из FrameDesc)
    uint32_t capacity_bits;        // общая ёмкость фрейма, включая преамбулу и заголовок
    uint32_t preamble_len_bits;    // ожидаемая длина преамбулы в битах
    uint32_t frame_bits_total;     // сколько бит данных ожидается

    // Буферы для кэша и заглушек
    uint8_t *prev_payload_buf;     // последний успешный фрейм (при дублировании)
    uint8_t *miss_payload_buf;     // буфер для заполнения пропущенных фреймов
    int32_t  prev_payload_valid;   // валиден ли кэш предыдущего фрейма (для вставки дублей)
    uint16_t last_data_frame;      // предыдущий записанный DATA-фрейм (для учёта пропусков)
    uint16_t last_written_frame;   // номер последнего записанного фрейма (для учёта повторов)
    int8_t   payload_previous;     // предыдущий класс данных (фильтр повтора фреймов, логи)

    // Логирование данных, UI
    uint32_t chunk_num;            // номер блока в сыром PCM-буфере (для статистики)
    uint32_t total_frames;         // общий счётчик фреймов (обновляется по ST_FRAME)
    uint32_t valid_frames;         // счётчик CRC-валидных фреймов (для статистики)
    int32_t  frame_valid_prev;     // валиден ли предыдущий фрейм (для разделителей)
    uint32_t out_bytes;            // сколько всего байт записано в файл(ы)

    // Выходные файлы
    FILE *out_fp;   // выходной файл согласно классу фрейма
    FILE *data_fp;  // вывод из фреймов с полезными данными
    FILE *inf_fp;   // вывод из фреймов метаданных (TOC/EOF/META)

    // Файловые сессии
    char     out_name[256];        // имя выходного файла (из аргументов или TOC/EOF)
    char     inf_name[256];        // имя файла с дампом метаданных ($out_name.inf)
    int32_t  filename_got;         // флаг, что новое имя выходного файла уже получено
    int32_t  eof_processed;        // флаг, что обработан первый EOF-фрейм и сброшены файлы

    // Отложенная обработка стерео-фреймов
    AskDemodState pending[2];      // копия состояния фрейма (включая буфер)
    int32_t  pending_ready;        // флаг готовности к обработке (после сохранения фрейма)
    uint8_t *pending_frame[2];     // копия буфера данных (выделяется один раз)

} RxDemodCtx;



/* Forward-объявления обработчиков */
DemodResult process_sample(RxDemodCtx *rctx, AskDemodState *proc, int32_t sample);
void read_frame_samples(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_n, DemodResult *res, int32_t sample);
void calc_frame_dimensions(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_n);
void reset_frame_state(AskDemodState *proc);
void handle_frame(RxDemodCtx *rctx, AskDemodState *proc);
void handle_file_sessions(RxDemodCtx *rctx, AskDemodState *proc);
void extract_kv_value(const uint8_t *buf, int len, const char *key, char *dst);
void rx_log_frame_status(RxDemodCtx *rctx, AskDemodState *proc, int is_written);

/* Отложенная обработка фреймов (для стерео) */
void save_pending_frame(RxDemodCtx *rctx, AskDemodState *proc);
void process_pending_frames(RxDemodCtx *rctx, int src_channels);
void reset_file_session(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_r);


/* Основной цикл демодуляции */
int rx_run(const char *in_name, const char *out_name)
{
    // Контексты
    int rc = 0;
    PcmSource src = {0};
    RxDemodCtx rctx = {0};
    OfdmParams ofdm = {0};
    AskDemodState proc = {0};
    AskDemodState proc_r = {0};
    PcmUnpackFn unpack_samples;
    DemodResult res, resR;

    // Объявление переменных
    uint32_t step, i;
    uint32_t chunk, chunk_num = 0;
    int32_t  sample, sampleR;
    int32_t  hk_stat = 0;  // хот-кеи
    int32_t  is_stream = 0;

    // Буферы распакованных сэмплов (Q31)
    int32_t *lbuf = NULL;
    int32_t *rbuf = NULL;

    // Таймер для бенчмарка (мсек)
    double  elapsed_ms;
    clock_t time_start, time_end;

    // Сокращённые имена файлов (для UI)
    const char *in_fname = strstr(in_name,"\\") ? strrchr(in_name,'\\') +1 : in_name;
    const char *out_fname = strstr(out_name,"\\") ? strrchr(out_name,'\\') +1 : out_name;


    // Открыть источник (входной поток)
    if (open_source(&src, &cfg, in_name) != 0) {
        fprintf(stderr, "RX: cannot open input %s source.\n", cfg.input.pcm_backend);
        rc = 1; goto cleanup;
    } else {
        // режим потока: нажатие паузы не блокирует цикл чтения
        if (!strcmp(in_name, "-") || strcasecmp(cfg.input.pcm_backend, "wav")) is_stream = 1;
    }

    // Выбрать выходные файлы для декодированных данных
    if (!strcmp(out_name, "-")) {
        rctx.data_fp = stdout;  // вывод данных в pipe/поток
        rctx.inf_fp = stderr;   // вывод метаданных в консоль
        rctx.out_fp = rctx.data_fp;  // по умолчанию - данные
    } else {
        // имена по умолчанию (если не получены из TOC)
        sprintf(rctx.out_name, "%s", out_name);  // данные
        sprintf(rctx.inf_name, "%s.inf", out_name);  // служебка
        // открытие происходит при поступлении данных!
        rctx.out_fp = rctx.data_fp = rctx.inf_fp = NULL;
    }


    // Инициализация параметров OFDM (геометрия фрейма)
    ofdm.qam_order = (uint16_t)cfg.ofdm.qam_order;
    ofdm.symbols_per_frame = (uint8_t)cfg.ofdm.symbols_per_frame;
    ofdm.low_freq = (uint32_t)cfg.ofdm.data_low_freq;
    ofdm.high_freq = (uint32_t)cfg.ofdm.data_high_freq;
    ofdm.sample_rate = (uint32_t)src.sample_rate;  // частота, на которой работает источник
    ofdm.fft_size = (uint16_t)cfg.ofdm.fft_size;
    ofdm.frame_stereo = (src.channels >= 2) ? 1 : 0;  // только каналы (стерео-пары детектятся автоматически)
    ofdm.bits_per_symbol = bits_per_symbol_from_qam(cfg.ofdm.qam_order);
    ofdm.active_carriers = calc_active_carriers(&ofdm);
    ofdm.custom_bytes = 0; proc.frame.ofdm_encoding = 1; // при OFDM

    // Переопределения при принудительном ASK
    if (cfg.ofdm.prefer_ask) {
        // отключаем sync_word, MSB/LSB-кодирование payload и OFDM-расчёт ёмкости
        SERVICE_BITS = HEADER_BITS + CRC_BITS; proc.frame.ofdm_encoding = 0;
        if (cfg.ask.frame_bytes >= PAYLOAD_MIN) ofdm.custom_bytes = cfg.ask.frame_bytes;
    }

    // Расчёты ёмкости фрейма (биты и символы во фрейме и преамбуле)
    step = lcm(ofdm.bits_per_symbol * 16, 8);  // мин. общее кратное байту и 16 символам
    rctx.frame_bits_total = ofdm.active_carriers * ofdm.symbols_per_frame * ofdm.bits_per_symbol;  // OFDM-версия
    rctx.frame_bits_total = ofdm.custom_bytes ? SERVICE_BITS + ofdm.custom_bytes * 8 : rctx.frame_bits_total;  // ASK
    rctx.frame_bits_total = (rctx.frame_bits_total + step-1) / step * step;  // округление вверх до $step
    rctx.preamble_len_bits = (cfg.ask.preamble_symbols + cfg.ask.training_symbols) * ofdm.bits_per_symbol;  // ASK-версия
    rctx.payload_bytes = (uint16_t)calc_payload_bytes(&ofdm);  // макс. размер полезных данных (ожидаемый)
    rctx.capacity_bits = rctx.preamble_len_bits + rctx.frame_bits_total;  // для расчёта битрейта (ожидаемого)

    // Общие параметры фреймов
    rctx.agc_enabled = cfg.rx.agc_enabled;
    rctx.invert_phase = cfg.rx.invert_phase;
    rctx.reset_frames = cfg.rx.reset_state_frames;
    rctx.filename_got = 0;  // получение имени файла
    rctx.eof_processed = 0;  // готовность к сбросу
    rctx.last_data_frame = rctx.last_written_frame = 0;
    rctx.total_frames = rctx.valid_frames = 0;
    rctx.payload_previous = -1;
    rctx.prev_payload_valid = 0;

    // Заполняем структуру фрейма
    proc.frame.hdr.frame_number = 0;
    proc.frame.payload_bytes = PAYLOAD_MAX;  // читаем любую ёмкость
    proc.frame.sync.preamble_pattern = 0;  // в ASK не используется
    proc.frame.sync.preamble_len_bits = rctx.preamble_len_bits;

    // В стерео счётчик фреймов делится на 2 канала
    if (src.channels >= 2) rctx.reset_frames /= 2;


    /* Параметры m-ASK/PAM модуляции
       ----------------------------- */
    #define actx rctx.ask  // контекст
    #define acfg rctx.ask.cfg  // профиль

    // Инициализация профиля настроек
    acfg.min_amp = amp_from_db(cfg.ask.low_level_db, 32);
    acfg.max_amp = amp_from_db(cfg.ask.high_level_db, 32);
    acfg.sync_amp = acfg.max_amp;  // всегда равна макс. бит-уровню
    acfg.cycle_samples = cfg.ask.cycle_samples;  // исходная частота несущей
    acfg.symbol_bits = ofdm.bits_per_symbol;  // бит на символ (1,2,4...)

    // Преамбула, заголовок, пилот-символы
    acfg.preamble_symbols = cfg.ask.preamble_symbols;  // кол-во начальных НЧ-маркеров в преамбуле
    acfg.training_symbols = cfg.ask.training_symbols;  // символы после НЧ-маркеров для вычисления порогов
    acfg.pilot_interval = cfg.ask.pilot_interval;  // интервал чередования синхро-импульса в символах

    // Пороги для тренировочных символов
    acfg.sync_train_max = acfg.preamble_symbols + acfg.training_symbols * 2/4;  // конец замера порога максимального бита (TODO: 1/4)
    acfg.sync_train_min = acfg.preamble_symbols + acfg.training_symbols * 4/4;  // конец замера порога нулевого бита (TODO: 2/4)
    acfg.sync_train_lv2 = acfg.preamble_symbols + acfg.training_symbols * 3/4;  // конец замера верхнего среднего порога (для 4+ASK)
    acfg.sync_train_lv1 = acfg.preamble_symbols + acfg.training_symbols * 4/4;  // конец замера нижнего среднего порога (для 4+ASK)

    // Вычисление битовых уровней и порогов амплитуд
    acfg.num_levels = ofdm.qam_order;  // битовых уровней (2,4,16...)
    ask_calc_thresh_levels(&acfg, acfg.min_amp, acfg.max_amp, acfg.amp_levels, acfg.thresh_levels, 32);

    // Заполним и довычислим остальные поля ASK-контекста
    actx.agc_enabled = rctx.agc_enabled;
    actx.agc_percent = cfg.ask.agc_percent;  // если "-1", то подбирается в ask_finish_period
    actx.agc_smoothing = cfg.ask.agc_smoothing;  // если "-1", то подбирается в ask_finish_period
    actx.drift_margin = cfg.ask.amp_bad_zone;  // нестабильная зона по краям порога символа в % (коррекция ISI, управление АРУ)
    actx.correct_dynamics = cfg.ask.correct_dynamics;  // экспериментально: коррекция ДД порогов в % с учётом измерений в данных
    actx.correct_nonlinear = cfg.ask.correct_nonlinear;  // экспериментально: коррекция нелинейности средних уровней в канале (для 4-ASK)
    actx.measure_dynamics = cfg.ask.measure_dynamics;  // экспериментально: замер ДД в сигнале по тренировочным символам
    actx.invert_phase = rctx.invert_phase;  // если "-1", то используется авто-обнаружение (по-канально)
    actx.reset_frames = rctx.reset_frames;
    actx.sample_rate = src.sample_rate;  // частота дискретизации потока (при измерениях Fc/Fs)
    actx.preamble_detect  = acfg.preamble_symbols;  // детект, если накоплена вся НЧ-часть преамбулы
    actx.data_periods_max = acfg.preamble_symbols * 2;  // порог фиксации несущей частоты при re-sync (2x преамбула)
    actx.dynamics_average = 100; actx.dynamics_counter = 1;  // динамический диапазон за файл (средний proc.dynamics_percent)
    actx.workarounds = cfg.ask.workarounds;  // временные "костыли" и хаки улучшений при разных параметрах сигнала

    // Глобальное измерение несущей частоты (изменяемое)
    actx.freq_samples = 0;  // вычисляется внутри ST_FRAME меж-канально
    actx.carrier_freq = cfg.ask.carrier_freq;  // рассчитана в config.h

    // Ожидаемые размеры фрейма (по конфигу - могут изменяться в процессе!)
    acfg.extra_symbols  = SERVICE_BITS - (SERVICE_BITS + acfg.symbol_bits-1) / acfg.symbol_bits;  // доп. символы от 2-ASK заголовка (при 4+ASK)
    actx.frame_symbols  = rctx.frame_bits_total / acfg.symbol_bits + acfg.extra_symbols;   // символов в теле фрейма (заголовок + данные)
    actx.pilot_symbols  = (actx.frame_symbols + acfg.pilot_interval -1) / acfg.pilot_interval;  // пилотов во фрейме (для битрейта и re-sync)
    actx.frame_periods  = (acfg.preamble_symbols * 2 + acfg.training_symbols + actx.frame_symbols + actx.pilot_symbols) / 2;  // детект потерь (расчёт для Fc/2)
    actx.frame_bits_min = actx.frame_bits_max = SERVICE_BITS + PAYLOAD_MIN * 8;  // минимальная и ожидаемая ёмкость фрейма (динамически повышается после чтения header)
    actx.order_changed  = actx.gray_mapping = 0;  // флаги смены порядка ASK и Грей-кодирования (выставляются после чтения из заголовка)

    // Инициализация состояния демодуляции
    proc.sample_count = 0;
    proc.gain_mul = 1000;  // начальное усиление (= 1.0)
    proc.invert_phase = actx.invert_phase;  // начальное значение (по-канальное)
    proc.frame_bits_actual = actx.frame_bits_min;  // реальный размер фрейма (из заголовка)
    ask_reset_rx_values(&proc);  // сброс всего остального (для удобства)

    // Скопируем общие поля в контекст Правого канала
    memcpy(&proc_r, &proc, sizeof(AskDemodState));
    proc.channel = "L"; proc_r.channel = "R"; // теги каналов

    // Инициализация метаданных (-1 = не задано)
    memset(&rctx.meta, 0, sizeof(MetadataCtx));
    rctx.meta.track_number = rctx.meta.total_tracks = -1;
    rctx.meta.time_stamp = rctx.meta.duration = -1;
    rctx.meta.year_id = rctx.meta.genre_id = -1;
    rctx.meta.ui_font_file = cfg.meta.ui_font_file;  // ttf-файл шрифта
    rctx.meta.ui_font_name = cfg.meta.ui_font_name;  // название шрифта
    rctx.meta.ui_font_size = cfg.meta.ui_font_size;  // размер шрифта
    rctx.meta.ui_font_bold = cfg.meta.ui_font_bold;  // жирность шрифта


    /* Фреймовые буферы (под макс. размер)
       ----------------------------------- */
    // Выделение фреймовых буферов для приёма битов
    proc.bitbuf = (uint8_t*)calloc(1, BITSCAP_MAX);
    proc_r.bitbuf = (uint8_t*)calloc(1, BITSCAP_MAX);
    if (!proc.bitbuf || !proc_r.bitbuf) {
        fprintf(stderr, "RX: out of memory for bit buffer.\n");
        rc = 1; goto cleanup;
    }

    // Выделение фреймовых буферов для payload и заглушек
    proc.payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    proc_r.payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    rctx.prev_payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    rctx.miss_payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    if (!proc.payload_buf || !proc_r.payload_buf 
    || (!rctx.prev_payload_buf || !rctx.miss_payload_buf)) {
        fprintf(stderr, "RX: out of memory for payload buffers.\n");
        rc = 1; goto cleanup;
    }

    // Отложенная обработка фреймов
    rctx.pending_ready = 0;  // флаг готовности к обработке
    rctx.pending[0].payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    rctx.pending[1].payload_buf = (uint8_t*)calloc(1, PAYLOAD_MAX);
    if (!rctx.pending[0].payload_buf || !rctx.pending[1].payload_buf) {
        fprintf(stderr, "RX: out of memory for pending frame buffers.\n");
        rc = 1; goto cleanup;
    }

    /* Параметры вывода
       ---------------- */
    // Буферы для чтения сырых сэмплов (как в analog.c)
    lbuf = (int32_t*)malloc(src.chunk_frames * sizeof(int32_t));
    rbuf = (int32_t*)malloc(src.chunk_frames * sizeof(int32_t));
    if (!lbuf || !rbuf) {
        fprintf(stderr, "RX: out of memory for sample buffers.\n");
        rc = 1; goto cleanup;
    }

    // Выбор функции распаковки (конверсия PCM-байтов в 32-бит)
    unpack_samples = pcm_select_unpack_fn(src.channels, src.format);
    if (!unpack_samples) {
        fprintf(stderr, "RX: unsupported input format.\n");
        rc = 1; goto cleanup;
    }

    // Рассчитать примерный битрейт
    calc_stream_bitrate (
        &ofdm, &acfg, src.sample_rate, rctx.capacity_bits, rctx.payload_bytes, actx.pilot_symbols
    );


    /* Инициализация AAC-декодера (FAAD2)
       ---------------------------------- */
    if (cfg.rx.aac_decoding) {
        aac_decoder_create(&rctx.adec);  // см. rx_adts.c
        sprintf(rctx.adec.out_file, out_name);  // имя файла для wav sink

        if (cfg.meta.metadata_window) {
            // Вывод метаданных в отдельное окно (LCD/GUI)
            if (strcasecmp(cfg.output.pcm_backend, "wav")) 
                metadata_window_init(&rctx.meta, "WAITING FOR STREAM...");
        }
    }


    /* Подготовка к запуску
       -------------------- */
    // Подсказки по клавишному управлению
    fprintf(stderr, "Keyboard Controls:\n");
    fprintf(stderr, "- Use [p] or [space] to PAUSE reading stream\n");
    fprintf(stderr, "- Use [s] or [escape] to STOP reading stream\n\n");

    // Вывести информацию текущей сессии
    fprintf(stderr, "RX started:         %.27s -> %.27s\n", in_fname, out_fname);
    fprintf(stderr, "Input source:       %s <- %d Hz, %d channels, %d-bit\n", 
            cfg.input.pcm_backend, src.sample_rate, src.channels, src.format); if (cfg.ofdm.prefer_ask) {
    fprintf(stderr, "Modulation:         %i-ASK, %i bits/symbol, 1 cycles/symbol\n", 
            acfg.num_levels, acfg.symbol_bits);  // ASK version
    fprintf(stderr, "Frame levels:       max = %.1f dB, min = %.1f dB (total range: %.0f dB)\n", 
            cfg.ask.high_level_db, cfg.ask.low_level_db, cfg.ask.high_level_db-cfg.ask.low_level_db); } else {
    fprintf(stderr, "Modulation:         %d sub-carriers, %d sym/frame, %d bits/symbol\n", 
            ofdm.active_carriers, ofdm.symbols_per_frame, acfg.symbol_bits); }  // OFDM
    fprintf(stderr, "Frame capacity:     %u bits (%u bytes), data = %u bits (%u bytes)\n", 
            rctx.capacity_bits, rctx.capacity_bits/8, rctx.payload_bytes*8, rctx.payload_bytes);
    fprintf(stderr, "Preamble symbols:   marker = %d, training = %d, total: %d bits (%d bytes)\n", 
            acfg.preamble_symbols, acfg.training_symbols, rctx.preamble_len_bits, rctx.preamble_len_bits/8);
    fprintf(stderr, "Header length:      sync = %u, main = %u, crc = %u+%u+%u bytes (%u bytes)\n", 
            cfg.ofdm.prefer_ask ? 0 : SYNC_BITS/8, HEADER_BITS/8, 2, 2, 2, SERVICE_BITS/8);
    fprintf(stderr, "Pilot symbols:      interval = %d, in-frame total = %d symbols\n", 
            acfg.pilot_interval, actx.pilot_symbols);
    fprintf(stderr, "Expected bitrate:   raw = %d bit/s, useful = %d bit/s\n", 
            total_bitrate, payload_bitrate);
    fprintf(stderr, "De-Emphasis EQ:     rumble-filter = %.1f dB, bias-profile: %d (%s)\n", 
            cfg.rx.eq_rumble_filter_db, cfg.rx.eq_bias_profile, cfg.rx.eq_bias_profile ? "by tape" : "disabled");
    fprintf(stderr, "Line-Adaptive EQ:   auto-bias: %s (dz=%.2f), de-ringing: %s (dz=%.2f)\n", 
            cfg.rx.eq_adaptive_bias ? "on":"off", cfg.rx.eq_bias_deadzone, cfg.rx.eq_adaptive_isi ? "on":"off", cfg.rx.eq_isi_deadzone);
    fprintf(stderr, "AAC decoder:        type: %s, initialized: %s, loading mode: DLL\n%s", 
            rctx.adec.faad_handle ? "FAAD2" : "none", cfg.rx.aac_decoding ? "yes" : "no", cfg.rx.preprocess_eq ? "\n" : "");


    /* Инициализация адаптивного эквалайзера
       ------------------------------------- */
    if (ask_adaptive_eq_init(&proc.eq, src.sample_rate, proc.channel) != 0) {
        fprintf(stderr, "RX: m-ASK Adaptive EQ init failed (left).\n");
    } else if (src.channels >= 2) {
        if (ask_adaptive_eq_init(&proc_r.eq, src.sample_rate, proc_r.channel) != 0) {
            fprintf(stderr, "RX: m-ASK Adaptive EQ init failed (right).\n");
        }
    }
    if (proc.eq.enabled && proc.eq.dump_samples) {
        // открываем файл для дампа из EQ-фильтров...
        proc.eq.raw_samples = proc_r.eq.raw_samples = fopen("raw_samples.pcm", "wb");
        if (!proc.eq.raw_samples) proc.eq.dump_samples = proc_r.eq.dump_samples = 0; // ошибка
    }


    /* Основной цикл обработки сэмплов
       ------------------------------- */
    time_start = clock();  // старт бенчмарка
    while ((chunk = src.read_chunk(src.ctx, src.chunk_frames)) > 0) {
        // Распаковываем в lbuf, rbuf (моно или стерео)
        // Источник отдаёт сырые байты, распаковываем unpack_samples в int32_t
        // Если 2 канала - то в каждом свой фрейм, либо дубли (двойное моно)
        rctx.chunk_num++; // update counter

        /* Проверка нажатия хот-кеев */
        hk_stat = check_hotkeys(is_stream);
        if (hk_stat == 1) break;     // отправлен стоп: выходим
        if (hk_stat == 2) continue;  // пауза в потоке: пропуск цикла

        /* Распаковка сэмплов в 32 бита */
        unpack_samples(src.chunk_buf, lbuf, rbuf, chunk);

        // Обрабатываем каждый сэмпл
        for (i = 0; i < chunk; i++) {
            // Если стерео, то фреймы могут быть независимыми
            sample = lbuf[i]; sampleR = rbuf[i];

            // Читаем и обрабатываем каждый сэмпл
            read_frame_samples(&rctx, &proc, &proc_r, &res, sample);

            // Обработка второго канала (независимая)
            if (src.channels >= 2) {
                read_frame_samples(&rctx, &proc_r, &proc, &resR, sampleR);
            }

            // Оба канала вышли из обработки - можем парсить!
            if (rctx.pending_ready && ( 
               (proc.state == ST_SEARCH && proc_r.state == ST_SEARCH) || 
               (res == FRAME_NEW || resR == FRAME_NEW) )) {  // force
                process_pending_frames(&rctx, src.channels);

                // Достигнут конец файла: сбросим состояния для нового
                if (rctx.eof_processed >= 2) {  // минимум стерео-пара
                    reset_file_session(&rctx, &proc, &proc_r);
                }
            }
        }
    }

    // Завершаем последние фреймы (выход из цикла)
    sample = -sample; sampleR = -sampleR;  // форсим zero-cross
    read_frame_samples(&rctx, &proc, &proc_r, &res, sample);
    if (src.channels >= 2) {  // обрабатываем второй канал
        read_frame_samples(&rctx, &proc_r, &proc, &resR, sampleR);
    }
    rctx.pending_ready = 1;  // форсим готовность к парсингу
    process_pending_frames(&rctx, src.channels);  // сохраняем пару


    // Отмечаем время окончания
    time_end = clock(); // остановить таймер
    elapsed_ms = (double)(time_end - time_start) / CLOCKS_PER_SEC;

    // Сбрасываем DSP-обработчики
    ask_adaptive_eq_free(&proc.eq);
    ask_adaptive_eq_free(&proc_r.eq);

    // Высвобождаем AAC-декодер
    aac_decoder_free(&rctx.adec);

    // Получаем средний ДД записи
    actx.dynamics_average /= actx.dynamics_counter;

    // Информируем о завершении и выводим процент ошибок
    fprintf(stderr, "\n\nRX finished: %s.", strcmp(out_name, "-") ? out_fname : "<stream>");
    fprintf(stderr, "\nTotal decoded bytes:  %u", rctx.out_bytes);
    fprintf(stderr, "\nTotal decoded frames: %u", rctx.total_frames);
    fprintf(stderr, "\nValid/invalid frames: %u/%u (%u%%)", 
        rctx.valid_frames, rctx.total_frames - rctx.valid_frames, 
        !rctx.total_frames ? 0 : (rctx.valid_frames * 100) / rctx.total_frames );
    fprintf(stderr, "\nLine dynamics range:  %.1f dB (%u%%)", 
        (cfg.ask.high_level_db - cfg.ask.low_level_db) * actx.dynamics_average / 100, actx.dynamics_average);
    fprintf(stderr, "\nTotal elapsed time:   %.3f seconds\n", elapsed_ms);


cleanup:
    if (lbuf) free(lbuf);
    if (rbuf) free(rbuf);
    if (src.ctx) src.close(src.ctx);
    if (proc.bitbuf) free(proc.bitbuf);
    if (proc_r.bitbuf) free(proc_r.bitbuf);
    if (proc.payload_buf) free(proc.payload_buf);
    if (proc_r.payload_buf) free(proc_r.payload_buf);
    if (rctx.prev_payload_buf) free(rctx.prev_payload_buf);
    if (rctx.miss_payload_buf) free(rctx.miss_payload_buf);
    if (rctx.pending[0].payload_buf) free(rctx.pending[0].payload_buf);
    if (rctx.pending[1].payload_buf) free(rctx.pending[1].payload_buf);
    if (strcmp(out_name, "-") != 0) {
        if (rctx.data_fp) fclose(rctx.data_fp);
        if (rctx.inf_fp)  fclose(rctx.inf_fp);
        if (rctx.out_fp)  fclose(rctx.out_fp);
    }
    #ifdef _WIN32
    if (rctx.meta.ui_window_handle != NULL) {
        metadata_window_free(&rctx.meta);  // удалить UI-окно
    }
    #endif
    return rc;
}


/* Общий цикл чтения входных аудио-сэмплов (на 1 канал) */
inline void read_frame_samples(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_n, DemodResult *res, int32_t sample)
{
    // Обработка одного сэмпла
    *res = process_sample(rctx, proc, sample);

    // Проверяем результат обработки
    if (*res == FRAME_HEADER) {
        // Достигнут заголовок фрейма, читаем
        if (parse_frame_header(&proc->frame, proc->bitbuf, proc->payload_buf) == 0) {
            // header успешно прочитан, обновляем размеры фрейма
            calc_frame_dimensions(rctx, proc, proc_n);
        }
    } else if (*res >= FRAME_FULL) {
        // Фрейм полностью принят, разбираем
        if (proc->frame_bits_pair > 0) {
            // вычислим полный размер блока (с метаданными)
            proc->frame.payload_extra = (uint16_t)(proc->frame_bits_pair - SERVICE_BITS) / 8;
        }
        // Декодируем биты фрейма в payload_buf
        parse_frame_payload(&proc->frame, proc->bitbuf, proc->payload_buf);

        // Помещаем фрейм в буфер для сортировки
        save_pending_frame(rctx, proc);

        // Сброс состояния для следующего фрейма
        reset_frame_state(proc);
    }
}

/* Основная функция обработки каждого сэмпла (прокси) */
inline DemodResult process_sample(RxDemodCtx *rctx, AskDemodState *proc, int32_t sample)
{
    if (!rctx || !proc) return FRAME_NONE;
    return ask_process_sample(&rctx->ask, proc, sample);  // для m-ASK
}

/* Пересчёт параметров фрейма после чтения заголовка (прокси) */
inline void calc_frame_dimensions(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_n)
{
    // Если заголовок успешно прочитан, обновляем ёмкость фрейма
    proc->frame_bits_actual = SERVICE_BITS + proc->frame.hdr.payload_used * 8;

    // Пересчитаем геометрию фрейма (для правильной детекции)
    ask_calc_frame_dimensions(&rctx->ask, proc);  // для m-ASK

    // Обновим ёмкость стерео-пары, если текущий фрейм её увеличил
    if (proc->frame_bits_actual > proc->frame_bits_pair) {
        proc->frame_bits_pair = proc_n->frame_bits_pair = proc->frame_bits_actual;
    }
}

/* Сброс состояния после фрейма (прокси) */
static void reset_frame_state(AskDemodState *proc)
{
    if (!proc) return;
    ask_reset_frame_state(proc);  // для m-ASK
}


/* Функция накопления фреймов для отложенной обработки */
inline void save_pending_frame(RxDemodCtx *rctx, AskDemodState *proc)
{
    // Присвоим номера каналам (из тега в структуре)
    int ch = 0; if (proc->channel == "R") ch = 1;

    // Копируем полезные данные (из локального буфера)
    memcpy(rctx->pending[ch].payload_buf, proc->payload_buf, PAYLOAD_MAX);

    // Копируем важные параметры (без msb_buf/lsb_buf)
    rctx->pending[ch].frame = proc->frame;
    rctx->pending[ch].partial_bits = proc->partial_bits;

    // Дополнительные параметры (для логов)
    rctx->pending[ch].eq = proc->eq;  // эквалайзер
    rctx->pending[ch].gain_mul = proc->gain_mul;  // АРУ
    rctx->pending[ch].dynamics_percent = proc->dynamics_percent;  // ДД

    // Инициируем готовность к отложенной обработке
    rctx->pending_ready += 1;  // вызов при паре ST_SEARCH
}

/* Функция обработки накопленных фреймов (вызывается по таймеру) */
inline void process_pending_frames(RxDemodCtx *rctx, int src_channels)
{
    int i, ch;
    int frame_order[2] = {0, 1};

    // Сортируем каналы по номеру фрейма (если оба есть)
    if (rctx->pending[0].frame.hdr.frame_number > 0 &&
        rctx->pending[1].frame.hdr.frame_number > 0) {
        if (rctx->pending[0].frame.hdr.frame_number 
          > rctx->pending[1].frame.hdr.frame_number) {
            frame_order[0] = 1; frame_order[1] = 0;
        }
        // Сортировка с учётом предыдущего номера в классе
        for (i = 0; i < 2; i++) {
            if (rctx->pending[i].frame.crc_hdr_valid && 
               (rctx->pending[i].frame.hdr.payload_class == rctx->payload_previous) &&
               (rctx->pending[i].frame.hdr.frame_number == rctx->last_written_frame + 1 ||
                rctx->pending[i].frame.hdr.frame_number == rctx->last_written_frame)) {
                frame_order[i] = 0; frame_order[abs(i-1)] = 1;
            }
        }
    }

    // Обрабатываем в порядке возрастания номера
    for (i = 0; i < 2; i++) {
        ch = frame_order[i];
        if (rctx->pending_ready >= 2  // форсируем на паре
        || (rctx->pending[ch].frame.hdr.frame_number > 0)) {
            // запись или пропуск фрейма (по CRC-политикам)
            handle_frame(rctx, &rctx->pending[ch]);
            rctx->pending[ch].frame.hdr.frame_number = 0;  // очищаем номер
        }
    }

    // Снимаем флаг готовности до следующего вызова
    rctx->pending_ready = 0;  // включится в save_pending_frame
}


/* Управление фреймом: политика записи, выбор выходного файла */
static void handle_frame(RxDemodCtx *rctx, AskDemodState *proc)
{
    uint32_t i = 0;
    int write_frame = 1;
    uint32_t payload_len = proc->frame.payload_bytes;
    uint8_t  payload_class = proc->frame.hdr.payload_class;  // x/10*10?
    uint16_t frame_number = proc->frame.hdr.frame_number;
    uint32_t expected_number = rctx->last_data_frame + 1;  // ожидаемый номер

    // Определяем, несёт ли фрейм полезные данные
    int is_data_frame = (payload_class <= PAYLOAD_VIDEO +VIDEO_WMV);
    int is_adts_audio = (payload_class == PAYLOAD_AUDIO +AUDIO_AAC);

    // Определяем, нужно ли вставлять пропущенные фреймы
    int update_prev   = (is_data_frame && cfg.rx.core_policy == 2);
    int fill_missing  = (is_data_frame && !is_adts_audio && cfg.rx.fill_missing && cfg.rx.core_policy <= 2);

    // Фильтрация уже записанных фреймов (служебка, двойное моно)
    int same_class    = (payload_class == rctx->payload_previous);
    int is_duplicated = (same_class && frame_number == rctx->last_written_frame);

    // Пропускаем фреймы с битым хедером или метаданными
    if (!proc->frame.crc_hdr_valid || 
       (!is_data_frame && !proc->frame.frame_valid)) {
        write_frame = 0; goto handle_frame_end;  // ничего не делаем
    }

    // Сбросить данные от предыдущего файла (для нового)
    if (payload_class != PAYLOAD_EOF && rctx->eof_processed) {
        // флаг конца файла, старое имя, отступ от старого лога
        rctx->eof_processed = rctx->filename_got = 0; fprintf(stderr,"\n");
        if (rctx->data_fp != stdout) {
            // сбрасываем FILE-дескрипторы для открытия новых
            if (rctx->data_fp) fclose(rctx->data_fp); rctx->data_fp = NULL;
            if (rctx->inf_fp)  fclose(rctx->inf_fp);  rctx->inf_fp  = NULL;
        }
    }

    // Применяем политики записи DATA-фрейма
    if (!proc->frame.crc_msb_valid) {
        // Обрабатываем политику для CRC core
        switch (cfg.rx.core_policy) {
            case 0:  // пишем как есть
                break;
            case 1:  // заполняем весь фрейм нулями
                if (is_adts_audio) write_frame = 0;
                memset(proc->payload_buf, 0, payload_len);
                break;
            case 2:  // вставляем последний успешный фрейм
                if (rctx->prev_payload_valid)
                memcpy(proc->payload_buf, rctx->prev_payload_buf, payload_len);
                break;
            case 3:  // пропускаем фрейм
                write_frame = 0;
                break;
        }
    } else if (!proc->frame.crc_lsb_valid) {
        // CRC core OK, обрабатываем LSB-зону
        switch (cfg.rx.crc_lsb_policy) {
            case 0:
                break;
            case 1:
                if (is_adts_audio) write_frame = 0;
                memset(proc->payload_buf, 0, payload_len);
                break;
            case 2:
                if (rctx->prev_payload_valid)
                memcpy(proc->payload_buf, rctx->prev_payload_buf, payload_len);
                break;
            case 3:
                write_frame = 0;
                break;
        }
    }

    // Сообщить о прерванном фрейме
    if (proc->partial_bits > 0) {
        fprintf(stderr, "\nCropped frame #%u (bits, bytes got=%u/%u, expects=%u/%u)",
            proc->frame.hdr.frame_number, 
            proc->partial_bits, proc->partial_bits / 8, 
            rctx->frame_bits_total, rctx->frame_bits_total / 8 );
    }

    // Обработка записи фрейма
    if (write_frame) {
        // Обновляем кэш для предыдущего успешного DATA-фрейма
        if (update_prev && proc->frame.frame_valid) {
            memcpy(rctx->prev_payload_buf, proc->payload_buf, payload_len);
            rctx->prev_payload_valid = 1;  // переиспользуем при вставке копий
        }

        // Вставка пропущенных DATA-фреймов, если найден разрыв между номерами
        if (fill_missing && frame_number > expected_number) {
            uint32_t missing = frame_number - expected_number;

            rctx->frame_valid_prev = 0;
            for (i = 0; i < missing; i++) {
                // Применяем политику
                if (cfg.rx.core_policy == 2 && rctx->prev_payload_valid) {
                    memcpy(rctx->miss_payload_buf, rctx->prev_payload_buf, payload_len);
                } else {
                    memset(rctx->miss_payload_buf, 0, payload_len);
                }
                // Записываем заглушки в выходной файл
                fwrite(rctx->miss_payload_buf, 1, proc->frame.hdr.payload_used, rctx->data_fp);
                rctx->out_bytes += proc->frame.hdr.payload_used;

                // Опционально: лог пропущенного фрейма
                fprintf(stderr, "\nFilling frame #%u (miss, t=%u, b=%u, d=%u, o=%u, c=N/A, g=N/A)",
                    expected_number +i, payload_class, rctx->chunk_num, proc->frame.hdr.payload_used, rctx->out_bytes);
            }
        }

        // Фрейм готов к записи!
        if (!is_duplicated) {
            // Читаем метаданные из свободного хвоста фрейма
            if (proc->frame.payload_extra > proc->frame.hdr.payload_used) {
                deserialize_metadata(&proc->frame, &rctx->meta, rctx->inf_fp, proc->payload_buf);
            }

            // Открытие файлов для записи и получение их имени
            if (!rctx->filename_got) handle_file_sessions(rctx, proc);

            // Выбрать, декодировать ли фрейм или выводить в файл
            if (!is_adts_audio || !rctx->adec.faad_handle) {
                char frame_title[64] = {0};  // заголовки к метаданным

                // Выбор выходного файла в зависимости от payload_class
                switch (payload_class / 10 * 10) {
                    case PAYLOAD_TOC:    // таблицы файлов
                        sprintf(frame_title, "\n# Class: TOC\n# Offset: %u\n",   rctx->out_bytes);
                        rctx->out_fp = rctx->inf_fp; break;
                    case PAYLOAD_EOF:    // маркер конца файла
                        sprintf(frame_title, "\n# Class: EOF\n# Offset: %u\n",   rctx->out_bytes);
                        rctx->out_fp = rctx->inf_fp; break;
                    case PAYLOAD_META:   // метаданные
                        sprintf(frame_title, "\n# Class: META\n# Offset: %u\n",  rctx->out_bytes);
                        rctx->out_fp = rctx->inf_fp; break;
                    case PAYLOAD_CALIB:  // калибровочная инфа
                        sprintf(frame_title, "\n# Class: CALIB\n# Offset: %u\n", rctx->out_bytes);
                        rctx->out_fp = rctx->inf_fp; break;
                    default:             // полезные данные
                        rctx->out_fp = rctx->data_fp;
                }
                if (frame_number == 1 && frame_title[0] != '\0') {
                fwrite(frame_title, 1, strlen(frame_title), rctx->out_fp); }  // запись META-заголовка
                fwrite(proc->payload_buf, 1, proc->frame.hdr.payload_used, rctx->out_fp);  // запись фрейма

            } else {
                // Декодирование AAC-фреймов и вывод в PCM sink
                if (aac_decoder_process(&rctx->adec, proc->payload_buf, proc->frame.hdr.payload_used) != 0) {
                    fprintf(stderr, "\nFAAD2 AAC decoder failed to process frame.\n");
                }
            }

            rctx->out_bytes += proc->frame.hdr.payload_used;
        }

        // Запомнить номера фреймов для будущих проверок дублей и пропусков
        rctx->last_written_frame = frame_number;
        if (is_data_frame) rctx->last_data_frame = frame_number;

        // Флаг конца файла (сбросим в корне состояния RX для нового файла)
        if (payload_class == PAYLOAD_EOF) rctx->eof_processed += 1;  // стерео-пара
    }

    handle_frame_end:
    // Для аудио повторяем последний успешный
    // …PCM-фрагмент прочитанного ADTS-фрейма
    if (is_adts_audio && !write_frame) {
        aac_conceal_play(&rctx->adec, -1);
    }

    // Показать информацию и обновить статистику
    if (rctx->eof_processed <= 2)  // побочки сброса
        rx_log_frame_status(rctx, proc, write_frame);
}


/* Helper для управления выходными файлами и сбросом состояний */
static void inline handle_file_sessions(RxDemodCtx *rctx, AskDemodState *proc)
{
    char *fpath_ptr;  // указатель на конец пути
    char  fname_new[256] = {0};  // новое имя файла из KV-строки
    char  fname_dir[256] = {0};  // исходная папка для новых файлов
    char  fname_out[512] = {0};  // полный путь основного файла
    char  fname_inf[512] = {0};  // новый файл для дампа метаданных

    // Корневой класс данных (без под-классов)
    uint8_t payload_class = proc->frame.hdr.payload_class / 10 * 10;

    // Для pipe-выхода файлы не используются
    if (rctx->data_fp == stdout) {
        rctx->filename_got = 1; return;  // больше не запускаем
    }

    // Получение настоящего имени файла (из метаданных)
    if (payload_class == PAYLOAD_TOC || payload_class == PAYLOAD_EOF) {
        extract_kv_value(proc->payload_buf, proc->frame.hdr.payload_used, "NAME=", fname_new);
    }
    if (fname_new[0] != '\0') {
        // отмечаем факт получения имени
        rctx->filename_got = 1;

        // извлекаем полный путь исходного файла
        sprintf(fname_dir, rctx->out_name);
        if (fpath_ptr = strrchr(fname_dir, '\\')) *fpath_ptr = '\0';
        else sprintf(fname_dir, ".");  // нет пути

        // добавляем строку пути к новому имени
        sprintf(fname_out, "%s\\%s", fname_dir, fname_new);

        // создаём файл для вывода метаданных
        sprintf(fname_inf, "%s.inf", fname_out);

        // не перезаписываем файл, если уже есть
        //if (access(fname_out, 0) == 0) {
        strcat(fname_out, ".out");  // добавим суффикс
        //}
        // закрываем открытые дескрипторы
        if (rctx->data_fp) fclose(rctx->data_fp);
        if (rctx->inf_fp)  fclose(rctx->inf_fp);
        rctx->data_fp = NULL; rctx->inf_fp = NULL;

        // переименовываем временные файлы в целевые
        remove(fname_out); rename(rctx->out_name, fname_out);
        remove(fname_inf); rename(rctx->inf_name, fname_inf);
        fprintf(stderr, "\nA new file session has been started: '%s'.\n", fname_new);

    } else {
        // имя файла не получено, используем дефолт
        sprintf(fname_out, rctx->out_name);  // данные
        sprintf(fname_inf, rctx->inf_name);  // сервис
        if (!rctx->data_fp) fprintf(stderr, "\nA new file session has been started: '%s'.\n", fname_out);
    }

    // Открыть файлы для записи, если ещё не открыты
    if (payload_class != PAYLOAD_EOF) {  // не открываем в конце
        if (cfg.rx.aac_decoding) {
            sprintf(rctx->adec.out_file, fname_out);  // WAV-sink
        }
        if (!rctx->data_fp) rctx->data_fp = fopen(fname_out, "wb");
        if (!rctx->inf_fp)  rctx->inf_fp  = fopen(fname_inf, "wb");
        if (!rctx->data_fp || !rctx->inf_fp) fprintf(stderr, "\nRX: cannot open output file '%s'.\n", fname_out);
    }
}

/* Helper для получения значений из KV-строк в TOC/META/EOF-фреймах */
static void inline extract_kv_value(const uint8_t *buf, int len, const char *key, char *dst)
{
    int i, j;
    int key_len = strlen(key);

    for (i = 0; i <= len - key_len; ++i) {
        if (memcmp(buf + i, key, key_len) == 0) {
            i += key_len; j = 0;
            while (i < len && buf[i] != '\n' && buf[i] != '\r' && j < 255) {
                dst[j++] = buf[i++];
            }
            dst[j] = '\0'; return;
        }
    }
    dst[0] = '\0';
}


/* Helper сброса состояния файла и демодуляции по окончании файла */
static void inline reset_file_session(RxDemodCtx *rctx, AskDemodState *proc, AskDemodState *proc_r)
{
    // статистика чтения фреймов
    rctx->last_data_frame = 0;
    rctx->last_written_frame = 0;
    rctx->payload_previous = -1;
    rctx->frame_valid_prev = 0;
    rctx->prev_payload_valid = 0;
    rctx->eof_processed += 1;  // хак для лога

    // состояние ASK-демодулятора
    rctx->ask.frame_bits_max = rctx->ask.frame_bits_min;
    ask_reset_rx_values(proc);
    ask_reset_rx_values(proc_r);

    // отложенные копии фреймов
    memset(&rctx->pending[0].frame, 0, sizeof(FrameDesc));
    memset(&rctx->pending[1].frame, 0, sizeof(FrameDesc));
    fprintf(log_fp, "[*] INF: Got EOF at sample %d, RX session is reset.\n", 
        proc->sample_count );
}


/* Вывод в консоль сводки по текущему фрейму */
static void inline rx_log_frame_status(RxDemodCtx *rctx, AskDemodState *proc, int is_written)
{
    /* выбрать тип фрейма */
    const char *frame_status = "good";
    const char *payload_class = "unknown";
    const char *payload_class_str[] = {
        "data", "audio", "image", "video", "toc", "meta", "eof", "calib"
    };
    const char *frame_action = is_written ? "Written" : "Skipped";

    if (proc->frame.hdr.payload_class <= 79) payload_class = payload_class_str[proc->frame.hdr.payload_class/10];
    if (proc->frame.hdr.payload_class != rctx->payload_previous || 
       !proc->frame.frame_valid || !rctx->frame_valid_prev) fprintf(stderr, "\n");

    if (!proc->frame.frame_valid)   frame_status = "fail";  // битые CRC
    if (!proc->frame.crc_hdr_valid) frame_status = "dead";  // убит заголовок
    else if (proc->frame.hdr.payload_class == 10) frame_action = "Decoded";  // AAC

    /* вывести лог фрейма */
    fprintf(stderr, 
        "\r%s frame #%u (%s, t=%s, d=%u, o=%u, g=%u,%.1f,%.1f, dr=%u%%)  ",
        frame_action,
        proc->frame.hdr.frame_number,
        frame_status,
        payload_class,
        proc->frame.hdr.payload_used,
        rctx->out_bytes,
       (proc->gain_mul+1) / 10,
        proc->eq.bias_gain_db,
        proc->eq.isi_gain_db,
        proc->dynamics_percent );
        // TODO: try %(-)(0)(#)s column formatting

    /* обновить статистику */
    if (proc->frame.crc_hdr_valid) 
    rctx->payload_previous = proc->frame.hdr.payload_class;
    rctx->frame_valid_prev = proc->frame.frame_valid;
    rctx->total_frames += 1;  // обновить счётчики
    if (proc->frame.frame_valid) rctx->valid_frames += 1;
}

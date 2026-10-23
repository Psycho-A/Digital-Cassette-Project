/* modulation.c */
/* -----------------------
 * Basic OFDM/m-ASK/PAM stuff:
 * - Value calculation helpers;
 * - Frame mod/demod functions (ASK)
 */
#include "frame.h"
#include "modulation.h"

/* DSP-обработчики */
#include "modulation_dsp.c"


/* --------------
   OFDM-модуляция
   -------------- */

/* битовая ёмкость одного символа */
int bits_per_symbol_from_qam(int qam)
{
    switch (qam) {
    case 2:   return 1;
    case 4:   return 2;
    case 8:   return 3;
    case 16:  return 4;
    case 32:  return 5;
    case 64:  return 6;
    case 128: return 7;
    case 256: return 8;
    default:  return 2;
    }
}

/* число поднесущих из диапазона частот */
int calc_active_carriers(OfdmParams *ofdm)
{
    int active_carriers = 0;
    double bin_hz = (double)ofdm->sample_rate / ofdm->fft_size;
    int low_bin = (int)(ofdm->low_freq / bin_hz);
    int high_bin = (int)(ofdm->high_freq / bin_hz);

    if (low_bin < 1) low_bin = 1;
    if (high_bin >= ofdm->fft_size/2) high_bin = (ofdm->fft_size/2) - 1;

    active_carriers = high_bin - low_bin;  // test +1?
    return (active_carriers < 8) ? 8 : active_carriers;
}

/* ёмкость полезных данных во фрейме */
int calc_payload_bytes(OfdmParams *ofdm)
{
    // найдём НОК байта и 16 символов + общую ёмкость
    int lcm_step = lcm(ofdm->bits_per_symbol * 16, 8);
    int total_bits = ofdm->bits_per_symbol * ofdm->active_carriers * ofdm->symbols_per_frame;

    // переопределение для ASK, если задано профилем
    if (ofdm->custom_bytes >= PAYLOAD_MIN) 
        total_bits = SERVICE_BITS + ofdm->custom_bytes * 8;
        total_bits = (total_bits + lcm_step-1) / lcm_step * lcm_step;  // округлим вверх до НОК

    if (total_bits <= SERVICE_BITS) return 0;
    else return (total_bits - SERVICE_BITS) / 8;
}




/* ---------------------------
   M-ASK/PAM модуляция [общее]
   --------------------------- */

/* Битрейт сырых и полезных данных при ASK-модуляции (в бит/с) */
void calc_stream_bitrate(OfdmParams *ofdm, AskParams *ask, 
    int sample_rate, int capacity_bits, int payload_bytes, int pilot_symbols)
{
    // вычислим дополнительные потери в ASK-фрейме на служебные данные:
    // преамбула на Fc/2 (preamble_symbols *= 2), 2-ASK заголовок, пилот-символы (эквиваленты бит)
    int extra_bitrate = (ask->preamble_symbols +ask->extra_symbols +pilot_symbols) * ofdm->bits_per_symbol;

    // рассчитаем полный битрейт на сыром фрейме, и битрейт полезных данных (эталонная cycle_samples)
    total_bitrate = (sample_rate / ask->cycle_samples) * (ofdm->frame_stereo +1) * ofdm->bits_per_symbol;
    payload_bitrate = total_bitrate * 1000 / ((capacity_bits + extra_bitrate) * 1000 / (payload_bytes * 8));
}


/* Общий helper для расчёта порогов уровней */
/* Вызывается в TX/RX-init и ask_finish_period_v2 */
void ask_calc_thresh_levels(AskParams *ask, uint32_t min_amp, uint32_t max_amp, 
    uint32_t *amp_levels, uint32_t *thresh_levels, int pcm_fmt)
{
    uint32_t i, one_amp;
    uint32_t step_top, step_bit;

    // определим макс. уровень в PCM-сигнале
    switch (pcm_fmt) {
        case 8:  one_amp = 127; break;
        case 16: one_amp = 32767; break;
        case 24: one_amp = 8388607; break;
        default: one_amp = 2147483647; break;
    }

    // определяем допуски вокруг каждого уровня
    step_top = one_amp - max_amp;  // область между sync и 0 дБ
    step_bit = (max_amp - min_amp) / (ask->num_levels -1);  // ширина битовых уровней

    for (i = 0; i < ask->num_levels; i++) {
        amp_levels[i] = min_amp + i * step_bit;
        thresh_levels[i] = amp_levels[i] - step_bit / 2;
    }
    // проверим, что нулевой порог не ушёл за 0 (бывает при 2-ASK)
    if (amp_levels[0] <= step_bit) thresh_levels[0] = min_amp / 4;  // четверть 0-уровня

    // амплитудные границы преамбулы (считаем отдельно)
    amp_levels[ask->num_levels +0] = max_amp;  // синхро-уровень
    amp_levels[ask->num_levels +1] = one_amp;  // 0 дБ
    thresh_levels[ask->num_levels +0] = max_amp / 4;  // четверть от синхро-уровня
    thresh_levels[ask->num_levels +1] = max_amp + step_top / 2;  // среднее между max и 0 дБ

    // выведем в лог все вычисленные уровни (для отладки)
    for (i = 0; i < ask->num_levels +2; i++) {
        fprintf(log_fp, "[+] LOG: ASK bit level %i: %i\t[thresh: %i]\n%s", 
            i, amp_levels[i], thresh_levels[i], i > ask->num_levels ? "\n" : "");
    }
}




/* ------------------------
   M-ASK/PAM модуляция [TX]
   ------------------------ */

/* Helper создания шаблонов периодов для каждого ASK-уровня */
/* Вызывается в TX после ask_calc_thresh_levels и генерирует 
   готовые шаблоны синусоидальных периодов, которые затем читаются 
   в ask_write_frame при записи каждого символа в PCM-поток */
int ask_build_wave_table(AskParams *ask)
{
    int level, i, N, H;
    int sync_lo = ask->num_levels +0;
    int sync_hi = ask->num_levels +1;
    int32_t *wave, target_amp, max_val;
    double t, phase, val, scale;

    // Выделим память (только под нужное число уровней)
    ask->wave_table = (int32_t**)calloc(ask->num_levels +2, sizeof(int32_t*));
    for (level = 0; level < sync_hi +1; ++level) {
        ask->wave_table[level] = (int32_t*)calloc(ask->cycle_samples *2, sizeof(int32_t));
        if (!ask->wave_table || !ask->wave_table[level]) return -1;  // ошибка
    }

    for (level = 0; level < sync_hi +1; ++level) {
        wave = ask->wave_table[level];
        target_amp = ask->amp_levels[level];
        N = ask->cycle_samples;  // несущая частота

        // Обрабатываем условия преамбулы (2 доп. слота)
        if (level >= sync_lo) target_amp = ask->sync_amp;   // оба слота
        if (level == sync_lo) N *= 2;  // половинная частота, 2x сэмплов

        // Генерируем положительный полупериод (симметричный)
        H = N / 2; for (i = 0; i < H; ++i) {
            t = (i - (H - 1) * 0.5) / H;
            phase = t * M_PI;  // от -pi/2 до +pi/2
            val = cos(phase);  // cos(0) = 1 (центр), cos(±pi/2) = 0 (края)
            wave[i] = (int32_t)(target_amp * val + 0.5);
        }

        // Находим максимум в положительном полупериоде
        max_val = 0; for (i = 0; i < H; ++i) {
            if (wave[i] > max_val) max_val = wave[i];
        }

        // Нормируем полупериод, чтобы максимум стал равен target_amp
        if (max_val > 0) {
            scale = (double)target_amp / max_val;
            for (i = 0; i < H; ++i) wave[i] = (int32_t)(wave[i] * scale + 0.5);
            // TODO: проводить анализ "соседа" и поднимать пик до центра по 0.7/0.3
        }

        // Отрицательный полупериод: копируем с инверсией знака
        for (i = 0; i < H; ++i) wave[H + i] = -wave[i];

        // Отладочный лог для оценки полученных уровней
        fprintf(log_fp, "[*] LOG: ASK sin level %i: amp = %d, wave = [ ", level, target_amp);
        for (i = 0; i < N; ++i) fprintf(log_fp, "%d%s", wave[i], (i < N-1) ? ", " : " ]\n");
    }

    return 0;
}


/* Helper записи M-ASK в моно/стерео:
 - DATA / MEDIA - stereo-pair write, 
 - TOC / META / EOF - dual-mono write */
int ask_write_frame(PcmSink *sink, AudioDspCtx *dsp, FrameDesc *desc, FrameDesc *desc_r, 
    AskParams *ask, uint8_t *bitbuf_L, uint8_t *bitbuf_R, uint32_t bits)
{
    int32_t  lbuf[256], rbuf[256];  // под размер 1 символа
    uint32_t sym_len, idx;
    uint32_t i, b, s, symbol = 0;

    int32_t *waveL, *waveR;  // шаблоны уровней
    uint32_t bit_max = ask->num_levels -1;
    uint32_t sync_lo = ask->num_levels +0;
    uint32_t sync_hi = ask->num_levels +1;

    uint32_t bits_per_symbol = ask->symbol_bits;
    uint32_t sync_bits     = desc->sync.preamble_len_bits;                // длина преамбулы в битах
    uint32_t sync_symbols  = sync_bits / bits_per_symbol;                 // символов в преамбуле
    int32_t  gray_mapping  = desc->hdr.symbols_bits & 0x10 ? 1 : 0;       // наличие битового кодирования Грея

    uint32_t header_extra  = ask->extra_symbols;                          // добавка от 2-ASK заголовка (при 4+ASK)
    uint32_t total_symbols = bits / bits_per_symbol + header_extra;       // включая преамбулу и 2-ASK заголовок
    uint8_t  h2ask_counter = header_extra > 0 ? SERVICE_BITS : 0;         // счётчик символов в 2-ASK заголовке
    uint8_t* bitbuf_ptr_L  = bitbuf_L + sync_bits;                        // указатели на начало данных в bitbuf_*
    uint8_t* bitbuf_ptr_R  = bitbuf_R + sync_bits;

    uint32_t pilot_counter = 0;
    uint32_t pilot_delta   = ask->pilot_interval;
    uint32_t cycle_samples = ask->cycle_samples;

    if (bits <= 0) return -1;

    if (ask->variable_len) {
        // записываем только полезную длину фрейма
        bits = sync_bits + SERVICE_BITS + desc->hdr.payload_used * 8;
        if (desc_r->hdr.payload_used > desc->hdr.payload_used) {
            // выравниваем границы фреймов по большей длине из каналов
            bits = sync_bits + SERVICE_BITS + desc_r->hdr.payload_used * 8;
        }
        total_symbols = (bits + bits_per_symbol-1) / bits_per_symbol + header_extra;
    }
    if (pilot_delta > 0) {
        // считаем кол-во пилот-символов и пополняем total_symbols
        total_symbols += (total_symbols-sync_symbols + pilot_delta-1) / pilot_delta;
        if (ask->training_symbols > 3) pilot_counter = pilot_delta;  // не пишем первым
    }

    for (i = 0; i < total_symbols; ++i) {
        // исходные частота периода и b/s...
        cycle_samples = ask->cycle_samples;
        bits_per_symbol = ask->symbol_bits;

        // определяем, является ли символ частью преамбулы
        if (i < sync_symbols) {
            // вставляем НЧ-маркер и символы тренировки
            if (i < ask->sync_train_max) {
                // снижаем частоту преамбулы в 2 раза
                waveL = waveR = ask->wave_table[sync_lo]; cycle_samples *= 2;
            } else {
                // записываем тренировочные макс./мин. уровни
                waveL = waveR = (i < ask->sync_train_min) ? ask->wave_table[bit_max] : ask->wave_table[0];
            }

        } else if (pilot_delta > 0 && pilot_counter == 0) {
            // вставляем пилот каждые N символов (в оба канала)
            waveL = waveR = ask->wave_table[sync_hi]; pilot_counter = pilot_delta;

        } else {
            // проверим наличие 2-ASK заголовка
            if (h2ask_counter > 0) bits_per_symbol = 1;  // временно меняем

            // собираем bits_per_symbol бит в 1 символ
            symbol = 0;
            for (b = 0; b < bits_per_symbol; b++) 
            symbol = (symbol << 1) | (*bitbuf_ptr_L++);  // двигаем позицию
            if (h2ask_counter > 0) symbol *= bit_max;  // масштабируем до max_amp
            if (symbol >= bit_max) symbol = bit_max;
            if (gray_mapping && !h2ask_counter) symbol = symbol ^ (symbol >> 1);  // linear -> gray
            waveL = waveR = ask->wave_table[symbol];  // шаблон уровня (сначала как моно)

            // аналогично для правого канала (если есть)
            if (desc_r->hdr.payload_used > 0) {
                symbol = 0;
                for (b = 0; b < bits_per_symbol; b++) 
                symbol = (symbol << 1) | (*bitbuf_ptr_R++);
                if (h2ask_counter > 0) symbol *= bit_max;
                if (symbol >= bit_max) symbol = bit_max;
                if (gray_mapping && !h2ask_counter) symbol = symbol ^ (symbol >> 1);
                waveR = ask->wave_table[symbol];  // перезаписываем моно-значение
            }
            // уменьшаем счётчики для следующего символа
            if (pilot_counter > 0) pilot_counter--;
            if (h2ask_counter > 0) h2ask_counter--;
        }

        // генерация PCM-сэмплов для символа
        idx = 0; for (s = 0; s < cycle_samples; ++s) {
            // накапливаем сэмплы 1 символа (DSP-оптимизация)
            lbuf[idx] = waveL[s]; rbuf[idx] = waveR[s];
            idx++; sym_len = idx; if (s == cycle_samples-1) {
                // накопили буфер до 1 символа, эквализуем и пишем в sink
                dsp_process(dsp, lbuf, rbuf, sym_len); // pre-emphasis EQ
                for (idx = 0; idx < sym_len; ++idx) {
                    sink->write_sample(sink->ctx, lbuf[idx], rbuf[idx]);
                }
            }
        }
    }

    return 0;
}

/* Helper для записи lead-in/lead-out символов (для синхронизации) */
int ask_write_lead_symbols(PcmSink *sink, AudioDspCtx *dsp, AskParams *ask, uint32_t num_symbols, int type_flag)
{
    uint32_t sym_len, idx, i, s;
    uint32_t cycle_samples = ask->cycle_samples;

    // шаблон периода с нулевым уровнем
    int32_t *wave = ask->wave_table[0];
    int32_t  wbuf[256];  // буфер под размер символа

    if (num_symbols <= 0) return -1;
    fprintf(stderr, "\nBuilding file's %s sequence (%d zero-symbols)", 
        type_flag ? "lead-out" : "lead-in", num_symbols);

    // генерация PCM-сэмплов для символа
    for (i = 0; i < num_symbols +2; ++i) {
        idx = 0; for (s = 0; s < cycle_samples; ++s) {
            // добавим 1 символ тишины в начало/конец файла
            if (type_flag == 0 && i == 0) wbuf[idx] = 0;
            else if (type_flag && i == num_symbols+1) wbuf[idx] = 0;
            else wbuf[idx] = wave[s];  // сэмпл с сигналом

            // накапливаем сэмплы символа (DSP-оптимизация)
            idx++; sym_len = idx; if (s == cycle_samples-1) {
                // накопили буфер до 1 символа, эквализуем и пишем в sink
                dsp_process(dsp, wbuf, wbuf, sym_len); // pre-emphasis EQ
                for (idx = 0; idx < sym_len; ++idx) {
                    sink->write_sample(sink->ctx, wbuf[idx], wbuf[idx]);
                }
            }
        }
    }

    return 0;
}




/* --------------------
   M-ASK модуляция [RX]
   -------------------- */
#define acfg rctx->cfg
#define adeq proc->eq

/* Настройка частотных допусков длины периода (в %) */
#define tMin  53  // нижний допуск при поиске данных (толерантно к "звону")
#define tMax 147  // верхний допуск при поиске данных (толерантно к "звону")
#define sMin  80  // нижний допуск при поиске преамбулы (более строгий)
#define sMax 120  // верхний допуск при поиске преамбулы (более строгий)
#define pMin 160  // нижний допуск отношения несущей к преамбуле
#define pMax 280  // верхний допуск отношения несущей к преамбуле

/* Макрос вычисления средней частоты (со сглаживанием) */
#define measureFreq(x,y) x ? (x * y + proc->samp_in_period * (100-y) + 1) / 100 : proc->samp_in_period

/* Макросы сравнения частот (дрейф и выход из преамбулы) */
#define isSameFreq(x,y) (x > (y * tMin) / 100 && x < (y * tMax) / 100)
#define isHalfFreq(x,y) (x > (y * pMin) / 100 && x < (y * pMax) / 100)

/* Макросы вычисления средних амплитуд (со сглаживанием) */
#define measureLevel(x,y) x ? (x / 100) * y + (peak / 100) * (100-y) : peak
#define measureMedian(x) x ? (x / 200) * 199 + (peak / 200) : peak  // замер по данным

/* Набор хаков и костылей для улучшений */
#include "modulation_fix.c"  // TODO: изменяемый!


/* Инициализация и сброс (для удобства) */
void ask_reset_rx_values(AskDemodState *proc)
{
    // детектор периодов
    proc->prev_sign = 0;
    proc->prev_sample = 0;
    proc->before_sample = 0;
    proc->was_going_up = 0;
    proc->was_going_down = 0;
    proc->period_mid = 0;
    proc->samp_in_period = 0;

    // детектор пиков
    memset(proc->period_peaks, 0, 
    sizeof(proc->period_peaks) );

    // состояние АРУ
    proc->agc_stage = 0;
    proc->gain_mul = 1000;  // = 1.0
    proc->pilot_counter = 0;
    proc->pilot_errors = 0;
    proc->amp_disp_ratio = 1000;

    // частотный детектор
    proc->period_samples = 0;
    proc->period_min = 0;
    proc->period_max = 0;
    proc->wrong_samples = 0;
    proc->probe_samples = 0;
    proc->probe_periods = 0;

    // измерения порогов
    proc->max_amp_meas = 0;
    proc->min_amp_meas = 0;
    proc->data_max_meas = 0;
    proc->data_min_meas = 0;
    proc->data_max_count = 0;
    proc->data_min_count = 0;

    // параметры фрейма
    proc->idle_periods = 0;
    proc->lost_frames = 0;
    proc->frame.hdr.frame_number = 0;
    proc->frame.payload_extra = 0;

    // преамбула/EQ/FSM
    ask_reset_preamble(proc);
    ask_reset_frame_state(proc);
    ask_adaptive_eq_reset(&proc->eq);
}


/* Helper для пересчёта ожидаемых параметров фрейма */
/* Вызов после чтения заголовка и 'CRC OK' в parse_frame_header */
void ask_calc_frame_dimensions(AskDemodCtx *rctx, AskDemodState *proc)
{
    // Динамическая смена порядка модуляции
    uint8_t new_symbol_bits = proc->frame.hdr.symbols_bits & 0x0F;
    uint16_t new_num_levels = 0;
    uint32_t total_bitrate, payload_bitrate;

    // Определим, использует ли фрейм кодирование Грея
    rctx->gray_mapping = (proc->frame.hdr.symbols_bits & 0x10) ? 1 : 0;

    if (new_symbol_bits > 0 && new_symbol_bits != acfg.symbol_bits) {
        // Вычислим порядок модуляции (int-логика)
        new_num_levels = 1 << new_symbol_bits;
        rctx->order_changed = 1;  // флаг пересчёта порогов в паре

        // Обновим контекст RX для чтения в новых фреймах
        acfg.symbol_bits = new_symbol_bits; acfg.num_levels = new_num_levels;
        acfg.extra_symbols = SERVICE_BITS - (SERVICE_BITS + new_symbol_bits-1) / new_symbol_bits;
        fprintf(stderr, "\nModulation is obtained: %d levels, %d bit/symbol, mapping=%d.\n", new_num_levels, new_symbol_bits, rctx->gray_mapping);
    }
    if (rctx->order_changed == 1) {
        // Пересчитаем пороги, чтобы не потерять фреймы и статистику
        ask_calc_thresh_levels(&acfg, acfg.min_amp, acfg.max_amp, acfg.amp_levels, acfg.thresh_levels, 32);  // эталонные
        ask_calc_thresh_levels(&acfg, proc->min_amp_meas, proc->max_amp_meas, proc->amps_frame, proc->thresh_frame, 32);  // фрейм
        rctx->order_changed = (new_num_levels >= 2) ? 1 : 0;  // сброс, если пройдена пара
    }

    // Пересчёт при получении большего payload_used или смене порядка...
    if (proc->frame_bits_actual > rctx->frame_bits_max || new_num_levels > 1) {
        // Обновим ожидаемую длину фрейма в битах (триггер конца при обрывах)
        rctx->frame_bits_max = proc->frame_bits_actual;

        // Пересчитаем число символов во фрейме (для расчёта полной длины)
        rctx->frame_symbols = (rctx->frame_bits_max / acfg.symbol_bits) + acfg.extra_symbols;
        rctx->pilot_symbols = (rctx->frame_symbols + acfg.pilot_interval -1) / acfg.pilot_interval;

        // Вычислим ожидаемый полезный битрейт потока (для показа в лог)
        if (new_num_levels >= 2) {
            total_bitrate = rctx->carrier_freq * new_symbol_bits * (cfg.tx.stereo_mode + 1);
            payload_bitrate = (total_bitrate * 92) / 100;  // 92% - грубое приближение к накладным расходам
            fprintf(stderr, "Carrier: %d Hz, Bitrate (raw/data): %d/%d bits/s.\n", rctx->carrier_freq, total_bitrate, payload_bitrate);
        }

        // Пересчитаем число периодов во фрейме (для детекта полного пропуска)
        // Используется вдвое меньшее число, т.к. мы ищем пропуски на частоте НЧ-преамбулы (Fc/2)
        rctx->frame_periods = (acfg.preamble_symbols * 2 + acfg.training_symbols + rctx->frame_symbols + rctx->pilot_symbols) / 2;
        fprintf(log_fp, "\n[%s] INF: Got new frame dimensions: %d-ASK, %d bits/symbol, %d bits, %d pilots.\n", 
            proc->channel, acfg.num_levels, acfg.symbol_bits, rctx->frame_bits_max, rctx->pilot_symbols);
    }
}


/* Основная функция обработки каждого сэмпла */
DemodResult ask_process_sample(AskDemodCtx *rctx, AskDemodState *proc, int32_t sample)
{
    int8_t sign, idx;
    int rising_cross, falling_cross;
    int new_period, valid_period;
    int32_t sample_range = 0;
    int32_t sample_frac  = 0;

    int going_up, going_down, mid;
    int peak_positive, peak_negative;

    // Предварительная коррекция EQ
    // ----------------------------
    if (proc->eq.enabled) {
        ask_adaptive_eq_process(&proc->eq, &sample);
    }

    // Детекция пиков (по перегибам)
    // -----------------------------
    going_up      = (sample >= proc->prev_sample);
    going_down    = (sample <  proc->prev_sample);
    peak_positive = (proc->was_going_up && going_down);  // зафиксирован положительный максимум
    peak_negative = (proc->was_going_down && going_up);  // зафиксирован отрицательный минимум

    // Обновление пика и двух соседних значений
    if (peak_positive || peak_negative) {
        // назначаем индекс и заполняем массивы обоих полупериодов
        idx = peak_positive ? 0 : 1;
        proc->period_peaks[idx][0] = (proc->prev_sign < 0) ? -proc->before_sample : proc->before_sample;
        proc->period_peaks[idx][1] = (proc->prev_sign < 0) ? -proc->prev_sample   : proc->prev_sample;  // пик
        proc->period_peaks[idx][2] = (proc->prev_sign < 0) ? -sample              : sample;  // сосед за пиком

        // вычислим направление фазы по отношению полупериодов
        if (proc->invert_phase == -1 && idx == 1) {
            if (proc->period_peaks[1][1] > acfg.min_amp / 16) {
                // инверсия, если отрицательный пик выше -36 дБ и громче в 2 раза
                proc->invert_phase = (proc->period_peaks[1][1] / (proc->period_peaks[0][1]+1) >= 2);
                if (proc->invert_phase >= 1) fprintf(log_fp, 
                "[%s] WRN: Phase inversion detected at sample %d (peak: %i)\n", proc->channel, proc->sample_count, proc->period_peaks[1][1]);
            }
        }
        // есть оба полупериода, вычислим средний пик и среднюю точку
        if (idx == 1 - proc->invert_phase) {
            ask_average_peak(rctx, proc);  // усреднение полупериодов с коррекцией фазы
            proc->period_mid = (proc->period_peaks[0][1] - proc->period_peaks[1][1]) / 2;
        }
    }

    // Детекция границ периодов
    // ------------------------
    mid = proc->period_mid;  // вирт. ноль (0 для абсолютного)
    sign = (sample > 0) ? 1 : (sample < 0) ? -1 : 0;

    // Флаги пересечения средней точки
    rising_cross  = (proc->prev_sample <= mid && sample > mid);
    falling_cross = (proc->prev_sample >= mid && sample < mid);
    new_period    = (proc->invert_phase >= 1) ? falling_cross : rising_cross;

    // Обработка предыдущего периода
    // -----------------------------
    if (new_period) {
        valid_period = 1;

        // Посчитаем длину периода с sub-sample интерполяцией
        sample_range = (sample - proc->prev_sample) / 100;

        // Вычисляем дробную часть пересечения mid (в сотых долях сэмпла)
        sample_frac = (sample_range != 0) ? (sample - mid) / sample_range : 50;
        if (sample_frac < 0) sample_frac = 0; else if (sample_frac > 100) sample_frac = 100;

        // Дополним предыдущий период (хвост до прошедшего crossing)
        proc->samp_in_period += (100 - sample_frac);

        // Проверим валидность длины порогами из finish_period
        if (proc->period_samples > 0) {
            if (proc->samp_in_period < proc->period_min 
            ||  proc->samp_in_period > proc->period_max) valid_period = 0;
        }
        if (valid_period) {
            // валидный период: обрабатываем нормально
            proc->wrong_samples = 0;  // сброс невалидных сэмплов
            ask_finish_period(rctx, proc);
        } else {
            // невалидный период: суммируем прошедшие сэмплы
            proc->wrong_samples += proc->samp_in_period;  // будут обработаны по таймеру
            if (proc->state == ST_FRAME && proc->wrong_samples < proc->period_max) 
                proc->wrong_samples = proc->period_max;   // фикс наползаний при выпадениях
        }
    }
    // Таймаут: при превышении частотных порогов завершаем период
    if (proc->period_samples) {
        if ((new_period && proc->wrong_samples  >= proc->period_max)  // несколько мелких периодов
        || (!new_period && proc->samp_in_period >= proc->period_max)) {  // в сигнале долго не было crossing
            // сбрасываем пики, чтобы не сбивать измерения
            memset(proc->period_peaks, 0, sizeof(proc->period_peaks));
            if (proc->state > ST_SEARCH) 
                fprintf(log_fp, "[%s] DBG: Wrong period at sample %d (%d of %d samples in period)\n", proc->channel, proc->sample_count, proc->samp_in_period, proc->period_samples);

            // принудительно завершаем сэмплы как один период
            proc->wrong_samples = 0;
            ask_finish_period(rctx, proc);
            new_period = 1;  // имитируем crossing (для возврата статуса)
        }
    }

    // Сохранение текущих значений
    // ---------------------------
    // Накопления сэмплов и амплитуд (модульно)
    proc->samp_in_period = new_period ? sample_frac : proc->samp_in_period + 100;
    proc->prev_sign = sign; proc->sample_count++;

    // Запоминаем значения состояний (знаковые)
    proc->was_going_up   = going_up;            // уровень нарастал
    proc->was_going_down = going_down;          // уровень убывал
    proc->before_sample  = proc->prev_sample;   // позапрошлый сэмпл
    proc->prev_sample    = sample;              // предыдущий сэмпл

    // Возвращаем статус фрейма по результатам
    // ---------------------------------------
    if (new_period) {
        if (proc->state == ST_FRAME) {
            if (proc->frame_bits_got == 0) return FRAME_NEW;  // сброс ожидания отложенной обработки
            if (proc->frame_bits_got == SERVICE_BITS) return FRAME_HEADER;  // прочитан заголовок, необходим парсинг

        } else if (proc->state == ST_SEARCH) {
            if (proc->frame_bits_got >= proc->frame_bits_actual) return FRAME_FULL;  // фрейм полностью собран, декодируем полностью
            if (proc->frame_bits_got >  SERVICE_BITS && !proc->frame.crc_hdr_valid) return FRAME_ABRUPT;  // битый заголовок, прерываем чтение
            if (proc->frame_bits_got >  SERVICE_BITS &&  proc->pilot_errors >= 4) return FRAME_ABRUPT;  // обрыв - прерываем, но декодируем
        }
    }
    return FRAME_NONE;
}

/* Уточняет вершину пика через параболическую интерполяцию */
inline int32_t parabolic_peak(const int32_t *s)
{
    // Операторы для вычисления параболы
    int32_t diff, num, denom, delta;
    int32_t fine_peak;  // уточнённый пик

    // Масштабируем все сэмплы в 16 бит
    int32_t y0 = s[0] >> 16;
    int32_t y1 = s[1] >> 16;
    int32_t y2 = s[2] >> 16;

    // Вычисляем знаменатель для параболы
    denom = 2 * y1 - y0 - y2;
    if (denom <= 0) return s[1];  // не пик

    // Вычисляем поправку к исходному пику
    diff  = y2 - y0;  // разность соседей
    num   = diff * diff;  // квадрат разности
    delta = num / (8 * denom);  // разница

    // Итоговый пик (с лимитом до 16 бит)
    fine_peak = y1 + delta;
    if (fine_peak >  32767) fine_peak =  32767;
    if (fine_peak < -32768) fine_peak = -32768;

    // Возвращаем, масштабируя до 32 бит
    return fine_peak << 16;
}

/* Подсчёт сред. пика по 2 полу-периодам и боковым сэмплам */
inline void ask_average_peak(AskDemodCtx *rctx, AskDemodState *proc)
{
    int32_t peak, before, beyond, i;
    int32_t avg = 71;  // вес пика (70-72%)

    // Вычисляем средние пики на полупериодах
    if (proc->samp_in_period > 100) {
        for (i = 0; i < 2; i++) {
            // копия триады сэмплов для вычислений
            before = proc->period_peaks[i][0];
            peak   = proc->period_peaks[i][1];
            beyond = proc->period_peaks[i][2];

            // виртуальный "ресемплинг" для уточнения вершины
            if (acfg.num_levels >= 4 && rctx->freq_samples < 700) {
                // новый пик выше, нужен более сильный вес (80-90%)
                avg  = rctx->carrier_freq > 10500 ? 83 : 89;  // оптимизация
                peak = parabolic_peak(&proc->period_peaks[i][0]);
                proc->period_peaks[i][1] = peak;  // обновим слот для midpoint
            }

            // получаем среднее по высшему из наклонов (коррекция фазы)
            if (before >= beyond) peak = (peak/100 * avg) + (before/100 * (100-avg));
            else if (beyond > before) peak = (peak/100 * avg) + (beyond/100 * (100-avg));
            proc->period_peaks[i][3] = peak;  // обновим слот для bit-детекции
        }
    }

    // Получаем среднее с обоих полупериодов и сохраняем в 3й слот
    peak = proc->period_peaks[0][3] / 2 + proc->period_peaks[1][3] / 2;
    proc->period_peaks[0][3] = proc->period_peaks[1][3] = peak;

    // TODO: Асимметрия полу-волн для анализа грязности канала
    /* i = (proc->period_peaks[0][1] - proc->period_peaks[1][1]) / ((proc->period_peaks[0][1] + proc->period_peaks[1][1]) / 100);
    fprintf(log_fp, "\n[%s] DBG: Peak asymmetry index (I/II): %d%% (sample: %d)\n", proc->channel, i, proc->sample_count ); */
}

/* Хелпер обновления множителя АРУ (по дрейфу или пилоту) */
inline void ask_update_agc(AskDemodCtx *rctx, AskDemodState *proc, uint32_t peak, uint32_t target, uint32_t alpha)
{
    uint32_t gain_cur = proc->gain_mul;  // текущий gain_mul, измеренный по дрейфу или пилоту

    // Вычисляем множитель по сравнению с эталонным значением
    if (peak > 2000) {  // при шумах оставляем прошлое значение gain_mul
        gain_cur = target / (peak / 1000);

        // коррекция при ручной %-поправке и лимит диапазона (±8 раз)
        if (rctx->agc_percent != 100) gain_cur = (gain_cur * rctx->agc_percent) / 100;
        if (gain_cur < 250) gain_cur = 250; else if (gain_cur > 8000) gain_cur = 8000;

        // рассчитать текущий множитель, используя коэфф. сглаживания
        if (proc->agc_stage == 0 || alpha == 0) proc->gain_mul = gain_cur; 
        else proc->gain_mul = (proc->gain_mul * alpha + gain_cur * (100-alpha)) / 100;

        // логирование статистики по каналу (только в начале фрейма)
        if (proc->pilot_counter == acfg.pilot_interval && proc->frame_bits_got < SERVICE_BITS) {
            fprintf(log_fp, "[%s] AGC: Got new gain multiplier: %d (stage: %d, alpha: %d)\n", 
                proc->channel, proc->gain_mul, proc->agc_stage, alpha );
        }
        // перевести АРУ в штатный режим работы
        if (proc->agc_stage == 1) proc->agc_stage = 2;  // gain_mul активен
    }
}


/* Завершение текущего периода (вызывается на выбранном crossing) */
static void ask_finish_period(AskDemodCtx *rctx, AskDemodState *proc)
{
    int level = -1;  // неизвестно
    int symbol = 0;  // для неизвестных значений
    int mask = 0;
    uint8_t bit = 0;  // декодированное значение
    uint32_t i = 0;

    uint32_t peak = proc->period_peaks[0][3];  // вычислено выше
    uint32_t thresh_adj[260];  // пороги, корректируемые АРУ
    uint32_t ask_levels_all = acfg.num_levels +2;  // уровни + преамбула

    uint32_t sync_freq_hz;  // частота преамбулы в Гц, для логов
    uint32_t period_len = (proc->samp_in_period + 50) / 100;  // для логов
    const char *ch = proc->channel;  // тег стерео-канала, для логов

    // Индексы синхро-сигналов (для детекции и АРУ)
    int bit_max = acfg.num_levels -1;  // максимальный битовый уровень
    int sync_lo = acfg.num_levels +0;  // нижний порог синхро-уровня
    int sync_hi = acfg.num_levels +1;  // верхний порог синхро-уровня
    int is_sync_level  = 0;  // флаг, что пик вписывается в синхро-пороги
    int is_pilot_level = 0;  // флаг, что пилот вписывается в синхро-пороги
    int alpha, thresh_step, amp_percent = 0;  // вес АРУ, ширина/края порога

    // Флаг, что ведётся чтение 2-ASK заголовка
    int is_2ask_header = (acfg.extra_symbols && proc->frame_bits_got < SERVICE_BITS);
    int uses_gray_code = (rctx->gray_mapping && !is_2ask_header);  // используется код Грея
    uint16_t symbol_bits = is_2ask_header ? 1 : acfg.symbol_bits;  // временный контекст

    if (proc->samp_in_period == 0) return;

    // Корректируем пороги детекции по АРУ
    for (i = 0; i < ask_levels_all; i++) {
        if (proc->agc_stage == 0) {
            // сначала копируем эталонные значения
            proc->amps_frame[i] = acfg.amp_levels[i];
            proc->thresh_frame[i] = acfg.thresh_levels[i];
            // на начальной стадии занижаем пороги
            thresh_adj[i] = acfg.thresh_levels[i] / 8;  // -24 дБ
        } else {
            // после тренировки или измерений АРУ используем множитель
            if (proc->agc_stage == 1) thresh_adj[i] =  proc->thresh_frame[i];
            if (proc->agc_stage >= 2) thresh_adj[i] = (proc->thresh_frame[i] / proc->gain_mul) * 1000;
        }
    }

    // Определим, в какой битовый порог попадает текущий пик
    if (peak > noise) {
        for (i = 0; i < acfg.num_levels; i++) {
            if (peak >= thresh_adj[i]) level = i;  // 0 -> ask_levels(1)
        }
        // определим, попадает ли текущий пик в пороги синхро-уровней
        is_sync_level  = (peak >= thresh_adj[sync_lo] && peak < INT32_MAX);  // макс.
        is_pilot_level = (peak >= thresh_adj[sync_lo] && peak < thresh_adj[sync_hi]);
    }

    // TODO: Костыли для M-ASK на лентах
    // ---------------------------------
    if (rctx->workarounds) ask_apply_workarounds(rctx, proc);  // набор хаков
    else {  // умолчания при авто-режимах:
        if (rctx->agc_percent == -1) rctx->agc_percent = 100;  // степень коррекции АРУ
        if (proc->eq.isi_deadzone == -1) proc->eq.isi_deadzone = 0.10;  // толерантность к звону
        if (proc->eq.bias_deadzone == -1) proc->eq.bias_deadzone = 0.35;  // толерантность к АЧХ
    }

    // Глобальная подстройка порогов по дрейфу уровня
    // ----------------------------------------------
    alpha = rctx->agc_smoothing >= 0 ? rctx->agc_smoothing : 50;  // дефолтное сглаживание

    if (level >= 0) {
        // вычислим положение пика относительно центра
        thresh_step = (thresh_adj[bit_max] - thresh_adj[0]) / bit_max;  // линейная разность
        amp_percent = (peak - thresh_adj[level]) / (thresh_step / 100) - 50;  // баланс в +/-%

        // выполним коррекцию данных фрейма (эвристика)
        if (proc->state == ST_FRAME) {
            // ступенька после обновления EQ: форсируем уровень
            if (proc->frame_bits_got < 2) level = alpha = 0;  // первые биты всегда 0
            else {
                // подстройка АРУ и коррекция магнитных ошибок в уровнях
                ask_correct_level_drift(rctx, proc, &level, &alpha, bit_max, peak, thresh_adj, thresh_step, amp_percent);
            }
        }
        // пересчитаем пороги с АРУ, чтобы пик оказался в центре
        ask_update_agc(rctx, proc, peak, proc->amps_frame[level], alpha);
        for (i = 0; i < ask_levels_all; i++) {
            thresh_adj[i] = (proc->thresh_frame[i] / proc->gain_mul) * 1000;
        }
    }

    // Обработка в зависимости от состояния автомата
    // ---------------------------------------------
    switch (proc->state) {
        case ST_SEARCH:
            // Считаем idle-периоды (потери фреймов)
            if (rctx->freq_samples) proc->idle_periods++;  // если уже был захват

            // Захват частоты данных при слепом поиске
            if (ask_probe_frequency(rctx, proc, level)) break;  // выход при успехе

            // Есть синхро, пробуем захватить преамбулу
            if (is_sync_level) {
                // считаем захваченные символы
                proc->preamble_got++;

                // меряем среднюю частоту периодов
                proc->sync_samples = measureFreq(proc->sync_samples, 75);

                // считаем средний пик НЧ-синхроуровня (для EQ)
                if (proc->preamble_got == 1) proc->sync_amp_meas = peak;
                else proc->sync_amp_meas = measureLevel(proc->sync_amp_meas, 90);  // 75?

                // захвачен НЧ-маркер, переходим к ВЧ-части!
                if (proc->preamble_got >= rctx->preamble_detect) {
                    proc->state = ST_PREAMBLE;  // стадия тренировки порогов и EQ
                    proc->period_samples = rctx->freq_samples;  // следом идёт несущая, повысим фильтр до Fc
                    fprintf(log_fp, "[%s] INF: Possible preamble at sample %d (%d samples in period)\n", ch, proc->sample_count, period_len);
                }
            } else {
                // сброс при любом несинхро-периоде
                ask_reset_preamble(proc);  // статистика

                // сброс состояний при слишком долгой потере данных
                if (proc->idle_periods >= rctx->frame_periods) {
                    fprintf(log_fp, "[%s] ERR: Frame is missing at sample %u (lost %u periods)\n", ch, proc->sample_count, proc->idle_periods *2);
                    proc->idle_periods = 0;
                    proc->lost_frames++;
                    if (rctx->reset_frames > 0) {
                        // превышен порог lost_frames
                        if (proc->lost_frames >= rctx->reset_frames) {
                            fprintf(log_fp, "[%s] ERR: Demod state reset due to too many lost data.\n", ch);
                            fprintf(stderr, "\nReading error! RX state reset due to too many lost data (%d frames in %s-ch).\n", proc->lost_frames, ch);
                            rctx->freq_samples = 0;  // сброс накоплений
                            rctx->carrier_freq = cfg.ask.carrier_freq;  // вернём несущую
                            rctx->frame_bits_max = rctx->frame_bits_min;  // вернём минимум
                            ask_reset_rx_values(proc);  // сбросим все proc-состояния
                        }
                    }
                }
            }
            break;

        case ST_PREAMBLE:
            // НЧ-маркер захвачен, читаем ВЧ-часть
            proc->preamble_got++;  // досчитываем символы

            // вычислим текущую частоту, накапливая усреднение
            if (proc->preamble_got == rctx->preamble_detect + 1) 
                // если уже задана, используем вместо сырой (защита от ISI)
                proc->probe_samples = measureFreq(rctx->freq_samples, 50);
                else proc->probe_samples = measureFreq(proc->probe_samples, 50);

            // убедимся, что частота НЧ-маркера была ~вдвое ниже
            if (isHalfFreq(proc->sync_samples, proc->probe_samples)) {
                // работа с тренируемыми порогами
                if (proc->preamble_got <= acfg.sync_train_max) {
                    // считаем средний пик "единичных" символов
                    proc->max_amp_meas = measureLevel(proc->max_amp_meas, 95);  // ~2 фрейма
                        break;  // продолжим накопление в следующем периоде

                } else if (proc->preamble_got <= acfg.sync_train_min) {
                    // считаем средний пик "нулевых" символов
                    proc->min_amp_meas = measureLevel(proc->min_amp_meas, 95);

                    // замеряем первый и дальний нулевые пики после amp_max
                    if (proc->eq.enabled) {  // de-ring коррекция (ISI)
                        if (proc->preamble_got == acfg.sync_train_max +1) proc->isi_meas_first = peak;
                        if (proc->preamble_got == acfg.sync_train_max +4) proc->isi_meas_steady = peak;
                    }
                    // уровни накоплены, вычисляем новые пороги для фрейма
                    if (proc->preamble_got == acfg.sync_train_min) {
                        if (ask_calc_frame_thresholds(rctx, proc) == 0) {
                            // в правильной преамбуле не может случиться (ложный захват)
                            fprintf(log_fp, "[%s] ERR: Invalid level thresholds at sample %d (%d, %d)\n", ch, proc->sample_count, proc->min_amp_meas, proc->max_amp_meas);
                            goto preamble_fail;  // перейти к сбросу и вернуться в поиск...
                        }
                    } else 
                        break;  // ещё не накопили...
                }

                // сохраним или обновим несущую частоту
                if (!isSameFreq(rctx->freq_samples, proc->probe_samples)) 
                rctx->freq_samples = proc->probe_samples;  // ещё не задана / сменилась
                rctx->carrier_freq = rctx->sample_rate * 100 / rctx->freq_samples;

                // сбрасываем состояние и переходим к фрейму
                proc->state = ST_FRAME;
                proc->idle_periods = 0;
                proc->probe_samples = 0;
                proc->pilot_counter = acfg.pilot_interval;  // запускаем отсчёт пилотов
                proc->period_samples = rctx->freq_samples;  // выставляем фильтр на поиск данных
                sync_freq_hz = 100 * rctx->sample_rate / proc->sync_samples;  // частота НЧ-маркера в Гц
                fprintf(log_fp, "[%s] INF: Preamble finished at sample %d (frequency = %d Hz)\n\n", ch, proc->sample_count, sync_freq_hz);
                fprintf(log_fp, "[%s] INF: Frame started at sample %d (expects %d bits)\n", ch, proc->sample_count +1, rctx->frame_bits_max);

            } else {
                // частота НЕ поднялась или изменилась (это помехи)
                preamble_fail:
                proc->state = ST_SEARCH;
                proc->period_samples = rctx->freq_samples * 2;  // будем искать новую преамбулу
                fprintf(log_fp, "[%s] WRN: Preamble reset at sample %d (%d samples in period)\n", ch, proc->sample_count, period_len);
            }

            proc->pilot_errors = 0;
            proc->bitbuf_pos = 0;
            proc->frame_bits_got = 0;
            proc->frame_bits_actual = rctx->frame_bits_min;
            ask_reset_preamble(proc);
            break;

        case ST_FRAME:
            // Внутри фрейма, вычислим символ периода...
            for (i = 0; i < acfg.num_levels; i++) {
                if (peak >= thresh_adj[i]) symbol = i;
            }

            // Измеряем пороги на чередовании уровней
            if (rctx->correct_dynamics > 0 && level >= 0) {
                if (symbol == 0 && proc->prev_symbol == bit_max) {
                    proc->data_min_meas = measureMedian(proc->data_min_meas);
                    proc->data_min_count++;  // считаем число измерений
                } else if (symbol == bit_max && proc->prev_symbol == 0) {
                    proc->data_max_meas = measureMedian(proc->data_max_meas);
                    proc->data_max_count++;  // нужно не менее training_symbols
                }
            }
            proc->prev_symbol = symbol;  // сохраняем последний символ
            proc->prev_symbol_pos = amp_percent;  // позиция символа

            // При 2-ASK заголовке нормализуем символ к 0 и 1
            if (is_2ask_header) symbol /= (acfg.num_levels / 2);

            // Проверим тип символа (данные/пилот)
            if (proc->pilot_counter > 0) {
                // Это данные, раскладываем биты символа в буфер
                if (uses_gray_code) {
                    // обратное преобразование Грея (4+ASK)
                    mask = symbol; while (mask >>= 1) symbol ^= mask;
                }
                mask = 1 << (symbol_bits - 1);
                for (i = 0; i < symbol_bits; i++) {
                    bit = (symbol & mask) ? 1 : 0; mask >>= 1;
                    proc->bitbuf[proc->bitbuf_pos++] = bit;
                    proc->frame_bits_got++;  // считаем биты
                }
                // Уменьшаем счётчик пилот-символов, если активен
                if (proc->pilot_counter > 0) proc->pilot_counter--;

                // Накоплены биты заголовка, будет возвращён статус
                if (proc->frame_bits_got == SERVICE_BITS) {
                    fprintf(log_fp, "[%s] INF: Frame header at sample %d (received %d bits)\n", ch, proc->sample_count, proc->frame_bits_got);
                }
                // У фрейма битый заголовок - возвращаемся не дочитывая
                if (proc->frame_bits_got >  SERVICE_BITS && !proc->frame.crc_hdr_valid) {
                    proc->state = ST_SEARCH;  // будем искать следующую преамбулу
                    fprintf(log_fp, "[%s] ERR: Frame header broken at sample %d (size %d bits)\n\n", ch, proc->sample_count, proc->frame_bits_got-1);
                }
                // Накоплена актуальная ёмкость фрейма, завершаем фрейм
                if (proc->frame_bits_got >= proc->frame_bits_actual) {
                    // продлим чтение, если соседний канал имел ёмкость больше
                    if (proc->frame_bits_actual < proc->frame_bits_pair) {
                        fprintf(log_fp, "[%s] INF: Frame extended to neighbor capacity (added %d bits)\n", ch, proc->frame_bits_pair-proc->frame_bits_actual);
                        proc->frame_bits_actual = proc->frame_bits_pair;  // ёмкость большего канала
                    } else {
                        proc->state = ST_SEARCH;  // ищем преамбулу (как выше)
                        fprintf(log_fp, "[%s] INF: Frame finished at sample %d (received %d bits)\n\n", ch, proc->sample_count, proc->frame_bits_got);
                        // сигнал о готовности будет возвращён из process_sample
                    }
                }

            } else {
                // Это синхро-импульс – не добавляем биты
                proc->pilot_counter = acfg.pilot_interval;
                if (is_pilot_level) {
                    proc->pilot_errors = 0;  // сбрасываем счётчик ошибок
                    if (rctx->agc_enabled) {  // делаем поправку усиления
                        ask_update_agc(rctx, proc, peak, proc->amps_frame[sync_lo], 30); // alpha
                    }
                } else if (proc->frame_bits_got > SERVICE_BITS) {
                    proc->pilot_errors++;  // накапливаем несовпадения уровня
                    if (proc->pilot_errors > 10) {  // ошибок много (~50 мс), фрейм оборван!
                        proc->state = ST_SEARCH; proc->partial_bits = proc->frame_bits_got;
                        fprintf(log_fp, "[%s] ERR: Frame abrupted at sample %d (received %d bits)\n\n", ch, proc->sample_count, proc->partial_bits);
                    }
                }
            }

            // Измерение частоты (меж-канальное)
            if (level >= 0 && period_len > 2) {
                // фильтруем разбросы с инерцией (защита от уползания)
                rctx->freq_samples = measureFreq(rctx->freq_samples, 96); // 95?
                rctx->carrier_freq = rctx->sample_rate * 100 / rctx->freq_samples;

                // обновим фильтр частоты (при поиске преамбулы снижаем до Fc/2)
                proc->period_samples = proc->state != ST_SEARCH ? rctx->freq_samples : rctx->freq_samples * 2;
            }
            break;
    }

    // Сброс для следующего периода
    // ----------------------------
    memset(proc->period_peaks, 0, sizeof(proc->period_peaks));  // пик
    // вычислим новые частотные пороги для фильтра
    if (proc->period_samples > 0) {
        // при поиске преамбулы - более строгая отсечка ложных срабатываний
        proc->period_min = proc->period_samples * (proc->state == ST_SEARCH ? sMin : tMin) / 100;
        proc->period_max = proc->period_samples * (proc->state == ST_SEARCH ? sMax : tMax) / 100;
    }
}


/* Сброса накоплений преамбулы в finish_period */
inline void ask_reset_preamble(AskDemodState *proc)
{
    // сброс измерений при ошибке
    if (proc->state == ST_SEARCH 
    && (proc->preamble_got || proc->lost_frames > 3)) {
        proc->max_amp_meas  = proc->min_amp_meas   = 0;
        proc->data_min_meas = proc->data_min_count = 0;
        proc->data_max_meas = proc->data_max_count = 0;
    }
    // сброс остальной статистики
    proc->preamble_got = proc->partial_bits = 0;
    proc->sync_samples = 0;
    proc->prev_symbol = -1;
    proc->prev_symbol_pos = 0;
}

/* Сброс состояния после захвата фрейма */
void ask_reset_frame_state(AskDemodState *proc)
{
    proc->state = ST_SEARCH;
    proc->frame_bits_got = 0;
    proc->frame_bits_pair = 0;
    proc->bitbuf_pos = 0;
    proc->idle_periods = 0;
    proc->lost_frames = 0;

    // пилоты и пробники
    proc->probe_samples = 0;
    proc->probe_periods = 0;
    proc->pilot_counter = 0;
    proc->pilot_errors = 0;

    // коррекция звона (EQ)
    proc->sync_amp_meas = 0;
    proc->isi_meas_first = 0;
    proc->isi_meas_steady = 0;
    proc->dynamics_percent = 0;

    // остальные параметры (период, амплитуда) остаются для непрерывности
}
/* modulation_dsp.c */
/* -----------------------
 * DSP-обработка при демодуляции сэмплов:
 * - Адаптивный эквалайзер для m-ASK;
 * - TODO: Символьно-частотный эквалайзер для OFDM
 */
#include "modulation.h"


/* -----------------------------
   m-ASK: Адаптивный эквалайзер
   (работает по анализу отношения
    боковых полос на преамбуле)
   ---------------------------- */

/* Инициализация */
int ask_adaptive_eq_init(AskAdaptiveEq *eq, int32_t sample_rate, const char *ch)
{
    int32_t carrier_freq = 0;
    double  new_preamp_db = 0.0;
    double  rumble_filter_hz, rumble_filter_q, rumble_filter_db;

    if (!eq || !cfg.rx.preprocess_eq) return 0;
    eq->enabled = cfg.rx.preprocess_eq ? 1 : 0;

    // Копирование важных полей
    eq->sample_rate = sample_rate;  // для вычислений частоты
    eq->cycle_samples = cfg.ask.cycle_samples;  // для вычисления несущей
    eq->preamp_db = cfg.rx.eq_preamp_db;  // общее предусиление сигнала (без учёта коррекции)
    eq->preamp_mode = cfg.rx.eq_preamp_fine;  // алгоритм предусиления (простой и точный по biquad)
    eq->gain_init_max = cfg.rx.eq_gain_max_step_db;  // макс. лимит при применении начального усиления
    eq->gain_alpha = cfg.rx.eq_gain_alpha;  // вес среднего при сглаживании усилений (0 = off)
    eq->dump_samples = cfg.rx.eq_dump_samples;  // дамп эквализованных сэмплов в файл (отладка)

    eq->bias_adaptive = cfg.rx.eq_adaptive_bias;  // включение динамического расчёта усиления shelf-фильтра
    eq->bias_deadzone = cfg.rx.eq_bias_deadzone;  // смещение при измерении НЧ/ВЧ-sync (толерантность)

    eq->isi_adaptive = cfg.rx.eq_adaptive_isi;  // включение динамического расчёта peak_hz и gain_db
    eq->isi_upside_pos = cfg.rx.eq_isi_upside_pos;  // позиция peak-фильтра на верхней боковой
    eq->isi_peak_q = cfg.rx.eq_isi_filter_q;  // добротность peak-фильтра на верхней боковой
    eq->isi_deadzone = cfg.rx.eq_isi_deadzone;  // смещение при измерении ratio звона (для плохих лент)
    eq->isi_gain_db = 0.0;  // начальное ослабление звона (по умолчанию отключено)

    // Определение несущей частоты (начальной)
    carrier_freq = cfg.ask.carrier_freq;  // из конфига
    if (carrier_freq * 2 > sample_rate || carrier_freq < 2000) {
        carrier_freq = sample_rate / eq->cycle_samples;  // умолчание, если неправильная
    }

    // Параметры rumble-компенсации ленты (low-shelf)
    rumble_filter_hz = 315;  // частота Dolby (верхний порог шума)
    rumble_filter_q  = Q_BUTTERWORTH;  // для плавного наклона и фазы
    rumble_filter_db = cfg.rx.eq_rumble_filter_db;  // 0..-20 дБ

    // Параметры bias-коррекции выше 2-3 кГц (high-shelf)
    eq->bias_filter_hz = 9000;  // частота перегиба (1/2 усиления)
    switch (cfg.rx.eq_bias_profile) {  // профили коррекции:
        case 1:  eq->bias_gain_db = 12.0; eq->bias_filter_q = 0.43; break;  // бюджетный Type I (43/50)
        case 2:  eq->bias_gain_db = -6.0; eq->bias_filter_q = 0.15; break;  // нативная запись Type II
        case 3:  eq->bias_gain_db = -9.0; eq->bias_filter_q = 0.15; break;  // запись Type II как Type I
        case 4:  eq->bias_gain_db = -6.0; eq->bias_filter_q = 0.15; break;  // нативная запись Type IV
        default: eq->bias_gain_db =  0.0; eq->bias_filter_q = 0.50; break;  // нет (только авто-коррекция)
    }
    if (eq->bias_adaptive == 1) eq->bias_gain_db /= 2;  // начальное усиление в адаптивном режиме
    if (eq->bias_adaptive >= 2) eq->bias_gain_db = 0.0;  // в peak-режиме не включаем (TODO)

    // Параметры ISI-коррекции (звон верхней БП, peak)
    eq->isi_peak_hz = carrier_freq * eq->isi_upside_pos;  // ширина ВБП = Fc

    // Инициализация Rumble-фильтра (фиксированная)
    biquad_init_lowshelf(&eq->pre_low, sample_rate, rumble_filter_hz, rumble_filter_q, rumble_filter_db);  // de-emphasis НЧ-грязи

    // Инициализация Bias-коррекции (фиксированная/динамическая)
    biquad_init_highshelf(&eq->pre_high, sample_rate, eq->bias_filter_hz, eq->bias_filter_q, eq->bias_gain_db);  // исправление ВЧ-кривой/ISI

    // Инициализация De-Ringing фильтра (с начальными значениями)
    // TODO: Q пика работает неадекватно на некоторых исходных ЧД (48k,54k etc)!
    biquad_init_peak(&eq->isi_peak, sample_rate, eq->isi_peak_hz, eq->isi_peak_q, eq->isi_gain_db);  // peak-версия коррекции

    // Начальное и рабочее предусиление (с защитой от клиппинга)
    new_preamp_db  = eq->preamp_db - biquad_gain_at_freq(&eq->pre_high, carrier_freq, sample_rate);  // поправка на bias-коррекцию
    eq->preamp_lin = new_preamp_db != 0.0 ? pow(10.0, new_preamp_db / 20.0) : 1.0;  // линейное значение (обновляемое)

    // Обнулим дефолтную статистику
    fprintf(stderr, "Initializing m-ASK Adaptive EQ [%s, Fp = %.0f Hz]...\n", ch, eq->isi_peak_hz);
    eq->channel = ch;
    eq->preamp_init = eq->preamp_lin;
    eq->bias_gain_init = eq->bias_gain_db;
    eq->bias_gain_avg = eq->isi_gain_avg = 0;
    eq->bias_gain_done = 0;
    eq->isi_prev_freq = carrier_freq;

    return 0;
}


/* Обработка сэмплов в ask_process_sample */
inline void ask_adaptive_eq_process(AskAdaptiveEq *eq, int32_t *sample)
{
    int16_t out16;  // raw-дамп сэмплов

    // Временно работаем в double [TODO]...
    double sample_f = (double)*sample / INT32_MAX;

    // Предусиление общего уровня записи
    if (eq->preamp_lin != 1.0) sample_f *= eq->preamp_lin;

    // Пред-обработка анти-рокот shelf-фильтром
    sample_f = biquad_process_sample(&eq->pre_low,  sample_f);  // rumble

    // Коррекция bias-спада и ISI (рассчитанная)
    sample_f = eq->bias_adaptive >= 2 ? 
               biquad_process_sample(&eq->isi_peak, sample_f) :  // peak curve
               biquad_process_sample(&eq->pre_high, sample_f);   // shelf curve

    // Конвертируем сэмпл обратно в int32_t...
    *sample = (int32_t)(sample_f * INT32_MAX);

    // Отладка: запись обработанных сэмплов
    if (eq->dump_samples) {
        // приводим к 16 бит для удобства
        out16 = (int16_t)(sample_f * 32767.0);
        fwrite(&out16, 2, 1, eq->raw_samples);
    }
}


/* Helper для расчёта предусиления сигнала с учётом усиления фильтров */
inline void ask_adaptive_eq_calc_preamp(AskAdaptiveEq *eq, int peak_update, double bias_delta, uint32_t carrier_freq)
{
    // Усиления на несущей
    double new_preamp_db = 0.0;
    double bias_gain_at_fc = 0.0;

    // Применим расчёт, если требуется
    if (peak_update || bias_delta != 0.0) {
        switch (eq->preamp_mode) {
            default:
            case 0:  // упрощённая версия (эмпирическая)
                new_preamp_db  = eq->preamp_db -(0.75 * eq->bias_gain_db);
                eq->preamp_lin = pow(10.0, new_preamp_db / 20.0);  // новое линейное значение
                break;
            case 1:  // точный расчёт (с учётом частот фильтров)
                bias_gain_at_fc = eq->bias_adaptive >= 2 ? 
                                  biquad_gain_at_freq(&eq->isi_peak, carrier_freq, eq->sample_rate) : 
                                  biquad_gain_at_freq(&eq->pre_high, carrier_freq, eq->sample_rate);
                new_preamp_db   = eq->preamp_db - bias_gain_at_fc;  // компенсация
                eq->preamp_lin  = pow(10.0, new_preamp_db / 20.0);  // обновляем значение
                break;
        }
    }
    // Защита от ухода в крайности
    if (eq->preamp_lin > 10.0) eq->preamp_lin = 10.0;  // +20 дБ максимум
    if (eq->preamp_lin < 0.10) eq->preamp_lin = 0.10;  // -20 дБ минимум
}

/* Подбор ISI peak-фильтра по анализу звона в периодах:
 * выполняется по выходу из ST_FRAME (до новой преамбулы) */
inline void ask_adaptive_eq_select(AskAdaptiveEq *eq, int32_t carrier_freq, 
    uint32_t meas_lf, uint32_t meas_hf, uint32_t amp_first, uint32_t amp_steady)
{
    double adapt_step = 0.5;  // шаг в дБ
    double gain_alpha = eq->gain_alpha;
    double gain_init_max = eq->gain_init_max;
    int    peak_needs_update = 0;

    double isi_ratio = 1.0;
    double isi_gain_delta = 0.0;
    double isi_deadzone = eq->isi_deadzone;
    double isi_affect = 0.0;

    double bias_ratio = 1.0;
    double bias_gain_delta = 0.0;
    double bias_deadzone = eq->bias_deadzone;

    if (!eq || !eq->enabled) return;

    // Оценка уровня звона/эхо в символах
    if (eq->isi_adaptive) {
        // убедимся, что оба периода прочитаны
        if (amp_first > noise && amp_steady > noise) {
            // вычислим звон в первом периоде (после спада)
            isi_ratio = (double)amp_first / amp_steady;

            // звон: ослабляем усиление, пока ratio не достиг нормы
            if (isi_ratio > 1.0 +isi_deadzone) isi_gain_delta = adapt_step;
        }
    }

    // Перестройка shelf-фильтра bias-коррекции
    if (eq->bias_adaptive) {
        // sync_amp_meas и max_amp_meas уже измерены в ask_finish_period
        if (meas_lf > noise && meas_hf > noise) {
            // вычислим разницу АЧХ между НЧ и ВЧ синхро-уровнями
            bias_ratio = (double)meas_lf / meas_hf;

            // посчитаем начальное усиление (в дБ)
            if (!eq->bias_gain_done) {
                if (!eq->isi_adaptive) {
                    if (bias_ratio != 1.0) bias_gain_delta = 20.0 * log10(bias_ratio) * 2.0;  // 2.0 - поправка
                } else {
                    isi_affect = 1 / (isi_ratio * 0.85);  // немного смягчим влияние звона (компенсация частоты)
                    if (bias_ratio <= isi_affect) bias_gain_delta = 20.0 * log10(bias_ratio) * 2.0;  // по АЧХ
                    if (bias_ratio >  isi_affect) bias_gain_delta = 20.0 * log10(isi_affect) * 2.0;  // по звону
                }
            }
            // далее изменяем усиление, пока знак ошибки не сменится
            else if (isi_gain_delta > 0.0) bias_gain_delta = -adapt_step;  // звон/эхо > ослабить ВЧ
            else if (bias_ratio > 1.0 +bias_deadzone) bias_gain_delta =  adapt_step;  // ВЧ слабее > усиливаем
            else if (bias_ratio < 1.0 -bias_deadzone) bias_gain_delta = -adapt_step;  // ВЧ громче > ослабляем

            // при сильном звоне замораживаем усиление
            if (bias_gain_delta != 0.0) {
                // защита порогов при вызове из преамбулы
                if (bias_gain_delta >  gain_init_max) bias_gain_delta =  gain_init_max;
                if (bias_gain_delta < -gain_init_max) bias_gain_delta = -gain_init_max;

                // обновим усиление текущей разницей
                eq->bias_gain_db += bias_gain_delta;

                // ограничение диапазона: ослабление требуется реже
                if (eq->bias_gain_db > 18.0) eq->bias_gain_db = 18.0;  // try 20
                if (eq->bias_gain_db < -9.0) eq->bias_gain_db = -9.0;

                // сглаживание перепадов усиления
                if (gain_alpha > 0.01) {
                    if (!eq->bias_gain_done) eq->bias_gain_avg = eq->bias_gain_db;
                    else eq->bias_gain_avg = eq->bias_gain_avg * gain_alpha + eq->bias_gain_db * (1.0-gain_alpha);
                    eq->bias_gain_db = eq->bias_gain_avg;  // копируем усреднение
                }

                // пересчитаем коэффициенты фильтра
                if (eq->bias_adaptive >= 2) peak_needs_update = 1;  // V2, обновляется с частотой
                else biquad_init_highshelf(&eq->pre_high, eq->sample_rate, eq->bias_filter_hz, eq->bias_filter_q, eq->bias_gain_db);
            }
            eq->bias_gain_done = 1;  // начальный режим установлен
        }
    }

    // Обновим частоту peak-фильтра, если изменилась
    if (eq->bias_adaptive >= 2) {  // только для peak-режима
        if (carrier_freq > 3000 && carrier_freq < 18000) {  // ограничим диапазон
            // пересчёт будет совершён при девиации > 5%
            if (abs(carrier_freq - eq->isi_prev_freq) >= 500) {
                // считаем новую частоту на ВБП по неё новой ширине и Fc
                eq->isi_peak_hz = carrier_freq * eq->isi_upside_pos;

                // защита от превышения sample_rate/2 (снизим позицию)
                if (eq->isi_peak_hz * 2.0 >= eq->sample_rate) {
                    eq->isi_peak_hz = 0.45 * eq->sample_rate;
                }
                // включим обновление и запомним частоту до следующего фрейма
                peak_needs_update = 1; eq->isi_prev_freq = carrier_freq;
            }
        }
    }

    // Перестраиваем peak-фильтр, если изменилось усиление/частота
    if (peak_needs_update) {
        biquad_init_peak(&eq->isi_peak, eq->sample_rate, eq->isi_peak_hz, eq->isi_peak_q, eq->bias_gain_db);
    }
    // в bias-merge режиме логируем ratio (вместо усиления)
    if (eq->isi_adaptive) eq->isi_gain_db = isi_ratio;

    // Также скорректируем итоговое выходное предусиление (3 варианта расчёта)
    ask_adaptive_eq_calc_preamp(eq, peak_needs_update, bias_gain_delta, carrier_freq);

    // Отсигналим в лог об изменениях параметров
    if (peak_needs_update || bias_gain_delta != 0.0) {
        fprintf(log_fp, "[%s] DEQ: Got new filter params: %.0f Hz, %.1f dB (ratios: %.2f/%.2f, amp: %.2f)\n\n", 
            eq->channel, eq->isi_peak_hz, eq->bias_gain_db, bias_ratio, isi_ratio, eq->preamp_lin);
    }
}


/* Сброс статистики и усилений (при ошибках) */
void ask_adaptive_eq_reset(AskAdaptiveEq *eq)
{
    if (eq->enabled) {
        // статистика и усиления
        eq->bias_gain_avg = 0.0;
        eq->bias_gain_done = 0;
        eq->isi_gain_db = 0.0;
        eq->isi_gain_avg = 0.0;
        eq->isi_prev_freq = 0;

        // возврат начальных значений
        eq->bias_gain_db = eq->bias_gain_init;
        eq->preamp_lin = eq->preamp_init;

        // коэффициенты фильтров
        biquad_reset(&eq->pre_low);
        biquad_reset(&eq->pre_high);
        biquad_reset(&eq->isi_peak);
    }
}

/* Закрытие дампов, освобождение ресурсов */
void ask_adaptive_eq_free(AskAdaptiveEq *eq)
{
    if (!eq) return;
    if (eq->raw_samples) fclose(eq->raw_samples);  // закрыть дампы
    memset(eq, 0, sizeof(AskAdaptiveEq));  // обнулить все данные
}

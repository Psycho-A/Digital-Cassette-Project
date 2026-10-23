/* modulation_fix.c */
/* -------------------
 * Здесь собраны утилиты, костыли и хаки RX, которые 
 * носят временный характер или часто переделываются:
 * - ask_probe_frequency - начальный замер частоты (re-sync);
 * - ask_calc_frame_thresholds - вычисление порогов в преамбуле;
 * - ask_apply_workarounds - набор хаков для мода параметров;
 *
 * Важно:
 * - Файл должен быть включен только текстуально в 
 *   общий "modulation.c" перед основными RX-функциями!
 */


/* --------------------
   M-ASK модуляция [RX]
   -------------------- */

/* Хелпер замера частоты при слепом поиске/resync */
inline int ask_probe_frequency(AskDemodCtx *rctx, AskDemodState *proc, int32_t level)
{
    int reset_state = 1;  // флаг сброса

    // частота уже известна!
    if (rctx->freq_samples > 0) {
        // переключим фильтр на поиск преамбулы (Fc/2)
        proc->period_samples = rctx->freq_samples * 2;

    } else if (level >= 0) {
        // частота ещё неизвестна
        if (proc->probe_samples == 0) {
            // задаём начальную частоту...
            proc->probe_samples = proc->samp_in_period;
            proc->probe_periods = 1; reset_state = 0; // начинаем копить

        } else if (isSameFreq(proc->samp_in_period, proc->probe_samples)) {
            // частота стабильна, усредняем с предыдущей
            proc->probe_samples = measureFreq(proc->probe_samples, 50);
            proc->probe_periods++; reset_state = 0;

            // накопили достаточно периодов одной частоты!
            if (proc->probe_periods >= rctx->data_periods_max) {
                // задаём несущую и фильтр поиска преамбулы (1/2 несущей)
                rctx->freq_samples = proc->probe_samples;
                rctx->carrier_freq = rctx->sample_rate * 100 / rctx->freq_samples;
                proc->period_samples = rctx->freq_samples * 2; // Fc/2
                fprintf(log_fp, "\n[%s] INF: Established new data frequency at sample %d (%d Hz)\n", proc->channel, proc->sample_count, rctx->carrier_freq);

                // инициализируем фильтры AEQ реальной частотой
                /* if (proc->eq.enabled) {
                    ask_adaptive_eq_select(&proc->eq, rctx->carrier_freq, 0,0,0,0);  // не трогаем усиления
                } */

                // сбросим ложные преамбулы и вернём код выхода
                ask_reset_preamble(proc); return 1;
            }
        } else {  // частота изменилась - сброс ложных преамбул
            ask_reset_preamble(proc);  // захват будет начат заново...
        }
    }
    // изменился уровень/частота, или частота уже задана!
    if (reset_state) proc->probe_periods = proc->probe_samples = 0;

    return 0;
}


/* Хелпер для вычисления и коррекции порогов уровней и ДД (TODO) */
inline int ask_calc_frame_thresholds(AskDemodCtx *rctx, AskDemodState *proc)
{
    // Локальные переменные
    int32_t  alpha = rctx->correct_dynamics;  // степень коррекции ДД порогов
    uint32_t amp_ref_ratio,  amp_meas_ratio;  // max/min из конфига и тренировочный
    uint32_t amp_data_ratio, amp_corr_ratio;  // max/min на данных и степень сжатия
    uint32_t amp_max_corrected = 0;  // скорректированное значение max-уровня

    // Если уровни невалидны - оставляем старые значения!
    if (proc->min_amp_meas > noise 
    && (proc->max_amp_meas > proc->min_amp_meas +noise)) {
        // корректируем max-уровень замерами реального ДД фрейма
        amp_meas_ratio = proc->max_amp_meas / (proc->min_amp_meas / 1000);  // отношение тренировочных уровней
        amp_max_corrected = proc->max_amp_meas;  // исходное значение (из тренировки выше)

        if (proc->data_min_count >= acfg.training_symbols 
        && (proc->data_max_count >= acfg.training_symbols)) {
            amp_data_ratio = proc->data_max_meas / (proc->data_min_meas / 1000);  // измеренное отношение на данных
            // если замеры валидны, применяем коррекцию (max ±25% от тренировочных)
            if (amp_data_ratio > amp_meas_ratio * 3/4 && amp_data_ratio < amp_meas_ratio * 4/3) {
                proc->amp_disp_ratio = (amp_data_ratio * 1000) / amp_meas_ratio;  // уровень дисперсии
                amp_corr_ratio = (proc->amp_disp_ratio * alpha + 1000 * (100-alpha)) / 100;  // коэфф. коррекции
                amp_max_corrected = (proc->max_amp_meas / 1000) * amp_corr_ratio;  // скорректированное значение
            }
        }
        // итоговые уровни получены, вычисляем новые пороги
        fprintf(log_fp, "[%s] AGC: Got new level thresholds for frame:\n", proc->channel);
        ask_calc_thresh_levels(&acfg, proc->min_amp_meas, amp_max_corrected, proc->amps_frame, proc->thresh_frame, 32);

        // обновим de-emphasis EQ и ISI-коррекцию
        if (proc->eq.enabled) {
            ask_adaptive_eq_select(&proc->eq, rctx->carrier_freq, 
                proc->sync_amp_meas, proc->max_amp_meas, 
                proc->isi_meas_first, proc->isi_meas_steady);
        }
        // переводим АРУ в режим готовности
        proc->agc_stage = 1; proc->gain_mul = 1000;

        // также проверяем сжатие ДД в текущей записи (для калибровок)
        if (rctx->measure_dynamics) {
            amp_ref_ratio = acfg.max_amp / (acfg.min_amp / 1000);  // эталонное отношение уровней (из конфига)
            proc->dynamics_percent = (amp_meas_ratio * 100) / amp_ref_ratio;  // качество дин. диапазона в процентах
            rctx->dynamics_average += proc->dynamics_percent; rctx->dynamics_counter++;  // накопление среднего ДД за сессию

            // сообщить о слишком низком ДД (плохая лента)...
            if (proc->dynamics_percent < 75) {  // TODO: использовать в PAYLOAD_CALIB-парсере!
                fprintf(log_fp, "[%s] WRN: Current dynamic range is too low (%d%% of original).\n", proc->channel, proc->dynamics_percent);
            }
        }
        // корректируем нелинейность ДД в порогах (для 4-ASK)
        if (acfg.num_levels == 4 && proc->amp_disp_ratio != 1000) {
            // дефолтная нелинейность (ОК в большинстве случаев)
            proc->thresh_frame[1] = proc->thresh_frame[1] / 100 * 98;  // 98%
            proc->thresh_frame[2] = proc->thresh_frame[2] / 100 * 99;  // 99%
            if (rctx->correct_nonlinear)  // TODO: вычислять нелинейность пропорционально проценту ДД?
                fprintf(stderr, "\n[%s] Frame bounds to trained bounds ratio = %d\n", proc->channel, proc->amp_disp_ratio);  // debug
        }
    }
    return amp_max_corrected;  // проверка ошибки
}


/* Эвристический метод борьбы со "звоном" и насыщением */
/* Корректирует пики на основе их отношений и положения в порогах */
inline void ask_correct_level_drift(AskDemodCtx *rctx, AskDemodState *proc, 
    int *level, int *alpha, int bit_max, uint32_t peak, uint32_t *thresh_adj, int32_t thresh_step, int32_t amp_percent)
{    // вычислим уровень "недоверенной" зоны (плохие биты)
    int32_t margin = (thresh_step / 100) * rctx->drift_margin;

    // подскажем макс. уровень при чтении 2-ASK заголовка
    if (proc->frame_bits_got < SERVICE_BITS) {
        if (*level > 1 && *level < bit_max) *level = bit_max;
    }

    // коррекция символа по его надёжности и влиянию ISI...
    // TODO: добавить учёт bias_gain_db или amp_disp_ratio в принятии решений!
    if (rctx->drift_margin > 0 && amp_percent < 100) {
        // проверим попадание пика в "недоверенные" края порога
        if (peak < thresh_adj[*level] +margin && *level > 0) {
            // пик очень близко к нижнему краю - исправляем насыщение
            if ((proc->prev_symbol == *level   && proc->prev_symbol_pos > amp_percent +65)    // символ тот же, но уплыл на 65+%: возможен звон
            || ( proc->prev_symbol == *level-1 && proc->prev_symbol_pos > amp_percent +75)    // символ слабо заплыл на порог (<25%): резкий дрейф
         /* || ( proc->prev_symbol >  *level   && proc->prev_symbol_pos > amp_percent +85) */ )  // символ упал ниже 85+%, но размыт: возможен звон (TODO)
            {
                *level -= 1;  // понизим уровень до исправленного
                fprintf(stderr, "\n[%s] WRN: Wobbly symbol at sample %d (prev/cur/exp: %i/%i>%i, health %i/%i%%)\n", 
                    proc->channel, proc->sample_count, proc->prev_symbol, *level+1, *level, amp_percent, proc->prev_symbol_pos);
            }
        }
        else if (peak > thresh_adj[*level+1] -margin && *level < bit_max) {
            // пик очень близко к верхнему краю - исправляем размытие
            if ((proc->prev_symbol == *level   && proc->prev_symbol_pos < amp_percent -65)    // символ тот же, но прыгнул на 65+%: возможен звон
            || ( proc->prev_symbol == *level+1 && proc->prev_symbol_pos < amp_percent -75)    // символ слабо уплыл с порога (<25%): резкий дрейф
            || ( proc->prev_symbol <  *level   && proc->prev_symbol_pos < amp_percent -85) )  // символ возрос на 85+%, но сплюснут: возможен звон
            {
                *level += 1;  // повысим уровень до исправленного
                fprintf(stderr, "\n[%s] WRN: Wobbly symbol at sample %d (prev/cur/exp: %i/%i>%i, health %i/%i%%)\n", 
                    proc->channel, proc->sample_count, proc->prev_symbol, *level-1, *level, amp_percent, proc->prev_symbol_pos);
            }
        }
    }

    // балансируем сглаживание АРУ по джиттеру в уровнях
    if (rctx->agc_smoothing < 0) {
        // ускорим адаптацию к границам порогов
        if (*level == bit_max) *alpha = 30;  // alt = 33%
        else {
            // чем дальше пик от центра, тем меньше его влияние
            *alpha = 49 + (amp_percent < 0 ? -amp_percent : amp_percent) / 5;  // alt = 50,6
            if (*alpha > 85) *alpha = 85;  // плохому пику оставим 15% веса
        }
    }
}


/* Набор костылей и хаков для улучшений (TODO: временный) */
/* Зависит от носителя и условий записи, подобрано эмпирически! */
inline void ask_apply_workarounds(AskDemodCtx *rctx, AskDemodState *proc)
{
    if (rctx->freq_samples < 250) return;

    // мёртвая зона коррекции АЧХ
    if (cfg.rx.eq_bias_deadzone == -1) {
        proc->eq.bias_deadzone = 0.35;  // умолчание
        if (proc->amp_disp_ratio > 1050) {
            // коррекция при высокой дисперсии
            proc->eq.bias_deadzone = 0.40 - (proc->amp_disp_ratio / 1000.0 - 1.0);
            if (proc->eq.bias_deadzone < 0.25) proc->eq.bias_deadzone = 0.25;  // меньше = хуже
        }
    }
    // поправка к gain_mul (TODO, не нужно для 2-ASK)
    if (cfg.ask.agc_percent == -1) {
        rctx->agc_percent = 100;  // умолчание
        if (acfg.num_levels >= 4 && proc->eq.bias_gain_db > 6.5) {
            // каждые 1 кГц после 9 кГц скручиваем по 1%
            rctx->agc_percent = 100 - (rctx->carrier_freq + 250) / 1000 + 9;
            if (rctx->agc_percent < 95)  rctx->agc_percent = 95;   // min 95%
            if (rctx->agc_percent > 100) rctx->agc_percent = 100;  // max 100%
        }
    }
    // коррекция толерантности к "звону"
    if (cfg.rx.eq_isi_deadzone == -1) {
        proc->eq.isi_deadzone = 0.10;  // умолчание
        if (acfg.num_levels == 2) {
            // для 2-ASK лучше больший допуск для ISI
            proc->eq.isi_deadzone = 0.12;  // +2%
        }
        else if (proc->eq.isi_adaptive) {
            // для 4+ASK в режиме bias-merge большая мёртвая зона вредна
            proc->eq.isi_deadzone = 0.03;  // оптимальный минимум
            // эмпирические подстройки (TODO: зависят от сигнала!)
            if (rctx->carrier_freq >  3500 && rctx->carrier_freq <  7700) proc->eq.isi_deadzone = 0.05;  // ~<7 kHz
            if (rctx->carrier_freq >  9500 && rctx->carrier_freq < 10500) proc->eq.isi_deadzone = 0.08;  // ~10 kHz
            if (rctx->carrier_freq > 11500 && rctx->carrier_freq < 13500) proc->eq.isi_deadzone = 0.11;  // ~12 kHz
        }
    }
    /* fprintf(stderr, "\n[%s] DBG: Got new params: disperse=%d, alpha=%d, agc=%d%%, deadzones=%.2f/%.2f\n", 
        proc->channel, proc->amp_disp_ratio, rctx->agc_smoothing, rctx->agc_percent, proc->eq.bias_deadzone, proc->eq.isi_deadzone ); */
}

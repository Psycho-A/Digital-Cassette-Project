/* rx_dsp_denoise.c
 *
 * Denoise with HPF-cascades (C89)
 * - Supports cascaded HPF (order = 2,4,8..) by repeating identical 2nd-order HPF sections
 * - Works with Q24 samples in-place (l[], r[])
 *
 * API:
 *   ctx = dsp_denoise_init(ctx, cfg);
 *   dsp_denoise_process(l, r, n, ctx);
 *   dsp_denoise_reset(ctx, cfg);
 *   dsp_denoise_free(ctx);
 *
 * Undoc:
 * - Can be used as a Compander in TX_ANALOG recording:
 *   reduction_level_db and enhance_peaks_db signs must be inverted,
 *   and rest of other values *must* have exact matching.
 *
 * Notes:
 * - All internal arithmetic uses int32/int64 with Q24 scaling.
 * - Uses biquad_i from dsp_filters.h.
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspDenoiseCtx {
    /* config */
    int enabled;
    int rumble_enabled;

    /* число каскадов фильтров */
    int filter_n_sections;   /* filter_order / 2 */
    int rumble_n_sections;

    /* линейные усиления в Q24 */
    int32_t reduction_lin;   /* reduction gain (Q24) */
    int32_t enhance_lin;     /* enhancement gain (Q24) */

    /* коэффициенты биквадов (целочисленные) */
    biquad_i *hpL;      /* HPF анализ */
    biquad_i *hpR;
    biquad_i *shelfL;   /* high-shelf для применения */
    biquad_i *shelfR;
    biquad_i *rumbleL;
    biquad_i *rumbleR;

    /* предрасчитанные параметры для shelf (общие для всех секций) */
    double shelf_cs;       /* cos(omega) per section */
    double shelf_alpha;    /* alpha per section */
    double shelf_inv_den;  /* 1/(2*n_sections), используется для расчёта */

    /* RMS окно – храним квадраты в Q48 как int64 */
    int64_t *rms_window_buf_L;
    int64_t *rms_window_buf_R;
    uint32_t rms_window_len;
    uint32_t rms_window_pos;
    int64_t  rms_window_sum_L;
    int64_t  rms_window_sum_R;

    /* пороги в масштабе Q48 (умножены на DSP_ONE^2) */
    int64_t tiny_sum;      /* очень маленький порог (например -80 дБ MS) * wlen */
    int64_t red_low_sum;   /* нижняя граница knee для reduction * wlen */
    int64_t red_high_sum;  /* верхняя граница knee для reduction * wlen */
    int64_t enh_low_sum;   /* нижняя граница knee для enhancement * wlen */
    int64_t enh_high_sum;  /* верхняя граница knee для enhancement * wlen */

    /* флаги для оптимизации */
    int has_smooth_red;  /* reduction_lin != DSP_ONE && knee_width > 0 */
    int has_smooth_enh;  /* enhance_lin != DSP_ONE && knee_width > 0 */

    /* состояние для плавных переходов gain */
    uint32_t attack_samples;    /* precomputed from attack_ms */
    uint32_t release_samples;
    int32_t  total_lin_L_cur;   /* текущий gain (Q24) */
    int32_t  total_lin_R_cur;
    int32_t  total_lin_L_step;  /* шаг за сэмпл (Q24) */
    int32_t  total_lin_R_step;
    int32_t  last_target_L;     /* последний целевой gain (Q24) */
    int32_t  last_target_R;
    uint32_t rem_samples_L;     /* remaining samples in ramp */
    uint32_t rem_samples_R;

    /* предыдущие значения для избежания пересчёта коэффициентов */
    int32_t total_lin_L_prev, total_lin_R_prev;
    int32_t nb0_L_prev, nb1_L_prev, nb2_L_prev, na1_L_prev, na2_L_prev;
    int32_t nb0_R_prev, nb1_R_prev, nb2_R_prev, na1_R_prev, na2_R_prev;

    /* указатель на функцию обработки */
    void (*process)(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);
};


/* пороги в масштабе Q48 (для сравнения с суммами квадратов) */
#define ms_threshold_q48(db) ((int64_t)(pow(10.0, (db)/10.0) * DSP_ONE * DSP_ONE + 0.5))

/* some forwards for calling by init */
void dsp_denoise_free(struct DspDenoiseCtx*);
void dsp_denoise_process_stereo(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);
void dsp_denoise_process_mono(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);

/* Инициализация (расчёты в Q24) */
/* caller must fill cfg fields first (attack_ms, etc) */
struct DspDenoiseCtx* dsp_denoise_init(struct DspDenoiseCtx *ctx, DspDenoiseConfig cfg, int sample_rate)
{
    int i; biquad_d btmp;

    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP Denoise [int] module...\n");
    if (!ctx) ctx = (struct DspDenoiseCtx*)calloc(1, sizeof(struct DspDenoiseCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Denoise memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;
    ctx->rumble_enabled = (cfg.rumble_filter_order >= 2) ? 1 : 0;

    /* compute valid filters order */
    ctx->filter_n_sections = cfg.filter_order / 2;
    if (ctx->filter_n_sections < 1) ctx->filter_n_sections = 1; // guard

    /* pre-compute linear gains in Q24 */
    ctx->reduction_lin = (int32_t)(pow(10.0, cfg.reduction_level_db / 20.0) * DSP_ONE + 0.5);
    ctx->enhance_lin = (int32_t)(pow(10.0, cfg.enhance_peaks_db / 20.0) * DSP_ONE + 0.5);
    if (ctx->reduction_lin <= 0) ctx->reduction_lin = 1;  // guard clamp
    if (ctx->enhance_lin <= 0) ctx->enhance_lin = 1;      // guard clamp

    /* cache invalid markers */
    ctx->total_lin_L_prev = DSP_ONE;
    ctx->total_lin_R_prev = DSP_ONE;

    /* smoothing flags to skip excess calcs */
    ctx->has_smooth_red = (ctx->reduction_lin != DSP_ONE && cfg.knee_width_db > 0.0) ? 1 : 0;
    ctx->has_smooth_enh = (ctx->enhance_lin != DSP_ONE && cfg.knee_width_db > 0.0) ? 1 : 0;

    /* allocate biquad HPF sections per channel */
    ctx->hpL = (biquad_i*)calloc(ctx->filter_n_sections, sizeof(biquad_i));
    ctx->hpR = (biquad_i*)calloc(ctx->filter_n_sections, sizeof(biquad_i));
    if (!ctx->hpL || !ctx->hpR) {
        fprintf(stderr, "Failed to allocate DSP Denoise HPF sections.\n");
        dsp_denoise_free(ctx); return NULL;
    }
    /* initialize each section as identical HPF (same f0/Q) */
    for (i = 0; i < ctx->filter_n_sections; ++i) {
        biquad_init_highpass(&btmp, sample_rate, cfg.cutoff_freq_hz, cfg.filter_q);
        biquad_int_from_double(&ctx->hpL[i], &btmp, DSP_ONE);
        biquad_int_from_double(&ctx->hpR[i], &btmp, DSP_ONE);
    }

    /* initialize rumble high-pass filter */
    if (ctx->rumble_enabled) {
        ctx->rumble_n_sections = (int)cfg.rumble_filter_order / 2;
        if (ctx->rumble_n_sections < 1) ctx->rumble_n_sections = 1;

        ctx->rumbleL = (biquad_i*)calloc(ctx->rumble_n_sections, sizeof(biquad_i));
        ctx->rumbleR = (biquad_i*)calloc(ctx->rumble_n_sections, sizeof(biquad_i));
        if (!ctx->rumbleL || !ctx->rumbleR) {
            fprintf(stderr, "Failed to allocate DSP Denoise rumble sections.\n");
            dsp_denoise_free(ctx); return NULL;
        }
        for (i = 0; i < ctx->rumble_n_sections; ++i) {
            biquad_init_highpass(&btmp, sample_rate, cfg.rumble_cutoff_hz, Q_BUTTERWORTH);
            biquad_int_from_double(&ctx->rumbleL[i], &btmp, DSP_ONE);
            biquad_int_from_double(&ctx->rumbleR[i], &btmp, DSP_ONE);
        }
    }

    /* initialize the high-shelf cascade:
       - create per-section shelf arrays and distribute total reduction_db evenly across sections
       - per_section_db = total_db / n_sections */
    ctx->shelfL = (biquad_i*)calloc(ctx->filter_n_sections, sizeof(biquad_i));
    ctx->shelfR = (biquad_i*)calloc(ctx->filter_n_sections, sizeof(biquad_i));
    if (!ctx->shelfL || !ctx->shelfR) {
        fprintf(stderr, "Failed to allocate DSP Denoise shelf sections.\n");
        dsp_denoise_free(ctx); return NULL;
    }

    /* compute shelf per-section denominator */
    ctx->shelf_inv_den = 1.0 / (2.0 * (double)ctx->filter_n_sections);
    if (ctx->shelf_inv_den <= 0.0) ctx->shelf_inv_den = 1.0; // guard against zero sections

    /* pre-compute common shelf's omega/sn/cs/alpha */
    {
        double omega = DSP_TWO_PI * cfg.cutoff_freq_hz / sample_rate;
        ctx->shelf_cs = cos(omega);
        ctx->shelf_alpha = sin(omega) / (2.0 * (double)cfg.filter_q);
    }

    /* выделяем RMS буферы для каналов
       (храним квадраты в Q48 как int64) */
    {
        uint32_t wlen = 0;
        double rms_periods = cfg.rms_window_cycles;
        if (rms_periods && cfg.cutoff_freq_hz) {
            /* compute RMS window by number of $cutoff_hz freq periods */
            wlen = (uint32_t)(rms_periods * (double)sample_rate / cfg.cutoff_freq_hz + 0.5);
        } else {
            /* otherwise, use legacy formula based on attack_ms/4 time */
            wlen = (uint32_t)(cfg.attack_ms * 0.001 * (double)sample_rate * 0.25 + 0.5);
        }
        if (wlen < 4) wlen = 4;  // guard
        ctx->rms_window_len = wlen;
        ctx->rms_window_pos = 0;
        ctx->rms_window_sum_L = 0;
        ctx->rms_window_sum_R = 0;
        ctx->rms_window_buf_L = (int64_t*)calloc(wlen, sizeof(int64_t));
        ctx->rms_window_buf_R = (int64_t*)calloc(wlen, sizeof(int64_t));
        if (!ctx->rms_window_buf_L || !ctx->rms_window_buf_R) {
            fprintf(stderr, "Failed to allocate DSP Denoise RMS windows.\n");
            dsp_denoise_free(ctx); return NULL;
        }
    }

    /* пороги RMS в масштабе Q48 (MS) */
    {
        double tiny_db = -80.0;  // no-noise marker
        double red_low_db  = cfg.noise_threshold_db - cfg.knee_width_db * 0.5;
        double red_high_db = cfg.noise_threshold_db + cfg.knee_width_db * 0.5;
        double enh_low_db  = cfg.peaks_floor_db - cfg.knee_width_db * 0.5;
        double enh_high_db = cfg.peaks_floor_db + cfg.knee_width_db * 0.5;

        /* масштабированные пороги */
        ctx->tiny_sum     = ms_threshold_q48(tiny_db) * ctx->rms_window_len;
        ctx->red_low_sum  = ms_threshold_q48(red_low_db) * ctx->rms_window_len;
        ctx->red_high_sum = ms_threshold_q48(red_high_db) * ctx->rms_window_len;
        ctx->enh_low_sum  = ms_threshold_q48(enh_low_db) * ctx->rms_window_len;
        ctx->enh_high_sum = ms_threshold_q48(enh_high_db) * ctx->rms_window_len;
    }

    /* инициализация ramp-состояний */
    ctx->rem_samples_L = ctx->rem_samples_R = 0;
    ctx->total_lin_L_cur = ctx->total_lin_R_cur = DSP_ONE;
    ctx->total_lin_L_step = ctx->total_lin_R_step = 0;
    ctx->last_target_L = ctx->last_target_R = DSP_ONE;

    /* pre-compute attack/release sample counts */
    ctx->attack_samples  = (uint32_t)( (cfg.attack_ms  * 0.001 * sample_rate) + 0.5 );
    ctx->release_samples = (uint32_t)( (cfg.release_ms * 0.001 * sample_rate) + 0.5 );
    if (ctx->attack_samples < 1) ctx->attack_samples = 1;  // guard
    if (ctx->release_samples < 1) ctx->release_samples = 1;  // guard

    /* выбор функции обработки */
    if (cfg.linked_stereo)
    ctx->process = dsp_denoise_process_mono;         // common computing for both channels
    else ctx->process = dsp_denoise_process_stereo;  // per-channel computing

    return ctx;  // fill ctx->denoise
}


/* The core processing loop
   Inputs: l[], r[] contain samples already converted to Q24
   Output: l[], r[] replaced with processed Q24 samples
   NOTE: we apply cascade high-shelf multiplicatively to the full input signal!
*/
void dsp_denoise_process(struct DspDenoiseCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    /* process with selected branch */
    ctx->process(ctx, l, r, n);
}


/* версия с независимой по-канальной обработкой
   -------------------------------------------- */
void dsp_denoise_process_stereo(struct DspDenoiseCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, s;
    int32_t total_lin_L, total_lin_R;
    int32_t nb0, nb1, nb2, na1, na2;
    int32_t gain_diff;

    /* предзагружаем часто используемые переменные */
    uint32_t wlen = ctx->rms_window_len;
    uint32_t wpos = ctx->rms_window_pos;
    int64_t *rms_window_buf_L = ctx->rms_window_buf_L;
    int64_t *rms_window_buf_R = ctx->rms_window_buf_R;
    int64_t  rms_window_sum_L = ctx->rms_window_sum_L;
    int64_t  rms_window_sum_R = ctx->rms_window_sum_R;

    uint32_t filter_nsec = ctx->filter_n_sections;
    uint32_t rumble_nsec = ctx->rumble_n_sections;
    uint32_t rumble_enabled = ctx->rumble_enabled;
    uint32_t smoothing_enabled = (ctx->attack_samples > 1 || ctx->release_samples > 1);

    biquad_i *hpL = ctx->hpL;
    biquad_i *hpR = ctx->hpR;
    biquad_i *shelfL = ctx->shelfL;
    biquad_i *shelfR = ctx->shelfR;
    biquad_i *rumbleL = ctx->rumbleL;
    biquad_i *rumbleR = ctx->rumbleR;

    int32_t reduction = ctx->reduction_lin;
    int32_t enhance   = ctx->enhance_lin;
    int32_t has_smooth_red = ctx->has_smooth_red;
    int32_t has_smooth_enh = ctx->has_smooth_enh;

    int64_t tiny_sum = ctx->tiny_sum;
    int64_t red_low_sum  = ctx->red_low_sum;
    int64_t red_high_sum = ctx->red_high_sum;
    int64_t enh_low_sum  = ctx->enh_low_sum;
    int64_t enh_high_sum = ctx->enh_high_sum;

    double cs = ctx->shelf_cs;
    double alpha = ctx->shelf_alpha;
    double inv_den = ctx->shelf_inv_den;

    int32_t cur_gain_L = ctx->total_lin_L_cur;
    int32_t cur_gain_R = ctx->total_lin_R_cur;
    int32_t last_target_L = ctx->last_target_L;
    int32_t last_target_R = ctx->last_target_R;
    uint32_t remL = ctx->rem_samples_L;
    uint32_t remR = ctx->rem_samples_R;
    int32_t stepL = ctx->total_lin_L_step;
    int32_t stepR = ctx->total_lin_R_step;

    for (i = 0; i < n; ++i) {
        int32_t hp_l, hp_r;
        int64_t sqL, sqR;
        int64_t mean_ms_L, mean_ms_R;
        int32_t out_l, out_r;

        int32_t xl = l[i];
        int32_t xr = r[i];

        /* 1. HF анализ через HPF */
        hp_l = xl; hp_r = xr;
        for (s = 0; s < filter_nsec; ++s) {
            hp_l = biquad_process_int(&hpL[s], hp_l, 24);
            hp_r = biquad_process_int(&hpR[s], hp_r, 24);
        }

        /* 2. Квадраты HF (Q48) */
        sqL = (int64_t)hp_l * (int64_t)hp_l;
        sqR = (int64_t)hp_r * (int64_t)hp_r;

        /* 3. Обновление RMS окон */
        /* вычитаем старый элемент */
        rms_window_sum_L -= rms_window_buf_L[wpos];
        rms_window_sum_R -= rms_window_buf_R[wpos];
        /* добавляем новый */
        rms_window_buf_L[wpos] = sqL;
        rms_window_buf_R[wpos] = sqR;
        rms_window_sum_L += sqL;
        rms_window_sum_R += sqR;
        /* двигаем позицию */
        wpos++; if (wpos >= wlen) wpos = 0;

        /* средние значения (MS) – храним как сумму, делить не будем, а при сравнении умножим порог на wlen */
        mean_ms_L = rms_window_sum_L;
        mean_ms_R = rms_window_sum_R;

        /* 4. Определение целевого gain */
        total_lin_L = DSP_ONE;
        total_lin_R = DSP_ONE;

        /* левый канал */
        if (mean_ms_L > tiny_sum) {
            float t; int32_t t_q24;
            if (mean_ms_L <= red_low_sum) {
                total_lin_L = reduction;
            } else if (mean_ms_L >= enh_high_sum) {
                total_lin_L = enhance;
            } else if (has_smooth_red && mean_ms_L > red_low_sum && mean_ms_L < red_high_sum) {
                // линейная интерполяция в MS-домене [uses 1 float due to int64 overflow!]
                t = ((float)red_high_sum - (float)mean_ms_L) / ((float)red_high_sum - (float)red_low_sum);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5);
                total_lin_L = (int32_t)(((int64_t)reduction * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            } else if (has_smooth_enh && mean_ms_L > enh_low_sum && mean_ms_L < enh_high_sum) {
                t = ((float)mean_ms_L - (float)enh_low_sum) / ((float)enh_high_sum - (float)enh_low_sum);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5);
                total_lin_L = (int32_t)(((int64_t)enhance * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            }
        }
        /* правый канал (аналогично) */
        if (mean_ms_R > tiny_sum) {
            float t; int32_t t_q24;
            if (mean_ms_R <= red_low_sum) {
                total_lin_R = reduction;
            } else if (mean_ms_R >= enh_high_sum) {
                total_lin_R = enhance;
            } else if (has_smooth_red && mean_ms_R > red_low_sum && mean_ms_R < red_high_sum) {
                t = ((float)red_high_sum - (float)mean_ms_R) / ((float)red_high_sum - (float)red_low_sum);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5);
                total_lin_R = (int32_t)(((int64_t)reduction * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            } else if (has_smooth_enh && mean_ms_R > enh_low_sum && mean_ms_R < enh_high_sum) {
                t = ((float)mean_ms_R - (float)enh_low_sum) / ((float)enh_high_sum - (float)enh_low_sum);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5);
                total_lin_R = (int32_t)(((int64_t)enhance * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            }
        }

        /* 5. Сглаживание gain (ramp) */
        if (smoothing_enabled) {
            /* левый */
            gain_diff = total_lin_L - last_target_L;
            if (gain_diff < 0) gain_diff = -gain_diff;

            if (remL == 0 || gain_diff > 1) {
                last_target_L = total_lin_L;  // record current goal

                if (total_lin_L < cur_gain_L) remL = ctx->release_samples;
                else remL = ctx->attack_samples;

                if (remL == 0) remL = 1;  // precaution
                stepL = (total_lin_L - cur_gain_L) / (int32_t)remL;  // целочисленное деление
            }
            if (remL > 0) {
                cur_gain_L += stepL;
                remL--;
                if (remL == 0) {
                    cur_gain_L = total_lin_L;
                    stepL = 0;  // TODO: re-added
                }
            }

            /* правый */
            gain_diff = total_lin_R - last_target_R;
            if (gain_diff < 0) gain_diff = -gain_diff;
            if (remR == 0 || gain_diff > 1) {
                last_target_R = total_lin_R;
                if (total_lin_R < cur_gain_R) remR = ctx->release_samples;
                else remR = ctx->attack_samples;
                if (remR == 0) remR = 1;
                stepR = (total_lin_R - cur_gain_R) / (int32_t)remR;
            }
            if (remR > 0) {
                cur_gain_R += stepR;
                remR--;
                if (remR == 0) {
                    cur_gain_R = total_lin_R;
                    stepR = 0;  // TODO: re-added
                }
            }

            // TODO: re-added
            /* use the smoothed values for next processing */
            total_lin_L = cur_gain_L;
            total_lin_R = cur_gain_R;

        //} else {
        //    cur_gain_L = total_lin_L;
        //    cur_gain_R = total_lin_R;
        }

        /* 6. Применение shelf к сигналу */
        out_l = xl; out_r = xr;

        /* левый канал */
        // TODO: cur_gain_* -> total_lin_*
        if (total_lin_L != DSP_ONE) {
            if (total_lin_L != ctx->total_lin_L_prev) {
                compute_shelf_coeffs_int(total_lin_L, inv_den,cs,alpha, &nb0,&nb1,&nb2, &na1,&na2, DSP_ONE);
                ctx->total_lin_L_prev = total_lin_L;
                ctx->nb0_L_prev = nb0; ctx->nb1_L_prev = nb1; ctx->nb2_L_prev = nb2;
                ctx->na1_L_prev = na1; ctx->na2_L_prev = na2;
            } else {
                nb0 = ctx->nb0_L_prev; nb1 = ctx->nb1_L_prev; nb2 = ctx->nb2_L_prev;
                na1 = ctx->na1_L_prev; na2 = ctx->na2_L_prev;
            }
            for (s = 0; s < filter_nsec; ++s) {
                out_l = biquad_process_with_coeffs_int(&shelfL[s], nb0,nb1,nb2, na1,na2, out_l, 24);
            }
        }
        /* правый канал (аналогично) */
        if (total_lin_R != DSP_ONE) {
            if (total_lin_R != ctx->total_lin_R_prev) {
                compute_shelf_coeffs_int(total_lin_R, inv_den,cs,alpha, &nb0,&nb1,&nb2, &na1,&na2, DSP_ONE);
                ctx->total_lin_R_prev = total_lin_R;
                ctx->nb0_R_prev = nb0; ctx->nb1_R_prev = nb1; ctx->nb2_R_prev = nb2;
                ctx->na1_R_prev = na1; ctx->na2_R_prev = na2;
            } else {
                nb0 = ctx->nb0_R_prev; nb1 = ctx->nb1_R_prev; nb2 = ctx->nb2_R_prev;
                na1 = ctx->na1_R_prev; na2 = ctx->na2_R_prev;
            }
            for (s = 0; s < filter_nsec; ++s) {
                out_r = biquad_process_with_coeffs_int(&shelfR[s], nb0,nb1,nb2, na1,na2, out_r, 24);
            }
        }

        /* 7. Применение rumble фильтра */
        if (rumble_enabled) {
            for (s = 0; s < rumble_nsec; ++s) {
                out_l = biquad_process_int(&rumbleL[s], out_l, 24);
                out_r = biquad_process_int(&rumbleR[s], out_r, 24);
            }
        }

        /* 8. Запись результата (уже в Q24, насыщение не требуется, так как фильтры не должны выходить за пределы) */
        l[i] = out_l; r[i] = out_r;
    }

    /* сохранение состояния */
    ctx->rms_window_pos = wpos;
    ctx->rms_window_sum_L = rms_window_sum_L;
    ctx->rms_window_sum_R = rms_window_sum_R;

    ctx->total_lin_L_cur = cur_gain_L;
    ctx->total_lin_R_cur = cur_gain_R;
    ctx->last_target_L = last_target_L;
    ctx->last_target_R = last_target_R;
    ctx->rem_samples_L = remL;
    ctx->rem_samples_R = remR;
    ctx->total_lin_L_step = stepL;
    ctx->total_lin_R_step = stepR;
}


/* Моно-версия (упрощённо, аналогично стерео, но с общим RMS и gain) */
void dsp_denoise_process_mono(struct DspDenoiseCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, s;
    int32_t total_lin;                     // общий целевой gain для обоих каналов
    int32_t nb0, nb1, nb2, na1, na2;
    int32_t gain_diff;

    /* Предзагружаем переменные (аналогично стерео) */
    uint32_t wlen = ctx->rms_window_len;
    uint32_t wpos = ctx->rms_window_pos;
    int64_t *rms_window_buf_L = ctx->rms_window_buf_L;
    int64_t *rms_window_buf_R = ctx->rms_window_buf_R;
    int64_t rms_window_sum_L = ctx->rms_window_sum_L;
    int64_t rms_window_sum_R = ctx->rms_window_sum_R;

    uint32_t filter_nsec = ctx->filter_n_sections;
    uint32_t rumble_nsec = ctx->rumble_n_sections;
    uint32_t rumble_enabled = ctx->rumble_enabled;
    uint32_t smoothing_enabled = (ctx->attack_samples > 1 || ctx->release_samples > 1);

    biquad_i *hpL = ctx->hpL;
    biquad_i *hpR = ctx->hpR;
    biquad_i *shelfL = ctx->shelfL;
    biquad_i *shelfR = ctx->shelfR;
    biquad_i *rumbleL = ctx->rumbleL;
    biquad_i *rumbleR = ctx->rumbleR;

    int32_t reduction = ctx->reduction_lin;
    int32_t enhance   = ctx->enhance_lin;
    int32_t has_smooth_red = ctx->has_smooth_red;
    int32_t has_smooth_enh = ctx->has_smooth_enh;

    /* Удвоенные пороги для mono (сумма двух каналов) */
    int64_t tiny_sum_mono = ctx->tiny_sum * 2;
    int64_t red_low_sum_mono  = ctx->red_low_sum * 2;
    int64_t red_high_sum_mono = ctx->red_high_sum * 2;
    int64_t enh_low_sum_mono  = ctx->enh_low_sum * 2;
    int64_t enh_high_sum_mono = ctx->enh_high_sum * 2;

    double cs = ctx->shelf_cs;
    double alpha = ctx->shelf_alpha;
    double inv_den = ctx->shelf_inv_den;

    /* Для mono используем только левый канал для хранения общего состояния gain */
    int32_t cur_gain   = ctx->total_lin_L_cur;
    int32_t last_target = ctx->last_target_L;
    uint32_t rem       = ctx->rem_samples_L;
    int32_t step       = ctx->total_lin_L_step;

    for (i = 0; i < n; ++i) {
        int32_t hp_l, hp_r;
        int64_t sqL, sqR;
        int64_t total_sum;     // сумма квадратов обоих каналов
        int32_t out_l, out_r;

        int32_t xl = l[i];
        int32_t xr = r[i];

        /* 1. HF анализ через HPF (как в стерео) */
        hp_l = xl; hp_r = xr;
        for (s = 0; s < filter_nsec; ++s) {
            hp_l = biquad_process_int(&hpL[s], hp_l, 24);
            hp_r = biquad_process_int(&hpR[s], hp_r, 24);
        }

        /* 2. Квадраты HF (Q48) */
        sqL = (int64_t)hp_l * (int64_t)hp_l;
        sqR = (int64_t)hp_r * (int64_t)hp_r;

        /* 3. Обновление RMS окон (для каждого канала отдельно) */
        rms_window_sum_L -= rms_window_buf_L[wpos];
        rms_window_sum_R -= rms_window_buf_R[wpos];
        rms_window_buf_L[wpos] = sqL;
        rms_window_buf_R[wpos] = sqR;
        rms_window_sum_L += sqL;
        rms_window_sum_R += sqR;
        wpos++; if (wpos >= wlen) wpos = 0;

        /* Общая сумма квадратов (для двух каналов) */
        total_sum = rms_window_sum_L + rms_window_sum_R;

        /* 4. Определение общего целевого gain */
        total_lin = DSP_ONE;

        if (total_sum > tiny_sum_mono) {
            float t; int32_t t_q24;
            if (total_sum <= red_low_sum_mono) {
                total_lin = reduction;
            } else if (total_sum >= enh_high_sum_mono) {
                total_lin = enhance;
            } else if (has_smooth_red && total_sum > red_low_sum_mono && total_sum < red_high_sum_mono) {
                t = ((float)red_high_sum_mono - (float)total_sum) / ((float)red_high_sum_mono - (float)red_low_sum_mono);
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5f);
                total_lin = (int32_t)(((int64_t)reduction * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            } else if (has_smooth_enh && total_sum > enh_low_sum_mono && total_sum < enh_high_sum_mono) {
                t = ((float)total_sum - (float)enh_low_sum_mono) / ((float)enh_high_sum_mono - (float)enh_low_sum_mono);
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                t_q24 = (int32_t)(t * DSP_ONE + 0.5f);
                total_lin = (int32_t)(((int64_t)enhance * t_q24 + (int64_t)DSP_ONE * (DSP_ONE - t_q24)) >> 24);
            }
        }

        /* 5. Сглаживание общего gain (ramp) */
        if (smoothing_enabled) {
            gain_diff = total_lin - last_target;
            if (gain_diff < 0) gain_diff = -gain_diff;

            if (rem == 0 || gain_diff > 1) {
                last_target = total_lin;
                if (total_lin < cur_gain) rem = ctx->release_samples;
                else rem = ctx->attack_samples;
                if (rem == 0) rem = 1;
                step = (total_lin - cur_gain) / (int32_t)rem;
            }
            if (rem > 0) {
                cur_gain += step;
                rem--;
                if (rem == 0) {
                    cur_gain = total_lin;
                    step = 0;
                }
            }
            total_lin = cur_gain;   // используем сглаженное значение
        }

        /* 6. Применение shelf к обоим каналам с одинаковыми коэффициентами */
        out_l = xl; out_r = xr;

        if (total_lin != DSP_ONE) {
            /* Вычисляем коэффициенты, если изменились */
            if (total_lin != ctx->total_lin_L_prev) {
                compute_shelf_coeffs_int(total_lin, inv_den,cs,alpha, &nb0,&nb1,&nb2, &na1,&na2, DSP_ONE);
                ctx->total_lin_L_prev = total_lin;
                ctx->nb0_L_prev = nb0; ctx->nb1_L_prev = nb1; ctx->nb2_L_prev = nb2;
                ctx->na1_L_prev = na1; ctx->na2_L_prev = na2;
            } else {
                nb0 = ctx->nb0_L_prev; nb1 = ctx->nb1_L_prev; nb2 = ctx->nb2_L_prev;
                na1 = ctx->na1_L_prev; na2 = ctx->na2_L_prev;
            }
            /* Применяем ко всем секциям левого и правого каналов */
            for (s = 0; s < filter_nsec; ++s) {
                out_l = biquad_process_with_coeffs_int(&shelfL[s], nb0,nb1,nb2, na1,na2, out_l, 24);
                out_r = biquad_process_with_coeffs_int(&shelfR[s], nb0,nb1,nb2, na1,na2, out_r, 24);
            }
        }

        /* 7. Применение rumble фильтра (независимо для каждого канала) */
        if (rumble_enabled) {
            for (s = 0; s < rumble_nsec; ++s) {
                out_l = biquad_process_int(&rumbleL[s], out_l, 24);
                out_r = biquad_process_int(&rumbleR[s], out_r, 24);
            }
        }

        /* 8. Запись результата */
        l[i] = out_l; r[i] = out_r;
    }

    /* сохраняем состояние */
    ctx->rms_window_pos = wpos;
    ctx->rms_window_sum_L = rms_window_sum_L;
    ctx->rms_window_sum_R = rms_window_sum_R;

    /* для mono сохраняем общее состояние в левые поля (правые не используются) */
    ctx->total_lin_L_cur = cur_gain;
    ctx->last_target_L = last_target;
    ctx->rem_samples_L = rem;
    ctx->total_lin_L_step = step;
}


/* Сброс состояния */
void dsp_denoise_reset(struct DspDenoiseCtx *ctx)
{
    int i;
    if (!ctx) return;

    /* сброс фильтров */
    for (i = 0; i < ctx->filter_n_sections; ++i) {
        biquad_reset_int(&ctx->hpL[i]);
        biquad_reset_int(&ctx->hpR[i]);
        biquad_reset_int(&ctx->shelfL[i]);
        biquad_reset_int(&ctx->shelfR[i]);
    }
    if (ctx->rumble_enabled) {
        for (i = 0; i < ctx->rumble_n_sections; ++i) {
            biquad_reset_int(&ctx->rumbleL[i]);
            biquad_reset_int(&ctx->rumbleR[i]);
        }
    }
    /* сброс RMS окон */
    ctx->rms_window_sum_L = 0;
    ctx->rms_window_sum_R = 0;
    ctx->rms_window_pos = 0;
    memset(ctx->rms_window_buf_L, 0, ctx->rms_window_len * sizeof(int64_t));
    memset(ctx->rms_window_buf_R, 0, ctx->rms_window_len * sizeof(int64_t));

    /* сброс gain-состояний */
    ctx->total_lin_L_cur = DSP_ONE;
    ctx->total_lin_R_cur = DSP_ONE;
    ctx->last_target_L = DSP_ONE;
    ctx->last_target_R = DSP_ONE;
    ctx->rem_samples_L = 0;
    ctx->rem_samples_R = 0;
    ctx->total_lin_L_step = 0;
    ctx->total_lin_R_step = 0;
    ctx->total_lin_L_prev = 0;
    ctx->total_lin_R_prev = 0;
}

/* Освобождение памяти */
void dsp_denoise_free(struct DspDenoiseCtx *ctx)
{
    if (!ctx) return;
    if (ctx->hpL) { free(ctx->hpL); ctx->hpL = NULL; }
    if (ctx->hpR) { free(ctx->hpR); ctx->hpR = NULL; }
    if (ctx->shelfL) { free(ctx->shelfL); ctx->shelfL = NULL; }
    if (ctx->shelfR) { free(ctx->shelfR); ctx->shelfR = NULL; }
    if (ctx->rumbleL) { free(ctx->rumbleL); ctx->rumbleL = NULL; }
    if (ctx->rumbleR) { free(ctx->rumbleR); ctx->rumbleR = NULL; }
    if (ctx->rms_window_buf_L) { free(ctx->rms_window_buf_L); ctx->rms_window_buf_L = NULL; }
    if (ctx->rms_window_buf_R) { free(ctx->rms_window_buf_R); ctx->rms_window_buf_R = NULL; }
    free(ctx);   // free ctx->denoise stack
}

/* rx_dsp_agc.c
 *
 * Implementation of AGC as a module working on FPU.
 *
 * This file IS intended to be included textually from dsp_host.c
 * (so it can use shared helpers implemented above).
 */
struct DspAgcCtx {
    /* enable module */
    int enabled;

    /* настройки в double */
    double initial_gain;        /* линейный начальный gain */
    double target_rms;          /* целевое среднеквадратичное значение (не в dB) */
    double max_gain;            /* максимальный gain */
    double gain_attack_alpha;   /* альфа для attack (на сэмпл) */
    double gain_release_alpha;  /* альфа для release */
    double reset_alpha;         /* альфа для fade при сбросе */

    /* RMS окно */
    double  *rms_window_buf;    /* буфер квадратов (double) */
    uint32_t rms_window_len;
    uint32_t rms_window_pos;
    double   rms_window_sum;

    int is_active;
    double silence_threshold;   /* порог тишины в RMS (линейный) */
    uint32_t silence_counter_frames;

    /* состояние */
    double rms_avg;
    double gain;                /* текущий gain */
    uint32_t reset_after_frames;
    int reset_active;
    double epsilon;             /* tiny gain bypass (оптимизация) */
};


/* fill initial values */
/* caller must fill cfg fields first (target_rms_db, etc) */
struct DspAgcCtx* dsp_agc_init(struct DspAgcCtx *ctx, DspAgcConfig cfg, int sample_rate)
{
    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP AGC [fpu] module...\n");
    if (!ctx) ctx = (struct DspAgcCtx*)calloc(1, sizeof(struct DspAgcCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP AGC memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;

    /* target RMS (линейное, не в dB) */
    {
        double target_lin = pow(10.0, (double)cfg.target_rms_db / 20.0);
        ctx->target_rms = target_lin * target_lin;  // mean-square
    }

    ctx->max_gain     = pow(10.0, (double)cfg.max_gain_db / 20.0);
    ctx->initial_gain = pow(10.0, (double)cfg.initial_gain_db / 20.0);

    /* альфа для сглаживания gain */
    ctx->gain_attack_alpha  = 1.0 - exp(-1.0 / ((double)cfg.gain_attack_ms  * 0.001 * sample_rate));
    ctx->gain_release_alpha = 1.0 - exp(-1.0 / ((double)cfg.gain_release_ms * 0.001 * sample_rate));

    /* альфа для сброса (fade) */
    {
        double fade_ms = (double)cfg.reset_after_s * 500.0 / log(30.0);  // как в оригинале
        ctx->reset_alpha = 1.0 - exp(-1.0 / (fade_ms * 0.001 * sample_rate));
        ctx->reset_after_frames = (uint32_t)((double)cfg.reset_after_s * sample_rate + 0.5);
    }

    /* порог тишины (линейный RMS) */
    {
        double sil_lin = pow(10.0, (double)cfg.silence_threshold_db / 20.0);
        ctx->silence_threshold = sil_lin * sil_lin;  // mean-square
    }

    /* RMS окно */
    {
        uint32_t wlen = (uint32_t)((double)cfg.rms_window_ms * sample_rate / 1000.0 + 0.5);
        if (wlen < 1) wlen = 1;
        ctx->rms_window_len = wlen;
        ctx->rms_window_pos = 0;
        ctx->rms_window_sum = 0.0;
        ctx->rms_window_buf = (double*)calloc(wlen, sizeof(double));
        if (!ctx->rms_window_buf) {
            fprintf(stderr, "Failed to allocate DSP AGC RMS buffers.\n");
            free(ctx); return NULL;
        }
    }

    ctx->is_active = 0;
    ctx->silence_counter_frames = 0;
    ctx->rms_avg = 0.0;
    ctx->gain = ctx->initial_gain;
    ctx->reset_active = 0;
    ctx->epsilon = GAIN_EPSILON;

    return ctx;  // fill ctx->agc
}


/* The core processing loop.
   Inputs: l[], r[] contain samples already converted to Q24
   Output: l[], r[] replaced with processed <double> samples */
void dsp_agc_process(struct DspAgcCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    /* предзагрузка переменных */
    uint32_t i;
    uint32_t wlen = ctx->rms_window_len;
    uint32_t wpos = ctx->rms_window_pos;
    double *rms_window_buf = ctx->rms_window_buf;
    double  rms_window_sum = ctx->rms_window_sum;

    double cur_gain = ctx->gain;
    double initial_gain = ctx->initial_gain;
    double alpha_attack = ctx->gain_attack_alpha;
    double alpha_release = ctx->gain_release_alpha;
    double alpha_reset = ctx->reset_alpha;
    double target_rms = ctx->target_rms;
    double max_gain = ctx->max_gain;
    double silence_threshold = ctx->silence_threshold;
    uint32_t reset_frames = ctx->reset_after_frames;

    int is_active = ctx->is_active;
    uint32_t silence_counter = ctx->silence_counter_frames;
    int reset_active = ctx->reset_active;
    double rms_avg = ctx->rms_avg;
    double eps = ctx->epsilon;  // 1/q24

    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    /* обработка сэмплов */
    for (i = 0; i < n; ++i) {
        uint32_t j;
        double ms, old_val;
        double alpha, out_l, out_r;
        double yl_scaled, yr_scaled;
        double desired = cur_gain;  // по умолчанию без изменений
        int compute_desired = 0;    // включение расчёта усиления

        /* конверсия в double */
        double xl = (double)l[i] / DSP_ONE;
        double xr = (double)r[i] / DSP_ONE;

        /* мгновенная мощность (mean square) */
        ms = (xl*xl + xr*xr) * 0.5;

        /* обновление окна */
        old_val = rms_window_buf[wpos];
        rms_window_sum -= old_val;
        rms_window_buf[wpos] = ms;
        rms_window_sum += ms;
        wpos++; if (wpos >= wlen) wpos = 0;

        /* среднее RMS (mean square) */
        rms_avg = rms_window_sum / (double)wlen;

        /* активация */
        if (!is_active && rms_avg >= silence_threshold) {
            /* заполняем окно текущим значением */
            for (j = 0; j < wlen; ++j) rms_window_buf[j] = ms;
            rms_window_sum = ms * (double)wlen; wpos = 0;
            is_active = 1;
            silence_counter = 0;
            rms_avg = ms;
        }

        /* определяем состояние сэмпла */
        if (!is_active) {
            desired = initial_gain;

        } else {
            if (rms_avg < silence_threshold) {
                /* обнаружена тишина */
                silence_counter++;

                if (silence_counter < reset_frames / 3) {
                    /* первая часть тишины */
                    compute_desired = 1;

                } else if (silence_counter < reset_frames) {
                    /* создаём плавный fade */
                    reset_active = 1;
                    desired = initial_gain;

                } else {
                    /* делаем полный сброс */
                    is_active = 0;
                    rms_avg = 0.0;
                    silence_counter = 0;
                    reset_active = 0;
                    cur_gain = initial_gain;
                    desired = initial_gain;
                    /* очистка окна */
                    for (j = 0; j < wlen; ++j) rms_window_buf[j] = 0.0;
                    rms_window_sum = 0.0; wpos = 0;
                }

            } else {
                /* найден сигнал */
                reset_active = 0;
                silence_counter = 0;
                compute_desired = 1;
            }

            /* считаем усиление от RMS */
            if (compute_desired) {
                if (rms_avg > eps) {
                    double desired_lin = sqrt(target_rms / rms_avg);
                    if (desired_lin > max_gain) desired_lin = max_gain;
                    desired = desired_lin;
                } else {
                    desired = cur_gain;  // оставить как есть
                }
            }
        }

        /* сглаживание gain */
        if (reset_active) alpha = alpha_reset;
        else alpha = (desired > cur_gain) ? alpha_attack : alpha_release;
        cur_gain += alpha * (desired - cur_gain);

        /* применение усиления */
        out_l = xl * cur_gain;
        out_r = xr * cur_gain;

        /* преобразование в Q24 (с насыщением) */
        yl_scaled = out_l * DSP_ONE;
        yr_scaled = out_r * DSP_ONE;

        l[i] = (yl_scaled >= INT32_MAX) ? INT32_MAX :
               (yl_scaled <= INT32_MIN) ? INT32_MIN : (int32_t)yl_scaled;
        r[i] = (yr_scaled >= INT32_MAX) ? INT32_MAX :
               (yr_scaled <= INT32_MIN) ? INT32_MIN : (int32_t)yr_scaled;
    }

    /* сохранение состояния */
    ctx->rms_window_pos = wpos;
    ctx->rms_window_sum = rms_window_sum;
    ctx->gain = cur_gain;
    ctx->rms_avg = rms_avg;
    ctx->is_active = is_active;
    ctx->silence_counter_frames = silence_counter;
    ctx->reset_active = reset_active;
}


/* reset state */
void dsp_agc_reset(struct DspAgcCtx *ctx)
{
    if (!ctx) return;
    ctx->is_active = 0;
    ctx->rms_avg = 0.0;
    ctx->gain = ctx->initial_gain;
    ctx->silence_counter_frames = 0;
    ctx->reset_active = 0;
    /* clear RMS window */
    if (ctx->rms_window_buf && ctx->rms_window_len > 0) {
        memset(ctx->rms_window_buf, 0, (size_t)ctx->rms_window_len * sizeof(int32_t));
        ctx->rms_window_pos = 0;
        ctx->rms_window_sum = 0.0;
    }
}

/* clean-up: free window buffer (call if you add destructor) */
void dsp_agc_free(struct DspAgcCtx *ctx)
{
    if (!ctx) return;
    if (ctx->rms_window_buf) {
        free(ctx->rms_window_buf);
        ctx->rms_window_buf = NULL;
    }
    free(ctx);  // free ctx->agc stack
}

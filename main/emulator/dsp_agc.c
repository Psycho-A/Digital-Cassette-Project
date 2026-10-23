/* rx_dsp_agc.c
 *
 * Implementation of AGC as a module working in Q24.
 *
 * This file IS intended to be included textually from dsp_host.c
 * (so it can use shared helpers implemented above).
 */
struct DspAgcCtx {
    /* enable module */
    int enabled;

    /* --- precalc (runtime constants) --- */
    int32_t initial_gain_q24;        /* linear gain in Q24 (can be > DSP_ONE) */
    int32_t target_rms_q24;          /* target mean-square in Q24 (<= DSP_ONE) */
    int32_t max_gain_q24;            /* cap, can be > DSP_ONE */
    int32_t gain_attack_alpha_q24;   /* per-sample alpha for attack (Q24) */
    int32_t gain_release_alpha_q24;  /* per-sample alpha for release (Q24) */
    int32_t reset_alpha_q24;         /* per-sample alpha for reset fade (Q24) */

    /* --- RMS sliding-window buffer (new) --- */
    int32_t *rms_window_buf;         /* circular buffer of instantaneous power samples (Q24) */
    uint32_t rms_window_len;         /* length in frames */
    uint32_t rms_window_pos;         /* current write position */
    int64_t  rms_window_sum;         /* running sum (int64) of buffer values (units: Q24) */

    int is_active;                   /* boolean: currently active (we adapted after seeing signal above threshold) */
    int32_t silence_threshold_q24;   /* silence detection threshold: mean-square Q24 value below which we treat as silence */
    uint32_t silence_counter_frames; /* silence counter in frames while is_active = 1 */

    /* --- state --- */
    int32_t rms_avg_q24;             /* average mean-square (Q24) */
    int32_t gain_q24;                /* current gain (Q24) */
    uint32_t reset_after_frames;     /* computed from reset_after_s */
    int reset_active;
};


/* fill initial values */
/* caller must fill cfg fields first (target_rms_db, etc) */
struct DspAgcCtx* dsp_agc_init(struct DspAgcCtx *ctx, DspAgcConfig cfg, int sample_rate)
{
    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP AGC [int] module...\n");
    if (!ctx) ctx = (struct DspAgcCtx*)calloc(1, sizeof(struct DspAgcCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP AGC memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled;

    /* derive target MS (linear in Q24) */
    {
        double target_lin = pow(10.0, (double)cfg.target_rms_db / 20.0);
        double target_ms = target_lin * target_lin;
        ctx->target_rms_q24 = (int32_t)(target_ms * (double)DSP_ONE + 0.5);
    }

    /* gain values (can be > DSP_ONE) */
    ctx->max_gain_q24 = db_to_q24((float)cfg.max_gain_db);
    ctx->initial_gain_q24 = db_to_q24((float)cfg.initial_gain_db);

    /* gain smoothing alpha (per-sample) */
    ctx->gain_attack_alpha_q24 = time_to_alpha_q24((double)cfg.gain_attack_ms, sample_rate);
    ctx->gain_release_alpha_q24 = time_to_alpha_q24((double)cfg.gain_release_ms, sample_rate);

    /* reset fade computed as reset_after_s/2 (we will wait reset/2 then fade over reset/2) */
    {
        double tau_eff = log(30); // ln(100) for 99%, ln(20) for 95%...
        double fade_ms = ((double)cfg.reset_after_s * 1000.0 / 2.0) / tau_eff;
        ctx->reset_alpha_q24 = time_to_alpha_q24(fade_ms, sample_rate);
        /* compute frames for reset detection */
        ctx->reset_after_frames = (uint32_t)((double)cfg.reset_after_s * sample_rate + 0.5);
    }

    /* silence detection threshold (in mean-square units, Q24),
       threshold is computed on linear RMS and then squared to mean-square. */
    {
        double silence_lin = pow(10.0, (double)cfg.silence_threshold_db / 20.0);
        double silence_ms = silence_lin * silence_lin;
        ctx->silence_threshold_q24 = (int32_t)(silence_ms * (double)DSP_ONE + 0.5);
    }

    /* allocate and init sliding window buffers for RMS */
    {
        uint32_t wlen = (uint32_t)((double)cfg.rms_window_ms * sample_rate / 1000.0 + 0.5);
        if (wlen < 1) wlen = 1;
        ctx->rms_window_len = wlen;
        ctx->rms_window_pos = 0;
        ctx->rms_window_sum = 0;  /* int64: measure-only, not DSP-critical */
        ctx->rms_window_buf = (int32_t*)calloc(wlen, sizeof(int32_t));
        if (!ctx->rms_window_buf) {
            fprintf(stderr, "Failed to allocate DSP AGC RMS buffers.\n");
            free(ctx); return NULL;
        }
    }

    /* zero other state */
    ctx->is_active = 0;  // start inactive: we won't boost until signal > threshold
    ctx->silence_counter_frames = 0;
    ctx->rms_avg_q24 = 0;
    ctx->gain_q24 = ctx->initial_gain_q24;
    ctx->reset_active = 0;

    return ctx;  // fill ctx->agc
}


/* The core processing loop.
   Inputs: l[], r[] contain samples already converted to Q24
   Output: l[], r[] replaced with processed Q24 samples */
void dsp_agc_process(struct DspAgcCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    /* предзагрузка переменных */
    uint32_t i;
    uint32_t wlen = ctx->rms_window_len;
    uint32_t wpos = ctx->rms_window_pos;
    int32_t *rms_window_buf = ctx->rms_window_buf;
    int64_t  rms_window_sum = ctx->rms_window_sum;

    int32_t cur_gain = ctx->gain_q24;
    int32_t initial_gain = ctx->initial_gain_q24;
    int32_t alpha_attack = ctx->gain_attack_alpha_q24;
    int32_t alpha_release = ctx->gain_release_alpha_q24;
    int32_t alpha_reset = ctx->reset_alpha_q24;
    int32_t target_rms = ctx->target_rms_q24;
    int32_t max_gain = ctx->max_gain_q24;
    int32_t silence_threshold = ctx->silence_threshold_q24;
    uint32_t reset_frames = ctx->reset_after_frames;

    int is_active = ctx->is_active;
    uint32_t silence_counter = ctx->silence_counter_frames;
    int reset_active = ctx->reset_active;
    int32_t rms_avg = ctx->rms_avg_q24;

    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    /* === Q24 processing with silence gating and reset behavior ===
       - update rms_avg per-sample via sliding window (no EMA) and adapt gain
       - determine 'active' only when window-mean >= silence_threshold
       - if active == 0: keep desired=initial_gain (no boosting)
       - if active == 1: compute desired normally; if rms low but silence_counter < reset/3: compute desired normally
           once silence_counter >= reset_after_frames/3 -> fade to initial over reset_after_frames/3
       - per-sample smoothing uses attack/release alphas (already per-sample)
    */
    for (i = 0; i < n; ++i) {
        uint32_t j;
        int64_t sqL, sqR;
        int32_t ms, old_val, alpha;
        int64_t delta, outL, outR;
        int32_t desired = cur_gain;  // по умолчанию без изменений
        int compute_desired = 0;     // включение расчёта усиления

        /* copy input samples */
        int32_t xl = l[i];
        int32_t xr = r[i];

        /* instantaneous mean power across channels in Q24 */
        sqL = ((int64_t)xl * xl) >> 24;
        sqR = ((int64_t)xr * xr) >> 24;
        ms  = (int32_t)((sqL + sqR) >> 1);  // деление на 2 для среднего

        /* sliding-window update: subtract oldest, add new, compute mean */
        old_val = rms_window_buf[wpos];
        rms_window_sum -= (int64_t)old_val;
        rms_window_buf[wpos] = ms;
        rms_window_sum += (int64_t)ms;
        wpos++; if (wpos >= wlen) wpos = 0;  // advance pos

        /* compute window mean in Q24 (rounded) */
        rms_avg = (int32_t)(rms_window_sum / (int64_t)wlen);

        /* activation: if not active and window-mean >= threshold -> become active */
        if (!is_active && rms_avg >= silence_threshold) {
            /* seed window with current ms to avoid large initial changes */
            for (j = 0; j < wlen; ++j) rms_window_buf[j] = ms;
            rms_window_sum = (int64_t)ms * (int64_t)wlen; wpos = 0;
            is_active = 1;
            silence_counter = 0;
            rms_avg = ms;
        }

        /* decide desired target for this sample:
           - if not active: keep initial_gain
           - if active:
               * if rms low but silence_counter < reset/fp: compute desired normally (so short drops tolerated)
               * if silence_counter >= reset/fp: desired = initial_gain (we are fading back)
        */
        if (!is_active) {
            desired = ctx->initial_gain_q24;

        } else {
            if (rms_avg < silence_threshold) {
                /* обнаружена тишина, запускаем счётчик */
                silence_counter++;

                if (silence_counter < reset_frames / 3) {
                    /* начало тишины: продолжаем считать gain */
                    compute_desired = 1;  // выполняется позже!

                } else if (silence_counter < reset_frames) {
                    /* середина тишины: включаем фэйд и сбрасываем gain */
                    reset_active = 1;
                    desired = ctx->initial_gain_q24;

                } else {
                    /* полный сброс: выключаем активность, чистим накопители, возвращаем gain к initial */
                    is_active = 0;
                    rms_avg = 0;
                    silence_counter = 0;
                    reset_active = 0;
                    cur_gain = ctx->initial_gain_q24;
                    desired = ctx->initial_gain_q24;  // сброс desired (применяется дальше)
                    /* очистка окна RMS, чтобы по выходу из тишины не унаследовать тишину */
                    for (j = 0; j < wlen; ++j) rms_window_buf[j] = 0;
                    rms_window_sum = 0; wpos = 0;
                }

            } else {
                /* нормальный режим: сигнал присутствует */
                reset_active = 0;
                silence_counter = 0;
                compute_desired = 1;
            }

            /* считаем усиление от RMS */
            if (compute_desired) {
                if (rms_avg > 1) {
                    /* отношение target_rms / rms_avg в Q24 */
                    int64_t ratio = ((int64_t)target_rms << 24) / (int64_t)rms_avg;
                    /* корень из ratio (TODO: float, replace to q24_sqrt!) */
                    float root = (float)sqrt((float)ratio / DSP_ONE);
                    int64_t desired_lin = (int64_t)(root * DSP_ONE + 0.5);  // результат в Q24 с округлением
                    if (desired_lin > (int64_t)max_gain) desired_lin = max_gain;
                    desired = (int32_t)desired_lin;
                } else {
                    /* keep current (avoid snapping to max_gain on silence) */
                    desired = cur_gain;
                }
            }
        }

        /* select per-sample alpha: if reset_active use reset one, else attack/release */
        if (reset_active) alpha = alpha_reset;
        else alpha = (desired > cur_gain) ? alpha_attack : alpha_release;

        /* per-sample gain smoothing */
        delta = (int64_t)alpha * (int64_t)(desired - cur_gain);  // Q48
        cur_gain += (int32_t)(delta >> 24);

        /* apply resulting gain */
        outL = (int64_t)xl * (int64_t)cur_gain;  // Q48
        outR = (int64_t)xr * (int64_t)cur_gain;

        /* write output samples in Q24 */
        l[i] = (int32_t)(outL >> 24);
        r[i] = (int32_t)(outR >> 24);
    }

    /* сохранение состояния */
    ctx->rms_window_pos = wpos;
    ctx->rms_window_sum = rms_window_sum;
    ctx->gain_q24 = cur_gain;
    ctx->rms_avg_q24 = rms_avg;
    ctx->is_active = is_active;
    ctx->silence_counter_frames = silence_counter;
    ctx->reset_active = reset_active;
}


/* reset state */
void dsp_agc_reset(struct DspAgcCtx *ctx)
{
    if (!ctx) return;
    ctx->is_active = 0;
    ctx->rms_avg_q24 = 0;
    ctx->gain_q24 = ctx->initial_gain_q24;
    ctx->silence_counter_frames = 0;
    ctx->reset_active = 0;
    /* clear RMS window */
    if (ctx->rms_window_buf && ctx->rms_window_len > 0) {
        memset(ctx->rms_window_buf, 0, (size_t)ctx->rms_window_len * sizeof(int32_t));
        ctx->rms_window_pos = 0;
        ctx->rms_window_sum = 0;
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

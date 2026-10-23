/* rx_dsp_limiter.c
 *
 * Zero-latency (chunk-wise) limiter (C89)
 * - Uses Q24 input/output (samples provided in Q24)
 * - No lookahead. Implements knee (linear in amplitude domain) and release ramp.
 * - Stereo: per-channel independent processing.
 * - Mono-mode: shared detector (max(|L|,|R|)) -> single target applied to both channels.
 *
 * API mirrors generic DSP style:
 *   ctx = dsp_limiter_init(ctx, cfg);
 *   dsp_limiter_process(l, r, n, ctx);
 *   dsp_limiter_reset(ctx, cfg);
 *   dsp_limiter_free(ctx);
 *
 * Notes:
 * - Uses double for internal arithmetic (temporary/emulation).
 * - Allocations done once in init (calloc). No per-sample allocation.
 * - Hot loop avoids pow/log/fabs; uses simple arithmetic and ternary abs.
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspLimiterCtx {
    /* config */
    int enabled;
    double knee_width_db;

    /* gain values */
    double epsilon;        // minimum diff (1/Q24)
    double threshold_lin;  // linear (20*log10 -> lin)
    double preamp_lin;     // unity = DSP_ONE

    /* knee range mapping */
    double map_out_lo;
    double map_out_hi;
    double map_in_lo;
    double map_in_hi;
    double map_in_den_inv;

    /* smoothing state (per-channel) */
    int smoothing;
    uint32_t release_samples;
    double cur_gain_L;
    double cur_gain_R;
    double step_L;
    double step_R;
    uint32_t release_rem_L;
    uint32_t release_rem_R;

    /* hold перед release */
    uint32_t hold_samples;  // длительность удержани€ (из hold_ms)
    uint32_t hold_rem_L;    // оставшиес€ сэмплы удержани€
    uint32_t hold_rem_R;

    /* previous absolute amplitudes (for rise detection) */
    double prev_abs_L;
    double prev_abs_R;

    /* pointer to selected processing branch (stereo/mono) */
    void (*process)(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
};


/* some forwards for calling by init */
void dsp_limiter_free(struct DspLimiterCtx*);
void dsp_limiter_process_stereo(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
void dsp_limiter_process_mono(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
double limiter_compute_target(struct DspLimiterCtx*, double, double);

/* fill initial values */
/* caller must fill cfg fields first (threshold_db, etc) */
struct DspLimiterCtx* dsp_limiter_init(struct DspLimiterCtx *ctx, DspLimiterConfig cfg, int sample_rate)
{
    /* init locals */
    double knee_factor;

    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP Limiter [fpu] module...\n");
    if (!ctx) ctx = (struct DspLimiterCtx*)calloc(1, sizeof(struct DspLimiterCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Limiter memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;
    ctx->knee_width_db = -1.0 * fabs(cfg.knee_width_db);  // always negative

    /* linear conversions (use pow in init only) */
    ctx->threshold_lin = pow(10.0, cfg.threshold_db / 20.0);
    knee_factor = pow(10.0, ctx->knee_width_db / 20.0);  // < 1.0

    /* compute linear preamp */
    if (cfg.preamp_db != 0.0) {
        ctx->preamp_lin = pow(10.0, cfg.preamp_db / 20.0);
    } else {
        ctx->preamp_lin = 1.0;
    }

    /* hold/release samples */
    ctx->hold_samples = (uint32_t)((double)cfg.hold_ms * 0.001 * sample_rate + 0.5);
    ctx->release_samples = (uint32_t)((double)cfg.release_ms * 0.001 * sample_rate + 0.5);
    if (ctx->hold_samples < 1) ctx->hold_samples = 1;
    if (ctx->release_samples < 0) ctx->release_samples = 0;

    /* compute knee: compressor-on-rise mapping for range [thr..1.0] */
    if (ctx->knee_width_db != 0.0) {
        ctx->map_out_lo = ctx->threshold_lin * knee_factor;
        ctx->map_out_hi = ctx->threshold_lin;
        ctx->map_in_lo  = ctx->map_out_lo;  // нижн€€ граница входа = knee_low
        ctx->map_in_hi  = 1.0;
        /* pre-compute inverse of denom (+guard) */
        ctx->map_in_den_inv = 1.0 / (ctx->map_in_hi - ctx->map_in_lo);
        if (ctx->map_in_den_inv <= 1e-12) ctx->map_in_den_inv = 0.0;
    }

    /* init smoothing state (unity, no ramp) */
    ctx->cur_gain_L = 1.0;
    ctx->cur_gain_R = 1.0;
    ctx->epsilon = GAIN_EPSILON;  // tiny step (1/Q24), можно увеличить
    ctx->step_L = ctx->step_R = 0.0;
    ctx->hold_rem_L = ctx->hold_rem_R = 0;
    ctx->release_rem_L = ctx->release_rem_R = 0;
    ctx->prev_abs_L = ctx->prev_abs_R = 0.0;
    ctx->smoothing = (ctx->release_samples > 0.0);

    /* select processing branch:
       if linked_stereo == 1 -> prefer mono branch (process_mono),
       else use stereo per-channel branch. */
    if (cfg.linked_stereo) ctx->process = dsp_limiter_process_mono;
    else ctx->process = dsp_limiter_process_stereo;

    return ctx;
}


/* core processing dispatcher (in-place Q24 buffers) */
void dsp_limiter_process(struct DspLimiterCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    /* process with selected branch */
    ctx->process(ctx, l, r, n);
}


/* per-channel independent processing (stereo) */
void dsp_limiter_process_stereo(struct DspLimiterCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    // предзагрузка состо€ни€
    double eps = ctx->epsilon;
    double curL = ctx->cur_gain_L;
    double curR = ctx->cur_gain_R;
    double stepL = ctx->step_L;
    double stepR = ctx->step_R;
    uint32_t hld_remL = ctx->hold_rem_L;
    uint32_t hld_remR = ctx->hold_rem_R;
    uint32_t rls_remL = ctx->release_rem_L;
    uint32_t rls_remR = ctx->release_rem_R;
    double prevL = ctx->prev_abs_L;
    double prevR = ctx->prev_abs_R;
    int has_smoothing = ctx->smoothing;
    uint32_t i;
    double xl_in, xr_in;
    double abs_l, abs_r;
    double targetL, targetR;
    double scaled_l, scaled_r;

    for (i = 0; i < n; ++i) {
        // конвертируем Q24 -> double в [-1..+1) домене
        xl_in = (double)l[i] / (double)DSP_ONE;
        xr_in = (double)r[i] / (double)DSP_ONE;

        // примен€ем предусиление
        if (ctx->preamp_lin != 1.0) {
            xl_in *= ctx->preamp_lin;
            xr_in *= ctx->preamp_lin;
        }

        // абсолютные амплитуды (no fabs)
        abs_l = xl_in < 0.0 ? -xl_in : xl_in;
        abs_r = xr_in < 0.0 ? -xr_in : xr_in;

        // вычисл€ем целевые усилени€ дл€ сэмпла
        targetL = limiter_compute_target(ctx, abs_l, prevL);
        targetR = limiter_compute_target(ctx, abs_r, prevR);

        // обновить прошлые значени€ дл€ детекта подъЄма
        prevL = abs_l; prevR = abs_r;

        // примен€ем сглаживание по hold+release
        if (has_smoothing) {
            // --- дл€ Ћ≈¬ќ√ќ канала ---
            if (targetL < curL - eps) {
                // превышение порога Ц мгновенно уменьшаем gain
                stepL = 0.0; curL = targetL;
                hld_remL = rls_remL = 0;  // сброс
            } else if (targetL > curL + eps) {
                // сигнал ниже порога Ц активируем hold, если ещЄ не активен
                if (hld_remL == 0) {
                    hld_remL = ctx->hold_samples;  // начинаем отсчЄт
                }
            }
            // обработка hold (уменьшение счЄтчика)
            if (hld_remL > 0) {
                hld_remL--;
                if (hld_remL == 0) {
                    // врем€ удержани€ истекло Ц запускаем release
                    rls_remL = ctx->release_samples;
                    if (rls_remL == 0) rls_remL = 1;
                    stepL = (targetL - curL) / (double)rls_remL;
                }
            }
            // выполнение release, если активен
            if (rls_remL > 0) {
                curL += stepL;
                rls_remL--;
                if (rls_remL == 0) {
                    stepL = 0.0; curL = targetL;  // сброс погрешности
                }
            }

            // --- дл€ ѕ–ј¬ќ√ќ канала (идентично)
            if (targetR < curR - eps) {
                stepR = 0.0; curR = targetR;
                hld_remR = rls_remR = 0;
            } else if (targetR > curR + eps) {
                if (hld_remR == 0) {
                    hld_remR = ctx->hold_samples;
                }
            }
            if (hld_remR > 0) {
                hld_remR--;
                if (hld_remR == 0) {
                    rls_remR = ctx->release_samples;
                    if (rls_remR == 0) rls_remR = 1;
                    stepR = (targetR - curR) / (double)rls_remR;
                }
            }
            if (rls_remR > 0) {
                curR += stepR;
                rls_remR--;
                if (rls_remR == 0) {
                    stepR = 0.0; curR = targetR;
                }
            }

        } else {
            // сглаживание отключено
            curL = targetL; curR = targetR;
        }

        // примен€ем усиление и конвертируем в Q24 (с насыщением)
        scaled_l = xl_in * curL * (double)DSP_ONE;
        scaled_r = xr_in * curR * (double)DSP_ONE;

        if (scaled_l >= 2147483647.0) l[i] = 2147483647;
        else if (scaled_l <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)(scaled_l);

        if (scaled_r >= 2147483647.0) r[i] = 2147483647;
        else if (scaled_r <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)(scaled_r);
    }

    // сохран€ем состо€ни€ в ctx */
    ctx->cur_gain_L = curL;
    ctx->cur_gain_R = curR;
    ctx->step_L = stepL;
    ctx->step_R = stepR;
    ctx->hold_rem_L = hld_remL;
    ctx->hold_rem_R = hld_remR;
    ctx->release_rem_L = rls_remL;
    ctx->release_rem_R = rls_remR;
    ctx->prev_abs_L = prevL;
    ctx->prev_abs_R = prevR;
}


/* mono/shared detector processing:
   - compute target from max(|L|,|R|)
   - apply same target & smoothing to both channels (shared gain)
*/
void dsp_limiter_process_mono(struct DspLimiterCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    double eps = ctx->epsilon;
    double cur = ctx->cur_gain_L;
    double step = ctx->step_L;
    uint32_t hld_rem = ctx->hold_rem_L;
    uint32_t rls_rem = ctx->release_rem_L;
    double prev = ctx->prev_abs_L;
    int has_smoothing = ctx->smoothing;
    uint32_t i;
    double xl_in, xr_in;
    double abs_l, abs_r, abs_max;
    double scaled_l, scaled_r;
    double target;

    for (i = 0; i < n; ++i) {
        xl_in = (double)l[i] / (double)DSP_ONE;
        xr_in = (double)r[i] / (double)DSP_ONE;

        if (ctx->preamp_lin != 1.0) {
            xl_in *= ctx->preamp_lin;
            xr_in *= ctx->preamp_lin;
        }

        abs_l = xl_in < 0.0 ? -xl_in : xl_in;
        abs_r = xr_in < 0.0 ? -xr_in : xr_in;
        abs_max = abs_l > abs_r ? abs_l : abs_r;

        target = limiter_compute_target(ctx, abs_max, prev);
        prev = abs_max;

        if (has_smoothing) {
            if (target < cur - eps) {
                step = 0.0; cur = target;
                hld_rem = rls_rem = 0;
            } else if (target > cur + eps) {
                if (hld_rem == 0) {
                    hld_rem = ctx->hold_samples;
                }
            }
            if (hld_rem > 0) {
                hld_rem--;
                if (hld_rem == 0) {
                    rls_rem = ctx->release_samples;
                    if (rls_rem == 0) rls_rem = 1;
                    step = (target - cur) / (double)rls_rem;
                }
            }
            if (rls_rem > 0) {
                cur += step;
                rls_rem--;
                if (rls_rem == 0) {
                    step = 0.0; cur = target;
                }
            }
        } else {
            cur = target;
        }

        scaled_l = xl_in * cur * (double)DSP_ONE;
        scaled_r = xr_in * cur * (double)DSP_ONE;

        if (scaled_l >= 2147483647.0) l[i] = 2147483647;
        else if (scaled_l <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)(scaled_l);

        if (scaled_r >= 2147483647.0) r[i] = 2147483647;
        else if (scaled_r <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)(scaled_r);
    }

    ctx->cur_gain_L = cur;
    ctx->step_L = step;
    ctx->hold_rem_L = hld_rem;
    ctx->release_rem_L = rls_rem;
    ctx->prev_abs_L = prev;
}


/* helper: compute target gain from absolute amplitude 'a' (both double, linear domain)
   returns value in [0..1], where 1.0 == unity (no attenuation)
   Avoids any pow/log in hot path (all pow() used in init).
   prev_a is previous absolute amplitude for the channel (for rise detection).
*/
double limiter_compute_target(struct DspLimiterCtx *ctx, double a, double prev_a)
{
    /* init locals */
    int is_rising = 0;
    int has_smoothing = ctx->smoothing;
    double eps = ctx->epsilon;
    double target = 1.0;
    double t, in, out_a;

    if (a <= 0.0) return 1.0;

    if (ctx->knee_width_db >= 0.0) {
        /* no knee: hard limit value (<= 1.0) */
        if (a > ctx->threshold_lin) {
            target = ctx->threshold_lin / (a > eps ? a : eps);
        }

    } else {
        /* wave rising detection (if release > 0) */
        if (a > (prev_a + eps)) is_rising = 1;

        /* compressor-like knee: map input range [thr..1.0] -> [map_out_lo..thr]
           Apply only on rising transients; descending sides are handled by release ramp.
           if below threshold, no mapping (bypass) */
        if ((is_rising || !has_smoothing) && a > ctx->map_in_lo) {
            in = a; t = 1.0;
            if (in > ctx->map_in_hi) in = ctx->map_in_hi;

            /* normalized interpolation factor */
            if (ctx->map_in_den_inv > 0.0) t = (in - ctx->map_in_lo) * ctx->map_in_den_inv;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;

            /* compute target output amplitude */
            out_a = ctx->map_out_lo + t * (ctx->map_out_hi - ctx->map_out_lo);
            target = out_a / a;
        }
    }

    /* target gain safety clamp */
    if (target > 1.0) target = 1.0;
    else if (target < 0.0) target = 0.0;

    return target;
}


/* reset state */
void dsp_limiter_reset(struct DspLimiterCtx *ctx)
{
    if (!ctx) return;
    ctx->cur_gain_L = 1.0;
    ctx->cur_gain_R = 1.0;
    ctx->step_L = ctx->step_R = 0.0;
    ctx->hold_rem_L = ctx->hold_rem_R = 0;
    ctx->release_rem_L = ctx->release_rem_R = 0;
    ctx->prev_abs_L = ctx->prev_abs_R = 0;
}

/* free context memory */
void dsp_limiter_free(struct DspLimiterCtx *ctx)
{
    if (!ctx) return;
    free(ctx);
}

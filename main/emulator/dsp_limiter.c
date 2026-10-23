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
 * - Uses int64 for internal arithmetic (temporary/emulation).
 * - Allocations done once in init (calloc). No per-sample allocation.
 * - Hot loop avoids pow/log/fabs; uses simple arithmetic and ternary abs.
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspLimiterCtx {
    /* config */
    int enabled;
    double knee_width_db;

    /* threshold (double+Q24) */
    double threshold_lin;
    int32_t threshold_q24;

    /* gain values */
    int32_t eps_q24;     // minimum diff (1/Q24)
    int32_t preamp_q24;  // unity = DSP_ONE

    /* knee range mapping */
    int32_t map_out_lo_q24;
    int32_t map_out_hi_q24;
    int32_t map_in_lo_q24;
    int32_t map_in_hi_q24;
    int32_t map_in_den_q24;

    /* release smoothing (Q24) */
    int smoothing;
    uint32_t release_samples;
    int32_t cur_gain_L;
    int32_t cur_gain_R;
    int32_t step_L;
    int32_t step_R;
    uint32_t release_rem_L;
    uint32_t release_rem_R;

    /* hold перед release (Q24) */
    uint32_t hold_samples;  // длительность удержани€ (из hold_ms)
    uint32_t hold_rem_L;    // оставшиес€ сэмплы удержани€
    uint32_t hold_rem_R;

    /* previous absolute amplitudes (for rise detection) */
    int32_t prev_abs_L;
    int32_t prev_abs_R;

    /* pointer to selected processing branch (stereo/mono) */
    void (*process)(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
};


/* some forwards for calling */
void dsp_limiter_free(struct DspLimiterCtx*);
void dsp_limiter_process_stereo(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
void dsp_limiter_process_mono(struct DspLimiterCtx*, int32_t*, int32_t*, uint32_t);
int32_t limiter_compute_target(struct DspLimiterCtx*, int32_t, int32_t);

/* fill initial values */
/* caller must fill cfg fields first (threshold_db, etc) */
struct DspLimiterCtx* dsp_limiter_init(struct DspLimiterCtx *ctx, DspLimiterConfig cfg, int sample_rate)
{
    /* init locals */
    double knee_factor;

    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP Limiter [int] module...\n");
    if (!ctx) ctx = (struct DspLimiterCtx*)calloc(1, sizeof(struct DspLimiterCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Limiter memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;
    ctx->knee_width_db = -1.0 * fabs(cfg.knee_width_db);  // always negative

    /* compute double thresholds (kept for debug) */
    ctx->threshold_lin = pow(10.0, (double)cfg.threshold_db / 20.0);

    /* convert to Q24 and get linear knee */
    ctx->threshold_q24 = (int32_t)(ctx->threshold_lin * (double)DSP_ONE + 0.5);
    knee_factor = pow(10.0, ctx->knee_width_db / 20.0);  // < 1.0

    /* compute Q24 preamp */
    if (cfg.preamp_db != 0.0) {
        double preamp_lin = pow(10.0, cfg.preamp_db / 20.0);
        ctx->preamp_q24 = (int32_t)(preamp_lin * (double)DSP_ONE + 0.5);  // max value = +42.1 dB
    } else {
        ctx->preamp_q24 = DSP_ONE;
    }

    /* hold/release samples */
    ctx->hold_samples = (uint32_t)((double)cfg.hold_ms * 0.001 * sample_rate + 0.5);
    ctx->release_samples = (uint32_t)((double)cfg.release_ms * 0.001 * sample_rate + 0.5);
    if (ctx->hold_samples < 1) ctx->hold_samples = 1;
    if (ctx->release_samples < 0) ctx->release_samples = 0;

    /* compute knee: compressor-on-rise mapping for range [thr..1.0] */
    if (ctx->knee_width_db != 0.0) {
        ctx->map_out_lo_q24 = (int32_t)(ctx->threshold_lin * knee_factor * (double)DSP_ONE + 0.5);
        ctx->map_out_hi_q24 = ctx->threshold_q24;
        ctx->map_in_lo_q24  = ctx->map_out_lo_q24;  // нижн€€ граница входа = knee_low
        ctx->map_in_hi_q24  = (int32_t)(1.0 * (double)DSP_ONE + 0.5);
        /* pre-compute inverse of denom (+guard) */
        ctx->map_in_den_q24 = ctx->map_in_hi_q24 - ctx->map_in_lo_q24;
        if (ctx->map_in_den_q24 <= 0) ctx->map_in_den_q24 = 1;
    }

    /* init Q24 smoothing state */
    ctx->cur_gain_L = DSP_ONE;
    ctx->cur_gain_R = DSP_ONE;
    ctx->eps_q24 = 1;  // tiny step (1/Q24), можно увеличить
    ctx->step_L = ctx->step_R = 0;
    ctx->hold_rem_L = ctx->hold_rem_R = 0;
    ctx->release_rem_L = ctx->release_rem_R = 0;
    ctx->prev_abs_L = ctx->prev_abs_R = 0;
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
    int32_t eps = ctx->eps_q24;
    int32_t curL = ctx->cur_gain_L;
    int32_t curR = ctx->cur_gain_R;
    int32_t stepL = ctx->step_L;
    int32_t stepR = ctx->step_R;
    uint32_t hld_remL = ctx->hold_rem_L;
    uint32_t hld_remR = ctx->hold_rem_R;
    uint32_t rls_remL = ctx->release_rem_L;
    uint32_t rls_remR = ctx->release_rem_R;
    int32_t prevL = ctx->prev_abs_L;
    int32_t prevR = ctx->prev_abs_R;
    int has_smoothing = ctx->smoothing;
    uint32_t i;
    int32_t xl_q24, xr_q24;
    int32_t abs_l, abs_r;
    int32_t targetL, targetR;
    int64_t tmp64;

    for (i = 0; i < n; ++i) {
        xl_q24 = l[i]; xr_q24 = r[i];

        // примен€ем предусиление
        if (ctx->preamp_q24 != DSP_ONE) {
            tmp64 = (int64_t)xl_q24 * (int64_t)ctx->preamp_q24;
            xl_q24 = (int32_t)(tmp64 >> 24);
            tmp64 = (int64_t)xr_q24 * (int64_t)ctx->preamp_q24;
            xr_q24 = (int32_t)(tmp64 >> 24);
        }

        // абсолютные амплитуды (no fabs)
        abs_l = xl_q24 < 0 ? -xl_q24 : xl_q24;
        abs_r = xr_q24 < 0 ? -xr_q24 : xr_q24;

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
                stepL = 0; curL = targetL;
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
                    stepL = (int32_t)(((int64_t)(targetL - curL)) / (int64_t)rls_remL);
                }
            }
            // выполнение release, если активен
            if (rls_remL > 0) {
                curL = (int32_t)((int64_t)curL + (int64_t)stepL);
                rls_remL--;
                if (rls_remL == 0) {
                    stepL = 0; curL = targetL;  // сброс погрешности
                }
            }

            // --- дл€ ѕ–ј¬ќ√ќ канала (идентично)
            if (targetR < curR - eps) {
                stepR = 0; curR = targetR;
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
                    stepR = (int32_t)(((int64_t)(targetR - curR)) / (int64_t)rls_remR);
                }
            }
            if (rls_remR > 0) {
                curR = (int32_t)((int64_t)curR + (int64_t)stepR);
                rls_remR--;
                if (rls_remR == 0) {
                    stepR = 0; curR = targetR;
                }
            }

        } else {
            // сглаживание отключено
            curL = targetL; curR = targetR;
        }

        // примен€ем усиление: y = (x * gain_q24) >> 24 (Q24, без округлени€)
        tmp64 = (int64_t)xl_q24 * (int64_t)curL; l[i] = (int32_t)(tmp64 >> 24);
        tmp64 = (int64_t)xr_q24 * (int64_t)curR; r[i] = (int32_t)(tmp64 >> 24);
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
    int32_t eps = ctx->eps_q24;
    int32_t cur = ctx->cur_gain_L;
    int32_t step = ctx->step_L;
    uint32_t hld_rem = ctx->hold_rem_L;
    uint32_t rls_rem = ctx->release_rem_L;
    int32_t prev = ctx->prev_abs_L;
    int has_smoothing = ctx->smoothing;
    uint32_t i;
    int32_t xl_q24, xr_q24;
    int32_t abs_l, abs_r, abs_max;
    int32_t target;
    int64_t tmp64;

    for (i = 0; i < n; ++i) {
        xl_q24 = l[i]; xr_q24 = r[i];

        if (ctx->preamp_q24 != DSP_ONE) {
            tmp64 = (int64_t)xl_q24 * (int64_t)ctx->preamp_q24;
            xl_q24 = (int32_t)(tmp64 >> 24);
            tmp64 = (int64_t)xr_q24 * (int64_t)ctx->preamp_q24;
            xr_q24 = (int32_t)(tmp64 >> 24);
        }

        abs_l = xl_q24 < 0 ? -xl_q24 : xl_q24;
        abs_r = xr_q24 < 0 ? -xr_q24 : xr_q24;
        abs_max = abs_l > abs_r ? abs_l : abs_r;

        target = limiter_compute_target(ctx, abs_max, prev);
        prev = abs_max;

        if (has_smoothing) {
            if (target < cur - eps) {
                step = 0; cur = target;
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
                    step = (int32_t)(((int64_t)(target - cur)) / (int64_t)rls_rem);
                }
            }
            if (rls_rem > 0) {
                cur = (int32_t)((int64_t)cur + (int64_t)step);
                rls_rem--;
                if (rls_rem == 0) {
                    step = 0; cur = target;
                }
            }
        } else {
            cur = target;
        }

        tmp64 = (int64_t)xl_q24 * (int64_t)cur; l[i] = (int32_t)(tmp64 >> 24);
        tmp64 = (int64_t)xr_q24 * (int64_t)cur; r[i] = (int32_t)(tmp64 >> 24);
    }

    ctx->cur_gain_L = cur;
    ctx->step_L = step;
    ctx->hold_rem_L = hld_rem;
    ctx->release_rem_L = rls_rem;
    ctx->prev_abs_L = prev;
}


/* helper: compute target gain from absolute amplitude 'a' (both double, linear domain)
   returns value in [0..1] in Q24, where 1.0 == unity (no attenuation)
   Avoids any pow/log in hot path (all pow() used in init).
   prev_a is previous absolute amplitude for the channel (for rise detection).
*/
int32_t limiter_compute_target(struct DspLimiterCtx *ctx, int32_t a_q24, int32_t prev_q24)
{
    /* init locals */
    int is_rising = 0;
    int has_smoothing = ctx->smoothing;
    int32_t eps = ctx->eps_q24;
    int32_t target_q24 = DSP_ONE;
    int32_t in, t_q24, delta;
    int32_t out_a_q24, denom32;
    int64_t tmp64;

    if (a_q24 <= 0) return DSP_ONE;

    if (ctx->knee_width_db >= 0.0) {
        /* no knee: hard limit value (<= 1.0) */
        if (a_q24 > ctx->threshold_q24) {
            tmp64 = (int64_t)ctx->threshold_q24 << 24;
            denom32 = a_q24 > eps ? a_q24 : eps;
            target_q24 = (int32_t)(tmp64 / (int64_t)denom32);
        }

    } else {
        /* wave rising detection (if release > 0) */
        if (a_q24 > (prev_q24 + eps)) is_rising = 1;

        /* compressor-like knee: map input range [thr..1.0] -> [map_out_lo_q24..thr]
           Apply only on rising transients; descending sides are handled by release ramp.
           if below threshold, no mapping (bypass) */
        if ((is_rising || !has_smoothing) && a_q24 > ctx->map_in_lo_q24) {
            in = a_q24;
            if (in > ctx->map_in_hi_q24) in = ctx->map_in_hi_q24;

            /* normalized interpolation factor */
            if (ctx->map_in_den_q24 == 0) t_q24 = DSP_ONE;
            else {
                tmp64 = (int64_t)(in - ctx->map_in_lo_q24) << 24;
                t_q24 = (int32_t)(tmp64 / (int64_t)ctx->map_in_den_q24);
            }
            if (t_q24 < 0) t_q24 = 0;
            else if (t_q24 > DSP_ONE) t_q24 = DSP_ONE;

            /* compute target output amplitude */
            delta = ctx->map_out_hi_q24 - ctx->map_out_lo_q24;
            tmp64 = (int64_t)ctx->map_out_lo_q24 << 24;
            tmp64 += (int64_t)t_q24 * (int64_t)delta; /* Q48 */
            out_a_q24 = (int32_t)(tmp64 >> 24);

            tmp64 = (int64_t)out_a_q24 << 24;
            denom32 = a_q24 > eps ? a_q24 : eps;
            target_q24 = (int32_t)(tmp64 / (int64_t)denom32);
        }
    }

    /* target gain safety clamp */
    if (target_q24 > DSP_ONE) target_q24 = DSP_ONE;
    else if (target_q24 < 0) target_q24 = 0;

    return target_q24;
}


/* reset state */
void dsp_limiter_reset(struct DspLimiterCtx *ctx)
{
    if (!ctx) return;
    ctx->cur_gain_L = DSP_ONE;
    ctx->cur_gain_R = DSP_ONE;
    ctx->step_L = ctx->step_R = 0;
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

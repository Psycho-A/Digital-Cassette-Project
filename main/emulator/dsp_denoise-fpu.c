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
 * - Uses double for coefficients and state.
 * - Allocations done once in init (calloc). No per-sample allocation.
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspDenoiseCtx {
    /* config */
    int enabled;
    int rumble_enabled;

    /* number of filter cascades */
    int filter_n_sections;   /* filter_order / 2 */
    int rumble_n_sections;

    /* precomputed linear gains and dB values */
    double reduction_lin;    /* pow(10, reduction_db/20) */
    double enhance_lin;      /* pow(10, enhance_db/20) */

    /* per-channel cascaded sections: pointers to arrays length n_sections */
    biquad_d *hpL;     /* HP analysis chain (used to compute HF) */
    biquad_d *hpR;
    biquad_d *shelfL;  /* cascade high-shelf to be applied multiplicatively to full signal */
    biquad_d *shelfR;
    biquad_d *rumbleL;
    biquad_d *rumbleR;

    /* precomputed constants for fast on-the-fly coefficient recompute */
    double shelf_cs;       /* cos(omega) per section */
    double shelf_alpha;    /* alpha per section */
    double shelf_inv_den;  /* 1/(2*n_sections), используетс€ дл€ расчЄта */

    /* RMS sliding-window (per-channel) for HF detection */
    double  *rms_window_buf_L;   /* squared instantaneous HF power, double */
    double  *rms_window_buf_R;
    uint32_t rms_window_len;     /* length in samples */
    uint32_t rms_window_pos;     /* current write position */
    double   rms_window_sum_L;   /* running sum of buffer values */
    double   rms_window_sum_R;
    double   rms_inv_len;        /* optimization helper */

    /* RMS precomputed thresholds */
    double tiny_ms, epsilon;         /* no-noise/tiny gain bypass (optimization) */
    double red_low_ms, red_high_ms;  /* downward denoiser */
    double enh_low_ms, enh_high_ms;  /* upward peaks enhancer */

    /* gain-smoothing triggers */
    int has_smooth_red;  /* reduction_lin != 1, knee_width_db > 0 */
    int has_smooth_enh;  /* enhance_lin != 1, knee_width_db > 0 */

    /* attack/release implementation */
    uint32_t attack_samples;    /* precomputed from attack_ms */
    uint32_t release_samples;   /* precomputed from release_ms */
    double   total_lin_L_cur;   /* текущий (smoothed) gain, L */
    double   total_lin_R_cur;   /* текущий (smoothed) gain, R */
    double   total_lin_L_step;  /* per-sample step while ramping, L */
    double   total_lin_R_step;  /* per-sample step while ramping, R */
    double   last_target_L;     /* previously used total_lin, R */
    double   last_target_R;     /* previously used total_lin, R */
    uint32_t rem_samples_L;     /* remaining samples in ramp, L */
    uint32_t rem_samples_R;     /* remaining samples in ramp, R */

    /* previous values (optimization) */
    double total_lin_L_prev, total_lin_R_prev;
    double nb0_L_prev, nb1_L_prev, nb2_L_prev, na1_L_prev, na2_L_prev;
    double nb0_R_prev, nb1_R_prev, nb2_R_prev, na1_R_prev, na2_R_prev;

    /* pointer to select processing branch on init (stereo/dual-mono) */
    void (*process)(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);
};


/* some forwards for calling by init */
void dsp_denoise_free(struct DspDenoiseCtx*);
void dsp_denoise_process_stereo(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);
void dsp_denoise_process_mono(struct DspDenoiseCtx*, int32_t*, int32_t*, uint32_t);

/* fill initial values */
/* caller must fill cfg fields first (attack_ms, etc) */
struct DspDenoiseCtx* dsp_denoise_init(struct DspDenoiseCtx *ctx, DspDenoiseConfig cfg, int sample_rate)
{
    /* allocate container; caller will accept returned ctx */
    int i; fprintf(stderr, "Initializing DSP Denoise [fpu] module...\n");
    if (!ctx) ctx = (struct DspDenoiseCtx*)calloc(1, sizeof(struct DspDenoiseCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Denoise memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;
    ctx->rumble_enabled = (cfg.rumble_filter_order >= 2) ? 1 : 0;

    /* compute valid filters order */
    ctx->filter_n_sections = cfg.filter_order / 2;
    if (ctx->filter_n_sections < 1) ctx->filter_n_sections = 1; // guard

    /* pre-compute linear gains */
    ctx->reduction_lin = pow(10.0, cfg.reduction_level_db / 20.0);
    ctx->enhance_lin = pow(10.0, cfg.enhance_peaks_db / 20.0);
    if (ctx->reduction_lin <= 0.0) ctx->reduction_lin = 1e-12;  // guard clamp
    if (ctx->enhance_lin <= 0.0) ctx->enhance_lin = 1e-12;      // guard clamp

    /* cache invalid markers */
    ctx->epsilon = GAIN_EPSILON; // min gain step
    ctx->total_lin_L_prev = ctx->total_lin_R_prev = 1.0;

    /* bools to skip excess calcs in hot-path */
    ctx->has_smooth_red = (ctx->reduction_lin != 1.0 && cfg.knee_width_db > 0.0) ? 1 : 0;
    ctx->has_smooth_enh = (ctx->enhance_lin != 1.0 && cfg.knee_width_db > 0.0) ? 1 : 0;

    /* allocate biquad HPF sections per channel */
    ctx->hpL = (biquad_d*)calloc((size_t)ctx->filter_n_sections, sizeof(biquad_d));
    ctx->hpR = (biquad_d*)calloc((size_t)ctx->filter_n_sections, sizeof(biquad_d));
    if (!ctx->hpL || !ctx->hpR) {
        fprintf(stderr, "Failed to allocate DSP Denoise HPF sections.\n");
        dsp_denoise_free(ctx); return NULL;
    }
    /* initialize each section as identical HPF (same f0/Q) */
    for (i = 0; i < ctx->filter_n_sections; ++i) {
        biquad_init_highpass(&ctx->hpL[i], sample_rate, cfg.cutoff_freq_hz, cfg.filter_q);
        biquad_init_highpass(&ctx->hpR[i], sample_rate, cfg.cutoff_freq_hz, cfg.filter_q);
        ctx->hpL[i].z1 = ctx->hpL[i].z2 = 0.0;
        ctx->hpR[i].z1 = ctx->hpR[i].z2 = 0.0;
    }

    /* initialize rumble high-pass filter */
    if (ctx->rumble_enabled) {
        ctx->rumble_n_sections = (int)cfg.rumble_filter_order / 2;
        if (ctx->rumble_n_sections < 1) ctx->rumble_n_sections = 1;

        ctx->rumbleL = (biquad_d*)calloc((size_t)ctx->rumble_n_sections, sizeof(biquad_d));
        ctx->rumbleR = (biquad_d*)calloc((size_t)ctx->rumble_n_sections, sizeof(biquad_d));
        if (!ctx->rumbleL || !ctx->rumbleR) {
            fprintf(stderr, "Failed to allocate DSP Denoise rumble sections.\n");
            dsp_denoise_free(ctx); return NULL;
        }
        for (i = 0; i < ctx->rumble_n_sections; ++i) {
            biquad_init_highpass(&ctx->rumbleL[i], sample_rate, cfg.rumble_cutoff_hz, Q_BUTTERWORTH);
            biquad_init_highpass(&ctx->rumbleR[i], sample_rate, cfg.rumble_cutoff_hz, Q_BUTTERWORTH);
            ctx->rumbleL[i].z1 = ctx->rumbleL[i].z2 = 0.0;
            ctx->rumbleR[i].z1 = ctx->rumbleR[i].z2 = 0.0;
        }
    }

    /* initialize the high-shelf cascade:
       - create per-section shelf arrays and distribute total reduction_db evenly across sections
       - per_section_db = total_db / n_sections */
    ctx->shelfL = (biquad_d*)calloc((size_t)ctx->filter_n_sections, sizeof(biquad_d));
    ctx->shelfR = (biquad_d*)calloc((size_t)ctx->filter_n_sections, sizeof(biquad_d));
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
        for (i = 0; i < ctx->filter_n_sections; ++i) {
            ctx->shelfL[i].z1 = ctx->shelfL[i].z2 = 0.0;
            ctx->shelfR[i].z1 = ctx->shelfR[i].z2 = 0.0;
        }
    }

    /* allocate RMS windows (per-channel)
       window length: rms_periods or attack_ms/4 */
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
        ctx->rms_window_sum_L = 0.0;
        ctx->rms_window_sum_R = 0.0;
        ctx->rms_inv_len = 1.0 / (double)wlen;
        ctx->rms_window_buf_L = (double*)calloc((size_t)wlen, sizeof(double));
        ctx->rms_window_buf_R = (double*)calloc((size_t)wlen, sizeof(double));
        if (!ctx->rms_window_buf_L || !ctx->rms_window_buf_R) {
            fprintf(stderr, "Failed to allocate DSP Denoise RMS windows.\n");
            dsp_denoise_free(ctx); return NULL;
        }
    }

    /* precompute RMS-domain thresholds and tiny threshold (avoid log/sqrt in hot loop) */
    {
        double tiny_db = -80.0;  // no-noise marker
        double red_low_db  = cfg.noise_threshold_db - cfg.knee_width_db * 0.5;
        double red_high_db = cfg.noise_threshold_db + cfg.knee_width_db * 0.5;
        double enh_low_db  = cfg.peaks_floor_db - cfg.knee_width_db * 0.5;
        double enh_high_db = cfg.peaks_floor_db + cfg.knee_width_db * 0.5;

        /* tiny threshold: if RMS below -80 dB -> treat as clean (no work) */
        ctx->tiny_ms = pow(10.0, tiny_db / 10.0); /* = 1e-8 */

        /* denoiser: if RMS below noise_threshold_db, apply reduction_db */
        ctx->red_low_ms = pow(10.0, red_low_db / 10.0);
        ctx->red_high_ms = pow(10.0, red_high_db / 10.0);

        /* enhance peaks: if RMS above peaks_floor_db, apply enhance_db */
        ctx->enh_low_ms = pow(10.0, enh_low_db / 10.0);
        ctx->enh_high_ms = pow(10.0, enh_high_db / 10.0);

        if (!ctx->red_low_ms  || ctx->red_low_ms <= 0.0) ctx->red_low_ms = 0.0;
        if (!ctx->red_high_ms || ctx->red_high_ms <= ctx->red_low_ms) ctx->red_high_ms = ctx->red_low_ms + 1e-12;
        if (!ctx->enh_low_ms  || ctx->enh_low_ms <= 0.0) ctx->enh_low_ms = 0.0;
        if (!ctx->enh_high_ms || ctx->enh_high_ms <= ctx->enh_low_ms) ctx->enh_high_ms = ctx->enh_low_ms + 1e-12;
    }

    /* initialize Attack/Release values */
    ctx->rem_samples_L = ctx->rem_samples_R = 0;
    ctx->total_lin_L_cur = ctx->total_lin_R_cur = 1.0;  // unity
    ctx->total_lin_L_step = ctx->total_lin_R_step = 0.0;
    ctx->last_target_L = ctx->last_target_R = 1.0;

    /* pre-compute attack/release sample counts */
    ctx->attack_samples  = (uint32_t)( (cfg.attack_ms  * 0.001 * sample_rate) + 0.5 );
    ctx->release_samples = (uint32_t)( (cfg.release_ms * 0.001 * sample_rate) + 0.5 );
    if (ctx->attack_samples < 1) ctx->attack_samples = 1;  // guard
    if (ctx->release_samples < 1) ctx->release_samples = 1;  // guard

    /* select stereo or mono processing branch */
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


/* верси€ с независимой по-канальной обработкой
   -------------------------------------------- */
void dsp_denoise_process_stereo(struct DspDenoiseCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    /* preloaded aliases (optimization) */
    double eps = ctx->epsilon;  // 1/q24
    double near_1m = 1.0 - eps;
    double near_1g = 1.0 + eps;

    double tiny_ms = ctx->tiny_ms;  // no-noise bypass marker
    double red_low_ms  = ctx->red_low_ms;   // reduction knee 0
    double red_high_ms = ctx->red_high_ms;  // reduction knee 1
    double enh_low_ms  = ctx->enh_low_ms;   // enhance peaks knee 0
    double enh_high_ms = ctx->enh_high_ms;  // enhance peaks knee 1

    uint32_t wlen = ctx->rms_window_len;
    double rms_inv_len = ctx->rms_inv_len;
    uint32_t filter_nsec = ctx->filter_n_sections;
    uint32_t rumble_nsec = ctx->rumble_n_sections;
    uint32_t rumble_enabled = ctx->rumble_enabled;
    uint32_t smoothing_enabled = (ctx->attack_samples > 1 || ctx->release_samples > 1);

    double cs = ctx->shelf_cs;
    double alpha = ctx->shelf_alpha;
    double inv_den = ctx->shelf_inv_den;

    biquad_d *hpL = ctx->hpL, *hpR = ctx->hpR;
    biquad_d *shelfL = ctx->shelfL, *shelfR = ctx->shelfR;
    biquad_d *rumbleL = ctx->rumbleL, *rumbleR = ctx->rumbleR;

    /* per-sample processing:
       convert Q24 -> double, filter, convert back */
    uint32_t i, s;
    for (i = 0; i < n; ++i) {
        double xl_in, xr_in;
        double hp_l, hp_r;
        double out_l, out_r;
        double yl_scaled, yr_scaled;

        double inst_sq_L, inst_sq_R;
        double mean_ms_L, mean_ms_R;
        double total_lin_L, total_lin_R;
        double nb0,nb1,nb2, na1,na2;
        double gain_diff = 0.0;

        /* normalize input Q24 -> double */
        xl_in = (double)l[i] / (double)DSP_ONE;
        xr_in = (double)r[i] / (double)DSP_ONE;

        /* ===== HF ANALYSIS / PROCESSING POINT =====
           - RMS, envelope [hp_l/hp_r]
           - downward / upward expanding
        */

        /* HF extraction via cascaded HPF (analysis branch) */
        hp_l = xl_in; hp_r = xr_in;
        for (s = 0; s < filter_nsec; ++s) {
            hp_l = biquad_process_sample(&hpL[s], hp_l);
            hp_r = biquad_process_sample(&hpR[s], hp_r);
        }

        /* instantaneous squared per-channel (store for window) */
        inst_sq_L = hp_l * hp_l; inst_sq_R = hp_r * hp_r;

        /* update RMS sliding window (mean-square domain) */
        /* - subtract oldest, add new, update sum */
        {
            double old_L = ctx->rms_window_buf_L[ctx->rms_window_pos];
            double old_R = ctx->rms_window_buf_R[ctx->rms_window_pos];
            /* we stored squared-power per channel previously; combine to mean-square here */
            ctx->rms_window_sum_L -= old_L;
            ctx->rms_window_sum_R -= old_R;
            /* store new per-channel squared values (for future) */
            ctx->rms_window_buf_L[ctx->rms_window_pos] = inst_sq_L;
            ctx->rms_window_buf_R[ctx->rms_window_pos] = inst_sq_R;
            ctx->rms_window_sum_L += inst_sq_L;
            ctx->rms_window_sum_R += inst_sq_R;
            /* advance position */
            if (++ctx->rms_window_pos >= wlen) ctx->rms_window_pos = 0;
        }

        /* compute window mean-square (per channel) */
        mean_ms_L = ctx->rms_window_sum_L * rms_inv_len;
        mean_ms_R = ctx->rms_window_sum_R * rms_inv_len;

        /* set default gain (bypass) */
        total_lin_L = 1.0; total_lin_R = 1.0;

        /* compute total_lin per channel WITHOUT dB ops:
           - if mean_ms <= red_low_ms -> full reduction (reduction_lin)
           - if mean_ms >= red_high_ms -> unity (1.0)
           - else -> linear interpolation in MS-domain between reduction_lin and 1.0
           - quick skip: if very quiet Ч no processing.
        */
        if (mean_ms_L > tiny_ms) {
            if (mean_ms_L <= red_low_ms) total_lin_L = ctx->reduction_lin;
            else if (mean_ms_L >= enh_high_ms) total_lin_L = ctx->enhance_lin;

            /* linear interpolations in MS domain */
            else if (ctx->has_smooth_red && mean_ms_L > red_low_ms && mean_ms_L < red_high_ms) {
                double t = (red_high_ms - mean_ms_L) / (red_high_ms - red_low_ms); /* from 1->0 */
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_L = t * ctx->reduction_lin + (1.0 - t) * 1.0;
            }
            else if (ctx->has_smooth_enh && mean_ms_L > enh_low_ms && mean_ms_L < enh_high_ms) {
                double t = (mean_ms_L - enh_low_ms) / (enh_high_ms - enh_low_ms);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_L = (1.0 - t) * 1.0 + t * ctx->enhance_lin;
            }
        }
        if (mean_ms_R > tiny_ms) {
            if (mean_ms_R <= red_low_ms) total_lin_R = ctx->reduction_lin;
            else if (mean_ms_R >= enh_high_ms) total_lin_R = ctx->enhance_lin;

            else if (ctx->has_smooth_red && mean_ms_R > red_low_ms && mean_ms_R < red_high_ms) {
                double t = (red_high_ms - mean_ms_R) / (red_high_ms - red_low_ms);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_R = t * ctx->reduction_lin + (1.0 - t) * 1.0;
            }
            else if (ctx->has_smooth_enh && mean_ms_R > enh_low_ms && mean_ms_R < enh_high_ms) {
                double t = (mean_ms_R - enh_low_ms) / (enh_high_ms - enh_low_ms);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_R = (1.0 - t) * 1.0 + t * ctx->enhance_lin;
            }
        }

        /* === Smoothing (cheap, robust, no fabs in hot-path) === */
        if (smoothing_enabled) {
            /* LEFT channel */
            /* detect if target has changed since last time using last_target_L (cheap) */
            gain_diff = total_lin_L - ctx->last_target_L;
            if (gain_diff < 0.0) gain_diff = -gain_diff;

            if (ctx->rem_samples_L == 0 || gain_diff > eps) {
                /* start (or restart) ramp toward new target */
                ctx->last_target_L = total_lin_L; /* record current goal */

                /* choose ramp length: attack when increasing (fast), release when decreasing */
                if (total_lin_L < ctx->total_lin_L_cur) ctx->rem_samples_L = ctx->release_samples;
                else ctx->rem_samples_L = ctx->attack_samples;

                /* precaution */
                if (ctx->rem_samples_L == 0) ctx->rem_samples_L = 1;
                ctx->total_lin_L_step = (total_lin_L - ctx->total_lin_L_cur) / (double)ctx->rem_samples_L;
            }
            /* advance one step if ramp in progress */
            if (ctx->rem_samples_L > 0) {
                ctx->total_lin_L_cur += ctx->total_lin_L_step;
                ctx->rem_samples_L--;
                if (ctx->rem_samples_L == 0) {
                    /* snap to exact target to avoid accumulating FP error */
                    ctx->total_lin_L_cur = total_lin_L;
                    ctx->total_lin_L_step = 0.0;
                }
            }

            /* RIGHT channel (mirror) */
            gain_diff = total_lin_R - ctx->last_target_R;
            if (gain_diff < 0.0) gain_diff = -gain_diff;

            if (ctx->rem_samples_R == 0 || gain_diff > eps) {
                ctx->last_target_R = total_lin_R;
                if (total_lin_R < ctx->total_lin_R_cur) ctx->rem_samples_R = ctx->release_samples;
                else ctx->rem_samples_R = ctx->attack_samples;

                if (ctx->rem_samples_R == 0) ctx->rem_samples_R = 1;
                ctx->total_lin_R_step = (total_lin_R - ctx->total_lin_R_cur) / (double)ctx->rem_samples_R;
            }
            if (ctx->rem_samples_R > 0) {
                ctx->total_lin_R_cur += ctx->total_lin_R_step;
                ctx->rem_samples_R--;
                if (ctx->rem_samples_R == 0) {
                    ctx->total_lin_R_cur = total_lin_R;
                    ctx->total_lin_R_step = 0.0;
                }
            }

            /* use the smoothed values for next processing */
            total_lin_L = ctx->total_lin_L_cur;
            total_lin_R = ctx->total_lin_R_cur;
        }

        /* === Per-sample recompute shelf coefficients and process sample ===
           here we want per-sample/per-channel dynamic shelf gain applied to HF
        */
        /* start from dry input */
        out_l = xl_in; out_r = xr_in;
        {
            /* apply new coeffs to each shelf section using their own state */
            /* if total_lin ~= 1.0 -> skip all shelf recompute and processing */
            if (total_lin_L < near_1m || total_lin_L > near_1g) {
                /* optimization: recompute coeffs ONLY on real gain change (1/Q24) */
                gain_diff = total_lin_L - ctx->total_lin_L_prev;
                if (gain_diff < 0.0) gain_diff = -gain_diff;

                if (gain_diff > eps) {
                    compute_shelf_coeffs(total_lin_L, inv_den,cs,alpha, &nb0,&nb1,&nb2, &na1,&na2);
                    /* keep current values to compare in next sample */
                    ctx->total_lin_L_prev = total_lin_L;
                    ctx->nb0_L_prev = nb0; ctx->nb1_L_prev = nb1; ctx->nb2_L_prev = nb2;
                    ctx->na1_L_prev = na1; ctx->na2_L_prev = na2;
                } else {
                    /* otherwise, just re-use previous values without recomputing */
                    nb0 = ctx->nb0_L_prev; nb1 = ctx->nb1_L_prev; nb2 = ctx->nb2_L_prev;
                    na1 = ctx->na1_L_prev; na2 = ctx->na2_L_prev;
                }
                for (s = 0; s < filter_nsec; ++s) {
                    out_l = biquad_process_with_coeffs(&shelfL[s], nb0,nb1,nb2, na1,na2, out_l);
                }
            }
            if (total_lin_R < near_1m || total_lin_R > near_1g) {
                gain_diff = total_lin_R - ctx->total_lin_R_prev;
                if (gain_diff < 0.0) gain_diff = -gain_diff;

                if (gain_diff > eps) {
                    compute_shelf_coeffs(total_lin_R, inv_den,cs,alpha, &nb0,&nb1,&nb2, &na1,&na2);
                    ctx->total_lin_R_prev = total_lin_R;
                    ctx->nb0_R_prev = nb0; ctx->nb1_R_prev = nb1; ctx->nb2_R_prev = nb2;
                    ctx->na1_R_prev = na1; ctx->na2_R_prev = na2;
                } else {
                    nb0 = ctx->nb0_R_prev; nb1 = ctx->nb1_R_prev; nb2 = ctx->nb2_R_prev;
                    na1 = ctx->na1_R_prev; na2 = ctx->na2_R_prev;
                }
                for (s = 0; s < filter_nsec; ++s) {
                    out_r = biquad_process_with_coeffs(&shelfR[s], nb0,nb1,nb2, na1,na2, out_r);
                }
            }
        }

        /* ===== POST-PROCESSING POINT ===== */

        /* apply rumble HP-filter if set (after shelf) */
        if (rumble_enabled) {
            for (s = 0; s < rumble_nsec; ++s) {
                out_l = biquad_process_sample(&rumbleL[s], out_l);
                out_r = biquad_process_sample(&rumbleR[s], out_r);
            }
        }

        /* convert back to Q24 (with saturation to int32 range) */
        yl_scaled = out_l * (double)DSP_ONE;
        yr_scaled = out_r * (double)DSP_ONE;

        /* saturate to int32 */
        if (yl_scaled >= 2147483647.0) l[i] = 2147483647;
        else if (yl_scaled <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)(yl_scaled);

        if (yr_scaled >= 2147483647.0) r[i] = 2147483647;
        else if (yr_scaled <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)(yr_scaled);
    }
}


/* верси€ с ћќЌќ-обработкой обоих каналов (~30% быстрее)
   ----------------------------------------------------- */
void dsp_denoise_process_mono(struct DspDenoiseCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    double eps = ctx->epsilon;
    double near_1m = 1.0 - eps;
    double near_1g = 1.0 + eps;

    double tiny_ms = ctx->tiny_ms;
    double red_low_ms = ctx->red_low_ms;
    double red_high_ms = ctx->red_high_ms;
    double enh_low_ms = ctx->enh_low_ms;
    double enh_high_ms = ctx->enh_high_ms;

    uint32_t wlen = ctx->rms_window_len;
    double rms_inv_len = ctx->rms_inv_len;
    uint32_t filter_nsec = ctx->filter_n_sections;
    uint32_t rumble_nsec = ctx->rumble_n_sections;
    uint32_t rumble_enabled = ctx->rumble_enabled;
    uint32_t smoothing_enabled = (ctx->attack_samples > 1 || ctx->release_samples > 1);

    double cs = ctx->shelf_cs;
    double alpha = ctx->shelf_alpha;
    double inv_den = ctx->shelf_inv_den;

    biquad_d *hpL = ctx->hpL, *hpR = ctx->hpR;
    biquad_d *shelfL = ctx->shelfL, *shelfR = ctx->shelfR;
    biquad_d *rumbleL = ctx->rumbleL, *rumbleR = ctx->rumbleR;

    uint32_t i, s;
    for (i = 0; i < n; ++i) {
        double xl_in, xr_in;
        double hp_l, hp_r;
        double out_l, out_r;
        double yl_scaled, yr_scaled;

        double inst_sq_L, inst_sq_R;
        double mean_ms;  // unified mean
        double total_lin_shared;
        double nb0,nb1,nb2,na1,na2;
        double gain_diff = 0.0;

        xl_in = (double)l[i] / (double)DSP_ONE;
        xr_in = (double)r[i] / (double)DSP_ONE;

        hp_l = xl_in; hp_r = xr_in;
        for (s = 0; s < filter_nsec; ++s) {
            hp_l = biquad_process_sample(&hpL[s], hp_l);
            hp_r = biquad_process_sample(&hpR[s], hp_r);
        }

        inst_sq_L = hp_l * hp_l; inst_sq_R = hp_r * hp_r;

        /* update windows & sums */
        {
            double oldL = ctx->rms_window_buf_L[ctx->rms_window_pos];
            double oldR = ctx->rms_window_buf_R[ctx->rms_window_pos];
            ctx->rms_window_sum_L -= oldL;
            ctx->rms_window_sum_R -= oldR;
            ctx->rms_window_buf_L[ctx->rms_window_pos] = inst_sq_L;
            ctx->rms_window_buf_R[ctx->rms_window_pos] = inst_sq_R;
            ctx->rms_window_sum_L += inst_sq_L;
            ctx->rms_window_sum_R += inst_sq_R;
            if (++ctx->rms_window_pos >= wlen) ctx->rms_window_pos = 0;
        }

        /* mean across channels (MS domain) */
        mean_ms = (ctx->rms_window_sum_L + ctx->rms_window_sum_R) * (0.5 * rms_inv_len);

        /* compute total_lin_shared */
        total_lin_shared = 1.0;
        if (mean_ms > tiny_ms) {
            if (mean_ms <= red_low_ms) total_lin_shared = ctx->reduction_lin;
            else if (mean_ms >= enh_high_ms) total_lin_shared = ctx->enhance_lin;

            else if (ctx->has_smooth_red && mean_ms > red_low_ms && mean_ms < red_high_ms) {
                double t = (red_high_ms - mean_ms) / (red_high_ms - red_low_ms);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_shared = t * ctx->reduction_lin + (1.0 - t) * 1.0;
            } else if (ctx->has_smooth_enh && mean_ms > enh_low_ms && mean_ms < enh_high_ms) {
                double t = (mean_ms - enh_low_ms) / (enh_high_ms - enh_low_ms);
                if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                total_lin_shared = (1.0 - t) * 1.0 + t * ctx->enhance_lin;
            }
        }

        /* single smoothing (one ramp) Ч store result in both cur_total_lin_* */
        if (smoothing_enabled) {
            gain_diff = total_lin_shared - ctx->last_target_L;
            if (gain_diff < 0.0) gain_diff = -gain_diff;

            if (ctx->rem_samples_L == 0 || gain_diff > eps) {
                ctx->last_target_L = total_lin_shared;
                /* choose ramp: keep your policy; here we use same as stereo branch */
                if (total_lin_shared < ctx->total_lin_L_cur) ctx->rem_samples_L = ctx->release_samples;
                else ctx->rem_samples_L = ctx->attack_samples;
                if (ctx->rem_samples_L == 0) ctx->rem_samples_L = 1;
                ctx->total_lin_L_step = (total_lin_shared - ctx->total_lin_L_cur) / (double)ctx->rem_samples_L;
            }
            if (ctx->rem_samples_L > 0) {
                ctx->total_lin_L_cur += ctx->total_lin_L_step;
                ctx->rem_samples_L--;
                if (ctx->rem_samples_L == 0) ctx->total_lin_L_cur = total_lin_shared;
            }
            /* copy smoothed value to right channel state (shared smoothing) */
            ctx->total_lin_R_cur = ctx->total_lin_L_cur;
            total_lin_shared = ctx->total_lin_L_cur;
        }

        /* apply shelf once (compute once) */
        out_l = xl_in; out_r = xr_in;

        if (total_lin_shared < near_1m || total_lin_shared > near_1g) {
            gain_diff = total_lin_shared - ctx->total_lin_L_prev;
            if (gain_diff < 0.0) gain_diff = -gain_diff;

            if (gain_diff > eps) {
                compute_shelf_coeffs(total_lin_shared, inv_den, cs, alpha, &nb0,&nb1,&nb2,&na1,&na2);
                /* save values in left prev slot (we'll reuse for right) */
                ctx->total_lin_L_prev = total_lin_shared;
                ctx->nb0_L_prev = nb0; ctx->nb1_L_prev = nb1; ctx->nb2_L_prev = nb2;
                ctx->na1_L_prev = na1; ctx->na2_L_prev = na2;
            } else {
                nb0 = ctx->nb0_L_prev; nb1 = ctx->nb1_L_prev; nb2 = ctx->nb2_L_prev;
                na1 = ctx->na1_L_prev; na2 = ctx->na2_L_prev;
            }
            /* apply the same coeffs to both left and right shelf chains */
            for (s = 0; s < filter_nsec; ++s) out_l = biquad_process_with_coeffs(&shelfL[s], nb0,nb1,nb2, na1,na2, out_l);
            for (s = 0; s < filter_nsec; ++s) out_r = biquad_process_with_coeffs(&shelfR[s], nb0,nb1,nb2, na1,na2, out_r);
        }

        /* rumble filter */
        if (rumble_enabled) {
            for (s = 0; s < rumble_nsec; ++s) {
                out_l = biquad_process_sample(&rumbleL[s], out_l);
                out_r = biquad_process_sample(&rumbleR[s], out_r);
            }
        }

        /* write-back */
        yl_scaled = out_l * (double)DSP_ONE;
        yr_scaled = out_r * (double)DSP_ONE;

        if (yl_scaled >= 2147483647.0) l[i] = 2147483647;
        else if (yl_scaled <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)(yl_scaled);
        if (yr_scaled >= 2147483647.0) r[i] = 2147483647;
        else if (yr_scaled <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)(yr_scaled);
    }
}


/* reset states */
void dsp_denoise_reset(struct DspDenoiseCtx *ctx)
{
    int i;
    if (!ctx) return;

    /* сброс фильтров */
    for (i = 0; i < ctx->filter_n_sections; ++i) {
        ctx->hpL[i].z1 = ctx->hpL[i].z2 = 0.0;
        ctx->hpR[i].z1 = ctx->hpR[i].z2 = 0.0;
        ctx->shelfL[i].z1 = ctx->shelfL[i].z2 = 0.0;
        ctx->shelfR[i].z1 = ctx->shelfR[i].z2 = 0.0;
    }
    if (ctx->rumble_enabled) {
        for (i = 0; i < ctx->rumble_n_sections; ++i) {
            ctx->rumbleL[i].z1 = ctx->rumbleL[i].z2 = 0.0;
            ctx->rumbleR[i].z1 = ctx->rumbleR[i].z2 = 0.0;
        }
    }
    /* сброс RMS окон */
    ctx->rms_window_sum_L = 0.0;
    ctx->rms_window_sum_R = 0.0;
    ctx->rms_window_pos = 0;
    memset(ctx->rms_window_buf_L, 0, ctx->rms_window_len * sizeof(double));
    memset(ctx->rms_window_buf_R, 0, ctx->rms_window_len * sizeof(double));

    /* сброс gain-состо€ний */
    ctx->total_lin_L_cur = 1.0;
    ctx->total_lin_R_cur = 1.0;
    ctx->last_target_L = 1.0;
    ctx->last_target_R = 1.0;
    ctx->rem_samples_L = 0;
    ctx->rem_samples_R = 0;
    ctx->total_lin_L_step = 0.0;
    ctx->total_lin_R_step = 0.0;
    ctx->total_lin_L_prev = 0.0;
    ctx->total_lin_R_prev = 0.0;
}

/* clean-up: free memory */
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

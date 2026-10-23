/* rx_dsp_width.c
 *
 * Stereo widen: double implementation (converted to Q24 at I/O)
 * Width = 0% -> mono, 100% -> original, 200% -> double width.
 * Uses biquads for optional high-pass / low-pass filtering.
 * Uses DSP_ONE and DSP_TWO_PI from dsp_host.h.
 *
 * Notes:
 * - Internal arithmetic in double, then converted to Q24 at output.
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspWidthCtx {
    /* config */
    int enabled;

    /* precomputed params (double) */
    double side_gain;  // усиление side-канала (линейное)

    /* HPF/LPF on side (double) */
    int hpf_enabled;
    int lpf_enabled;

    /* double-биквады */
    biquad_d hpf;
    biquad_d lpf;
};


/* fill initial values */
/* caller must fill cfg fields first (width_percent, etc) */
struct DspWidthCtx* dsp_width_init(struct DspWidthCtx *ctx, DspWidthConfig cfg, int sample_rate)
{
    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP Width [fpu] module...\n");
    if (!ctx) ctx = (struct DspWidthCtx*)calloc(1, sizeof(struct DspWidthCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Width memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;

    /* pre-compute mid/side gains (linear) */
    ctx->side_gain = (double)cfg.width_percent / 100.0;
    if (ctx->side_gain < 0.0) ctx->side_gain = 0.0;

    /* high-pass / low-pass filtering */
    ctx->hpf_enabled = cfg.hpf_enable ? 1 : 0;
    if (ctx->hpf_enabled) {
        biquad_init_highpass(&ctx->hpf, sample_rate, cfg.hpf_cutoff_hz, Q_BUTTERWORTH);
    }
    ctx->lpf_enabled = cfg.lpf_enable ? 1 : 0;
    if (ctx->lpf_enabled) {
        biquad_init_lowpass(&ctx->lpf, sample_rate, cfg.lpf_cutoff_hz, Q_BUTTERWORTH);
    }

    return ctx;
}


/* Process: in-place buffers l[], r[] in Q24 */
void dsp_width_process(struct DspWidthCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    /* init locals */
    uint32_t i;
    double xl, xr;
    double out_l, out_r;
    double M, S, S_scaled;
    double yl_scaled, yr_scaled;

    double side_gain = ctx->side_gain;
    int hpf_enabled = ctx->hpf_enabled;
    int lpf_enabled = ctx->lpf_enabled;
    biquad_d *hpf = &ctx->hpf;
    biquad_d *lpf = &ctx->lpf;

    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    for (i = 0; i < n; ++i) {
        /* normalize */
        xl = (double)l[i] / (double)DSP_ONE;
        xr = (double)r[i] / (double)DSP_ONE;

        /* создаём mid/side каналы */
        M = (xl + xr) * 0.5;
        S = (xl - xr) * 0.5;

        /* применяем HPF/LPF к side */
        if (hpf_enabled) S = biquad_process_sample(hpf, S);
        if (lpf_enabled) S = biquad_process_sample(lpf, S);

        /* применяем расширение к side */
        S_scaled = S * side_gain;

        /* создаём новые L/R-каналы из m+s */
        out_l = M + S_scaled;
        out_r = M - S_scaled;

        /* преобразование в Q24 с насыщением */
        yl_scaled = out_l * (double)DSP_ONE;
        yr_scaled = out_r * (double)DSP_ONE;

        if (yl_scaled >= 2147483647.0) l[i] = 2147483647;
        else if (yl_scaled <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)yl_scaled;

        if (yr_scaled >= 2147483647.0) r[i] = 2147483647;
        else if (yr_scaled <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)yr_scaled;
    }
}


/* reset internal state (clear filter registers) */
void dsp_width_reset(struct DspWidthCtx *ctx)
{
    if (!ctx) return;
    ctx->hpf.z1 = ctx->hpf.z2 = 0.0;
    ctx->lpf.z1 = ctx->lpf.z2 = 0.0;
}

/* free context memory */
void dsp_width_free(struct DspWidthCtx *ctx)
{
    if (!ctx) return;
    free(ctx);
}

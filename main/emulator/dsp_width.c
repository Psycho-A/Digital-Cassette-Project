/* rx_dsp_width.c
 *
 * Stereo widen: Q24 implementation (C89)
 * Width = 0% -> mono, 100% -> original, 200% -> double width.
 * Uses biquads for optional high-pass / low-pass filtering.
 * Uses GAIN_EPSILON from dsp_host.h.
 *
 * Notes:
 * - Uses int64 for internal arithmetic (temporary?).
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspWidthCtx {
    /* config */
    int enabled;

    /* precomputed params (Q24) */
    int32_t side_gain_q24;  // усиление side-канала

    /* HPF/LPF on side (Q24) */
    int hpf_enabled;
    int lpf_enabled;

    /* целочисленные biquad */
    biquad_i hpf;
    biquad_i lpf;
};


/* fill initial values */
/* caller must fill cfg fields first (width_percent, etc) */
struct DspWidthCtx* dsp_width_init(struct DspWidthCtx *ctx, DspWidthConfig cfg, int sample_rate)
{
    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP Width [int] module...\n");
    if (!ctx) ctx = (struct DspWidthCtx*)calloc(1, sizeof(struct DspWidthCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP Width memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;

    /* pre-compute mid/side gains (linear/Q24) */
    {
        /* width_percent = values > 100% means increase side */
        double side_lin = (double)cfg.width_percent / 100.0;
        if (side_lin < 0.0) side_lin = 0.0;
        ctx->side_gain_q24 = (int32_t)(side_lin * (double)DSP_ONE + 0.5);
    }

    /* high-pass / low-pass filtering */
    ctx->hpf_enabled = cfg.hpf_enable ? 1 : 0;
    if (ctx->hpf_enabled) {
        biquad_d bf;
        biquad_init_highpass(&bf, sample_rate, cfg.hpf_cutoff_hz, Q_BUTTERWORTH);
        biquad_int_from_double(&ctx->hpf, &bf, DSP_ONE);
    }
    ctx->lpf_enabled = cfg.lpf_enable ? 1 : 0;
    if (ctx->lpf_enabled) {
        biquad_d bf;
        biquad_init_lowpass(&bf, sample_rate, cfg.lpf_cutoff_hz, Q_BUTTERWORTH);
        biquad_int_from_double(&ctx->lpf, &bf, DSP_ONE);
    }

    return ctx;
}


/* Process: in-place buffers l[], r[] in Q24 */
void dsp_width_process(struct DspWidthCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    // переменные
    uint32_t i;
    int32_t xl, xr;
    int32_t M, S;
    int64_t S_scaled;

    int32_t side_gain = ctx->side_gain_q24;
    int hpf_enabled = ctx->hpf_enabled;
    int lpf_enabled = ctx->lpf_enabled;
    biquad_i *hpf = &ctx->hpf;
    biquad_i *lpf = &ctx->lpf;

    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    for (i = 0; i < n; ++i) {
        // временные буферы
        xl = l[i]; xr = r[i];

        // создаём mid/side каналы
        M = (xl + xr) / 2;  // всегда помещается в int32
        S = (xl - xr) / 2;

        // применяем HPF/LPF к side (64 бит)
        if (hpf_enabled) S = biquad_process_int(hpf, S, 24);
        if (lpf_enabled) S = biquad_process_int(lpf, S, 24);

        // применяем расширение к side
        S_scaled = ((int64_t)S * side_gain) >> 24;  // необходимо 64 бита

        // создаём новые L/R-каналы из m+s
        l[i] = M + (int32_t)S_scaled;
        r[i] = M - (int32_t)S_scaled;
    }
}


/* reset internal state (clear filter registers) */
void dsp_width_reset(struct DspWidthCtx *ctx)
{
    if (!ctx) return;
    biquad_reset_int(&ctx->hpf);
    biquad_reset_int(&ctx->lpf);
}

/* free context memory */
void dsp_width_free(struct DspWidthCtx *ctx)
{
    if (!ctx) return;
    free(ctx);
}

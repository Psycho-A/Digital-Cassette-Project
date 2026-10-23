/* dsp_host.c (C89-friendly, no int64)
 * The main DSP-analog node for effect modules
 * All assets are RX/TX_ANALOG compatible.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp_host.h"

/* ---------------------
   common module helpers
   --------------------- /*

/* fixed helper (use double to avoid int64) */
int32_t q24_mul(int32_t a, int32_t b)
{
    double res = ((double)a * (double)b) / (double)DSP_ONE;
    if (res > 2147483647.0) res = 2147483647.0;
    if (res < -2147483648.0) res = -2147483648.0;
    return (int32_t)res;
}

/* db -> linear Q24 (no cap here; max_gain may be > 1.0) */
int32_t db_to_q24(float db)
{
    double lin = pow(10.0, (double)db / 20.0);
    double v = lin * (double)DSP_ONE;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    return (int32_t)(v);
}

/* convert ms-time constant -> smoothing alpha (per-sample)
   alpha = 1 - exp(-1/(tau_samples))
   where tau_samples = tau_seconds * sr
*/
int32_t time_to_alpha_q24(double ms, uint32_t sr)
{
    double tau_s, denom, a;
    if (ms <= 0.0) return DSP_ONE;
    tau_s = ms * 0.001;
    denom = tau_s * (double)sr;
    if (denom <= 0.0) return DSP_ONE;
    a = 1.0 - exp(-1.0 / denom);
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return (int32_t)(a * (double)DSP_ONE);
}


/* ------------------------------
   include modules implementation
   ------------------------------
   (we include C file to keep a single TU and to let modules use helpers)
*/
#if USE_FIXED_POINT == 1
    #include "dsp_denoise.c"
    #include "dsp_width.c"
    #include "dsp_eq.c"
    #include "dsp_agc.c"
    #include "dsp_limiter.c"
#else
    #include "dsp_denoise-fpu.c"
    #include "dsp_width-fpu.c"
    #include "dsp_eq-fpu.c"
    #include "dsp_agc-fpu.c"
    #include "dsp_limiter-fpu.c"
#endif


/* initialize */
/* caller must fill config fields first (cfg->denoise, etc) */
void dsp_init(AudioDspCtx *ctx, const AppConfig *cfg, int src_rate)
{
    fprintf(stderr, "Initializing DSP chain [mode: %d]...\n", ctx->fsm_mode);
    if (!ctx || !cfg) { fprintf(stderr, "Failed to allocate DSP memory.\n"); return; }

    /* force sample rate from audio-input, otherwise use config value */
    ctx->sample_rate = src_rate >= 8000 ? src_rate : cfg->output.sample_rate;

    /* initialize effects */
    ctx->denoise = NULL;
    ctx->width = NULL; ctx->eq = NULL;
    ctx->agc = NULL; ctx->limiter = NULL;

    switch (ctx->fsm_mode) {
        case ANALOG_TX:   // TX mode (tape recording)
            ctx->enabled = cfg->analog_tx.enable_dsp;
            if (cfg->denoise_tx.enabled) ctx->denoise = dsp_denoise_init(ctx->denoise, cfg->denoise_tx, ctx->sample_rate);
            if (cfg->eq_tx.enabled) ctx->eq = dsp_eq_init(ctx->eq, cfg->eq_tx, ctx->sample_rate);
            if (cfg->agc_tx.enabled) ctx->agc = dsp_agc_init(ctx->agc, cfg->agc_tx, ctx->sample_rate);
            if (cfg->limiter_tx.enabled) ctx->limiter = dsp_limiter_init(ctx->limiter, cfg->limiter_tx, ctx->sample_rate);
            break;

        case ANALOG_RX:   // RX mode (tape/generic play)
            ctx->enabled = cfg->analog_rx.enable_dsp;
            if (cfg->denoise_rx.enabled) ctx->denoise = dsp_denoise_init(ctx->denoise, cfg->denoise_rx, ctx->sample_rate);
            if (cfg->width_rx.enabled) ctx->width = dsp_width_init(ctx->width, cfg->width_rx, ctx->sample_rate);
            if (cfg->eq_rx.enabled) ctx->eq = dsp_eq_init(ctx->eq, cfg->eq_rx, ctx->sample_rate);
            if (cfg->agc_rx.enabled) ctx->agc = dsp_agc_init(ctx->agc, cfg->agc_rx, ctx->sample_rate);
            if (cfg->limiter_rx.enabled) ctx->limiter = dsp_limiter_init(ctx->limiter, cfg->limiter_rx, ctx->sample_rate);
            break;

        case TXMOD_PRE:   // TX-modulation (pre-emphasis EQ)
            ctx->enabled = cfg->tx.preemphasis_eq;
            if (cfg->tx.eq.enabled) ctx->eq = dsp_eq_init(ctx->eq, cfg->tx.eq, ctx->sample_rate);
            break;

        case DEMOD_PRE:   // RX-demodulation (de-emphasis EQ)
            ctx->enabled = cfg->rx.preprocess_eq;
            if (cfg->rx.eq.enabled) ctx->eq = dsp_eq_init(ctx->eq, cfg->rx.eq, ctx->sample_rate);
            break;

        case DEMOD_POST:  // RX-demod_out (AAC-out post-process)
            ctx->enabled = cfg->rx.aac_postproc;
            if (cfg->dechirp_ad.enabled) ctx->denoise = dsp_denoise_init(ctx->denoise, cfg->dechirp_ad, ctx->sample_rate);
            if (cfg->width_ad.enabled) ctx->width = dsp_width_init(ctx->width, cfg->width_ad, ctx->sample_rate);
            if (cfg->eq_ad.enabled) ctx->eq = dsp_eq_init(ctx->eq, cfg->eq_ad, ctx->sample_rate);
            if (cfg->agc_ad.enabled) ctx->agc = dsp_agc_init(ctx->agc, cfg->agc_ad, ctx->sample_rate);
            if (cfg->limiter_ad.enabled) ctx->limiter = dsp_limiter_init(ctx->limiter, cfg->limiter_ad, ctx->sample_rate);
            break;
    }
}


/* processing (in-place) */
void dsp_process(AudioDspCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    if (!ctx) return;
    if (!ctx->enabled) return;
    if (n == 0) return;

    /* --- Q31 -> Q24 in-place (store Q24 back in l/r) --- */
    for (i = 0; i < n; ++i) {
        /* assume arithmetic right shift for signed ints (GCC/MSVC/clang do that) */
        int32_t lv = l[i], rv = r[i];
        l[i] = lv >> DSP_SHIFT; r[i] = rv >> DSP_SHIFT;
    }

    /* --- effects (operate in Q24, in-place) --- */
    if (ctx->denoise) dsp_denoise_process(ctx->denoise, l, r, n);
    if (ctx->width) dsp_width_process(ctx->width, l, r, n);
    if (ctx->eq) dsp_eq_process(ctx->eq, l, r, n);
    if (ctx->agc) dsp_agc_process(ctx->agc, l, r, n);
    if (ctx->limiter) dsp_limiter_process(ctx->limiter, l, r, n);

    /* --- final pass: convert Q24 back to Q31 --- */
    {
        int32_t max_q24_for_shift = 2147483647 / (1 << DSP_SHIFT);
        int32_t min_q24_for_shift = (-2147483647-1) / (1 << DSP_SHIFT);

        for (i = 0; i < n; ++i) {
            int32_t vl = l[i], vr = r[i];

            /* soft-clip (simple)
            int32_t excess;
            if (vl > SOFT_LIMIT_Q24) {
                excess = vl - SOFT_LIMIT_Q24;
                vl = SOFT_LIMIT_Q24 + (excess >> 1);
            } else if (vl < -SOFT_LIMIT_Q24) {
                excess = -SOFT_LIMIT_Q24 - vl;
                vl = -SOFT_LIMIT_Q24 - (excess >> 1);
            }
            if (vr > SOFT_LIMIT_Q24) {
                excess = vr - SOFT_LIMIT_Q24;
                vr = SOFT_LIMIT_Q24 + (excess >> 1);
            } else if (vr < -SOFT_LIMIT_Q24) {
                excess = -SOFT_LIMIT_Q24 - vr;
                vr = -SOFT_LIMIT_Q24 - (excess >> 1);
            } */

            /* convert back to Q31 (checks range to avoid overflow on left shift) */
            if (vl > max_q24_for_shift) l[i] = 2147483647;
            else if (vl < min_q24_for_shift) l[i] = (-2147483647-1);
            else l[i] = (vl << DSP_SHIFT);

            if (vr > max_q24_for_shift) r[i] = 2147483647;
            else if (vr < min_q24_for_shift) r[i] = (-2147483647-1);
            else r[i] = (vr << DSP_SHIFT);
        }
    }
}


/* reset state */
void dsp_reset(AudioDspCtx *ctx)
{
    if (!ctx) return;

    if (ctx->denoise) dsp_denoise_reset(ctx->denoise);
    if (ctx->width) dsp_width_reset(ctx->width);
    if (ctx->eq) dsp_eq_reset(ctx->eq);
    if (ctx->agc) dsp_agc_reset(ctx->agc);
    if (ctx->limiter) dsp_limiter_reset(ctx->limiter);
}

/* clean-up: free window buffer (call if you add destructor) */
void dsp_free(AudioDspCtx *ctx)
{
    if (!ctx) return;

    if (ctx->denoise) { dsp_denoise_free(ctx->denoise); ctx->denoise = NULL; }
    if (ctx->width) { dsp_width_free(ctx->width); ctx->width = NULL; }
    if (ctx->eq) { dsp_eq_free(ctx->eq); ctx->eq = NULL; }
    if (ctx->agc) { dsp_agc_free(ctx->agc); ctx->agc = NULL; }
    if (ctx->limiter) { dsp_limiter_free(ctx->limiter); ctx->limiter = NULL; }

    ctx = NULL; /* free(ctx); */
}

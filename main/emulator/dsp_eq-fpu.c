/* rx_dsp_eq.c
 *
 * Minimal 2-band EQ module using dsp_filters.h
 * - Two shelves in series: low-shelf then high-shelf
 * - Stereo (independent per-channel states)
 * - Input/Output: Q24 ints in-place (as in all pipelines)
 *
 * API:
 *   struct DspEqCtx *dsp_eq_init(struct DspEqCtx *ctx, const AppConfig *cfg);
 *   void dsp_eq_process(int32_t *l, int32_t *r, uint32_t n, struct DspEqCtx *ctx);
 *   void dsp_eq_reset(struct DspEqCtx *ctx, const AppConfig *cfg);
 *   void dsp_eq_free(struct DspEqCtx *ctx);
 *
 * Notes:
 * - Uses double for internal arithmetic (temporary/emulation).
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspEqCtx {
    /* enable module */
    int enabled;
    double preamp_lin;    // unity = 1.0

    /* number of filter cascades */
    int low_n_sections;   // low_shelf_order / 2
    int peak_n_sections;  // band_peak_order / 2
    int high_n_sections;  // high_shelf_order / 2

    /* array length n_sections */
    biquad_d *lowL, *lowR;
    biquad_d *peakL, *peakR;
    biquad_d *highL, *highR;
};


/* forward for calling by init */
void dsp_eq_free(struct DspEqCtx*);

/* fill initial values */
/* caller must fill cfg fields first (high_shelf_freq_hz, etc) */
struct DspEqCtx* dsp_eq_init(struct DspEqCtx *ctx, DspEqConfig cfg, int sample_rate)
{
    int i; double per_stage_gain_db;

    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP EQ [fpu] module...\n");
    if (!ctx) ctx = (struct DspEqCtx*)calloc(1, sizeof(struct DspEqCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP EQ memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;

    /* compute linear preamp */
    ctx->preamp_lin = cfg.preamp_db != 0.0 ? pow(10.0, cfg.preamp_db / 20.0) : 1.0;

    /* create valid filters order */
    ctx->low_n_sections = (int)cfg.low_shelf_order / 2;
    ctx->high_n_sections = (int)cfg.high_shelf_order / 2;
    ctx->peak_n_sections = 1;  // используем только Q

    /* allocate biquad arrays */
    ctx->lowL  = (biquad_d*)calloc((size_t)ctx->low_n_sections,  sizeof(biquad_d));
    ctx->lowR  = (biquad_d*)calloc((size_t)ctx->low_n_sections,  sizeof(biquad_d));
    ctx->peakL = (biquad_d*)calloc((size_t)ctx->peak_n_sections, sizeof(biquad_d));
    ctx->peakR = (biquad_d*)calloc((size_t)ctx->peak_n_sections, sizeof(biquad_d));
    ctx->highL = (biquad_d*)calloc((size_t)ctx->high_n_sections, sizeof(biquad_d));
    ctx->highR = (biquad_d*)calloc((size_t)ctx->high_n_sections, sizeof(biquad_d));
    if (!ctx->lowL || !ctx->lowR || !ctx->peakL || !ctx->peakR || !ctx->highL || !ctx->highR) {
        /* free any allocated and return NULL */
        fprintf(stderr, "Failed to allocate DSP EQ sections.\n");
        dsp_eq_free(ctx); return NULL;
    }

    /* initialize per-section coefficients:
       we split total gain dB across sections so that total dB == configured value;
       per_stage_gain_db = total_gain_db / n_sections (dB additivity).
    */
    if (ctx->low_n_sections > 0) {
        per_stage_gain_db = cfg.low_shelf_gain_db / (double)ctx->low_n_sections;
        for (i = 0; i < ctx->low_n_sections; ++i) {
            biquad_init_lowshelf(&ctx->lowL[i], sample_rate, cfg.low_shelf_freq_hz, cfg.low_shelf_q, per_stage_gain_db);
            biquad_init_lowshelf(&ctx->lowR[i], sample_rate, cfg.low_shelf_freq_hz, cfg.low_shelf_q, per_stage_gain_db);
        }
    }
    if (ctx->peak_n_sections > 0) {
        per_stage_gain_db = cfg.band_peak_gain_db / (double)ctx->peak_n_sections;
        for (i = 0; i < ctx->peak_n_sections; ++i) {
            biquad_init_peak(&ctx->peakL[i], sample_rate, cfg.band_peak_freq_hz, cfg.band_peak_q, per_stage_gain_db);
            biquad_init_peak(&ctx->peakR[i], sample_rate, cfg.band_peak_freq_hz, cfg.band_peak_q, per_stage_gain_db);
        }
    }
    if (ctx->high_n_sections > 0) {
        per_stage_gain_db = cfg.high_shelf_gain_db / (double)ctx->high_n_sections;
        for (i = 0; i < ctx->high_n_sections; ++i) {
            biquad_init_highshelf(&ctx->highL[i], sample_rate, cfg.high_shelf_freq_hz, cfg.high_shelf_q, per_stage_gain_db);
            biquad_init_highshelf(&ctx->highR[i], sample_rate, cfg.high_shelf_freq_hz, cfg.high_shelf_q, per_stage_gain_db);
        }
    }

    return ctx;  // fill ctx->eq
}


/* The core processing loop
   Inputs: l[], r[] contain samples already converted to Q24
   Output: l[], r[] replaced with processed Q24 samples */
void dsp_eq_process(struct DspEqCtx *ctx, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i; int s;
    if (n == 0) return;
    if (!ctx || !ctx->enabled) return;

    /* per-sample processing:
       convert Q24 -> double, filter, convert back */
    for (i = 0; i < n; ++i) {
        double xl, xr;
        double yl_scaled, yr_scaled;

        /* normalize */
        xl = (double)l[i] / (double)DSP_ONE;
        xr = (double)r[i] / (double)DSP_ONE;

        /* pre-amp samples */
        if (ctx->preamp_lin != 1.0) {
            xl *= ctx->preamp_lin;
            xr *= ctx->preamp_lin;
        }

        /* cascade low-shelf first */
        for (s = 0; s < ctx->low_n_sections; ++s) {
            xl = biquad_process_sample(&ctx->lowL[s], xl);
            xr = biquad_process_sample(&ctx->lowR[s], xr);
        }
        /* then cascade peak-filter */
        for (s = 0; s < ctx->peak_n_sections; ++s) {
            xl = biquad_process_sample(&ctx->peakL[s], xl);
            xr = biquad_process_sample(&ctx->peakR[s], xr);
        }
        /* last, cascade high-shelf */
        for (s = 0; s < ctx->high_n_sections; ++s) {
            xl = biquad_process_sample(&ctx->highL[s], xl);
            xr = biquad_process_sample(&ctx->highR[s], xr);
        }

        /* back to Q24 (+saturate) */
        yl_scaled = xl * (double)DSP_ONE;
        yr_scaled = xr * (double)DSP_ONE;

        if (yl_scaled >= 2147483647.0) l[i] = 2147483647;
        else if (yl_scaled <= -2147483648.0) l[i] = (-2147483647-1);
        else l[i] = (int32_t)yl_scaled;

        if (yr_scaled >= 2147483647.0) r[i] = 2147483647;
        else if (yr_scaled <= -2147483648.0) r[i] = (-2147483647-1);
        else r[i] = (int32_t)yr_scaled;
    }
}


/* reset states (clear zp registers) */
void dsp_eq_reset(struct DspEqCtx *ctx)
{
    int i; if (!ctx) return;
    for (i = 0; i < ctx->low_n_sections; ++i) {
        ctx->lowL[i].z1 = ctx->lowL[i].z2 = 0.0;
        ctx->lowR[i].z1 = ctx->lowR[i].z2 = 0.0;
    }
    for (i = 0; i < ctx->peak_n_sections; ++i) {
        ctx->peakL[i].z1 = ctx->peakL[i].z2 = 0.0;
        ctx->peakR[i].z1 = ctx->peakR[i].z2 = 0.0;
    }
    for (i = 0; i < ctx->high_n_sections; ++i) {
        ctx->highL[i].z1 = ctx->highL[i].z2 = 0.0;
        ctx->highR[i].z1 = ctx->highR[i].z2 = 0.0;
    }
}

/* clean-up: free window buffer (call if you add destructor) */
void dsp_eq_free(struct DspEqCtx *ctx)
{
    if (!ctx) return;
    if (ctx->lowL)  { free(ctx->lowL);  ctx->lowL  = NULL; }
    if (ctx->lowR)  { free(ctx->lowR);  ctx->lowR  = NULL; }
    if (ctx->peakL) { free(ctx->peakL); ctx->peakL = NULL; }
    if (ctx->peakR) { free(ctx->peakR); ctx->peakR = NULL; }
    if (ctx->highL) { free(ctx->highL); ctx->highL = NULL; }
    if (ctx->highR) { free(ctx->highR); ctx->highR = NULL; }
    free(ctx);  // free ctx->eq stack
}

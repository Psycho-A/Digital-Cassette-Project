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
 * - Uses int64 for internal arithmetic (temporary/emulation).
 * - This file IS intended to be included textually from dsp_host.c
 *   (so it can use shared helpers implemented above).
 */
struct DspEqCtx {
    /* enable module */
    int enabled;
    int32_t preamp_q24;   // unity = DSP_ONE

    /* number of filter cascades */
    int low_n_sections;   // low_shelf_order / 2
    int peak_n_sections;  // band_peak_order / 2
    int high_n_sections;  // high_shelf_order / 2

    /* массивы целочисленных биквадов */
    biquad_i *lowL, *lowR;
    biquad_i *peakL, *peakR;
    biquad_i *highL, *highR;
};


/* forward for calling by init */
void dsp_eq_free(struct DspEqCtx*);

/* fill initial values */
/* caller must fill cfg fields first (high_shelf_freq_hz, etc) */
struct DspEqCtx* dsp_eq_init(struct DspEqCtx *ctx, DspEqConfig cfg, int sample_rate)
{
    int i; double per_stage_gain_db;
    biquad_d btmp;  // временный double-массив

    /* allocate container; caller will accept returned ctx */
    fprintf(stderr, "Initializing DSP EQ [int] module...\n");
    if (!ctx) ctx = (struct DspEqCtx*)calloc(1, sizeof(struct DspEqCtx));
    if (!ctx) { fprintf(stderr, "Failed to allocate DSP EQ memory.\n"); return NULL; }

    ctx->enabled = cfg.enabled ? 1 : 0;

    /* compute Q24 preamp (max value = +42 dB) */
    ctx->preamp_q24 = cfg.preamp_db != 0.0 ? (int32_t)(pow(10.0, cfg.preamp_db / 20.0) * (double)DSP_ONE + 0.5) : DSP_ONE;

    /* create valid filters order */
    ctx->low_n_sections = (int)cfg.low_shelf_order / 2;
    ctx->high_n_sections = (int)cfg.high_shelf_order / 2;
    ctx->peak_n_sections = 1;  // используем только Q

    /* allocate biquad arrays */
    ctx->lowL  = (biquad_i*)calloc(ctx->low_n_sections,  sizeof(biquad_i));
    ctx->lowR  = (biquad_i*)calloc(ctx->low_n_sections,  sizeof(biquad_i));
    ctx->peakL = (biquad_i*)calloc(ctx->peak_n_sections, sizeof(biquad_i));
    ctx->peakR = (biquad_i*)calloc(ctx->peak_n_sections, sizeof(biquad_i));
    ctx->highL = (biquad_i*)calloc(ctx->high_n_sections, sizeof(biquad_i));
    ctx->highR = (biquad_i*)calloc(ctx->high_n_sections, sizeof(biquad_i));
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
            biquad_init_lowshelf(&btmp, sample_rate, cfg.low_shelf_freq_hz, cfg.low_shelf_q, per_stage_gain_db);
            // конвертируем в Q24 и сохраняем
            biquad_int_from_double(&ctx->lowL[i], &btmp, DSP_ONE);
            biquad_int_from_double(&ctx->lowR[i], &btmp, DSP_ONE);
        }
    }
    if (ctx->peak_n_sections > 0) {
        per_stage_gain_db = cfg.band_peak_gain_db / (double)ctx->peak_n_sections;
        for (i = 0; i < ctx->peak_n_sections; ++i) {
            biquad_init_peak(&btmp, sample_rate, cfg.band_peak_freq_hz, cfg.band_peak_q, per_stage_gain_db);
            biquad_int_from_double(&ctx->peakL[i], &btmp, DSP_ONE);
            biquad_int_from_double(&ctx->peakR[i], &btmp, DSP_ONE);
        }
    }
    if (ctx->high_n_sections > 0) {
        per_stage_gain_db = cfg.high_shelf_gain_db / (double)ctx->high_n_sections;
        for (i = 0; i < ctx->high_n_sections; ++i) {
            biquad_init_highshelf(&btmp, sample_rate, cfg.high_shelf_freq_hz, cfg.high_shelf_q, per_stage_gain_db);
            biquad_int_from_double(&ctx->highL[i], &btmp, DSP_ONE);
            biquad_int_from_double(&ctx->highR[i], &btmp, DSP_ONE);
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
        int32_t xl = l[i];
        int32_t xr = r[i];

        /* pre-amp samples */
        if (ctx->preamp_q24 != DSP_ONE) {
            xl = (int32_t)(((int64_t)xl * ctx->preamp_q24) >> 24);
            xr = (int32_t)(((int64_t)xr * ctx->preamp_q24) >> 24);
        }

        /* cascade low-shelf first */
        for (s = 0; s < ctx->low_n_sections; ++s) {
            xl = biquad_process_int(&ctx->lowL[s], xl, 24);
            xr = biquad_process_int(&ctx->lowR[s], xr, 24);
        }
        /* then cascade peak-filter */
        for (s = 0; s < ctx->peak_n_sections; ++s) {
            xl = biquad_process_int(&ctx->peakL[s], xl, 24);
            xr = biquad_process_int(&ctx->peakR[s], xr, 24);
        }
        /* last, cascade high-shelf */
        for (s = 0; s < ctx->high_n_sections; ++s) {
            xl = biquad_process_int(&ctx->highL[s], xl, 24);
            xr = biquad_process_int(&ctx->highR[s], xr, 24);
        }

        /* output to Q24 */
        l[i] = xl; r[i] = xr;
    }
}


/* reset states (clear zp registers) */
void dsp_eq_reset(struct DspEqCtx *ctx)
{
    int i; if (!ctx) return;
    for (i = 0; i < ctx->low_n_sections; ++i) {
        biquad_reset_int(&ctx->lowL[i]);
        biquad_reset_int(&ctx->lowR[i]);
    }
    for (i = 0; i < ctx->peak_n_sections; ++i) {
        biquad_reset_int(&ctx->peakL[i]);
        biquad_reset_int(&ctx->peakR[i]);
    }
    for (i = 0; i < ctx->high_n_sections; ++i) {
        biquad_reset_int(&ctx->highL[i]);
        biquad_reset_int(&ctx->highR[i]);
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

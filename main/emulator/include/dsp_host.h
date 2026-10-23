/* dsp_host.h */
#ifndef RX_DSP_H
#define RX_DSP_H
#include "stdint_compat.h"
#include "config.h"

/* Use fixed-point calculations for DSP modules,
   otherwise use floating-point (double) versions
*/
#define USE_FIXED_POINT 1

/* Working fixed point: Q24 (signed int32 with 24 fractional bits)
   - DSP_ONE = 1.0 in Q24
   - input/output external format is int32 full-scale; inside we shift >>7 to Q24
*/
#define DSP_SHIFT 7  // positive gain headroom
#define DSP_ONE ((int32_t)(1 << 24))  // 16,777,216
#define SOFT_LIMIT_Q24 ((int32_t)(0.99 * (double)DSP_ONE))
#define GAIN_EPSILON (1.0 / (double)DSP_ONE)  // min gain diff
#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647-1)

/* Forward (opaque) declaration of DSP context.
   We keep module internals in their *.c, so dsp_host.* don't see details. */
typedef struct {
    /* whole DSP */
    int enabled;
    int sample_rate;    // from config/caller
    enum {              // режимы обработки (секции cfg.*):
        ANALOG_TX = 0,  // - аналогова€ запись (без модул€ции)
        ANALOG_RX,      // - аналоговое воспроизведение (улучшение)
        TXMOD_PRE,      // - pre-emphasis ASK-сигнала (коррекци€ ленты)
        DEMOD_PRE,      // - de-emphasis ASK-сигнала (коррекци€ ленты)
        DEMOD_POST      // - пост-обработка аудио *после* AAC-декодера
    } fsm_mode;

    /* modules (opaque) */
    struct DspDenoiseCtx *denoise;
    struct DspWidthCtx *width;
    struct DspEqCtx *eq;
    struct DspAgcCtx *agc;
    struct DspLimiterCtx *limiter;

} AudioDspCtx;


/* lifecycle */
void dsp_init(AudioDspCtx *ctx, const AppConfig *cfg, int src_rate);
void dsp_reset(AudioDspCtx *ctx);
void dsp_free(AudioDspCtx *ctx);

/* processing (in-place on l/r) */
void dsp_process(AudioDspCtx *ctx, int32_t *l, int32_t *r, uint32_t n);

/* shared helpers */
int32_t q24_mul(int32_t a, int32_t b);
int32_t db_to_q24(float db);
int32_t time_to_alpha_q24(double ms, uint32_t sr);

/* shared filters */
#include "dsp_filters.h"


#endif
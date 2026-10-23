/* dsp_filters.h
 *
 * Simple Fade-in effect for FSM modes switch:
 * - Uses Q31 >> Q15 conversion for sample gain arithmetics.
 *
 * Implementation of RBJ BiQuad (IIR) helper functions (C89):
 * - High-pass, low-pass, high-shelf, low-shelf
 * - Supports cascaded HPF (order = 2,4,6...) by repeating identical 2nd-order HPF sections
 * - Includes all init functions for later EQ/Denoise (and other DSP modules) re-using
 * - Works with Q24/Q31 samples in-place (l[], r[])
 *
 * Notes:
 * - Uses Transposed Direct II form (numerically stable).
 * - Uses double for coefficients and state (+int32_t versions).
 * - Caller is responsible for allocation. No per-sample allocation.
 */
#ifndef RX_DSP_FILTERS_H
#define RX_DSP_FILTERS_H
#include <math.h>

#ifndef M_PI
#define M_PI 3.141592653589793
#endif
#ifndef DSP_TWO_PI
#define DSP_TWO_PI 6.28318530717958647692
#endif
#ifndef Q_BUTTERWORTH
#define Q_BUTTERWORTH 0.7071067811865476
#endif

/* fade-in effect for FSM modes */
typedef struct {
    uint32_t total;      // total fade samples
    uint32_t pos;        // current position
    int32_t  gain_q31;   // current gain in Q31
    int32_t  step_q31;   // gain increment per sample
} fade_q15;

/* small local biquad struct (coeffs/state) */
typedef struct {
    double b0, b1, b2;
    double a1, a2;       // normalized (a0 == 1)
    double z1, z2;       // state
} biquad_d;

/* целочисленный biquad (Q24/Q31) */
typedef struct {
    int32_t b0, b1, b2;  // коэффициенты в Q24/Q31
    int32_t a1, a2;      // a0 = 1 (нормализовано)
    int32_t x1, x2;      // предыдущие входы
    int32_t y1, y2;      // предыдущие выходы
} biquad_i;


/* forwards (double versions) */
void fadein_init(fade_q15 *f, uint32_t fade_samples);
void fadein_process(fade_q15 *f, int32_t *lbuf, int32_t *rbuf, uint32_t n);
void biquad_init_lowshelf(biquad_d *f, double fs, double f0, double Q, double gain_db);
void biquad_init_highshelf(biquad_d *f, double fs, double f0, double Q, double gain_db);
void biquad_init_peak(biquad_d *f, double fs, double f0, double Q, double gain_db);
void biquad_init_lowpass(biquad_d *f, double fs, double f0, double Q);
void biquad_init_highpass(biquad_d *f, double fs, double f0, double Q);
void biquad_init_bandpass(biquad_d *f, double fs, double f0, double Q);
double biquad_process_sample(biquad_d *f, double x);
double biquad_process_with_coeffs(biquad_d *f, double b0,double b1,double b2, double a1,double a2, double x);
void compute_shelf_coeffs(double gain_lin, double inv_den, double cs, double alpha, double *nb0, double *nb1, double *nb2, double *na1, double *na2);
void biquad_reset(biquad_d *f);

/* fixed-point versions (Q24/Q31) */
void biquad_int_from_double(biquad_i *q, const biquad_d *d, int32_t max);  // max = DSP_ONE or INT32_MAX
int32_t biquad_process_int(biquad_i *q, int32_t x, int32_t shift);  // shift >>= 24 or 31 (for Q24/Q31)
int32_t biquad_process_with_coeffs_int(biquad_i *q, int32_t b0,int32_t b1,int32_t b2, int32_t a1,int32_t a2, int32_t x, int32_t shift);
void compute_shelf_coeffs_int(int32_t gain_lin, double inv_den, double cs, double alpha, int32_t *b0,int32_t *b1,int32_t *b2, int32_t *a1,int32_t *a2, int32_t max);
void biquad_reset_int(biquad_i *q);


/* Fade-in effect helpers
   ---------------------- */

static void fadein_init(fade_q15 *f, uint32_t fade_samples)
{
    if (fade_samples <= 0) {
        f->total = 0; return;
    }
    f->total = fade_samples;
    f->pos = 0;
    f->gain_q31 = 0;

    /* 0x7FFFFFFF == 1.0 in Q31 */
    f->step_q31 = (int32_t)(0x7FFFFFFF / (int32_t)fade_samples);
}

/* Apply fade-in to first n samples while active */
static void fadein_process(fade_q15 *f, int32_t *lbuf, int32_t *rbuf, uint32_t n)
{
    uint32_t i;
    if (f->total <= 0 || f->pos >= f->total)
        return;

    for (i = 0; i < n; ++i) {
        int32_t g = f->gain_q31;

        /* apply gain using 16x16 -> 32 trick */
        lbuf[i] = (int32_t)(((lbuf[i] >> 16) * (g >> 15)));
        rbuf[i] = (int32_t)(((rbuf[i] >> 16) * (g >> 15)));

        /* advance gain and position */
        f->gain_q31 += f->step_q31; f->pos++;
    }

    /* if finished, make sure last samples are fully 1.0 */
    if (f->pos >= f->total) f->gain_q31 = 0x7FFFFFFF;
}


/* BiQuad helpers implementations
   ------------------------------ */

/* Low-shelf (RBJ) - formulas mirror high-shelf */
static void biquad_init_lowshelf(biquad_d *f, double fs, double f0, double Q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double omega = DSP_TWO_PI * f0 / fs;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);
    double sqrtA = sqrt(A);

    double b0 =      A*( (A+1.0) - (A-1.0)*cs + 2.0*sqrtA*alpha );
    double b1 =  2.0*A*( (A-1.0) - (A+1.0)*cs );
    double b2 =      A*( (A+1.0) - (A-1.0)*cs - 2.0*sqrtA*alpha );
    double a0 =          (A+1.0) + (A-1.0)*cs + 2.0*sqrtA*alpha;
    double a1 =   -2.0*( (A-1.0) + (A+1.0)*cs );
    double a2 =          (A+1.0) + (A-1.0)*cs - 2.0*sqrtA*alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    f->z1 = f->z2 = 0.0;
}

/* High-shelf (RBJ) biquad - double coeffs */
static void biquad_init_highshelf(biquad_d *f, double fs, double f0, double Q, double gain_db)
{
    /* RBJ cookbook: A = sqrt(10^(gain_db/20)) */
    double A = pow(10.0, gain_db / 40.0);
    double omega = DSP_TWO_PI * f0 / fs;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);
    double sqrtA = sqrt(A);

    double b0 =      A*( (A+1.0) + (A-1.0)*cs + 2.0*sqrtA*alpha );
    double b1 = -2.0*A*( (A-1.0) + (A+1.0)*cs );
    double b2 =      A*( (A+1.0) + (A-1.0)*cs - 2.0*sqrtA*alpha );
    double a0 =          (A+1.0) - (A-1.0)*cs + 2.0*sqrtA*alpha;
    double a1 =    2.0*( (A-1.0) - (A+1.0)*cs );
    double a2 =          (A+1.0) - (A-1.0)*cs - 2.0*sqrtA*alpha;

    /* normalize so that a0 == 1.0 */
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    /* clear state */
    f->z1 = f->z2 = 0.0;
}

/* Peaking/Notch filter (RBJ 2nd order)
   gain_db > 0 - peaking, gain_db < 0 - notch */
static void biquad_init_peak(biquad_d *f, double fs, double f0, double Q, double gain_db)
{
    double A  = pow(10.0, gain_db / 40.0);  // A = sqrt(10^(dB/20))
    double w0 = DSP_TWO_PI * f0 / fs;
    double sn = sin(w0);
    double cs = cos(w0);
    double alpha = sn / (2.0 * Q);

    double b0 =  1.0 + alpha * A;
    double b1 = -2.0 * cs;
    double b2 =  1.0 - alpha * A;
    double a0 =  1.0 + alpha / A;
    double a1 = -2.0 * cs;
    double a2 =  1.0 - alpha / A;

    /* нормализация (a0 > 1.0) */
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    f->z1 = f->z2 = 0.0;
}

/* High-pass (RBJ 2nd order) */
static void biquad_init_highpass(biquad_d *f, double fs, double f0, double Q)
{
    double omega = DSP_TWO_PI * f0 / fs;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);

    double b0 =  (1.0 + cs) / 2.0;
    double b1 = -(1.0 + cs);
    double b2 =  (1.0 + cs) / 2.0;
    double a0 =  1.0 + alpha;
    double a1 = -2.0 * cs;
    double a2 =  1.0 - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    f->z1 = f->z2 = 0.0;
}

/* Low-pass (RBJ 2nd order) */
static void biquad_init_lowpass(biquad_d *f, double fs, double f0, double Q)
{
    double omega = DSP_TWO_PI * f0 / fs;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);

    double b0 =  (1.0 - cs) / 2.0;
    double b1 =   1.0 - cs;
    double b2 =  (1.0 - cs) / 2.0;
    double a0 =  1.0 + alpha;
    double a1 = -2.0 * cs;
    double a2 =  1.0 - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    f->z1 = f->z2 = 0.0;
}

/* Band-pass (RBJ 2nd order) - constant skirt gain, peak gain = Q */
static void biquad_init_bandpass(biquad_d *f, double fs, double f0, double Q)
{
    double omega = DSP_TWO_PI * f0 / fs;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);

    double b0 = alpha;
    double b1 = 0.0;
    double b2 = -alpha;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cs;
    double a2 = 1.0 - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    f->z1 = f->z2 = 0.0;
}

/* helper: check sample for subnormal value [slower] */
static int is_subnormal(double x, double z1, double z2)
{
    /* returns 1 if sample is smaller than 1e-12 (<-100 dB) */
    return (fabs(x) < 1e-12 && fabs(z1) < 1e-12 && fabs(z2) < 1e-12);
}

/* simple Transposed-Direct-II processing in double */
static double biquad_process_sample(biquad_d *f, double x)
{
    double y;
    /* subnormal nums protection */
    if (fabs(x) < 1e-12 && 
    fabs(f->z1) < 1e-12 && 
    fabs(f->z2) < 1e-12) {
        f->z1 = f->z2 = 0.0; return 0.0;
    }
    y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x + f->z2 - f->a1 * y;
    f->z2 = f->b2 * x - f->a2 * y;

    return y;
}

/* helper: process with pre-calculated coeffs */
static double biquad_process_with_coeffs(biquad_d *f, double b0,double b1,double b2, double a1,double a2, double x)
{
    double y;
    /* check subnormals too? */
    y = b0 * x + f->z1;
    f->z1 = b1 * x + f->z2 - a1 * y;
    f->z2 = b2 * x - a2 * y;

    return y;
}

/* compute single-section high-shelf coeffs for given gain_lin and inv_den
   - gain_lin: overall linear gain to distribute across all sections
   - inv_den: 1.0 / (2.0 * n_sections)
   - cs, alpha: precomputed for the chosen f0/Q (we use cs/alpha from first section)
   - returns nb0,nb1,nb2,na1,na2 via pointers
*/
static void compute_shelf_coeffs(double gain_lin, double inv_den, double cs, double alpha,
                                 double *nb0,double *nb1,double *nb2, double *na1,double *na2)
{
    /* A_section = gain_lin^(1/(2*nsec)) -> exp(log(gain_lin) * inv_den) */
    double A = exp(log(gain_lin) * inv_den);
    double sqrtA = sqrt(A);

    /* RBJ highshelf */
    double b0 =      A*( (A+1.0) + (A-1.0)*cs + 2.0*sqrtA*alpha );
    double b1 = -2.0*A*( (A-1.0) + (A+1.0)*cs );
    double b2 =      A*( (A+1.0) + (A-1.0)*cs - 2.0*sqrtA*alpha );
    double a0 =          (A+1.0) - (A-1.0)*cs + 2.0*sqrtA*alpha;
    double a1 =    2.0*( (A-1.0) - (A+1.0)*cs );
    double a2 =          (A+1.0) - (A-1.0)*cs - 2.0*sqrtA*alpha;

    *nb0 = b0 / a0; *nb1 = b1 / a0; *nb2 = b2 / a0;
    *na1 = a1 / a0; *na2 = a2 / a0;
}

static void biquad_reset(biquad_d *f)
{
    if (!f) return;
    f->z1 = 0.0; f->z2 = 0.0;
}

/* Helper: Compute gain at target frequency (e.g. ASK carrier),
   when correcting with some biQuad filters at other frequency */
static double biquad_gain_at_freq(const biquad_d *f, double freq, double fs)
{
    double omega = 2.0 * M_PI * freq / fs;
    double cosw  = cos(omega);
    double sinw  = sin(omega);

    double b0 = f->b0, b1 = f->b1, b2 = f->b2;
    double a1 = f->a1, a2 = f->a2;

    double num_real = b0 + b1 * cosw + b2 * (2.0 * cosw * cosw - 1.0);
    double num_imag = -b1 * sinw - 2.0 * b2 * cosw * sinw;
    double den_real = 1.0 + a1 * cosw + a2 * (2.0 * cosw * cosw - 1.0);
    double den_imag = -a1 * sinw - 2.0 * a2 * cosw * sinw;

    double mag = sqrt( (num_real*num_real + num_imag*num_imag) /
                       (den_real*den_real + den_imag*den_imag) );

    return 20.0 * log10(mag);
}


/* Fixed-point BiQuad implementations
   ---------------------------------- */

/* преобразование double-коэффициентов в Q24/Q31 с округлением */
static void biquad_int_from_double(biquad_i *q, const biquad_d *d, int32_t max)
{
    q->b0 = (int32_t)(d->b0 * max + 0.5);
    q->b1 = (int32_t)(d->b1 * max + 0.5);
    q->b2 = (int32_t)(d->b2 * max + 0.5);
    q->a1 = (int32_t)(d->a1 * max + 0.5);
    q->a2 = (int32_t)(d->a2 * max + 0.5);
    q->x1 = q->x2 = q->y1 = q->y2 = 0;
}

/* обработка одного сэмпла (прямая форма I) */
static int32_t biquad_process_int(biquad_i *q, int32_t x, int32_t shift)
{
    int64_t y = (int64_t)q->b0 * x
              + (int64_t)q->b1 * q->x1
              + (int64_t)q->b2 * q->x2
              - (int64_t)q->a1 * q->y1
              - (int64_t)q->a2 * q->y2;
    y >>= shift;  // результат в Q24/Q31

    /* обновление состояний */
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = (int32_t)y;

    return (int32_t)y;
}

/* обработка сэмпла с пред-рассчитанными коэффициентами */
static int32_t biquad_process_with_coeffs_int(biquad_i *q, int32_t b0,int32_t b1,int32_t b2, int32_t a1,int32_t a2, int32_t x, int32_t shift)
{
    int64_t y = (int64_t)b0 * x
              + (int64_t)b1 * q->x1
              + (int64_t)b2 * q->x2
              - (int64_t)a1 * q->y1
              - (int64_t)a2 * q->y2;
    y >>= shift;  // результат в Q24/Q31

    /* обновление состояний */
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = (int32_t)y;

    return (int32_t)y;
}

/* вычисление коэффициентов shelf в Q24/Q31 */
static void compute_shelf_coeffs_int(int32_t gain_lin, double inv_den, double cs, double alpha,
                                     int32_t *b0,int32_t *b1,int32_t *b2, int32_t *a1,int32_t *a2, int32_t max)
{
    /* gain_lin – линейное усиление в int32_t (например, 0.5 = 0x800000) */
    double A = exp(log((double)gain_lin / max) * inv_den);  // A для одной секции
    double sqrtA = sqrt(A);

    /* коэффициенты high-shelf (как в double-версии) */
    double b0d =        A * ( (A+1.0) + (A-1.0)*cs + 2.0*sqrtA*alpha );
    double b1d = -2.0 * A * ( (A-1.0) + (A+1.0)*cs );
    double b2d =        A * ( (A+1.0) + (A-1.0)*cs - 2.0*sqrtA*alpha );
    double a0d =              (A+1.0) - (A-1.0)*cs + 2.0*sqrtA*alpha;
    double a1d =      2.0 * ( (A-1.0) - (A+1.0)*cs );
    double a2d =              (A+1.0) - (A-1.0)*cs - 2.0*sqrtA*alpha;

    /* нормализация и перевод в int32_t */
    *b0 = (int32_t)(b0d / a0d * max + 0.5);
    *b1 = (int32_t)(b1d / a0d * max + 0.5);
    *b2 = (int32_t)(b2d / a0d * max + 0.5);
    *a1 = (int32_t)(a1d / a0d * max + 0.5);
    *a2 = (int32_t)(a2d / a0d * max + 0.5);
}

/* сброс состояния целочисленного биквада */
static void biquad_reset_int(biquad_i *q)
{
    q->x1 = q->x2 = q->y1 = q->y2 = 0;
}

#endif
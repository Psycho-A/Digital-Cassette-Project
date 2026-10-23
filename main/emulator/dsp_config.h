/* dsp_config.h
 *
 * A derivative from main 'config.h' file.
 * Creates unified DSP-module structures,
 * compatible with both TX/RX-analog modes.
 *
 * Important note:
 * - Requires to be included before AppConfig declaration!
 */
#ifndef DSP_CONFIG_H
#define DSP_CONFIG_H

/* Q for biQuad filters */
static const double 
    default_filter_q = 0.7071067811865476;


/* DSP modules param listings
   -------------------------- */

/* Denoise/compand */
typedef struct {
    int enabled;
    int cutoff_freq_hz;
    double noise_threshold_db;
    double reduction_level_db;
    double attack_ms;
    double release_ms;
    double knee_width_db;
    double filter_q;
    int    filter_order;
    int    rms_window_cycles;
    double enhance_peaks_db;
    double peaks_floor_db;
    int    rumble_cutoff_hz;
    int    rumble_filter_order;
    int    linked_stereo;
    int    compander_mode;

} DspDenoiseConfig;


/* Stereo (width) */
typedef struct {
    int enabled;
    int width_percent;
    int hpf_enable;
    double hpf_cutoff_hz;
    int lpf_enable;
    double lpf_cutoff_hz;

} DspWidthConfig;


/* EQ (tembre) */
typedef struct {
    int enabled;
    double preamp_db;
    char*  low_shelf;
    int    low_shelf_freq_hz;
    double low_shelf_gain_db;
    double low_shelf_q;
    int    low_shelf_order;
    char*  band_peak;
    int    band_peak_freq_hz;
    double band_peak_gain_db;
    double band_peak_q;
    int    band_peak_order;
    char*  high_shelf;
    int    high_shelf_freq_hz;
    double high_shelf_gain_db;
    double high_shelf_q;
    int    high_shelf_order;

} DspEqConfig;


/* AGC (loudness) */
typedef struct {
    int enabled;
    double initial_gain_db;
    double target_rms_db;
    double max_gain_db;
    double rms_window_ms;
    double gain_attack_ms;
    double gain_release_ms;
    double silence_threshold_db;
    double reset_after_s;

} DspAgcConfig;


/* Limiter (realtime) */
typedef struct {
    int enabled;
    double threshold_db;
    double hold_ms;
    double release_ms;
    double knee_width_db;
    double preamp_db;
    int linked_stereo;

} DspLimiterConfig;


#endif
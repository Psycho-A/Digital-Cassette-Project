/* pcm_helpers.h */
#ifndef PCM_HELPERS_H
#define PCM_HELPERS_H

/* -------------------------------------------
   PCM format helpers for choosing by pointers
   -------------------------------------------
   Need to be included textually into pcm_io.c!
*/

/* Новая реализация хелперов конверсии форматов:
   * реализована конверсия из любого формата в любой.
   * не требуется отдельный pack_samples в DSP-цикле.
   
   - на вход принимаются 2 стерео-канала или 2 моно-копии;
   - внутренняя обработка делается всегда для двух каналов;
   - возвращаем число байт на 1 сэмпл для одного канала;
   - в функции записи сэмплов получаем реальное число байт;
   - в зависимости от числа каналов пишем левый или оба.
*/

/* константы для нормализации int -> float */
static const float float_scale_s8  = 1.0f / 128.0f;  // 2^7
static const float float_scale_s16 = 1.0f / 32768.0f;  // 2^15
static const float float_scale_s24 = 1.0f / 8388608.0f;  // 2^23
static const float float_scale_s32 = 1.0f / 2147483648.0f;  // 2^31

/* макрос для коррекции негативных float-значений в int32_t */
#define fix_float_sample(x) ((x) < 0 ? (-(x) ^ 0x80000000) : (x))


/* PCM 8-bit -> 8-bit unsigned */
static inline uint32_t pcm_pack_s8_to_s8(int32_t l, int32_t r, uint8_t *dst)
{
    dst[0] = (uint8_t)((l & 0xFF) + 128);
    dst[1] = (uint8_t)((r & 0xFF) + 128);
    return 1;  // байт в сэмпле одного канала
}
/* PCM 8-bit -> 16-bit signed */
static inline uint32_t pcm_pack_s8_to_s16(int32_t l, int32_t r, uint8_t *dst)
{
    int16_t lv = (int16_t)(l << 8);
    int16_t rv = (int16_t)(r << 8);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)(rv & 0xFF);
    dst[3] = (uint8_t)((rv >> 8) & 0xFF);
    return 2;
}
/* PCM 8-bit -> 24-bit signed */
static inline uint32_t pcm_pack_s8_to_s24(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l << 16;
    int32_t rv = r << 16;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)(rv & 0xFF);
    dst[4] = (uint8_t)((rv >> 8) & 0xFF);
    dst[5] = (uint8_t)((rv >> 16) & 0xFF);
    return 3;
}
/* PCM 8-bit -> 32-bit signed */
static inline uint32_t pcm_pack_s8_to_s32(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l << 24;
    int32_t rv = r << 24;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)((lv >> 24) & 0xFF);
    dst[4] = (uint8_t)(rv & 0xFF);
    dst[5] = (uint8_t)((rv >> 8) & 0xFF);
    dst[6] = (uint8_t)((rv >> 16) & 0xFF);
    dst[7] = (uint8_t)((rv >> 24) & 0xFF);
    return 4;
}
/* PCM 8-bit -> 32-bit IEEE float */
static inline uint32_t pcm_pack_s8_to_f32(int32_t l, int32_t r, uint8_t *dst)
{
    float fl = (float)l * float_scale_s8; // l уже знаковое
    float fr = (float)r * float_scale_s8;
    memcpy(dst, &fl, 4);
    memcpy(dst+4, &fr, 4);
    return 4;
}


/* PCM 16-bit -> 8-bit unsigned */
static inline uint32_t pcm_pack_s16_to_s8(int32_t l, int32_t r, uint8_t *dst)
{
    dst[0] = (uint8_t)((l >> 8) + 128);
    dst[1] = (uint8_t)((r >> 8) + 128);
    return 1;
}
/* PCM 16-bit -> 16-bit signed */
static inline uint32_t pcm_pack_s16_to_s16(int32_t l, int32_t r, uint8_t *dst)
{
    int16_t lv = (int16_t)l;
    int16_t rv = (int16_t)r;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)(rv & 0xFF);
    dst[3] = (uint8_t)((rv >> 8) & 0xFF);
    return 2;
}
/* PCM 16-bit -> 24-bit signed */
static inline uint32_t pcm_pack_s16_to_s24(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l << 8;
    int32_t rv = r << 8;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)(rv & 0xFF);
    dst[4] = (uint8_t)((rv >> 8) & 0xFF);
    dst[5] = (uint8_t)((rv >> 16) & 0xFF);
    return 3;
}
/* PCM 16-bit -> 32-bit signed */
static inline uint32_t pcm_pack_s16_to_s32(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l << 16;
    int32_t rv = r << 16;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)((lv >> 24) & 0xFF);
    dst[4] = (uint8_t)(rv & 0xFF);
    dst[5] = (uint8_t)((rv >> 8) & 0xFF);
    dst[6] = (uint8_t)((rv >> 16) & 0xFF);
    dst[7] = (uint8_t)((rv >> 24) & 0xFF);
    return 4;
}
/* PCM 16-bit -> 32-bit IEEE float */
static inline uint32_t pcm_pack_s16_to_f32(int32_t l, int32_t r, uint8_t *dst)
{
    float fl = (float)l * float_scale_s16;
    float fr = (float)r * float_scale_s16;
    memcpy(dst, &fl, 4);
    memcpy(dst+4, &fr, 4);
    return 4;
}


/* PCM 24-bit -> 8-bit unsigned */
static inline uint32_t pcm_pack_s24_to_s8(int32_t l, int32_t r, uint8_t *dst)
{
    dst[0] = (uint8_t)((l >> 16) + 128);
    dst[1] = (uint8_t)((r >> 16) + 128);
    return 1;
}
/* PCM 24-bit -> 16-bit signed */
static inline uint32_t pcm_pack_s24_to_s16(int32_t l, int32_t r, uint8_t *dst)
{
    int16_t lv = (int16_t)(l >> 8);
    int16_t rv = (int16_t)(r >> 8);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)(rv & 0xFF);
    dst[3] = (uint8_t)((rv >> 8) & 0xFF);
    return 2;
}
/* PCM 24-bit -> 24-bit signed */
static inline uint32_t pcm_pack_s24_to_s24(int32_t l, int32_t r, uint8_t *dst)
{
    dst[0] = (uint8_t)(l & 0xFF);
    dst[1] = (uint8_t)((l >> 8) & 0xFF);
    dst[2] = (uint8_t)((l >> 16) & 0xFF);
    dst[3] = (uint8_t)(r & 0xFF);
    dst[4] = (uint8_t)((r >> 8) & 0xFF);
    dst[5] = (uint8_t)((r >> 16) & 0xFF);
    return 3;
}
/* PCM 24-bit -> 32-bit signed */
static inline uint32_t pcm_pack_s24_to_s32(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l << 8;
    int32_t rv = r << 8;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)((lv >> 24) & 0xFF);
    dst[4] = (uint8_t)(rv & 0xFF);
    dst[5] = (uint8_t)((rv >> 8) & 0xFF);
    dst[6] = (uint8_t)((rv >> 16) & 0xFF);
    dst[7] = (uint8_t)((rv >> 24) & 0xFF);
    return 4;
}
/* PCM 24-bit -> 32-bit IEEE float */
static inline uint32_t pcm_pack_s24_to_f32(int32_t l, int32_t r, uint8_t *dst)
{
    float fl = (float)l * float_scale_s24;
    float fr = (float)r * float_scale_s24;
    memcpy(dst, &fl, 4);
    memcpy(dst+4, &fr, 4);
    return 4;
}


/* PCM 32-bit -> 8-bit unsigned */
static inline uint32_t pcm_pack_s32_to_s8(int32_t l, int32_t r, uint8_t *dst)
{
    /* масштабирование: сдвиг на 24 бита,
       затем смещение [-128..127] -> [0..255] */
    dst[0] = (uint8_t)((l >> 24) + 128);
    dst[1] = (uint8_t)((r >> 24) + 128);
    return 1;
}
/* PCM 32-bit -> 16-bit signed */
static inline uint32_t pcm_pack_s32_to_s16(int32_t l, int32_t r, uint8_t *dst)
{
    int16_t lv = (int16_t)(l >> 16);
    int16_t rv = (int16_t)(r >> 16);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)(rv & 0xFF);
    dst[3] = (uint8_t)((rv >> 8) & 0xFF);
    return 2;
}
/* PCM 32-bit -> 24-bit signed */
static inline uint32_t pcm_pack_s32_to_s24(int32_t l, int32_t r, uint8_t *dst)
{
    int32_t lv = l >> 8;
    int32_t rv = r >> 8;
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)(rv & 0xFF);
    dst[4] = (uint8_t)((rv >> 8) & 0xFF);
    dst[5] = (uint8_t)((rv >> 16) & 0xFF);
    return 3;
}
/* PCM 32-bit -> 32-bit signed */
static inline uint32_t pcm_pack_s32_to_s32(int32_t l, int32_t r, uint8_t *dst)
{
    dst[0] = (uint8_t)(l & 0xFF);
    dst[1] = (uint8_t)((l >> 8) & 0xFF);
    dst[2] = (uint8_t)((l >> 16) & 0xFF);
    dst[3] = (uint8_t)((l >> 24) & 0xFF);
    dst[4] = (uint8_t)(r & 0xFF);
    dst[5] = (uint8_t)((r >> 8) & 0xFF);
    dst[6] = (uint8_t)((r >> 16) & 0xFF);
    dst[7] = (uint8_t)((r >> 24) & 0xFF);
    return 4;
}
/* PCM 32-bit -> 32-bit IEEE float */
static inline uint32_t pcm_pack_s32_to_f32(int32_t l, int32_t r, uint8_t *dst)
{
    float fl = (float)l * float_scale_s32;
    float fr = (float)r * float_scale_s32;
    memcpy(dst, &fl, 4);
    memcpy(dst + 4, &fr, 4);
    return 4;
}


/* PCM 32-bit float -> 8-bit unsigned */
static inline uint32_t pcm_pack_f32_to_s8(int32_t l, int32_t r, uint8_t *dst)
{
    // интерпретируем l и r как raw float (биты)
    float fl, fr;
    int32_t lv, rv;
    // исправляем отрицание в int32
    l = fix_float_sample(l);
    r = fix_float_sample(r);
    // копируем модифицированные байты
    memcpy(&fl, &l, 4);
    memcpy(&fr, &r, 4);
    lv = (int32_t)(fl * 128.0f);
    rv = (int32_t)(fr * 128.0f);
    dst[0] = (uint8_t)(lv + 128);
    dst[1] = (uint8_t)(rv + 128);
    return 1;
}
/* PCM 32-bit float -> 16-bit signed */
static inline uint32_t pcm_pack_f32_to_s16(int32_t l, int32_t r, uint8_t *dst)
{
    float fl, fr;
    int16_t lv, rv;
    l = fix_float_sample(l);
    r = fix_float_sample(r);
    memcpy(&fl, &l, 4);
    memcpy(&fr, &r, 4);
    lv = (int16_t)(fl * 32768.0f);
    rv = (int16_t)(fr * 32768.0f);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)(rv & 0xFF);
    dst[3] = (uint8_t)((rv >> 8) & 0xFF);
    return 2;
}
/* PCM 32-bit float -> 24-bit signed */
static inline uint32_t pcm_pack_f32_to_s24(int32_t l, int32_t r, uint8_t *dst)
{
    float fl, fr;
    int32_t lv, rv;
    l = fix_float_sample(l);
    r = fix_float_sample(r);
    memcpy(&fl, &l, 4);
    memcpy(&fr, &r, 4);
    lv = (int32_t)(fl * 8388608.0f);
    rv = (int32_t)(fr * 8388608.0f);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)(rv & 0xFF);
    dst[4] = (uint8_t)((rv >> 8) & 0xFF);
    dst[5] = (uint8_t)((rv >> 16) & 0xFF);
    return 3;
}
/* PCM 32-bit float -> 32-bit signed */
static inline uint32_t pcm_pack_f32_to_s32(int32_t l, int32_t r, uint8_t *dst)
{
    float fl, fr;
    int32_t lv, rv;
    l = fix_float_sample(l);
    r = fix_float_sample(r);
    memcpy(&fl, &l, 4);
    memcpy(&fr, &r, 4);
    lv = (int32_t)(fl * 2147483648.0f);
    rv = (int32_t)(fr * 2147483648.0f);
    dst[0] = (uint8_t)(lv & 0xFF);
    dst[1] = (uint8_t)((lv >> 8) & 0xFF);
    dst[2] = (uint8_t)((lv >> 16) & 0xFF);
    dst[3] = (uint8_t)((lv >> 24) & 0xFF);
    dst[4] = (uint8_t)(rv & 0xFF);
    dst[5] = (uint8_t)((rv >> 8) & 0xFF);
    dst[6] = (uint8_t)((rv >> 16) & 0xFF);
    dst[7] = (uint8_t)((rv >> 24) & 0xFF);
    return 4;
}
/* PCM 32-bit float -> 32-bit IEEE float */
static inline uint32_t pcm_pack_f32_to_f32(int32_t l, int32_t r, uint8_t *dst)
{
    l = fix_float_sample(l);
    r = fix_float_sample(r);
    // просто копируем байты
    memcpy(dst, &l, 4);
    memcpy(dst+4, &r, 4);
    return 4;
}




/* Format Unpack helpers (for processing loop)
   ------------------------------------------- */

static inline void pcm_unpack_s8_dual_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const uint8_t *p = src;
    for (i = 0; i < n; ++i) {
        l[i] = ((int32_t)p[2*i]     - 128) << 24;
        r[i] = ((int32_t)p[2*i + 1] - 128) << 24;
    }
}
static inline void pcm_unpack_s8_mono_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i) {
        int32_t v = ((int32_t)src[i] - 128) << 24;
        l[i] = v; r[i] = v;
    }
}

static inline void pcm_unpack_s16_dual_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    const int16_t *p = (const int16_t *)src;
    uint32_t i;
    for (i = 0; i < n; ++i) {
        l[i] = ((int32_t)p[2*i])     << 16;
        r[i] = ((int32_t)p[2*i + 1]) << 16;
    }
}
static inline void pcm_unpack_s16_mono_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const int16_t *p = (const int16_t *)src;
    for (i = 0; i < n; ++i) {
        int32_t v = ((int32_t)p[i]) << 16;
        l[i] = v; r[i] = v;
    }
}

/* 24-bit packed to int32_t */
static inline int32_t convert_s24_to_s32(const uint8_t *p)
{
    int32_t v = (p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x00800000) v |= 0xFF000000;  // sign extend
    return v << 8;
}
static inline void pcm_unpack_s24_dual_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const uint8_t *p = src;
    for (i = 0; i < n; ++i) {
        l[i] = convert_s24_to_s32(p); p += 3;
        r[i] = convert_s24_to_s32(p); p += 3;
    }
}
static inline void pcm_unpack_s24_mono_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const uint8_t *p = src;
    for (i = 0; i < n; ++i) {
        int32_t v = convert_s24_to_s32(p); p += 3;
        l[i] = v; r[i] = v;
    }
}

/* S32 passthrough */
static inline void pcm_unpack_s32_dual_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const int32_t *p = (const int32_t *)src;
    for (i = 0; i < n; ++i) {
        l[i] = p[2*i];
        r[i] = p[2*i + 1];
    }
}
static inline void pcm_unpack_s32_mono_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const int32_t *p = (const int32_t *)src;
    for (i = 0; i < n; ++i) {
        l[i] = p[i]; r[i] = p[i];
    }
}

/* IEEE-754 float32 to int32_t */
static inline int32_t convert_f32_to_s32(float v)
{
    if (v > 1.0f) v = 1.0f;
    else if (v < -1.0f) v = -1.0f;
    return (int32_t)(v * 2147483647.0f);
}
static inline void pcm_unpack_f32_dual_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const float *p = (const float *)src;
    for (i = 0; i < n; ++i) {
        l[i] = convert_f32_to_s32(p[2*i]);
        r[i] = convert_f32_to_s32(p[2*i + 1]);
    }
}
static inline void pcm_unpack_f32_mono_to_s32(const uint8_t *src, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    const float *p = (const float *)src;
    for (i = 0; i < n; ++i) {
        int32_t v = convert_f32_to_s32(p[i]);
        l[i] = v; r[i] = v;
    }
}




/* Mono-Downmix helpers (if set in config)
   --------------------------------------- */

/* вывод как есть (bypass) */
static inline void pcm_downmix_none(int32_t *l, int32_t *r, uint32_t n)
{
    (void)l; (void)r; (void)n;
}

/* L/2 + R/2 == -6 dB перед сложением */
static inline void pcm_downmix_half(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i) {
        int32_t m = (l[i] >> 1) + (r[i] >> 1);
        l[i] = m; r[i] = m;
    }
}
/* max(|L|, |R|) с сохранением более громкого */
static inline void pcm_downmix_peak(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i) {
        int32_t al = l[i] < 0 ? -l[i] : l[i];
        int32_t ar = r[i] < 0 ? -r[i] : r[i];
        int32_t m  = (al > ar) ? l[i] : r[i];
        l[i] = m; r[i] = m;
    }
}


#endif
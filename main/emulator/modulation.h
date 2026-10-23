/* modulation.h */
/* -----------------------
 * Basic OFDM/m-ASK/PAM stuff:
 * - Modulation param descriptors;
 * - Frame mod/demod forwards (ASK)
 */
#ifndef MODULATION_H
#define MODULATION_H
#include "config.h"
#include "pcm_io.h"
#include "dsp_host.h"

/* Общее */
#define noise 10485760  // порог отсечки шума (-45 дБ в 32 бит)

typedef enum {
    FRAME_NONE,    // фрейм ещё не накоплен (ждём статуса)
    FRAME_NEW,     // маркер начала нового фрейма (сброс таймера)
    FRAME_HEADER,  // достигнут заголовок фрейма (парсим сведения)
    FRAME_FULL,    // фрейм полностью накоплен (демодулируем)
    FRAME_ABRUPT,  // фрейм преждевременно оборван (демодулируем)
    FRAME_BROKEN   // у фрейма битый заголовок (дальше на читаем)
} DemodResult;


/* OFDM модуляция
   -------------- */
typedef struct {
    uint16_t  fft_size;            // OFDM geometry
    uint32_t  sample_rate;         // Fs сигнала (из конфига или потока)
    uint32_t  low_freq;            // нижняя полезная частота (общая)
    uint32_t  high_freq;           // верхняя полезная частота (текущая)
    uint16_t  qam_order;           // порядок QAM: 2/4/16/64/256
    uint8_t   symbols_per_frame;   // кол-во OFDM-символов в одном фрейме
    uint8_t   bits_per_symbol;     // бит на символ, вычисляемое
    uint16_t  active_carriers;     // N поднесущих, вычисляемое
    uint8_t   frame_stereo;        // запись в 2 канала: 0/1
    uint16_t  custom_bytes;        // переопределение payload_bytes для ASK

} OfdmParams;

/* Прототипы функций (OFDM) */
int bits_per_symbol_from_qam(int qam);
int calc_active_carriers(OfdmParams *ofdm);
int calc_payload_bytes(OfdmParams *ofdm);


/* m-ASK/PAM модуляция
   ------------------- */
typedef struct {
    int32_t   min_amp;             // амплитуда самого младшего бита (в 32-битном PCM)
    int32_t   max_amp;             // амплитуда самого старшего бита (в 32-битном PCM)
    int32_t   sync_amp;            // амплитуда преамбулы/синхроуровня (обычно равна max_amp)

    uint32_t  num_levels;          // кол-во амплитуд для кодирования (= qam_order)
    uint32_t  amp_levels[260];     // значения амплитуд для символов 0..(levels-1)
    uint32_t  thresh_levels[260];  // пороги для различения символов (длина: levels-1)
    int32_t   **wave_table;        // сохранённые синус. шаблоны периодов для каждого уровня

    uint8_t   symbol_bits;         // кол-во бит на 1 символ (как в OFDM: 1,2,4,6,8)
    uint32_t  cycle_samples;       // сколько сэмплов в 1 периоде (исходная частота несущей)
    uint32_t  pilot_interval;      // через сколько символов данных вставлять синхроимпульс
    int       variable_len;        // обрезать фреймы по payload_used вместо capacity_bits

    uint32_t  preamble_symbols;    // ожидаемая длина НЧ-части преамбулы в символах m-ASK
    uint32_t  training_symbols;    // доп. символы ВЧ-преамбулы для тренировки порогов уровней
    uint32_t  extra_symbols;       // доп. символы от 2-ASK заголовка, получающиеся при 4+ASK
    uint32_t  sync_train_max;      // участок с шаблоном уровня макс. бита (тренировка порогов)
    uint32_t  sync_train_min;      // участок с шаблоном уровня нулевого бита (тренировка порогов)
    uint32_t  sync_train_lv1;      // участок с шаблоном нижн. среднего уровня (детект сжатия ДД)
    uint32_t  sync_train_lv2;      // участок с шаблоном верх. среднего уровня (детект сжатия ДД)

} AskParams;

/* RX: настройки */
typedef struct {
    AskParams cfg;                 // дескриптор m-ASK профиля (выше)

    // Параметры обработки
    int      agc_enabled;          // включение авто-подстройки усиления (из конфига)
    int32_t  agc_percent;          // ручная подстройка множителя gain_mul (из конфига)
    int32_t  agc_smoothing;        // вес среднего gain_mul относительно текущего (из конфига)
    int      invert_phase;         // детекция периода по нисходящим crossing (-1 = авто)
    uint32_t reset_frames;         // сколько фреймов можно потерять до сброса состояний
    uint32_t sample_rate;          // частота дискретизации потока (для вычисления несущей)
    int32_t  drift_margin;         // нестабильные края порога символа в % (для ручной коррекции)
    int      correct_dynamics;     // коррекция сжатия порогов в % с учётом измерений в данных
    int      correct_nonlinear;    // коррекция нелинейности средних уровней в канале (для 4-ASK)
    int      measure_dynamics;     // оценка ДД записи по training_symbols (калибровка уровней)
    int      workarounds;          // временные костыли улучшений при разных параметрах сигнала

    // Счётчики и пороги детекции
    uint32_t preamble_detect;      // сколько символов нужно для обнаружения преамбулы
    uint32_t data_periods_max;     // за сколько периодов установить несущую частоту (re-sync)

    // Обновляемые параметры (из фрейма)
    int      order_changed;        // флаг смены модуляции в текущей паре фреймов (для пересчёта)
    int      gray_mapping;         // флаг наличия кодирования Грея в данных фрейма (перемежение)
    uint32_t frame_bits_min;       // минимальная ёмкость в полном фрейме (служебка + 32 байта)
    uint32_t frame_bits_max;       // сколько бит ожидается в полном фрейме (header + payload)
    uint32_t frame_symbols;        // кол-во символов в теле фрейма (заголовок + extra + данные)
    uint32_t pilot_symbols;        // кол-во пилотов во фрейме (из body_symbols / pilot_interval)
    uint32_t frame_periods;        // сколько всего периодов во фрейме (для детекции потерь)
    uint32_t dynamics_average;     // средний динамический диапазон за сессию (dynamics_percent)
    uint32_t dynamics_counter;     // счётчик замеров дин. диапазона (для финального усреднения)

    // Частотный детектор (межканальный)
    uint32_t freq_samples;         // сколько в среднем сэмплов в 1 периоде несущей (в 2 каналах)
    int32_t  carrier_freq;         // несущая частота сигнала (из конфига, далее по freq_samples)

} AskDemodCtx;

/* RX: адаптивный эквалайзер */
typedef struct {
    // Общие параметры
    int      enabled;              // общий флаг включения обработки
    int      sample_rate;          // копия частоты потока (для вычисления несущей по сэмплам)
    int      cycle_samples;        // копия кол-ва сэмплов в периоде (для вычисления несущей)
    int      preamp_mode;          // режим предусиления: простой, точный, нормализация (-20 дБ)
    double   preamp_db;            // предварительное усиление всего сигнала (постоянное)
    double   preamp_init;          // начальное значение предусиления: конфиг+bias (для сбросов)
    double   preamp_lin;           // аттенюация выхода для избежания клиппинга (обновляемое)
    double   gain_init_max;        // макс. лимит при применении начального усиления (1..6 дБ)
    double   gain_alpha;           // весовой коэффициент среднего при сглаживании (0 = выкл.)

    // De-Emphasis (пред-коррекция)
    biquad_d pre_low;              // биквад low-shelf фильтра для rumble-компенсации (край НБП)
    biquad_d pre_high;             // биквад high-shelf фильтров для bias-корректировки ленты
    biquad_d isi_peak;             // биквад peak-фильтра для подавления post-cursor ISI (звона)

    // Adaptive Bias (shelf-коррекция ВЧ)
    int      bias_adaptive;        // включение динамического управления усилением коррекции
    double   bias_filter_hz;       // копия частоты среза shelf-фильтра (обычно 9000 Гц)
    double   bias_filter_q;        // добротность shelf-фильтра (из Type-профиля, ~0.15/0.50)
    double   bias_gain_init;       // начальное значение усиления, обычно +6 дБ (для сбросов)
    double   bias_deadzone;        // толерантность коррекции к изменениям АЧХ ("мёртвая зона")

    // Adaptive Bias (состояние обработки)
    int      bias_gain_done;       // флаг, что начальное усиление уже применялось (для delta)
    double   bias_gain_db;         // текущее усиление peak-фильтром на позиции ВБП (или ручное)
    double   bias_gain_avg;        // накопление среднего усиления (при gain_alpha != 0.0)

    // De-Ringing (post-cursor ISI)
    int      isi_adaptive;         // включение динамического расчёта частоты и подавления
    double   isi_upside_pos;       // позиция peak-фильтра на верхней боковой полосе (0.5 = центр)
    double   isi_peak_q;           // добротность peak-фильтра (из конфига, обычно 0.25-0.35)
    double   isi_deadzone;         // толерантность к звону при коррекции (плохие ленты и тракты)

    // De-Ringing (состояние обработки)
    double   isi_gain_db;          // текущее усиление peak-фильтром на позиции ВБП (или ручное)
    double   isi_gain_avg;         // накопление среднего усиления (при gain_alpha != 0.0)
    double   isi_peak_hz;          // вычисленная частота peak-фильтра на верхней боковой полосе
    int32_t  isi_prev_freq;        // предыдущая несущая частота (сокращение пересчётов фильтра)

    // Логирование и отладка...
    int      dump_samples;         // включение дампа эквализованных сэмплов в файл (отладка)
    FILE     *raw_samples;         // файл дампа эквализованных сэмплов (что видит демодулятор)
    const char   *channel;         // тег текущего стерео-канала в экземпляре (L, R)

} AskAdaptiveEq;

/* RX: цикл обработки */
typedef struct {
    FrameDesc frame;   // будет заполняться при разборе
    AskAdaptiveEq eq;  // состояние адаптивного эквалайзера

    // Конечный автомат
    enum {
        ST_SEARCH,                 // поиск преамбулы
        ST_PREAMBLE,               // чтение преамбулы
        ST_FRAME                   // чтение фрейма данных
    } state;

    // Детектор периодов и crossing
    int32_t  prev_sign;            // знак предыдущего сэмпла (для сравнения)
    int32_t  prev_sample;          // знаковое значение предыдущего сэмпла (детект перегибов)
    int32_t  before_sample;        // знаковое значение позапрошлого сэмпла (для массива пиков)
    int32_t  was_going_up;         // флаг, что амплитуда на текущем сэмпле возрастала
    int32_t  was_going_down;       // флаг, что амплитуда на текущем сэмпле убывала
    int32_t  period_mid;           // средняя точка текущего периода (виртуальный "ноль")
    int32_t  invert_phase;         // флаг начального обнаружения направления фазы (-1 = авто)
    uint32_t sample_count;         // общий счётчик обработанных сэмплов (для логов)

    // Накопление за текущий период
    uint32_t samp_in_period;       // сколько сэмплов в текущем периоде (x100)
    int32_t  period_peaks[2][4];   // массив для каждого полупериода: пик, 2 соседа, среднее

    // Чтение символов (= 1 период)
    uint32_t preamble_got;         // сколько всего бит преамбулы уже набрано
    uint32_t frame_bits_got;       // сколько всего бит данных уже набрано
    uint32_t frame_bits_actual;    // актуальный размер фрейма, прочитанный из заголовка
    uint32_t frame_bits_pair;      // больший frame_bits_actual из стерео-пары (для метаданных)
    uint32_t partial_bits;         // сколько бит накоплено в обрезанном фрейме
    uint32_t idle_periods;         // как долго мы находимся в ST_SEARCH (детекция lost_frames)
    uint32_t lost_frames;          // счётчик потерянных фреймов (если был шум/тишина)

    // Бит-буферы фрейма
    uint8_t *bitbuf;               // выделяется под capacity_bits
    uint32_t bitbuf_pos;           // текущая позиция (в битах)
    uint8_t *payload_buf;          // указатель на извлечённые данные (после разбора)

    // Параметры для АРУ
    int32_t  agc_stage;            // флаги состояния АРУ (инициализация/калибровка/процесс)
    uint32_t gain_mul;             // множитель для коррекции порогов амплитуд (x1000)
    uint32_t pilot_counter;        // счётчик для интервала пилотов (0 – следующий символ пилот)
    uint32_t pilot_errors;         // кол-во подряд идущих неверных уровней (= обрыв фрейма)

    // Частотный детектор
    uint32_t sync_samples;         // сколько сэмплов содержит текущая преамбула
    uint32_t period_samples;       // сколько в среднем сэмплов в 1 периоде (по sync_samples)
    uint32_t period_min;           // мин. порог отклонения частоты (в % от period_samples)
    uint32_t period_max;           // макс. порог отклонения частоты (вычисляется при crossing)
    uint32_t wrong_samples;        // счётчик сэмплов невалидных периодов (для авто-завершения)
    uint32_t probe_samples;        // пробник частоты периода с данными, пока period_samples = 0
    uint32_t probe_periods;        // счётчик набранных периодов с одной частотой во время пробы

    // Адаптивные пороги
    uint32_t sync_amp_meas;        // средняя амплитуда НЧ-преамбулы (по умолчанию = max_amp)
    uint32_t min_amp_meas;         // средняя амплитуда мин. уровня (по тренировочным символам)
    uint32_t max_amp_meas;         // средняя амплитуда макс. уровня (по тренировочным символам)
    uint32_t amps_frame[260];      // значения амплитуд текущего фрейма (после получения *_amp_meas)
    uint32_t thresh_frame[260];    // пороги амплитуд текущего фрейма (после получения *_amp_meas)

    // Коррекция нелинейности
    int32_t  prev_symbol;          // предыдущее значение символа (при измерении на чередовании)
    int32_t  prev_symbol_pos;      // позиция символа внутри порога в % (эвристическая коррекция)
    uint32_t data_min_meas;        // средняя амплитуда мин. уровня внутри фрейма (коррекция ДД)
    uint32_t data_max_meas;        // средняя амплитуда макс. уровня внутри фрейма (коррекция ДД)
    uint32_t data_min_count;       // кол-во измерений мин. уровня внутри фрейма (для валидации)
    uint32_t data_max_count;       // кол-во измерений макс. уровня внутри фрейма (для валидации)
    uint32_t amp_disp_ratio;       // разброс уровней порогов между статическими и динамическими

    // Коррекция ISI (для EQ)
    uint32_t isi_meas_first;       // уровень первого нулевого бита после amp_max (поиск звона)
    uint32_t isi_meas_steady;      // уровень среднего нулевого бита на участке (вычисление звона)

    // Логирование для отладки
    uint32_t dynamics_percent;     // текущий динамический диапазон по отношению к эталону
    const char *channel;           // тег текущего стерео-канала в экземпляре (L, R)

} AskDemodState;


/* Глобальные переменные */
int total_bitrate, payload_bitrate;  // расчёт битрейта потока (TX/RX)

/* Общие функции (ASK) */
void calc_stream_bitrate(OfdmParams *ofdm, AskParams *ask,  // расчёт битрейта потока (raw/effective)
    int sample_rate, int capacity_bits, int payload_bytes, int pilot_symbols);

void ask_calc_thresh_levels(AskParams *ask, uint32_t min_amp, uint32_t max_amp, 
    uint32_t *amp_levels, uint32_t *thresh_levels, int pcm_fmt);  // расчёт порогов уровней (применяется к разным массивам)

/* TX: Функция записи фрейма */
int ask_build_wave_table(AskParams *ask);  // пред-генерация амплитудных sin-шаблонов с периодами
int ask_write_lead_symbols(PcmSink *sink, AudioDspCtx *dsp, AskParams *ask, uint32_t num_symbols, int type_flag);  // запись lead-in/out секвенции (для синхронизации)
int ask_write_frame(PcmSink *sink, AudioDspCtx *dsp, FrameDesc *desc, FrameDesc *desc_r, AskParams *ask, 
    uint8_t *bitbuf_L, uint8_t *bitbuf_R, uint32_t bits);  // поддерживает pre-emphasis сэмплов (с DSP-звеном)

/* RX: Функции чтения фрейма */
DemodResult ask_process_sample(AskDemodCtx *rctx, AskDemodState *proc, int32_t sample);  // чтение входного сэмпла и вызов функций
void ask_finish_period(AskDemodCtx *rctx, AskDemodState *proc);  // обработка периодов (преамбула, тренировка, payload, пилоты)

/* RX: Утилиты для функций чтения */
void ask_average_peak(AskDemodCtx *rctx, AskDemodState *proc);  // helper для усреднения пика по 2 полупериодам с 2 соседними сэмплами
void ask_calc_frame_dimensions(AskDemodCtx *rctx, AskDemodState *proc);  // обновление геометрии фрейма (после чтения заголовка)
void ask_reset_preamble(AskDemodState *proc);  // сброс параметров преамбулы (при окончании или ошибке)
void ask_reset_frame_state(AskDemodState *proc);  // сброс состояния фрейма (когда прочитан)
void ask_reset_rx_values(AskDemodState *proc);  // начальные значения во всех контекстах (инициализация)

/* RX: Адаптивный эквалайзер */
int  ask_adaptive_eq_init(AskAdaptiveEq *eq, int32_t sample_rate, const char *ch);
void ask_adaptive_eq_process(AskAdaptiveEq *eq, int32_t *sample);  // обработка в process_sample
void ask_adaptive_eq_select(AskAdaptiveEq *eq, int32_t carrier_freq, uint32_t meas_lf, uint32_t meas_hf, uint32_t amp_first, uint32_t amp_steady);  // выбор параметров ISI-коррекции
void ask_adaptive_eq_reset(AskAdaptiveEq *eq);   // сброс статистики и усилений (при ошибках)
void ask_adaptive_eq_free(AskAdaptiveEq *eq);    // закрытие дампов и освобождение ресурсов


#endif
/* RX_adts.c */
/* -----------------
 * Функциональность:
 * - Инициализация AAC-декодера (FAAD2)
 * - Открытие PcmSink-бэкенда для вывода сэмплов
 * - Декодирование ADTS-фреймов в PCM-сэмплы
 * - Применение PCM-concealment для сглаживания ошибок
 *
 * Важные замечания:
 * - Это часть RX.c, и должна быть включена текстуально;
 * - Требуется видимость структур AppConfig, PcmSink и NeAAC*;
 * - Вызовы производятся из функций rx_run() и handle_frame().
 */
#ifdef _WIN32
#include "faad2_dll.h"  // загрузка DLL
#endif

/* Параметры декодирования */
typedef struct {
    // Контекст AAC+ декодера (FAAD2)
    PcmSink sink;                  // аудио-бэкенд для выхода декодера
    AppConfig cfg;                 // настройки, полученные из ADTS-фрейма
    AudioDspCtx dsp_out;           // доп. DSP-обработка PcmSink-сэмплов
    NeAACDecHandle faad_handle;    // главный хэндл декодера
    int faad_initialized;          // флаг, что инициализация уже прошла
    char out_file[256];            // копия имени выходного файла (для PcmSink)

    // Коррекция ошибок (concealment)
    uint8_t *adts_frame_buf;       // PCM-данные одного ADTS-фрейма (16-бит, interleaved)
    uint32_t adts_frame_samples;   // количество сэмплов в этом фрейме (total, все каналы)
    uint32_t adts_block_frames;    // сколько ADTS-фреймов в последнем успешном ASK-фрейме
    int32_t conceal_gain_percent;  // текущее усиление conceal-сэмплов в % (начальное: 60-75)
    int32_t conceal_step_percent;  // делитель % для затухания с каждым повтором (напр. 10)

} AudioDecCtx;

/* Forward некоторых функций */
void aac_decoder_create(AudioDecCtx *actx);
int  aac_decoder_process(AudioDecCtx *actx, uint8_t *payload_buf, uint16_t payload_size);
void aac_conceal_play(AudioDecCtx *actx, int fnum);
void aac_decoder_free(AudioDecCtx *actx);


/* Инициализация AAC-декодера (FAAD2)
   ---------------------------------- */
void aac_decoder_create(AudioDecCtx *actx)
{
    actx->faad_initialized = 0;
    if (g_faadOpen) {
        actx->faad_handle = g_faadOpen();
        if (actx->faad_handle) {
            // скопируем параметры аудио-вывода по умолчанию
            memcpy(&actx->cfg, &cfg, sizeof(AppConfig));
            actx->conceal_gain_percent = 66;  // начальное усиление conceal (-3 дБ)
            actx->conceal_step_percent = 10;  // шаг затухания (можно вынести в конфиг)
        } else {
            fprintf(stderr, "RX: failed to initialize FAAD2 decoder.\n");
        }
    }
}


/* Чтение параметров и декодирование
   --------------------------------- */
int aac_decoder_process(AudioDecCtx *actx, uint8_t *payload_buf, uint16_t payload_size)
{
    // Объявления переменных
    NeAACDecFrameInfo frame_info = {0};
    uint8_t *input_ptr = payload_buf;
    uint32_t remaining = payload_size;
    void *pcm; int16_t *samples = 0;
    int32_t left, right; uint32_t i;

    uint32_t block_frames  = 0;  // сколько ADTS-фреймов успешно декодировано
    uint32_t last_samples  = 0;  // количество сэмплов в последнем ADTS-фрейме
    uint32_t samples_bytes = 0;  // размер буфера сэмплов из FAAD2 в байтах
    actx->conceal_gain_percent = 66;  // сбросим начальное усиление на -3 дБ

    // Инициализация декодера первым ADTS-фреймом
    if (!actx->faad_initialized && g_faadInit(actx->faad_handle, 
        input_ptr, 32, &frame_info.samplerate, &frame_info.channels) == 0) {
        actx->faad_initialized = 1;  // декодер теперь содержит параметры потока

        // заполняем поля PcmSink полученными данными
        actx->cfg.output.sample_rate  = frame_info.samplerate;
        actx->cfg.output.pcm_channels = frame_info.channels;
        actx->cfg.winmm.list_devices  = 0; // не шумим в логах

        // инициализация DSP для исправления артефактов
        if (actx->cfg.rx.aac_postproc) {
            actx->sink.src_format = (PcmFormat)32;  // сэмплы теперь 32-битные
            actx->dsp_out.fsm_mode = DEMOD_POST; fprintf(stderr, "\n");
            dsp_init(&actx->dsp_out, &actx->cfg, frame_info.samplerate);
        } else {
            // если DSP нет, всегда используем 16 бит
            actx->sink.src_format = (PcmFormat)16;  // сообщить формат декодера
            actx->cfg.output.pcm_format = actx->sink.src_format;
        }

        // теперь открываем PcmSink с верным форматом
        if (open_sink(&actx->sink, &actx->cfg, actx->out_file) != 0) {
            fprintf(stderr, "\nCannot open output %s target.\n", actx->cfg.output.pcm_backend);
            return -1;
        }
    }

    // Читаем каждый ADTS-фрейм в payload отдельно
    while (remaining > 0) {
        frame_info.bytesconsumed = 0;
        pcm = g_faadDecode(actx->faad_handle, &frame_info, input_ptr, remaining);

        // Декодировано успешно!
        if (pcm && !frame_info.error && frame_info.bytesconsumed) {
            samples = (int16_t*)pcm;  // по умолчанию 16-битный вывод
            for (i = 0; i < frame_info.samples; i+= frame_info.channels) {
                left  = samples[i];  // для моно дублируем левый канал
                right = (frame_info.channels == 2) ? samples[i+1] : left;

                // DSP-коррекция артефактов (по-сэмпловая)
                if (actx->cfg.rx.aac_postproc) {
                    left <<= 16; right <<= 16;  // масштабируем в Q31
                    dsp_process(&actx->dsp_out, &left, &right, 1);
                }
                // Записываем сэмплы в аудио-выход (PcmSink)
                actx->sink.write_sample(actx->sink.ctx, left, right);
            }

            // Сохраняем успешный фрейм и conceal-статистику
            samples_bytes = frame_info.samples * sizeof(int16_t);
            last_samples  = frame_info.samples; block_frames++;
            if (last_samples >= 1024) { // выделяем буфер (если нет) и копируем фрейм
                if (!actx->adts_frame_buf) actx->adts_frame_buf = (uint8_t*)malloc(samples_bytes);
                if ( actx->adts_frame_buf) memcpy(actx->adts_frame_buf, samples, samples_bytes);
            }

        } else {
            // Ошибка, или декодер не вернул значения
            fprintf(stderr, "\nFAAD2 AAC decode error: %d (bytes read: %d)\n", 
                frame_info.error, frame_info.bytesconsumed);
                // вызов заглушки + защита от бесконечного цикла
                aac_conceal_play(actx, block_frames); return 0;
        }

        // Сдвигаем указатель и уменьшаем остаток
        input_ptr += frame_info.bytesconsumed;
        remaining -= frame_info.bytesconsumed;
    }

    // Если был хотя бы один успешный фрейм, обновляем concealment-данные
    if (samples && last_samples >= 1024) {
        if (actx->adts_frame_buf) {
            // Запоминаем, сколько было фреймов и сэмплов
            actx->adts_block_frames  = block_frames;  // фреймов в ASK-блоке
            actx->adts_frame_samples = last_samples;  // сэмплов в ADTS-фрейме
        } else {
            fprintf(stderr, "Failed to allocate conceal buffer.\n");
        }
    }

    return 0;
}


/* Функция воспроизведения concealment
   ----------------------------------- */
void inline aac_conceal_write
(AudioDecCtx *actx, int16_t *samples, int i, int gain) {
    // Аттенюация с эффектом затухания
    int left  = samples[i] * gain / 100;
    int right = (actx->sink.channels == 2) ? samples[i+1] * gain / 100 : left;
    // Ограничение пиков до -12 дБ (мягче)
    left  = (left  > 8192) ? 8192 : (left  < -8192 ? -8192 : left);
    right = (right > 8192) ? 8192 : (right < -8192 ? -8192 : right);

    // DSP-коррекция артефактов (аналогичная)
    if (actx->cfg.rx.aac_postproc) {
        left <<= 16; right <<= 16;  // Q15 -> Q31
        dsp_process(&actx->dsp_out, &left, &right, 1);
    }
    // Выдача обработанных сэмплов в sink
    actx->sink.write_sample(actx->sink.ctx, left, right);
}

void aac_conceal_play(AudioDecCtx *actx, int fnum)
{
    int32_t channels = actx->sink.channels;
    int16_t *samples = (int16_t*)actx->adts_frame_buf;
    int32_t frame_samples = actx->adts_frame_samples;  // в сэмплах (все каналы)
    int32_t block_frames  = actx->adts_block_frames;   // кол-во фреймов в блоке
    int32_t gain = actx->conceal_gain_percent;         // текущее ослабление сэмплов
    int32_t stride = channels, frame, reverse, i;

    if (!actx->adts_frame_buf || 
        !actx->adts_frame_samples || 
        !actx->adts_block_frames || 
        !actx->conceal_gain_percent) return;

    // Рассчитать остаток блоков (при ошибке декодера)
    if (fnum > 0) block_frames -= fnum;
    if (block_frames <= 0) block_frames = 1;

    // Каждый вызов воспроизводит ровно столько фреймов, сколько было в последнем успешном ASK-блоке
    for (frame = 0; frame < block_frames; frame++) {
        reverse = (frame % 2 == 1) ? 0 : 1;  // 0 – прямой, 1 – обратный
        if (!reverse) {
            for (i = 0; i < frame_samples; i+= stride)
            aac_conceal_write(actx, samples, i, gain);
        } else {
            for (i = frame_samples -stride; i >= 0; i-= stride)
            aac_conceal_write(actx, samples, i, gain);
        }
    }

    // Ослабить усиление на шаг (для следующего затухания)
    actx->conceal_gain_percent -= actx->conceal_step_percent;
    if (actx->conceal_gain_percent <= 0) actx->conceal_gain_percent = 0;
}

/* Освобождение памяти и контекста
   ------------------------------- */
void aac_decoder_free(AudioDecCtx *actx)
{
    if (actx->faad_handle) {
        g_faadClose(actx->faad_handle);
        actx->faad_handle = NULL;
    }
    if (actx->adts_frame_buf) {
        free(actx->adts_frame_buf);
    }
    // также закрываем PcmSink-вывод и DSP
    if (actx->sink.ctx) actx->sink.close(actx->sink.ctx);
    if (&actx->dsp_out) dsp_free(&actx->dsp_out);
}

/* TX_adts.c */
/* -----------------
 * Функциональность:
 * - Парсинг и запись ADTS-фреймов из HE-AAC+ в 
 *   payload-фрейм, с выравниванием по границам.
 * - AAC-декодер в RX принимает готовые блоки фреймов;
 *   функция управления применяет аудио-concealment.
 *
 * Важные замечания:
 * - Это часть TX.c, и должна быть включена текстуально;
 * - Вызовы производятся из функции tx_read_input_data().
 */
#ifdef _WIN32
#include "fdkaac_dll.h"  // загрузка DLL
#endif


/* ---------------------------------------------
   Режим чтения ADTS-фреймов из AAC-файла (FILE)
   --------------------------------------------- */

/* Структура ADTS-парсера (общая) */
typedef struct {
    uint8_t  pending[8];   // сохранённый ADTS-заголовок будущего фрейма (7 байт)
    uint16_t pending_len;  // длина-маркер сохранённого фрейма (для будущего переноса)
    int      stream_eof;   // маркер конца файла для возврата статуса TX_READ_EOF
    uint32_t stream_time;  // глобальный таймер потока в наносекундах (для метаданных)
} AdtsReader;

/* Таблица частот дискретизации */
int adts_sample_rate(int index)
{
    switch (index) {
    case 0:  return 96000;
    case 1:  return 88200;
    case 2:  return 64000;
    case 3:  return 48000;
    case 4:  return 44100;
    case 5:  return 32000;
    case 6:  return 24000;
    case 7:  return 22050;
    case 8:  return 16000;
    case 9:  return 12000;
    case 10: return 11025;
    case 11: return  8000;
    default: return 22050;  // HE-AAC default
    }
}


/* Функция накопления ADTS-фреймов в payload-фрейме (режим FILE) */
uint16_t tx_accumulate_adts(FILE *in, AdtsReader *adts, uint8_t *out_buf, uint16_t max_bytes, uint32_t *total)
{
    int32_t  state = 0;  // 0 – ищем 0xFF, 1 – проверили второй байт, 2 – читаем заголовок, 3 – читаем тело
    uint16_t out_pos = 0;
    uint8_t  byte = 0;
    uint16_t body_len = 0;
    size_t   body_got = 0;
    uint8_t  header[8];
    uint16_t header_pos = 0;

    // Поля ADTS-заголовка для валидации
    uint8_t  mpeg_layer;      // 0 - всегда (2 бита)
    uint8_t  aac_profile;     // 0–2, 3 - резерв (2 бита)
    uint8_t  rate_index;      // 0-11, 12-15 - резерв (4 бита)
    uint8_t  channel_config;  // 1-2, 0 - неопределено (3 бита)
    uint16_t frame_len = 0;   // 7-8191 - длина фрейма (13 бит)
    adts->stream_eof = 0;     // маркер конца файла (сбросим)

    // Если есть сохранённый заголовок от предыдущего вызова
    if (adts->pending_len > 0) {
        frame_len = adts->pending_len;
        if (max_bytes >= 7) {
            memcpy(out_buf, adts->pending, 7);  // записываем заголовок
            adts->pending_len = 0;   // очищаем маркер
            out_pos = 7; state = 3;  // переходим к чтению тела
        } else {
            // не должно случиться, если max_bytes > 7
            memcpy(out_buf, adts->pending, max_bytes);
            adts->pending_len = 0; out_pos = max_bytes;
        }
    }

    while (out_pos < max_bytes) {
        if (state == 3) {
            // Читаем тело фрейма (frame_len - 7 байт) одним блоком
            body_len = frame_len - 7;
            body_got = fread(out_buf + out_pos, 1, body_len, in);
            out_pos += (uint16_t)body_got;
            // Фрейм обработан, ищем следующий
            if (body_got >= body_len) {
                state = 0; continue;
            } else {
                // неожиданный конец файла, выходим
                adts->stream_eof = 1; break;
            }
        }

        // Побайтовый поиск синхро-слова и заголовка
        if (fread(&byte, 1, 1, in) != 1) {
            adts->stream_eof = 1; break;  // конец файла
        }

        if (state == 0) {
            if (byte == 0xFF) state = 1;
        } else if (state == 1) {
            if ((byte & 0xF0) == 0xF0) {
                header[0] = 0xFF;
                header[1] = byte;
                header_pos = 2; state = 2;
            } else {
                state = 0;  // заголовок не получен
            }
        } else if (state == 2) {
            header[header_pos++] = byte;
            if (header_pos == 7) {
                // Извлекаем поля ADTS-заголовка (для валидации)
                mpeg_layer     = ( header[1] >> 1) & 0x03;
                aac_profile    = ( header[2] >> 6) & 0x03;
                rate_index     = ( header[2] >> 2) & 0x0F;  // индексы 0-15
                channel_config = ((header[2] & 0x01) << 2) | ((header[3] >> 6) & 0x03);
                frame_len      = ((header[3] & 0x03) << 11) | (header[4] << 3) | (header[5] >> 5);

                // Выполняем проверку значений полей
                if (frame_len < 7 || frame_len > max_bytes 
                || (mpeg_layer != 0 || aac_profile > 2) 
                || (rate_index > 11 || channel_config > 2)) {
                    // один из параметров невалиден, пропускаем заголовок
                    fprintf(stderr, "\nInvalid ADTS header (size=%u, layer=%u, profile=%u, rate=%u, channels=%u)\n",
                    frame_len, mpeg_layer, aac_profile, rate_index, channel_config); state = 0; continue;
                }
                // Вычисляем длительность фрейма (для метаданных) в мкс
                adts->stream_time += (1000000 * 1024) / adts_sample_rate(rate_index);

                // Проверяем, помещается ли фрейм (заголовок + тело) в выходной буфер
                if (out_pos + frame_len <= max_bytes) {
                    // Копируем заголовок в выходной буфер
                    memcpy(out_buf + out_pos, header, 7);
                    out_pos += 7; state = 3;  // переходим к чтению тела блоками
                } else {
                    // Фрейм не помещается – сохраняем заголовок для следующего вызова
                    memcpy(adts->pending, header, 7);
                    adts->pending_len = frame_len; break;
                }
            }
        }
    }

    // Возвращаем кол-во записанных байт
    *total += out_pos; return out_pos;
}


/* -----------------------------------------------------
   Режим AAC-кодирования с чтением из потока (PcmSource)
   ----------------------------------------------------- */

/* Общая структура AAC-кодировщика */
typedef struct {
    // Источник аудио
    PcmSource src;                 // открытый PcmSource (файл или WinMM)
    int8_t   aac_version;          // копия версии AAC (0-2) из конфига
    uint32_t sample_rate;          // выходная частота дискретизации (из профиля)
    uint8_t  channels;             // число каналов (из src)
    uint8_t  pcm_format;           // битность (8/16/24/32)
    const char *in_file;           // копия имени входного файла (для PcmSource)

    PcmUnpackFn  unpack_samples;   // функция пред-распаковки сырых байт в int32_t
    PcmDownmixFn downmix_process;  // функция смешивания стерео в моно (по конфигу)

    // Кодер FDK-AAC
    HANDLE_AACENCODER encoder;     // только экземпляр

    // Буфер для сырых PCM-сэмплов (читается чанками из PcmSource)
    int32_t *pcm_buf[2];           // раздельные каналы (для стерео)
    uint32_t pcm_max_frames;       // макс. число фреймов в буфере (chunk_samples)
    uint32_t pcm_frames_read;      // сколько фреймов реально прочитано

    // Буфер для кодированных ADTS-фреймов
    uint8_t *enc_buf;              // кольцевой буфер с готовым AAC-потоком
    size_t   enc_buf_size;         // общий размер буфера
    size_t   enc_buf_pos;          // текущая позиция чтения
    size_t   enc_buf_len;          // длина валидных данных (от enc_buf_pos до конца)
    int16_t  enc_samples[4096];    // входной буфер FDK (16-бит), полученный из enc_buf

    // Статистика для пользователя
    uint32_t total_bytes_encoded;  // всего закодировано байт (для логов)

} AudioEncCtx;


/* Инициализация: Макрос отладки параметров */
#define g_setEncParam(enc, param, value) \
do { \
    rc = g_fdkSetParam((enc), (param), (value)); \
    if (rc != AACENC_OK) fprintf(stderr, "AAC encoder: aacEncoder_SetParam(%s) failed (error %d)\n", #param, rc); \
} while (0)

/* Инициализация: Подбор AAC-профиля при авто-детекте */
void calc_aac_profile(AudioEncCtx *ctx, int *profile, uint32_t *total_bps, int32_t user_kbps)
{
    // расчёт битрейта по двум значениям
    if (user_kbps >= 8) 
        *total_bps = user_kbps * 1000;  // берём из конфига
    else {
        if (*total_bps < 8000) *total_bps = 8000;  // мин. допуск - 8 кбит/с
        else if (!cfg.encoder.hard_cbr) 
            *total_bps = (*total_bps / 100) * 97;  // запас 3% под ABR и GAP
            else *total_bps = (*total_bps / 100) * 99;  // для CBR хватит 1%
    }

    // выбор AAC-профиля и числа каналов
    switch (ctx->aac_version) {
        // авто-подбор на основе битрейта
        case -1: 
            // чем ниже битрейт - тем выше AOT-профиль
            if (*total_bps >= 99000) *profile = 2;      // всегда LC-AAC
            else if (*total_bps >= 50000) {
                 if (ctx->channels >= 2) *profile = 5;  // HE-AAC v1
                 if (ctx->channels <= 1) *profile = 2;  // LC-AAC v0 в моно
            }
            else if (*total_bps >= 18000) {
                 if (ctx->channels >= 2) *profile = 29; // HE-AAC v2
                 if (ctx->channels <= 1) *profile = 5;  // HE-AAC v1 в моно
            }
            else {
                *profile = 5; ctx->channels = 1;        // всегда HEv1 моно
            }
            break;

        // выбор профиля по объектному индексу
        case  0: *profile = 2; break;   // force LC-AAC
        case  1: *profile = 5; break;   // force HE-AAC v1
        case  2:  default:
            // умолчание: HE-AAC v2 (или v1, если моно)
            *profile = (ctx->channels > 1) ? 29 : 5; break;
    }

    // фактическая частота кодирования для HE-AAC+
    if (*profile >= 5) ctx->sample_rate /= 2;  // половинная
}

/* Инициализация: Создание PCM-источника и AAC-кодера */
int aac_encoder_create(AudioEncCtx *ctx, uint32_t target_bitrate_bps)
{
    int ch = 0;
    int profile = 29;
    int downmix_mode;

    AACENC_ERROR   rc;  // коды ошибок
    AACENC_Info    info;
    AACENC_InArgs  in_args;
    AACENC_OutArgs out_args;
    AACENC_BufDesc in_buf_desc, out_buf_desc;

    fprintf(stderr, "AAC encoder: Initializing...\n");
    if (!ctx || !cfg.tx.aac_encoding) return -1;

    /* Открыть источник PcmSource
       -------------------------- */
    if (open_source(&ctx->src, &cfg, ctx->in_file) != 0) {
        fprintf(stderr, "Cannot open input source.\n");
        return -1;
    }

    /* Получить параметры аудио
       ------------------------ */
    ctx->aac_version = cfg.encoder.format;  // для расчёта профиля и тайм-кодов
    ctx->sample_rate = ctx->src.sample_rate;
    ctx->channels    = ctx->src.channels > 1 ? cfg.encoder.channels : 1;  // для кодирования
    ctx->pcm_format  = ctx->src.format <= 32 ? ctx->src.format : 32;  // для распаковки сэмплов

    // Подбор AAC-профиля на основе режима и битрейта
    calc_aac_profile(ctx, &profile, &target_bitrate_bps, cfg.encoder.bitrate);

    // Выбор функций распаковки сэмплов и микширования
    downmix_mode = (ctx->channels == 1) ? 1 : 0;
    ctx->unpack_samples = pcm_select_unpack_fn(ctx->src.channels, ctx->src.format);
    ctx->downmix_process = pcm_select_downmix_fn(ctx->src.channels, downmix_mode);
    if (!ctx->unpack_samples || !ctx->downmix_process) {
        fprintf(stderr, "Unsupported input PCM format.\n");
        goto fail_close_source;
    }

    /* Создать экземпляр кодера
       ------------------------ */
    if (g_fdkOpen(&ctx->encoder, 0, ctx->channels) != AACENC_OK) {
        fprintf(stderr, "Calling aacEncOpen() failed.\n");
        goto fail_close_source;
    }

    /* Установить параметры кодера
       --------------------------- */
    // Выбранный AAC-профиль (Audio Object type)
    g_setEncParam(ctx->encoder, AACENC_AOT, profile);

    // Режим каналов: 1 = mono, 2 = stereo
    g_setEncParam(ctx->encoder, AACENC_CHANNELMODE, ctx->channels);
    g_setEncParam(ctx->encoder, AACENC_SAMPLERATE, ctx->src.sample_rate);
    // TODO: нестандартные Fs в PcmSource дают ошибку - форсим 44100?

    // Битрейт: вычисленный из профиля модуляции или из конфига
    g_setEncParam(ctx->encoder, AACENC_BITRATE, target_bitrate_bps);  // принимается в битах/с!
    g_setEncParam(ctx->encoder, AACENC_BITRATEMODE, 0);               // для стрима всегда CBR (0)
    if (cfg.encoder.hard_cbr) {
        // ограничим пики битрейта (лучше для стрима, но хуже качеством)
        g_setEncParam(ctx->encoder, AACENC_PEAK_BITRATE, target_bitrate_bps);
    }

    // Контейнер: ADTS-фреймы (2)
    g_setEncParam(ctx->encoder, AACENC_TRANSMUX, 2);

    // Режим "afterburner" (качественное кодирование), 1 = включить
    g_setEncParam(ctx->encoder, AACENC_AFTERBURNER, cfg.encoder.hq_mode);

    // Остальные параметры можно оставить по умолчанию (0 = auto)
    g_setEncParam(ctx->encoder, AACENC_BANDWIDTH, 0);
    g_setEncParam(ctx->encoder, AACENC_GRANULE_LENGTH, 1024);
    g_setEncParam(ctx->encoder, AACENC_PROTECTION, 0);
    g_setEncParam(ctx->encoder, AACENC_ANCILLARY_BITRATE, 0);
    g_setEncParam(ctx->encoder, AACENC_METADATA_MODE, 0);

    /* Холодный вызов encoder для остаточной инициализации
       --------------------------------------------------- */
    // Все указатели, размеры и флаги – нулевые
    memset(&in_args, 0, sizeof(in_args));
    memset(&out_args, 0, sizeof(out_args));
    memset(&in_buf_desc, 0, sizeof(in_buf_desc));
    memset(&out_buf_desc, 0, sizeof(out_buf_desc));

    // Вызов с NULL-структурами буферов
    rc = g_fdkEncode(ctx->encoder, &in_buf_desc, &out_buf_desc, &in_args, &out_args);
    if (rc != AACENC_OK) {
        fprintf(stderr, "ReConfig (NULL-encode) failed: error %d.\n", rc);
        goto fail_close_encoder;
    }

    /* Получить информацию о кодере
       ---------------------------- */
    // Параметры: frameLength, maxOutBufBytes
    if (g_fdkInfo(ctx->encoder, &info) != AACENC_OK) {
        fprintf(stderr, "Calling aacEncInfo() failed.\n");
        goto fail_close_encoder;
    }

    /* Юзер-инфо / для отладки:
       ------------------------ */
    fprintf(stderr, "- input channels: %d\n", ctx->src.channels);
    fprintf(stderr, "- input sample_rate: %d\n", ctx->src.sample_rate);
    fprintf(stderr, "- input format: %d\n", ctx->pcm_format);
    fprintf(stderr, "- output profile: %d [%d]\n", cfg.encoder.format, profile);
    fprintf(stderr, "- output channels: %d\n", ctx->channels);
    fprintf(stderr, "- output sample_rate: %d\n", ctx->sample_rate);
    fprintf(stderr, "- output bitrate: %d [%d]\n", cfg.encoder.bitrate, target_bitrate_bps);
    fprintf(stderr, "- output quality: %d\n", cfg.encoder.hq_mode);
    fprintf(stderr, "- frame length samples: %d\n", info.frameLength);
    fprintf(stderr, "- frame buffer bytes: %d\n", info.maxOutBufBytes);

    /* Выделить PCM буферы
       ------------------- */
    // Размер чанка в PCM фреймах (сэмплов на канал)
    // FDK-AAC требует frameLength сэмплов на канал за один вызов encode.
    // Поэтому буфер должен вмещать frameLength * channels сэмплов.
    ctx->pcm_max_frames = info.frameLength;  // 1024 для HE-AAC, 2048 для HE-AACv2

    // Выделим раздельные буферы для каналов (как в rx.c)
    ctx->pcm_buf[0] = (int32_t*)calloc(ctx->pcm_max_frames, sizeof(int32_t));
    ctx->pcm_buf[1] = (int32_t*)calloc(ctx->pcm_max_frames, sizeof(int32_t));
    if (!ctx->pcm_buf[0] || !ctx->pcm_buf[1]) {
        fprintf(stderr, "Out of memory for input PCM buffers.\n");
        goto fail_free_bufs;
    }

    /* Выделить выходной буфер для кодированных данных
       ----------------------------------------------- */
    // info.maxOutBufBytes – максимальный размер одного выходного фрейма.
    // Для запаса сделаем буфер в 2-4 раза больше.
    ctx->enc_buf_size = info.maxOutBufBytes * 2;
    ctx->enc_buf = (uint8_t*)calloc(2, info.maxOutBufBytes);
    if (!ctx->enc_buf) {
        fprintf(stderr, "Out of memory for output AAC buffer.\n");
        goto fail_free_bufs;
    }

    ctx->enc_buf_pos = 0;
    ctx->enc_buf_len = 0;
    ctx->enc_samples[0] = 0;
    ctx->total_bytes_encoded = 0;
    return 0;

    /* Метки для выхода (при ошибках)
       ------------------------------ */
    fail_free_bufs:
    if (ctx->pcm_buf[0]) free(ctx->pcm_buf[0]);
    if (ctx->pcm_buf[1]) free(ctx->pcm_buf[1]);

    fail_close_encoder:
    g_fdkClose(&ctx->encoder);

    fail_close_source:
    ctx->src.close(ctx->src.ctx);
    return -1;
}


/* Функция чтения и кодирования в AAC одного PCM-чанка
   --------------------------------------------------- */
static int aac_encoder_process(AudioEncCtx *ctx, AdtsReader *adts)
{
    int i = 0;
    uint32_t raw_frames = 0;
    AACENC_ERROR   rc;
    AACENC_InArgs  in_args;
    AACENC_OutArgs out_args;
    AACENC_BufDesc in_buf_desc, out_buf_desc;
    AACENC_BufferIdentifier in_buf_id;
    AACENC_BufferIdentifier out_buf_id;
    void *in_buf_ptr, *out_buf_ptr;
    int in_buf_size, in_buf_el_size;
    int out_buf_size, out_buf_el_size;

    memset(&in_buf_desc, 0, sizeof(in_buf_desc));
    memset(&out_buf_desc, 0, sizeof(out_buf_desc));
    memset(&in_args, 0, sizeof(in_args));
    memset(&out_args, 0, sizeof(out_args));

    /* 1. Прочитать PCM-чанк из источника */
    raw_frames = ctx->src.read_chunk(ctx->src.ctx, ctx->pcm_max_frames);
    if (raw_frames == 0) {
        adts->stream_eof = 1; return 0;
    }
    ctx->pcm_frames_read = raw_frames;
    if (raw_frames < ctx->pcm_max_frames) {
        // в реальном потоке такого быть не должно, но если пришло меньше – дополним тишиной
        memset(ctx->src.chunk_buf + raw_frames * ctx->src.channels * (ctx->pcm_format/8), 0,
              (ctx->pcm_max_frames - raw_frames) * ctx->src.channels * (ctx->pcm_format/8));
    }

    /* 2. Распаковать в int32_t (lbuf, rbuf) */
    ctx->unpack_samples(ctx->src.chunk_buf, ctx->pcm_buf[0], ctx->pcm_buf[1], ctx->pcm_max_frames);

    /* 3. Даунмикс до нужного числа каналов */
    ctx->downmix_process(ctx->pcm_buf[0], ctx->pcm_buf[1], ctx->pcm_max_frames);

    /* 4. Подготовить входной буфер для FDK-AAC (16-бит) */
    if (ctx->channels == 1) {
        for (i = 0; i < (int)ctx->pcm_max_frames; ++i) {
            ctx->enc_samples[i] = (int16_t)(ctx->pcm_buf[0][i] >> 16);
        }
    } else {
        for (i = 0; i < (int)ctx->pcm_max_frames; ++i) {
            ctx->enc_samples[i*2]   = (int16_t)(ctx->pcm_buf[0][i] >> 16);
            ctx->enc_samples[i*2+1] = (int16_t)(ctx->pcm_buf[1][i] >> 16);
        }
    }

    /* 5. Вызвать aacEncEncode */
    in_buf_id = IN_AUDIO_DATA;
    in_buf_ptr = ctx->enc_samples;
    in_buf_size = ctx->pcm_max_frames * ctx->channels * sizeof(int16_t);
    in_buf_el_size = sizeof(int16_t);
    in_buf_desc.numBufs = 1;
    in_buf_desc.bufs = &in_buf_ptr;
    in_buf_desc.bufferIdentifiers = &in_buf_id;
    in_buf_desc.bufSizes = &in_buf_size;
    in_buf_desc.bufElSizes = &in_buf_el_size;

    out_buf_id = OUT_BITSTREAM_DATA;
    out_buf_ptr = ctx->enc_buf;
    out_buf_size = (int)ctx->enc_buf_size;
    out_buf_el_size = 1;
    out_buf_desc.numBufs = 1;
    out_buf_desc.bufs = &out_buf_ptr;
    out_buf_desc.bufferIdentifiers = &out_buf_id;
    out_buf_desc.bufSizes = &out_buf_size;
    out_buf_desc.bufElSizes = &out_buf_el_size;

    in_args.numInSamples = ctx->pcm_max_frames * ctx->channels;
    in_args.numAncBytes = 0;

    rc = g_fdkEncode(ctx->encoder, &in_buf_desc, &out_buf_desc, &in_args, &out_args);
    if (rc != AACENC_OK) {
        fprintf(stderr, "\nAAC encoder: frame encode failed (error %d).\n");
        return -1;
    }

    /* 6. Сохранить результат в enc_buf */
    ctx->enc_buf_len = out_args.numOutBytes;
    ctx->total_bytes_encoded += ctx->enc_buf_len;
    return 0;
}


/* Функция чтения PCM и раскладки ADTS-фреймов
   ------------------------------------------- */
// TODO: переименовать в "tx_encode_adts"?
uint16_t tx_accumulate_encoded_adts(AudioEncCtx *aenc, AdtsReader *adts, uint8_t *out_buf, uint16_t max_bytes, uint32_t *total)
{
    uint16_t frame_len;
    uint16_t out_pos = 0;
    adts->stream_eof = 0;  // сбросим маркер конца

    while (out_pos < max_bytes) {
        /* Если буфер пуст, запросить новый ADTS-фрейм */
        if (aenc->enc_buf_len == 0) {
            if (aac_encoder_process(aenc, adts) != 0) {
                adts->stream_eof = 1; break;
            }
            if (adts->stream_eof) break;
        }

        /* Размер текущего ADTS-фрейма (прочитать из заголовка) */
        // Предположим, что enc_buf_len – это размер одного фрейма (норма для CBR).
        frame_len = (uint16_t)aenc->enc_buf_len;

        if (out_pos + frame_len <= max_bytes) {
            /* Фрейм влезает целиком – копируем его */
            memcpy(out_buf + out_pos, aenc->enc_buf, frame_len);
            out_pos += frame_len; aenc->enc_buf_len = 0;  // буфер полностью использован

            /* Обновить таймер (в мкс, как в tx_accumulate_adts) */
            adts->stream_time += (1000000 * 1024) / aenc->sample_rate;  // реальная частота для HE-AAC
        } else {
            /* Не влезает - оставляем всё как есть, ждём следующего вызова */
            break;
        }
    }

    *total += out_pos;
    return out_pos;
}


/* Освобождение памяти и контекста */
void aac_encoder_free(AudioEncCtx *ctx)
{
    if (ctx->encoder) {
        g_fdkClose(&ctx->encoder);
        ctx->encoder = NULL;
    }
    // освобождаем выделенные буферы
    if (ctx->pcm_buf[0]) free(ctx->pcm_buf[0]);
    if (ctx->pcm_buf[1]) free(ctx->pcm_buf[1]);
    if (ctx->enc_buf) free(ctx->enc_buf);

    // также закрываем PcmSource-источник
    if (ctx->src.ctx) ctx->src.close(ctx->src.ctx);

    // покажем, сколько AAC-байт записали
    fprintf(stderr, "AAC total encoded/written: %d bytes\n", 
       ctx->total_bytes_encoded);
}

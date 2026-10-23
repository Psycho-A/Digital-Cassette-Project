/* metadata.c */
/* -----------------------
 * Basic MetaData parsing stuff:
 * - GUI Interface utilities [Win32];
 * - Serialize/DeSerialize functions;
 * - Conversion/read/update helpers
 */
#include "metadata.h"


/* ------------------------------
   Общее / Интерфейс вывода (GUI)
   ------------------------------ */

/* Инициализация Win32-окна метаданных */
int metadata_window_init(MetadataCtx *meta, char *init_text)
{
    #ifdef _WIN32
    WNDCLASS wc = {0};
    UINT pos_x = 4, pos_y = 4;   // положение окна
    UINT size_x = 360, size_y = 200;  // размер в пикселях
    RECT rc = {0, 0, size_x, size_y};
    HINSTANCE hInst = GetModuleHandle(NULL);
    HWND hWnd, hEdit; HFONT hFont;

    // Регистрируем класс
    wc.lpfnWndProc   = DefWindowProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "DCMetaWindowClass";
    RegisterClass(&wc);

    // Вычислим размер окна по размеру шрифта
    size_y = (meta->ui_font_size * 12 * 117) / 100;  // 12 строк, интервал ~1.17
    size_x = (size_y * 178) / 100;  // оптимальные пропорции x/y: 1.78
    rc.right = size_x; rc.bottom = size_y;   // применим новые размеры к RECT

    // Создаём окно (невидимое, но с текстовым полем внутри)
    hWnd = CreateWindowEx(WS_EX_TOOLWINDOW, wc.lpszClassName, "Digital Cassette: Stream Info", 
        WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), pos_x, pos_y, // def.x/y: CW_USEDEFAULT
        rc.right-rc.left, rc.bottom-rc.top, NULL, NULL, hInst, NULL);

    if (!hWnd) return -1;

    // Вставляем дочерний EDIT control (read-only, multi-line)
    hEdit = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY, 
        0, 0, rc.right, rc.bottom, hWnd, (HMENU)101, hInst, NULL);

    if (!hEdit) { DestroyWindow(hWnd); return -1; }

    // Установим шрифт текста (из конфига, иначе моноширинный)
    AddFontResource(meta->ui_font_file); SendMessage(hEdit, WM_FONTCHANGE, 0, 0);
    hFont = CreateFont(meta->ui_font_size, 0, 0, 0, meta->ui_font_bold, FALSE, FALSE, FALSE, ANSI_CHARSET, 
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, meta->ui_font_name);

    SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Показываем окно
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // Сохраняем дескриптор родительского окна в контекст
    meta->ui_window_handle = (HANDLE)hWnd;
    meta->ui_window_active = 1;

    // Выводим стартовое сообщение (например, "Waiting...")
    write_metadata_fields(meta, init_text, NULL);
    return 0;
    #else

    // Неуспех на не-Windows системах (TODO)
    meta->ui_window_active = 0;
    return -1;
    #endif
}

/* Закрытие ресурсов Win32-окна (для RX) */
void metadata_window_free(MetadataCtx *meta)
{
    DestroyWindow(meta->ui_window_handle);
    RemoveFontResource(meta->ui_font_file); // .ttf-шрифт
    meta->ui_window_active = 0; meta->ui_window_handle = NULL;
}


/* Helper записи метаданных в UI-окно или файл */
inline void write_metadata_fields(MetadataCtx *meta, char *out_buf, FILE *out_fp)
{
    /* Пишем в файл, если окно неактивно */
    if (out_fp && !meta->ui_window_active) {
        fwrite(out_buf, 1, strlen(out_buf), out_fp);
    }
    #ifdef _WIN32
    /* Если активно UI-окно - выводим туда */
    if (meta->ui_window_active && meta->ui_window_handle != NULL) {
        MSG msg; HWND hEdit;
        hEdit = GetDlgItem(meta->ui_window_handle, 101);
        if (hEdit) {
            SetWindowText(hEdit, out_buf);  // отправить метаданные в окно
            SetFocus(meta->ui_window_handle);  // убрать мерцающий курсор
        }
        // опрашиваем и обновляем окно (нужно)...
        while (PeekMessage(&msg, meta->ui_window_handle, 0, 0, PM_REMOVE)) {
            if (msg.message != WM_CLOSE) {
                TranslateMessage(&msg); DispatchMessage(&msg);
            } else {
                // корректная реакция на закрытие (крестиком)
                DestroyWindow(meta->ui_window_handle); meta->ui_window_handle = NULL;
            }
        }
    }
    #endif
}

/* Helper вывода текста жанра по его индексу */
inline const char* get_genre_name(uint8_t genre_id)
{
    size_t i;
    for (i = 0; i < GENRE_COUNT; i++) {
        if (genre_table[i].id == genre_id) return genre_table[i].name;
    }
    return "Stream";  // не определён...
}

/* Helper форматирования числовых полей и записи в буфер */
inline void format_numeric_fields(MetadataCtx *meta, char *out_buf)
{
    char    line[512];   // буфер для одной строки
    int32_t h, m, s;     // конверсия времени: прокси для ч:мм:сс
    char    tm_buf[64];  // конверсия времени: запись из strftime

    /* Выводим числовые поля (кэшированные) */
    if (meta->track_number >= 0 && meta->total_tracks <= 0) {
        sprintf(line, "Track:        %u\r\n", meta->track_number); strcat(out_buf, line);
    }
    if (meta->track_number >= 0 && meta->total_tracks >= 1) {
        sprintf(line, "Track:        %u / %u\r\n", meta->track_number, meta->total_tracks); strcat(out_buf, line);
    }
    if (meta->time_stamp >= 0) {
        s = meta->time_stamp; h = s / 3600; m = (s % 3600) / 60; s = s % 60;  // s -> H:M:S
        sprintf(line, "Time:         %d:%02d:%02d\r\n", h,m,s); strcat(out_buf, line);
    }
    if (meta->duration >= 0) {
        s = meta->duration; h = s / 3600; m = (s % 3600) / 60; s = s % 60;  // s -> H:M:S
        sprintf(line, "Duration:     %d:%02d:%02d\r\n", h,m,s); strcat(out_buf, line);
    }
    if (meta->record_time > 0) {
        strftime(tm_buf, sizeof(tm_buf), "%d/%m/%Y %H:%M:%S", localtime(&meta->record_time));  // format
        sprintf(line, "Record time:  %s\r\n", tm_buf); strcat(out_buf, line);
    }
    if (meta->year_id >= 0) {
        sprintf(line, "Album year:   %u\r\n", 1900 + meta->year_id); strcat(out_buf, line);
    }
    if (meta->genre_id >= 0) {
        sprintf(line, "Genre:        %s\r\n", get_genre_name((uint8_t)meta->genre_id)); strcat(out_buf, line);
    }
}




/* ----------------------
   Запись метаданных (TX)
   ---------------------- */

/* Упаковка метаданных в хвост payload_buf:
   in:  payload_buf  – полный буфер (содержит аудиоданные в начале)
        meta         – метаданные (time_stamp обновится внутри)
        st           – StreamState для получения ADTS-таймера
        desc         – FrameDesc с payload_used и payload_extra
   out: количество записанных байт метаданных (0, если хвоста нет)
   NB:  записывает данные начиная с позиции desc->hdr.payload_used,
        заполняет оставшееся место нулями;
   inf: для совместимости с Win32-окном требует переносов по \r\n
*/
void serialize_metadata(FrameDesc *desc, MetadataCtx *meta, uint32_t stream_time, uint8_t *buf)
{
    uint16_t audio_len = desc->hdr.payload_used;
    uint16_t extra_len = desc->payload_extra;
    uint16_t gap_bytes = extra_len > audio_len ? extra_len - audio_len : 0;
    uint16_t pos = 0, left = gap_bytes, copy_len;

    if (gap_bytes <= 0) return;

    /* Вспомогательные макросы для записи (запись в LE) */
    #define WRITE_BYTE(val) do { \
        if (left >= 1) { \
            buf[audio_len +pos] = (uint8_t)(val); \
            pos++; left--; \
        } \
    } while(0)

    #define WRITE_U16(val) do { \
        if (left >= 2) { \
            buf[audio_len +pos]   = (uint8_t)((val) & 0xFF); \
            buf[audio_len +pos+1] = (uint8_t)(((val) >> 8) & 0xFF); \
            pos += 2; left -= 2; \
        } \
    } while(0)

    #define WRITE_U32(val) do { \
        if (left >= 4) { \
            buf[audio_len +pos]   = (uint8_t)((val) & 0xFF); \
            buf[audio_len +pos+1] = (uint8_t)(((val) >> 8) & 0xFF); \
            buf[audio_len +pos+2] = (uint8_t)(((val) >> 16) & 0xFF); \
            buf[audio_len +pos+3] = (uint8_t)(((val) >> 24) & 0xFF); \
            pos += 4; left -= 4; \
        } \
    } while(0)

    #define WRITE_TEXT(field_len, field_text) do { \
        if (left >= 3) { \
            WRITE_BYTE(field_len); \
            copy_len = field_len; \
            if (copy_len > left) copy_len = left; \
            if (copy_len > 0) { \
                memcpy(buf + audio_len + pos, field_text, copy_len); \
                pos += copy_len; left -= copy_len; \
            } \
        } \
    } while(0)

    /* Обновим метку времени (из ADTS или разностью) */
    meta->time_stamp = (stream_time > 0) ?
        (uint16_t)(stream_time / 1000000) : // мкс
        (uint16_t)(time(NULL) - meta->record_time);

    /* Основные числовые поля (в порядке приоритета) */
    WRITE_BYTE(meta->track_number);
    WRITE_BYTE(meta->total_tracks);
    WRITE_U16(meta->time_stamp);
    WRITE_U16(meta->duration);
    WRITE_U32(meta->record_time);
    WRITE_BYTE(meta->year_id);
    WRITE_BYTE(meta->genre_id);

    /* Текстовые поля (требуют мин. по 3 байта: длина + 2 знака) */
    WRITE_TEXT(meta->artist_len, meta->artist_text);
    WRITE_TEXT(meta->title_len, meta->title_text);
    WRITE_TEXT(meta->album_len, meta->album_text);
    WRITE_TEXT(meta->comment_len, meta->comment_text);

    /* Заполняем остаток нулями (уже есть, но для надёжности) */
    if (left > 0) {
        memset(buf + audio_len + pos, 0, left);
        pos += left; left = 0;
    }

    /* Опционально: выводим метаданные в UI-окно
       ----------------------------------------- */
    if (meta->ui_window_active) {
        char  line[512]     = {0};
        char  out_buf[4096] = {0};
        FILE* out_fp = NULL;  // заглушка-блок

        // заголовок-разделитель
        sprintf(line, "RECORDING STREAM...\r\n===================\r\n"); strcat(out_buf, line);

        // числовые поля (с форматированием)
        format_numeric_fields(meta, out_buf);

        // текстовые поля (без _len обрезки)
        sprintf(line, "Artist:       %s\r\n", meta->artist_text); strcat(out_buf, line);
        sprintf(line, "Title:        %s\r\n", meta->title_text); strcat(out_buf, line);
        sprintf(line, "Album:        %s\r\n", meta->album_text); strcat(out_buf, line);
        sprintf(line, "Comment:      %s\r\n", meta->comment_text); strcat(out_buf, line);

        // вывод собранного текста в UI-окно
        write_metadata_fields(meta, out_buf, out_fp);
    }
}


/* Нормализация строки: нижний регистр, только буквы */
inline void normalize_genre_name(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0; char c;
    while (src[i] && j < dst_size -1) {
        c = (char)tolower((unsigned char)src[i]);
        if (c >= 'a' && c <= 'z') dst[j++] = c;
        i++;  // к следующему символу
    }
    dst[j] = '\0';
}

/* Поиск индекса жанра по неточной строке пользователя */
uint8_t find_genre_id(const char *user_input)
{
    uint8_t best_id = 12;  // "Other"
    size_t i; char query[128], table_name[128];
    if (!user_input || !*user_input) return best_id;

    /* Удаляем всё, кроме латинских букв */
    normalize_genre_name(user_input, query, sizeof(query));
    if (!*query) return best_id;

    /* Поиск точного совпадения (все слова) */
    for (i = 0; i < GENRE_COUNT; i++) {
        normalize_genre_name(genre_table[i].name, table_name, sizeof(table_name));
        if (strcmp(query, table_name) == 0) return genre_table[i].id;
    }
    /* Запрос содержит табличную строку как подстроку ("Folk-Rock" -> "Folk") */
    for (i = 0; i < GENRE_COUNT; i++) {
        normalize_genre_name(genre_table[i].name, table_name, sizeof(table_name));
        if (strstr(query, table_name) != NULL) return genre_table[i].id;
    }
    /* Табличная строка содержит запрос как подстроку ("Folk" -> "Folk-Rock") */
    for (i = 0; i < GENRE_COUNT; i++) {
        normalize_genre_name(genre_table[i].name, table_name, sizeof(table_name));
        if (strstr(table_name, query) != NULL) return genre_table[i].id;
    }

    return best_id;  // не определён...
}




/* ----------------------
   Чтение метаданных (RX)
   ---------------------- */

/* Чтение метаданных из хвоста payload_buf и вывод в файл:
   in:  desc         - FrameDesc с payload_used и payload_extra
        payload_buf  - полный буфер фрейма
        out_fp       - файл для вывода (например, rctx->inf_fp)
   out: количество прочитанных байт метаданных (0, если хвоста нет)
   NB:  читает поля в том же порядке, что и serialize_metadata,
        пропускает невлезшие поля, выводит с читаемыми префиксами
   inf: для совместимости с Win32-окном требует переносов по \r\n
*/
void deserialize_metadata(FrameDesc *desc, MetadataCtx *cache, FILE *out_fp, uint8_t *payload_buf)
{
    uint16_t audio_len = desc->hdr.payload_used;
    uint16_t extra_len = desc->payload_extra;
    uint16_t gap_bytes = extra_len > audio_len ? extra_len - audio_len : 0;
    uint16_t pos = 0, left = gap_bytes;
    uint8_t *buf = payload_buf + audio_len;

    char     out_buf[4096] = {0};
    int32_t  track_number = -1, total_tracks = -1;
    int32_t  year_id = -1, genre_id = -1;
    int32_t  time_stamp = -1, duration = -1;
    uint32_t record_time = 0;

    if (gap_bytes <= 0) return;

    /* Вспомогательные макросы чтения (зеркально TX) */
    #define READ_BYTE(dst) do { \
        if (left >= 1) { \
            dst = buf[pos]; \
            pos += 1; left -= 1; \
        } \
    } while(0)

    #define READ_U16(dst) do { \
        if (left >= 2) { \
            dst = buf[pos] | ((uint16_t)buf[pos+1] << 8); \
            pos += 2; left -= 2; \
        } \
    } while(0)

    #define READ_U32(dst) do { \
        if (left >= 4) { \
            dst = buf[pos] | ((uint32_t)buf[pos+1] << 8) | ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24); \
            pos += 4; left -= 4; \
        } \
    } while(0)

    /* Заголовок-разделитель */
    if (cache->ui_window_active) 
    sprintf(out_buf, "PLAYING STREAM...\r\n=================\r\n");
    else sprintf(out_buf, "\r\n# GAP metadata:\r\n");

    /* Числовые поля в том же порядке */
    READ_BYTE(track_number);
    READ_BYTE(total_tracks);
    READ_U16(time_stamp);
    READ_U16(duration);
    READ_U32(record_time);
    READ_BYTE(year_id);
    READ_BYTE(genre_id);

    /* Обновим кэш, если значения получены */
    if (track_number >= 0) cache->track_number = track_number;
    if (total_tracks >= 0) cache->total_tracks = total_tracks;
    if (time_stamp >= 0)   cache->time_stamp = time_stamp;
    if (duration >= 0)     cache->duration = duration;
    if (record_time > 0)   cache->record_time = record_time;
    if (year_id >= 0)      cache->year_id = year_id;
    if (genre_id >= 0)     cache->genre_id = genre_id;

    /* Выводим числовые поля (из кэша) */
    format_numeric_fields(cache, out_buf);  // проверка и формат

    /* Текстовые поля: мин. по 3 байта (длина + 2 символа) */
    read_text_field(out_buf, "Artist:      ", &cache->artist_len,  cache->artist_text,  buf, &pos, &left);
    read_text_field(out_buf, "Title:       ", &cache->title_len,   cache->title_text,   buf, &pos, &left);
    read_text_field(out_buf, "Album:       ", &cache->album_len,   cache->album_text,   buf, &pos, &left);
    read_text_field(out_buf, "Comment:     ", &cache->comment_len, cache->comment_text, buf, &pos, &left);

    /* Вывод собранного текста в файл или UI-окно */
    write_metadata_fields(cache, out_buf, out_fp);
}


/* Helper чтения и кэширования текстовых полей */
inline void read_text_field(char *out_buf, char *label, uint8_t *cache_len, char *cache_text, uint8_t *buf, uint16_t *pos, uint16_t *left)
{
    char line[512] = {0};
    uint16_t field_len = 0;
    uint16_t copy_len = 0;

    /* Читаем маркер полной длины */
    if (*left >= 3) {  // длина + 2 символа
        field_len = buf[(*pos)++]; (*left)--;
        copy_len = field_len;
        if (copy_len > *left) copy_len = *left;
        if (copy_len > 0) {
            memcpy(line, buf + *pos, copy_len);
            *pos += copy_len; *left -= copy_len;
        }
        line[copy_len] = '\0';

        if (field_len >= 2) {
            /* обновляем кэш, только если это имеет смысл: */
            if (field_len >= *cache_len && copy_len >= strlen(cache_text)    // новая строка полнее
            || (!strstr(line, cache_text) && !strstr(cache_text, line)) ) {  // текст строки изменился
                *cache_len = (uint8_t)field_len; strcpy(cache_text, line);
            }
        }
    }
    /* Выводим из кэша с многоточием, если поле ещё не полное */
    if (*cache_len >= 2) {
        sprintf(line, "%s %s%s\r\n", label, cache_text, strlen(cache_text) < *cache_len ? "..." : "");
        strcat(out_buf, line);
    }
}

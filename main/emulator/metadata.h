/* metadata.h */
/* -----------------------
 * Basic MetaData parsing stuff:
 * - Cache and config structures;
 * - De/Serialize/GUI forwards;
 * - Genre ID recognition tables
 */
#ifndef METADATA_H
#define METADATA_H
#include <ctype.h>
#include <string.h>
#include <time.h>
#include "frame.h"
#include "config.h"

#ifdef   _WIN32
#include <windows.h>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#endif


/* Контекст обработки (общий) */
typedef struct {
    // общие настройки:
    int insert_gaps;
    int insert_frames;
    int max_frames;

    // gap-версия (мини-поля):
    int32_t track_number;  // номер трека
    int32_t total_tracks;  // треков в альбоме
    int32_t time_stamp;    // текущее время (сек)
    int32_t duration;      // общее время (сек)
    time_t  record_time;   // время старта записи
    int32_t year_id;       // год: 1900 + ID_num
    int32_t genre_id;      // жанр: табличный ID (0-255)

    // gap-версия (текстовые поля):
    uint8_t artist_len, artist_text[260];    // длина и текст поля "исполнитель"
    uint8_t title_len, title_text[260];      // длина и текст поля "трек"
    uint8_t album_len, album_text[260];      // длина и текст поля "альбом"
    uint8_t comment_len, comment_text[260];  // длина и текст поля "комментарий"

    // вывод в консольное UI-окно:
    int     ui_window_active;  // флаг активности окна
    #ifdef _WIN32
    HANDLE  ui_window_handle;  // дескриптор окна (win32)
    char*   ui_font_file;      // путь к пользовательскому ttf-файлу шрифта
    char*   ui_font_name;      // имя гарнитуры шрифта (ttf или системного)
    int32_t ui_font_size;      // размер шрифта в окне (ttf или системного)
    int32_t ui_font_bold;      // жирность шрифта (диапазон: 0-900)
    #endif

} MetadataCtx;

/* Структура таблицы жанров (ниже) */
typedef struct {
    uint8_t id;
    const char *name;

} GenreEntry;


/* Запись или вывод в GUI-окно */
int  metadata_window_init(MetadataCtx *meta, char *init_text);    // создать окно для показа метаданных
void metadata_window_free(MetadataCtx *meta);    // закрыть окно и освободить ресурсы

/* Общие функции записи и чтения */
void serialize_metadata(FrameDesc *desc, MetadataCtx *meta, uint32_t stream_time, uint8_t *buf);    // TX
void deserialize_metadata(FrameDesc *desc, MetadataCtx *cache, FILE *out, uint8_t *payload_buf);    // RX

/* Вспомогательные утилиты */
const char* get_genre_name(uint8_t genre_id);    // поиск имени жанра по табличному ID genre_table
void format_numeric_fields(MetadataCtx *meta, char *out_buf);    // проверка и форматирование числовых полей
void write_metadata_fields(MetadataCtx *meta, char *out_buf, FILE *out_fp);    // запись метаданных в файл или GUI-окно
uint8_t find_genre_id(const char *user_input);    // TX: нестрогий поиск genre_id по тексту из конфига
void normalize_genre_name(const char *src, char *dst, size_t dst_size);    // TX: очистка строки жанра от мусора
void read_text_field(char*, char*, uint8_t*, char*, uint8_t*, uint16_t*, uint16_t*);    // RX: чтение текстовых полей и кэша


/* Вычисление крайнего ID жанров */
#define GENRE_COUNT (sizeof(genre_table) / sizeof(GenreEntry))


/* Таблица ID жанров (на базе ID3v1)
   --------------------------------- */
static const GenreEntry genre_table[] = {
//  стандартные теги
    { 0, "Blues" },
    { 1, "Classic Rock" },
    { 2, "Country" },
    { 3, "Dance" },
    { 4, "Disco" },
    { 5, "Funk" },
    { 6, "Grunge" },
    { 7, "Hip-Hop" },
    { 8, "Jazz" },
    { 9, "Metal" },
    { 10, "New Age" },
    { 11, "Oldies" },
    { 12, "Other" },
    { 13, "Pop" },
    { 14, "R&B" },
    { 15, "Rap" },
    { 16, "Reggae" },
    { 17, "Rock" },
    { 18, "Techno" },
    { 19, "Industrial" },
    { 20, "Alternative" },
    { 21, "Ska" },
    { 22, "Death Metal" },
    { 23, "Pranks" },
    { 24, "Soundtrack" },
    { 25, "Euro-Techno" },
    { 26, "Ambient" },
    { 27, "Trip-Hop" },
    { 28, "Vocal" },
    { 29, "Jazz+Funk" },
    { 30, "Fusion" },
    { 31, "Trance" },
    { 32, "Classical" },
    { 33, "Instrumental" },
    { 34, "Acid" },
    { 35, "House" },
    { 36, "Game" },
    { 37, "Sound Clip" },
    { 38, "Gospel" },
    { 39, "Noise" },
    { 40, "Alt. Rock" },
    { 41, "Bass" },
    { 42, "Soul" },
    { 43, "Punk" },
    { 44, "Space" },
    { 45, "Meditative" },
    { 46, "Instrumental Pop" },
    { 47, "Instrumental Rock" },
    { 48, "Ethnic" },
    { 49, "Gothic" },
    { 50, "Darkwave" },
    { 51, "Techno-Industrial" },
    { 52, "Electronic" },
    { 53, "Pop-Folk" },
    { 54, "Eurodance" },
    { 55, "Dream" },
    { 56, "Southern Rock" },
    { 57, "Comedy" },
    { 58, "Cult" },
    { 59, "Gangsta Rap" },
    { 60, "Top 40" },
    { 61, "Christian Rap" },
    { 62, "Pop/Funk" },
    { 63, "Jungle" },
    { 64, "Native American" },
    { 65, "Cabaret" },
    { 66, "New Wave" },
    { 67, "Psychedelic" },
    { 68, "Rave" },
    { 69, "Showtunes" },
    { 70, "Trailer" },
    { 71, "Lo-Fi" },
    { 72, "Tribal" },
    { 73, "Acid Punk" },
    { 74, "Acid Jazz" },
    { 75, "Polka" },
    { 76, "Retro" },
    { 77, "Musical" },
    { 78, "Rock & Roll" },
    { 79, "Hard Rock" },
//  расширенные теги (WinAmp 5)
    { 80, "Folk" },
    { 81, "Folk-Rock" },
    { 82, "National Folk" },
    { 83, "Swing" },
    { 84, "Fast-Fusion" },
    { 85, "Bebop" },
    { 86, "Latin" },
    { 87, "Revival" },
    { 88, "Celtic" },
    { 89, "Bluegrass" },
    { 90, "Avantgarde" },
    { 91, "Gothic Rock" },
    { 92, "Progressive Rock" },
    { 93, "Psychedelic Rock" },
    { 94, "Symphonic Rock" },
    { 95, "Slow Rock" },
    { 96, "Big Band" },
    { 97, "Chorus" },
    { 98, "Easy Listening" },
    { 99, "Acoustic" },
    { 100, "Humour" },
    { 101, "Speech" },
    { 102, "Chanson" },
    { 103, "Opera" },
    { 104, "Chamber Music" },
    { 105, "Sonata" },
    { 106, "Symphony" },
    { 107, "Booty Bass" },
    { 108, "Primus" },
    { 109, "Porn Groove" },
    { 110, "Satire" },
    { 111, "Slow Jam" },
    { 112, "Club" },
    { 113, "Tango" },
    { 114, "Samba" },
    { 115, "Folklore" },
    { 116, "Ballad" },
    { 117, "Power Ballad" },
    { 118, "Rhythmic Soul" },
    { 119, "Freestyle" },
    { 120, "Duet" },
    { 121, "Punk Rock" },
    { 122, "Drum Solo" },
    { 123, "A Cappella" },
    { 124, "Euro-House" },
    { 125, "Dance Hall" },
    { 126, "Goa Music" },
    { 127, "Drum & Bass" },
    { 128, "Club-House" },
    { 129, "Hardcore" },
    { 130, "Terror" },
    { 131, "Indie" },
    { 132, "BritPop" },
    { 133, "Noise Pop" },    // #
    { 134, "Noise Rock" },   // #
    { 135, "Beat" },
    { 136, "Spoken Word" },  // #
    { 137, "Heavy Metal" },
    { 138, "Black Metal" },
    { 139, "Crossover" },
    { 140, "Martial" },      // #
    { 141, "Christian Rock" },
    { 142, "Merengue" },
    { 143, "Salsa" },
    { 144, "Trash Metal" },
    { 145, "Anime" },
    { 146, "JPop" },
    { 147, "Synthpop" },
//  расширенные теги (WinAmp 6)
    { 148, "Abstract" },
    { 149, "Art Rock" },
    { 150, "Baroque" },
    { 151, "Bossa Nova" },   // #
    { 152, "Big Beat" },
    { 153, "Breakbeat" },
    { 154, "Chillout" },
    { 155, "Downtempo" },
    { 156, "Dub" },
    { 157, "EBM" },
    { 158, "Eclectic" },
    { 159, "Electro" },
    { 160, "Electroclash" },
    { 161, "Emo" },
    { 162, "Experimental" },
    { 163, "Garage" },
    { 164, "Global" },
    { 165, "IDM" },
    { 166, "Illbient" },
    { 167, "Industro-Goth" },
    { 168, "Jam Band" },
    { 169, "Krautrock" },
    { 170, "Leftfield" },
    { 171, "Lounge" },
    { 172, "Math Rock" },
    { 173, "New Romantic" },
    { 174, "Juke" },         // #
    { 175, "Post-Punk" },
    { 176, "Post-Rock" },
    { 177, "Psytrance" },
    { 178, "Shoegaze" },
    { 179, "Space Rock" },
    { 180, "Trop Rock" },
    { 181, "World Music" },
    { 182, "Neoclassical" },
    { 183, "Audiobook" },
    { 184, "Audio Theatre" },
    { 185, "Neue Deutsche Welle" },
    { 186, "Podcast" },
    { 187, "Indie Rock" },
    { 188, "Minimal" },      // #
    { 189, "Dubstep" },
    { 190, "Garage Rock" },
    { 191, "Psybient" },
//  кастомные теги (из фонотеки)
    { 192, "Afrobeat" },
    { 193, "Alt. Country" },
    { 194, "Ambient Techno" },
    { 195, "Art Punk" },
    { 196, "Atmospheric" },
    { 197, "Baroque Pop" },
    { 198, "Blackgaze" },
    { 199, "Blues Rock" },
    { 200, "Chamber Pop" },
    { 201, "Coldwave" },
    { 202, "Contemporary Classical" },
    { 203, "Country-Blues" },
    { 204, "Covers" },
    { 205, "Dark Ambient" },
    { 206, "Dark Cabaret" },
    { 207, "Dark Jazz" },
    { 208, "Doom Metal" },
    { 209, "Dream Pop" },
    { 210, "Dungeon Synth" },
    { 211, "Folk Metal" },
    { 212, "Freak Folk" },
    { 213, "Glam Rock" },
    { 214, "Glitch" },
    { 215, "Hardcore Punk" },
    { 216, "Horror Punk" },
    { 217, "Indie Folk" },
    { 218, "Indie Pop" },
    { 219, "Indietronica" },
    { 220, "Industrial Metal" },
    { 221, "Jangle Pop" },
    { 222, "Live Concert" },
    { 223, "Melancholic" },
    { 224, "Minimal Wave" },
    { 225, "Neo-Psychedelia" },
    { 226, "Neofolk" },
    { 227, "No Wave" },
    { 228, "Piano" },
    { 229, "Post-Black Metal" },
    { 230, "Post-Industrial" },
    { 231, "Power Electronics" },
    { 232, "Psychedelic Folk" },
    { 233, "Psychobilly" },
    { 234, "Radio" },
    { 235, "Retrowave" },
    { 236, "Rockabilly" },
    { 237, "Romantic" },
    { 238, "Rumba" },
    { 239, "Russian Music" },
    { 240, "Russian Rock" },
    { 241, "Slowcore" },
    { 242, "Soft Rock" },
    { 243, "Stoner Rock" },
    { 244, "Surf Rock" },
    { 245, "Symphonic Metal" },
    { 246, "Thrash Metal" },
    { 247, "Vaporwave" },
    { 248, "Viking Metal" },
    { 249, "Witch House" },
//  служебные теги (тесты, микрофон)
    { 250, "Music" },
    { 251, "Stream" },
    { 252, "Recording" },
    { 253, "Test Stream" },
    { 254, "Test Record" },
    { 255, "Unknown" },
};


#endif
/* -----------------------------------
 * Классы и типы данных payload фрейма
 * -----------------------------------
 * Основная терминология:
 * - PAYLOAD_ID : общий класс данных (DATA, AUDIO...)
 * - CLASS_TYPE : формат или контейнер класса (0-9)
 * 
 * Запись в $desc->hdr.payload_class:
 * - Сумма "класс (0-70) + подтип (0-9)", пример:
 * - desc->hdr.payload_class = PAYLOAD_AUDIO + AUDIO_AAC;
 * 
 * Чтение из $desc->hdr.payload_class:
 * - проверка: (desc->hdr.payload_class == PAYLOAD_AUDIO + AUDIO_AAC)
 * - корневой класс: (desc->hdr.payload_class/10 * 10 == PAYLOAD_TOC)
 */
#ifndef FRAME_TYPES_H
#define FRAME_TYPES_H

/* Классы данных
   ------------- */
#define PAYLOAD_DATA       00
#define PAYLOAD_AUDIO      10
#define PAYLOAD_IMAGE      20
#define PAYLOAD_VIDEO      30
#define PAYLOAD_TOC        40
#define PAYLOAD_META       50
#define PAYLOAD_EOF        60
#define PAYLOAD_CALIB      70


/* Форматы классов
   --------------- */

/* PAYLOAD_DATA */
#define DATA_GENERIC       0
#define DATA_COMPRESSED    1

/* PAYLOAD_AUDIO */
#define AUDIO_AAC          0
#define AUDIO_OPUS         1
#define AUDIO_OGG          2
#define AUDIO_MP3          3
#define AUDIO_MP4          4
#define AUDIO_MPC          5
#define AUDIO_PCM          6

/* PAYLOAD_IMAGE */
#define IMAGE_PNG          0
#define IMAGE_JPEG         1
#define IMAGE_BMP          2
#define IMAGE_GIF          3
#define IMAGE_WEBP         4
#define IMAGE_ICO          5
#define IMAGE_TGA          6

/* PAYLOAD_VIDEO */
#define VIDEO_TS           0
#define VIDEO_MP4          1
#define VIDEO_AVI          2
#define VIDEO_MKV          3
#define VIDEO_WEBM         5
#define VIDEO_MPG          4
#define VIDEO_WMV          6

/* PAYLOAD_META */
#define META_GENERIC       0
#define META_FILE          1
#define META_AUDIO         2
#define META_ID3BLOB       3
#define META_LYRICS        4
#define META_IMAGE         5

/* PAYLOAD_TOC */
#define TOC_ROOT           0
#define TOC_FILE           1

/* PAYLOAD_EOF */
#define EOF_ROOT           0
#define EOF_FILE           1

/* PAYLOAD_CALIB */
#define CALIB_GENERIC      0
#define CALIB_LEVEL        1
#define CALIB_FREQUENCY    2
#define CALIB_RESPONSE     3


#endif
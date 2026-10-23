/* faad2_dll.h */
/* ------------------
 * AAC-декодер FAAD2:
 * Указатели на базовые функции
 */
#ifndef FAAD2_DLL_H
#define FAAD2_DLL_H
#include <windows.h>

/* Форматы выходных сэмплов */
enum {
    FAAD_FMT_16BIT = 1,
    FAAD_FMT_24BIT,
    FAAD_FMT_32BIT,
    FAAD_FMT_FLOAT,
    FAAD_FMT_DOUBLE
};

/* Тип для хэндла декодера (непрозрачный указатель) */
typedef void* NeAACDecHandle;

/* Структура информации о декодированном фрейме (только нужные поля) */
typedef struct {
    unsigned long bytesconsumed;      // количество байт, прочитанных из входного буфера
    unsigned long samples;            // количество сэмплов на канал (длина одного канала)
    unsigned char channels;           // количество каналов (1 или 2)
    unsigned char error;              // код ошибки (0 = OK)
    unsigned long samplerate;         // частота дискретизации
    unsigned char sbr, ps;            // остальные поля не используем
    unsigned char object_type;
    unsigned char header_type;
    unsigned char num_front_channels;
    unsigned char num_side_channels;
    unsigned char num_back_channels;
    unsigned char num_lfe_channels;
    unsigned char channel_position[64];
} NeAACDecFrameInfo;

/* Структура конфигурации (только нужное поле) */
typedef struct {
    int defSampleFormat;  // выходной формат сэмплов
    unsigned char defObjectType;  // другие поля не изменяем!
    unsigned long defSampleRate;
    unsigned char outputFormat;
    unsigned char downMatrix;
    unsigned char useOldADTSFormat;
    unsigned char dontUpSampleImplicitSBR;
} NeAACDecConfiguration, *NeAACDecConfigurationPtr;

/* Указатели на функции из DLL */
typedef NeAACDecHandle (__cdecl *LPFAAD_OPEN)(void);
typedef void  (__cdecl *LPFAAD_CLOSE)(NeAACDecHandle);  // не использовать WINAPI, __cdecl можно опустить
typedef long  (__cdecl *LPFAAD_INIT)(NeAACDecHandle, unsigned char*, unsigned long, unsigned long*, unsigned char*);
typedef void* (__cdecl *LPFAAD_DECODE)(NeAACDecHandle, NeAACDecFrameInfo*, unsigned char*, unsigned long);

/* Указатели на пред-настройку (не используем) */
typedef void* (__cdecl *LPFAAD_GETCONFIG)(NeAACDecHandle);
typedef void  (__cdecl *LPFAAD_SETCONFIG)(NeAACDecHandle, void*);

/* Глобальные переменные (объявить в одном .c) */
extern HMODULE       g_hFaad;
extern LPFAAD_OPEN   g_faadOpen;
extern LPFAAD_CLOSE  g_faadClose;
extern LPFAAD_INIT   g_faadInit;
extern LPFAAD_DECODE g_faadDecode;

extern LPFAAD_GETCONFIG g_faadGetConfig;
extern LPFAAD_SETCONFIG g_faadSetConfig;

/* Объявление функций управления DLL */
int load_faad2_dll(void);  // загрузка DLL
int free_faad2_dll(void);  // выгрузка DLL


#endif
/* faad2_dll.c */
/* ------------------
 * AAC-декодер FAAD2:
 * Базовые функции для работы
 */
#include "faad2_dll.h"

/* Определение глобальных указателей */
HMODULE       g_hFaad = NULL;
LPFAAD_OPEN   g_faadOpen = NULL;
LPFAAD_CLOSE  g_faadClose = NULL;
LPFAAD_INIT   g_faadInit = NULL;
LPFAAD_DECODE g_faadDecode = NULL;

LPFAAD_GETCONFIG g_faadGetConfig = NULL;
LPFAAD_SETCONFIG g_faadSetConfig = NULL;

int load_faad2_dll(void)
{
    if (g_hFaad) return 0;  // уже загружена

    g_hFaad = LoadLibrary("resources\\libfaad2.dll");  // путь к DLL
    if (!g_hFaad) return -1;

    g_faadOpen   = (LPFAAD_OPEN)GetProcAddress(g_hFaad, "NeAACDecOpen");
    g_faadClose  = (LPFAAD_CLOSE)GetProcAddress(g_hFaad, "NeAACDecClose");
    g_faadInit   = (LPFAAD_INIT)GetProcAddress(g_hFaad, "NeAACDecInit");
    g_faadDecode = (LPFAAD_DECODE)GetProcAddress(g_hFaad, "NeAACDecDecode");

    g_faadGetConfig = (LPFAAD_GETCONFIG)GetProcAddress(g_hFaad, "NeAACDecGetCurrentConfiguration");
    g_faadSetConfig = (LPFAAD_SETCONFIG)GetProcAddress(g_hFaad, "NeAACDecSetConfiguration");

    if (!g_faadOpen || !g_faadClose || !g_faadInit || !g_faadDecode) {
        FreeLibrary(g_hFaad);
        g_hFaad = NULL; return -1;
    }
    return 0;
}

int free_faad2_dll(void)
{
    if (g_hFaad) {
        FreeLibrary(g_hFaad);
        g_hFaad = NULL;
    }
    // ошибка выгрузки
    if (g_hFaad) return -1;
    return 0;
}

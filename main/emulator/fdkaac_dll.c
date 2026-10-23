/* fdkaac_dll.c */
/* ------------------
 * AAC-кодер FDK-AAC:
 * Базовые функции для работы
 */
#include "fdkaac_dll.h"

HMODULE           g_hFdkAac = NULL;
LPFDK_OPEN        g_fdkOpen = NULL;
LPFDK_SET_PARAM   g_fdkSetParam = NULL;
LPFDK_GET_PARAM   g_fdkGetParam = NULL;
LPFDK_INFO        g_fdkInfo = NULL;
LPFDK_ENCODE      g_fdkEncode = NULL;
LPFDK_CLOSE       g_fdkClose = NULL;

int load_fdkaac_dll(void)
{
    if (g_hFdkAac) return 0;  // уже загружена

    g_hFdkAac = LoadLibrary("resources\\libfdkaac.dll");  // или fdkaac.dll
    if (!g_hFdkAac) return -1;

    g_fdkOpen     = (LPFDK_OPEN)GetProcAddress(g_hFdkAac, "aacEncOpen");
    g_fdkSetParam = (LPFDK_SET_PARAM)GetProcAddress(g_hFdkAac, "aacEncoder_SetParam");
    g_fdkGetParam = (LPFDK_GET_PARAM)GetProcAddress(g_hFdkAac, "aacEncoder_GetParam");
    g_fdkInfo     = (LPFDK_INFO)GetProcAddress(g_hFdkAac, "aacEncInfo");
    g_fdkEncode   = (LPFDK_ENCODE)GetProcAddress(g_hFdkAac, "aacEncEncode");
    g_fdkClose    = (LPFDK_CLOSE)GetProcAddress(g_hFdkAac, "aacEncClose");

    if (!g_fdkOpen || !g_fdkClose || !g_fdkSetParam || !g_fdkGetParam || !g_fdkEncode || !g_fdkInfo) {
        FreeLibrary(g_hFdkAac);
        g_hFdkAac = NULL; return -1;
    }
    return 0;
}

int free_fdkaac_dll(void)
{
    if (g_hFdkAac) {
        FreeLibrary(g_hFdkAac);
        g_hFdkAac = NULL;
    }
    // ошибка выгрузки
    if (g_hFdkAac) return -1;
    return 0;
}

/* control.c */
/* -----------------------
 * Basic Keyboard control stuff:
 * - Event helpers / utilities;
 * - Functions (cross-platform);
 */
#include "control.h"


int get_key(void)
{
    #ifdef _WIN32
        return _getch();
    #else
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
        return -1;
    #endif
}

int key_hit(void)
{
    #ifdef _WIN32
        return _kbhit();
    #else
        struct timeval tv = {0, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
    #endif
}

void control_poll(void)
{
    int key;
    if (!key_hit()) return;

    key = get_key();
    if (key == 's' || key == 'S' || key == 27) {
        // нажата клавиша "S" или "Escape"...
        g_stop_requested = 1;  // отправим запрос
        g_pause_active = 0;    // также отключим паузу

    } else if (key == 'p' || key == 'P' || key == ' ') {
        // нажата клавиша "P" или "Пробел"...
        g_pause_active = !g_pause_active;  // сменим состояние
    }
}

int check_hotkeys(int is_stream)
{
    // прошлое состояние
    g_pause_last = g_pause_active;

    // проверка нажатия клавиши
    control_poll();
    if (g_stop_requested) {
        fprintf(stderr, "\nUser control: Stop requested, exiting operation...\n");
        g_stop_requested = 0; return 1;
    }
    // обработка условий паузы
    if (g_pause_active) {
        // защита от спама в лог
        if (!g_pause_last) {
            fprintf(stderr, "\nUser control: Paused, (P) or (Space) to resume... ");
        }

        // режим аудио-потока: не блокируем цикл
        if (is_stream) {
            return 2;  // код отправки "continue"

        } else {
            // режим файла: блокируем цикл чтения
            g_pause_last = 1;
            while (g_pause_active) {
                control_poll();
                if (g_stop_requested) {
                    fprintf(stderr, "Stop sent, exiting...\n");
                    g_stop_requested = 0; return 1;
                }
                #ifdef _WIN32
                    Sleep(50);
                #else
                    usleep(50000);
                #endif
            }
        }
    }
    if (g_pause_last) {
        fprintf(stderr, "Resuming operation...\n\n");
    }

    return 0;  // ничего не нажато
}

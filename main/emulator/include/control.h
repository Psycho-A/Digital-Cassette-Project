/* control.h */
/* -----------------------
 * Basic Keyboard control stuff:
 * - Global key-event variables;
 * - Function forwards (cross-platform);
 */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
//#else
//#include <sys/select.h>
//#include <unistd.h>
//#include <sys/time.h>
#endif


/* Глобальные состояния*/
volatile int g_stop_requested;  // нажата клавиша остановки
volatile int g_pause_active;    // нажата клавиша паузы
volatile int g_pause_last;      // прошлое состояние паузы

/* Базовые функции */
int key_hit(void);  // проверка нажатия клавиши
int get_key(void);  // получение нажатой клавиши

/* Функции управления */
void control_poll(void);  // проверка событий от клавиши
int  check_hotkeys(int is_stream);  // отправка решений в цикл обработки


#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char byte;
    unsigned short etm_plus;
    unsigned short etm_minus;
} ETMTableEntry;

/* Подсчёт числа единиц в 10-битном слове */
int popcount10(unsigned short w) {
    int count = 0;
    int i;
    for (i = 0; i < 10; i++) {
        if (w & (1 << (9 - i))) count++;
    }
    return count;
}

/* Подсчёт числа единиц в байте */
int popcount8(unsigned char b) {
    int count = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & (1 << (7 - i))) count++;
    }
    return count;
}

/* Вычисление интервального дисбаланса (RD) */
int compute_rd(unsigned short word) {
    int positions[11];
    int count = 0;
    int i, sign, rd;
    int intervals[11];
    int num_intervals;

    for (i = 0; i < 10; i++) {
        if (word & (1 << (9 - i))) {
            positions[count++] = i;
        }
    }
    if (count == 0) return 999;

    num_intervals = count;
    for (i = 0; i < count - 1; i++) {
        intervals[i] = positions[i+1] - positions[i];
    }
    intervals[count - 1] = 10 - positions[count - 1];

    rd = 0;
    sign = 1;
    for (i = 0; i < num_intervals; i++) {
        rd += sign * intervals[i];
        sign = -sign;
    }
    return rd;
}

/* Сортировка вставками для массива unsigned short по числу единиц (возрастание) */
void sort_words_by_ones(unsigned short *arr, int n) {
    int i, j;
    unsigned short key;
    for (i = 1; i < n; i++) {
        key = arr[i];
        j = i - 1;
        while (j >= 0 && (popcount10(arr[j]) > popcount10(key) ||
                         (popcount10(arr[j]) == popcount10(key) && arr[j] > key))) {
            arr[j+1] = arr[j];
            j--;
        }
        arr[j+1] = key;
    }
}

/* Сортировка байтов по числу единиц (возрастание) */
void sort_bytes_by_ones(unsigned char *arr, int n) {
    int i, j;
    unsigned char key;
    for (i = 1; i < n; i++) {
        key = arr[i];
        j = i - 1;
        while (j >= 0 && (popcount8(arr[j]) > popcount8(key) ||
                         (popcount8(arr[j]) == popcount8(key) && arr[j] > key))) {
            arr[j+1] = arr[j];
            j--;
        }
        arr[j+1] = key;
    }
}

/* Формирует битовую строку для 10-битного слова (буфер должен быть не менее 11 символов) */
void word_to_bits(unsigned short word, char *buf) {
    int i;
    for (i = 0; i < 10; i++) {
        buf[i] = (word & (1 << (9 - i))) ? '1' : '0';
    }
    buf[10] = '\0';
}

int main() {
    /* Массивы для хранения слов по группам RD */
    unsigned short rd0[512], rd_plus2[512], rd_minus2[512];
    unsigned short rd_plus4[512], rd_minus4[512];
    unsigned short rd_plus6[512], rd_minus6[512];
    unsigned short rd_plus8[512], rd_minus8[512];
    unsigned short rd_plus10[512], rd_minus10[512];
    int n0=0, np2=0, nm2=0, np4=0, nm4=0, np6=0, nm6=0, np8=0, nm8=0, np10=0, nm10=0;

    unsigned short all_words[512];
    int w, i; // j;
    unsigned char bytes[256];
    ETMTableEntry table[256];
    unsigned short forced_00 = 0x210;   /* 0b1000010000 */
    unsigned short forced_ff = 0x3FF;   /* 0b1111111111 */
    int used_rd0 = 0, used_plus2 = 0, used_minus2 = 0;
    int used_plus4 = 0, used_minus4 = 0;
    int used_plus6 = 0, used_minus6 = 0;
    int used_plus8 = 0, used_minus8 = 0;
    int used_plus10 = 0, used_minus10 = 0;
    int b, assigned = 0;
    char bits_plus[11], bits_minus[11];
    int idx_00 = -1, idx_ff = -1;

    /* Смена кодировки на ANSI */
    system("chcp 1251> nul");

    /* Заполняем все слова с первым битом = 1 (512..1023) */
    for (w = 512; w < 1024; w++) {
        all_words[w - 512] = (unsigned short)w;
    }

    /* Распределяем по RD */
    for (i = 0; i < 512; i++) {
        int rd = compute_rd(all_words[i]);
        if (rd == 0) {
            rd0[n0++] = all_words[i];
        } else if (rd == 2) {
            rd_plus2[np2++] = all_words[i];
        } else if (rd == -2) {
            rd_minus2[nm2++] = all_words[i];
        } else if (rd == 4) {
            rd_plus4[np4++] = all_words[i];
        } else if (rd == -4) {
            rd_minus4[nm4++] = all_words[i];
        } else if (rd == 6) {
            rd_plus6[np6++] = all_words[i];
        } else if (rd == -6) {
            rd_minus6[nm6++] = all_words[i];
        } else if (rd == 8) {
            rd_plus8[np8++] = all_words[i];
        } else if (rd == -8) {
            rd_minus8[nm8++] = all_words[i];
        } else if (rd == 10) {
            rd_plus10[np10++] = all_words[i];
        } else if (rd == -10) {
            rd_minus10[nm10++] = all_words[i];
        }
        /* Игнорируем другие (их быть не должно) */
    }

    printf("/* Количество слов по группам:\n");
    printf("   RD=+0: %3d\n",                n0);
    printf("   RD=+2: %3d, RD=-2: %3d\n",    np2,  nm2);
    printf("   RD=+4: %3d, RD=-4: %3d\n",    np4,  nm4);
    printf("   RD=+6: %3d, RD=-6: %3d\n",    np6,  nm6);
    printf("   RD=+8: %3d, RD=-8: %3d\n",    np8,  nm8);
    printf("   RD=+10:%3d, RD=-10:%3d\n*/",  np10, nm10);

    /* Сортируем каждую группу по числу единиц (для визуального соответствия) */
    sort_words_by_ones(rd0, n0);
    sort_words_by_ones(rd_plus2, np2);
    sort_words_by_ones(rd_minus2, nm2);
    sort_words_by_ones(rd_plus4, np4);
    sort_words_by_ones(rd_minus4, nm4);
    sort_words_by_ones(rd_plus6, np6);
    sort_words_by_ones(rd_minus6, nm6);
    sort_words_by_ones(rd_plus8, np8);
    sort_words_by_ones(rd_minus8, nm8);
    sort_words_by_ones(rd_plus10, np10);
    sort_words_by_ones(rd_minus10, nm10);

    /* Удаляем обязательные слова из rd0, если они там есть */
    idx_00 = -1, idx_ff = -1;
    for (i = 0; i < n0; i++) {
        if (rd0[i] == forced_00) idx_00 = i;
        if (rd0[i] == forced_ff) idx_ff = i;
    }
    if (idx_00 != -1) {
        for (i = idx_00; i < n0-1; i++) rd0[i] = rd0[i+1];
        n0--;
    }
    if (idx_ff != -1) {
        for (i = idx_ff; i < n0-1; i++) rd0[i] = rd0[i+1];
        n0--;
    }

    /* Сортируем байты по весу (кроме 0x00 и 0xFF, их зафиксируем позже) */
    for (b = 0; b < 256; b++) bytes[b] = (unsigned char)b;
    sort_bytes_by_ones(bytes, 256);

    /* Обнуляем таблицу */
    memset(table, 0, sizeof(table));

    /* Заранее назначаем 0x00 и 0xFF */
    table[0x00].byte = 0x00;
    table[0x00].etm_plus = forced_00;
    table[0x00].etm_minus = forced_00;
    table[0xFF].byte = 0xFF;
    table[0xFF].etm_plus = forced_ff;
    table[0xFF].etm_minus = forced_ff;

    /* Проходим по всем байтам (кроме 0x00 и 0xFF) в порядке сортировки */
    for (b = 0; b < 256; b++) {
        unsigned char byte = bytes[b];
        if (byte == 0x00 || byte == 0xFF) continue;

        /* Пытаемся взять из RD=0, если остались */
        if (used_rd0 < n0) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd0[used_rd0];
            table[byte].etm_minus = rd0[used_rd0];
            used_rd0++;
            continue;
        }
        /* Если RD=0 кончились, берём пару из ±2, если есть оба */
        if (used_plus2 < np2 && used_minus2 < nm2) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd_plus2[used_plus2];
            table[byte].etm_minus = rd_minus2[used_minus2];
            used_plus2++;
            used_minus2++;
            continue;
        }
        /* Пары из ±4 */
        if (used_plus4 < np4 && used_minus4 < nm4) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd_plus4[used_plus4];
            table[byte].etm_minus = rd_minus4[used_minus4];
            used_plus4++;
            used_minus4++;
            continue;
        }
        /* Пары из ±6 */
        if (used_plus6 < np6 && used_minus6 < nm6) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd_plus6[used_plus6];
            table[byte].etm_minus = rd_minus6[used_minus6];
            used_plus6++;
            used_minus6++;
            continue;
        }
        /* Пары из ±8 */
        if (used_plus8 < np8 && used_minus8 < nm8) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd_plus8[used_plus8];
            table[byte].etm_minus = rd_minus8[used_minus8];
            used_plus8++;
            used_minus8++;
            continue;
        }
        /* Пары из ±10 */
        if (used_plus10 < np10 && used_minus10 < nm10) {
            table[byte].byte = byte;
            table[byte].etm_plus = rd_plus10[used_plus10];
            table[byte].etm_minus = rd_minus10[used_minus10];
            used_plus10++;
            used_minus10++;
            continue;
        }

        /* Если дошли сюда – значит не хватило слов даже с учётом всех RD */
        fprintf(stderr, "Ошибка: не хватило слов для байта 0x%02X!\n", byte);
        return 1;
    }

    /* Выводим таблицу с битовыми строками */
    printf("\nETMTableEntry etm_table[] = {\n");
    for (b = 0; b < 256; b++) {
        unsigned char byte = (unsigned char)b;
        unsigned short plus = table[byte].etm_plus;
        unsigned short minus = table[byte].etm_minus;
        int rd_plus = compute_rd(plus);
        int rd_minus = compute_rd(minus);
        word_to_bits(plus, bits_plus);
        word_to_bits(minus, bits_minus);
      //printf("    { 0x%02X, 0x%03X, 0x%03X },  // plus: %s (RD=%d), minus: %s (RD=%2d)\n",
        printf("    { 0x%02X, 0x%03X, 0x%03X },  // %s (RD=%d), %s (RD=%2d)\n",  // dec: %4d, %4d
               byte, plus, minus, bits_plus, rd_plus, bits_minus, rd_minus);
    }
    printf("};\n\n");

    system("pause> nul");
    return 0;
}

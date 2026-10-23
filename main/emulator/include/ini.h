#ifndef INIH_INI_H
#define INIH_INI_H

/* INI.h
   ----- */
typedef int (*ini_handler)(
    void* user,
    const char* section,
    const char* name,
    const char* value
);

/* Parse ini file, return 0 on success */
static void cut_inline_comment(char* s);
int ini_parse(const char* filename, ini_handler handler, void* user);

/* CFG.h
   ----- */
int    cfg_load(const char* filename);
void   cfg_free(void);

int    cfg_get_int  (const char* section, const char* key, int def);
double cfg_get_float(const char* section, const char* key, double def);
int    cfg_get_bool (const char* section, const char* key, int def);
char*  cfg_get_string(const char* section, const char* key, const char* def);

/* NEW: read with clamp value
   -------------------------- */
int cfg_read_bool(const char* section, const char* key, int def);
int cfg_read_int(const char* section, const char* key, int def, int min, int max);
int cfg_read_even(const char* section, const char* key, int def, int min, int max);
double cfg_read_float(const char* section, const char* key, double def, double min, double max);
int cfg_read_pow2(const char* section, const char* key, int def, int min, int max);

#endif /* INIH_INI_H */

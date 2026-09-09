#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Loads TASX.ini from the given path. Safe to call once; later calls are
   ignored unless the file path changes. Missing files keep built-in defaults. */
void config_load(const char* iniPath);

/* Drops all cached entries and re-reads the file (hot-reload).
   Missing file keeps previous defaults for absent keys. */
void config_reload(const char* iniPath);

/* Fills out with "<exe dir>\TASX.ini". */
void config_default_path(char* out, int outLen);

/* All getters are read-only and safe from any thread after config_load. */
int         config_get_int(const char* section, const char* key, int defVal);
const char* config_get_str(const char* section, const char* key, const char* defVal);
int         config_get_bool(const char* section, const char* key, int defVal);

/* 1 when the file was parsed successfully. */
int config_loaded(void);

#ifdef __cplusplus
}
#endif

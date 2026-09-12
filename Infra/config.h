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

/* 1 when the most recent load hit the internal table limit and silently
   dropped the remaining keys (config_max_entries() tells the limit) - the
   master logs this once per startup/hot-reload so silent config loss
   becomes visible instead of a mystery "my key does nothing". */
int config_truncated(void);

/* Capacity of the internal key table (section|key -> value entries). */
int config_max_entries(void);

/* Creates a default TASX.ini with documented values when the file is missing.
   Returns 1 if created, 0 if already exists or on failure. */
int config_create_default(const char* iniPath);

/* Enumeration: iterate entries for preset/manual overrides. */
int config_get_entry_count(void);
int config_get_entry(int index, char* secOut, int secLen, char* keyOut, int keyLen,
                     char* valOut, int valLen);

#ifdef __cplusplus
}
#endif

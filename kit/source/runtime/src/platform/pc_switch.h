/* pc_switch.h -- the one switch reader for the native-pipeline branch (pipeline M0, 2026-10-31).
 *
 * Every pipeline switch is read here, in one order: environment variable, then pc_settings.ini,
 * then the built-in default. A value that does not parse at one level falls through to the next,
 * so a typo in the ini gives the default, never a half-set switch.
 *
 * Header-only on purpose. The ini lookup is a __declspec(selectany) pointer that main.c sets in
 * configure_pc() once the settings file is known. Library objects (xbox_kernel, xbox_apu) and
 * lib-only tests include this header and link without main.c: with the pointer unset they read
 * the environment and the default only.
 *
 * Usage (read once at init and keep the result; each call reads the environment and the ini):
 *     int on   = pc_switch("RECOMP_PB_BULK", "Render", "PbBulk", 0);
 *     static const char *const wake[] = { "sleep0", "poll", "auto" };
 *     int mode = pc_switch_choice("RECOMP_NV2A_WAKE", "Render", "ExecWake", wake, 3, 0);
 * env or section/key may be NULL (env-only or ini-only switches).
 *
 * Ownership (PIPELINE-SPEC section 3): track T1 owns this file. The interface is frozen after M0:
 * add functions, never change these. Include it as "platform/pc_switch.h" from src\*.c and as
 * "../platform/pc_switch.h" from src\kernel and src\apu (those land in the kit's runtime tree). */
#pragma once
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads [section] key from the live pc_settings.ini into out; returns 1 when the key is present and
 * not empty. Set by main.c; null in lib-only tests and before configure_pc() runs. */
typedef int (*pc_switch_ini_fn)(const char *section, const char *key, char *out, unsigned out_size);
#if defined(_MSC_VER)
__declspec(selectany) pc_switch_ini_fn pc_switch_ini = 0;
#else
__attribute__((weak)) pc_switch_ini_fn pc_switch_ini = 0;
#endif

enum { PC_SWITCH_DEFAULT = 0, PC_SWITCH_INI = 1, PC_SWITCH_ENV = 2 };

static __inline void pc_switch_copy_(char *out, unsigned out_size, const char *text)
{
    unsigned i = 0;
    if (!out || !out_size) return;
    if (text)
        for (; text[i] && i + 1 < out_size; i++) out[i] = text[i];
    out[i] = 0;
}

static __inline int pc_switch_lower_(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static __inline int pc_switch_same_(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++)
        if (pc_switch_lower_((unsigned char)*a) != pc_switch_lower_((unsigned char)*b)) return 0;
    return !*a && !*b;
}

/* Integer text: decimal, 0x hex, or on/off, true/false, yes/no. Leading/trailing blanks allowed. */
static __inline int pc_switch_parse_int_(const char *text, int *value)
{
    char word[16];
    char *end;
    long n;
    unsigned i = 0;
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') text++;
    for (; text[i] && text[i] != ' ' && text[i] != '\t' && i + 1 < sizeof(word); i++) word[i] = text[i];
    word[i] = 0;
    if (!word[0]) return 0;
    if (pc_switch_same_(word, "on") || pc_switch_same_(word, "true") || pc_switch_same_(word, "yes")) { *value = 1; return 1; }
    if (pc_switch_same_(word, "off") || pc_switch_same_(word, "false") || pc_switch_same_(word, "no")) { *value = 0; return 1; }
    {
        const char *digits = (word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) ? word + 2 : word;
        n = strtol(digits, &end, digits == word ? 10 : 16);
        if (end == digits || *end) return 0;
    }
    *value = (int)n;
    return 1;
}

/* The raw text of a switch. Returns where it came from (PC_SWITCH_ENV / _INI / _DEFAULT);
 * an empty environment value or ini value counts as unset. */
static __inline int pc_switch_text(const char *env, const char *section, const char *key,
                                   const char *fallback, char *out, unsigned out_size)
{
    const char *e = env ? getenv(env) : NULL;
    char ini[128];
    if (e && *e) { pc_switch_copy_(out, out_size, e); return PC_SWITCH_ENV; }
    if (section && key && pc_switch_ini && pc_switch_ini(section, key, ini, (unsigned)sizeof(ini)) && ini[0]) {
        pc_switch_copy_(out, out_size, ini);
        return PC_SWITCH_INI;
    }
    pc_switch_copy_(out, out_size, fallback);
    return PC_SWITCH_DEFAULT;
}

/* An integer (or boolean) switch: env > ini > fallback. *source (may be NULL) says which. */
static __inline int pc_switch_ex(const char *env, const char *section, const char *key, int fallback, int *source)
{
    const char *e = env ? getenv(env) : NULL;
    char ini[128];
    int value;
    if (e && *e && pc_switch_parse_int_(e, &value)) { if (source) *source = PC_SWITCH_ENV; return value; }
    if (section && key && pc_switch_ini && pc_switch_ini(section, key, ini, (unsigned)sizeof(ini)) &&
        pc_switch_parse_int_(ini, &value)) { if (source) *source = PC_SWITCH_INI; return value; }
    if (source) *source = PC_SWITCH_DEFAULT;
    return fallback;
}

static __inline int pc_switch(const char *env, const char *section, const char *key, int fallback)
{
    return pc_switch_ex(env, section, key, fallback, NULL);
}

/* A named choice: returns the index of the matching name (case-insensitive), or a plain number
 * 0..count-1, else falls through to the next level; fallback is an index. */
static __inline int pc_switch_choice_parse_(const char *text, const char *const *names, int count, int *index)
{
    int i, n;
    char word[32];
    unsigned k = 0;
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') text++;
    for (; text[k] && text[k] != ' ' && text[k] != '\t' && k + 1 < sizeof(word); k++) word[k] = text[k];
    word[k] = 0;
    for (i = 0; i < count; i++)
        if (names[i] && pc_switch_same_(word, names[i])) { *index = i; return 1; }
    if (pc_switch_parse_int_(word, &n) && n >= 0 && n < count) { *index = n; return 1; }
    return 0;
}

static __inline int pc_switch_choice_ex(const char *env, const char *section, const char *key,
                                        const char *const *names, int count, int fallback, int *source)
{
    const char *e = env ? getenv(env) : NULL;
    char ini[128];
    int index;
    if (e && *e && pc_switch_choice_parse_(e, names, count, &index)) { if (source) *source = PC_SWITCH_ENV; return index; }
    if (section && key && pc_switch_ini && pc_switch_ini(section, key, ini, (unsigned)sizeof(ini)) &&
        pc_switch_choice_parse_(ini, names, count, &index)) { if (source) *source = PC_SWITCH_INI; return index; }
    if (source) *source = PC_SWITCH_DEFAULT;
    return fallback;
}

static __inline int pc_switch_choice(const char *env, const char *section, const char *key,
                                     const char *const *names, int count, int fallback)
{
    return pc_switch_choice_ex(env, section, key, names, count, fallback, NULL);
}

static __inline const char *pc_switch_source_name(int source)
{
    return source == PC_SWITCH_ENV ? "env" : source == PC_SWITCH_INI ? "ini" : "default";
}

#ifdef __cplusplus
}
#endif

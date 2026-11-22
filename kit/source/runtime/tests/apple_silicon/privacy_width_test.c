#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void xbox_log_redactw(wchar_t *, size_t);
const char *xbox_log_path(const char *, char *, size_t);
const char *xbox_log_pathw(const wchar_t *, char *, size_t);
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(void)
{
    CHECK(sizeof(WCHAR) == 2 && sizeof(wchar_t) >= sizeof(WCHAR));
    setenv("HOME", "/home/example", 1);
    char out[128];
    xbox_log_path("/home/example/data /Users/another/file", out, sizeof(out));
    CHECK(strcmp(out, "$HOME/data /Users/<user>/file") == 0);
    char small[10];
    xbox_log_path("/Users/alice/private", small, sizeof(small));
    CHECK(!strstr(small, "alice") && small[sizeof(small) - 1] == 0);
    wchar_t short_path[8] = L"/Users";
    xbox_log_redactw(short_path, 8);
    CHECK(short_path[7] == 0 && !wcschr(short_path, L'/'));
    xbox_log_pathw(L"/home/somebody/private", out, sizeof(out));
    CHECK(strcmp(out, "(redacted path)") == 0);
    wchar_t long_path[2200];
    for (size_t i = 0; i < 2199; ++i) long_path[i] = L'x';
    long_path[2199] = 0;
    xbox_log_pathw(long_path, out, sizeof(out));
    CHECK(strcmp(out, "(path)") == 0);
    puts("Host-path redaction and 16-bit guest character widths passed");
    return 0;
}

#define RECOMP_GENERATED_CODE
#include <recomp_types.h>
#include "platform/win32_compat.h"
#include "check.h"

#include <pthread.h>
#include <string.h>

enum { WORKERS = 4, ITERATIONS = 30000 };
static volatile uint32_t counter;
static volatile LONG signal_word;
static int payload;
static int visibility_failure;

static void *add_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < ITERATIONS; ++i)
        RECOMP_ATOMIC_ADD32(&counter, 1);
    return NULL;
}

static void *cas_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < ITERATIONS; ++i) {
        uint32_t old;
        do {
            old = RECOMP_ATOMIC_CAS32(&counter, 0, 0);
        } while (RECOMP_ATOMIC_CAS32(&counter, old, old + 1) != old);
    }
    return NULL;
}

static void *producer(void *unused)
{
    (void)unused;
    for (int i = 1; i <= ITERATIONS; ++i) {
        while (InterlockedCompareExchange(&signal_word, 0, 0) != 0)
            YieldProcessor();
        payload = i;
        InterlockedExchange(&signal_word, 1);
    }
    return NULL;
}

static void *consumer(void *unused)
{
    (void)unused;
    for (int i = 1; i <= ITERATIONS; ++i) {
        while (InterlockedCompareExchange(&signal_word, 0, 0) == 0)
            YieldProcessor();
        if (payload != i)
            visibility_failure = 1;
        InterlockedExchange(&signal_word, 0);
    }
    return NULL;
}

static int atomics(void)
{
    pthread_t threads[WORKERS];
    for (int pass = 0; pass < 2; ++pass) {
        counter = 0;
        for (int i = 0; i < WORKERS; ++i)
            CHECK(pthread_create(&threads[i], NULL, pass ? cas_worker : add_worker, NULL) == 0);
        for (int i = 0; i < WORKERS; ++i)
            CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(counter == WORKERS * ITERATIONS);
    }
    volatile int32_t value = -1;
    CHECK(InterlockedAnd(&value, ~1) == -1 && value == -2);
    CHECK(InterlockedAnd(&value, 0) == -2 && value == 0);
    pthread_t writers[2];
    CHECK(pthread_create(&writers[0], NULL, producer, NULL) == 0);
    CHECK(pthread_create(&writers[1], NULL, consumer, NULL) == 0);
    CHECK(pthread_join(writers[0], NULL) == 0);
    CHECK(pthread_join(writers[1], NULL) == 0);
    CHECK(!visibility_failure && signal_word == 0);
    return 0;
}

static int unicode(void)
{
    const char utf8[] = {'A', (char)0xc3, (char)0xa9,
                         (char)0xf0, (char)0x9f, (char)0x98, (char)0x80, 0};
    const WCHAR expected[] = {'A', 0x00e9, 0xd83d, 0xde00, 0};
    WCHAR wide[7] = {0, 0, 0, 0, 0, 0xbeef, 0xbeef};
    char back[12] = {0};
    CHECK(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0) == 5);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 5) == 5);
    CHECK(memcmp(wide, expected, sizeof(expected)) == 0 && wide[5] == 0xbeef);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL) == 8);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, wide, -1, back, 8, NULL, NULL) == 8);
    CHECK(memcmp(back, utf8, sizeof(utf8)) == 0);
    CHECK(xbox_wcslen(wide) == 4 && xbox_wcsncmp(wide, expected, 5) == 0);

    WCHAR short_buffer[3] = {0, 0, 0xfeed};
    SetLastError(0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, short_buffer, 2) == 0);
    CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER && short_buffer[2] == 0xfeed);
    memset(back, 0x55, sizeof(back));
    SetLastError(0);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, expected, -1, back, 3, NULL, NULL) == 0);
    CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER && back[3] == 0x55);
    const char explicit_nul[] = {'x', 0, 'y'};
    CHECK(MultiByteToWideChar(CP_UTF8, 0, explicit_nul, 3, wide, 3) == 3);
    CHECK(wide[0] == 'x' && wide[1] == 0 && wide[2] == 'y');
    const WCHAR explicit_wide[] = {'x', 0, 'y'};
    CHECK(WideCharToMultiByte(CP_UTF8, 0, explicit_wide, 3,
                              back, 3, NULL, NULL) == 3);
    CHECK(memcmp(back, explicit_nul, 3) == 0);

    const char max_scalar[] = {(char)0xf4, (char)0x8f, (char)0xbf, (char)0xbf};
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              max_scalar, 4, wide, 2) == 2);
    CHECK(wide[0] == 0xdbff && wide[1] == 0xdfff);
    CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wide, 2, back, 4, NULL, NULL) == 4);
    CHECK(memcmp(back, max_scalar, 4) == 0);

    const char invalid[][4] = {
        {(char)0xc0, (char)0x80, 0, 0},
        {(char)0xed, (char)0xa0, (char)0x80, 0},
        {(char)0xf4, (char)0x90, (char)0x80, (char)0x80},
        {(char)0xe2, (char)0x82, 0, 0},
    };
    const int invalid_size[] = {2, 3, 4, 2};
    for (unsigned i = 0; i < sizeof(invalid_size) / sizeof(invalid_size[0]); ++i) {
        SetLastError(0);
        CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  invalid[i], invalid_size[i], wide, 7) == 0);
        CHECK(GetLastError() == ERROR_NO_UNICODE_TRANSLATION);
    }
    const WCHAR unpaired[] = {0xd83d, 'a', 0};
    SetLastError(0);
    CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unpaired, -1,
                              back, sizeof(back), NULL, NULL) == 0);
    CHECK(GetLastError() == ERROR_NO_UNICODE_TRANSLATION);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, unpaired, -1,
                              back, sizeof(back), NULL, NULL) == 5);
    CHECK((uint8_t)back[0] == 0xef && (uint8_t)back[1] == 0xbf &&
          (uint8_t)back[2] == 0xbd && back[3] == 'a' && back[4] == 0);
    const WCHAR legacy[] = {0x00e9, 0xd83d, 0xde00, 0};
    BOOL used_default = FALSE;
    CHECK(WideCharToMultiByte(CP_ACP, 0, legacy, -1, back, sizeof(back),
                              "!", &used_default) == 3);
    CHECK((uint8_t)back[0] == 0xe9 && back[1] == '!' && back[2] == 0 &&
          used_default == TRUE);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "atomics") == 0)
        CHECK(atomics() == 0);
    if (argc == 1 || strcmp(argv[1], "unicode") == 0)
        CHECK(unicode() == 0);
    return 0;
}

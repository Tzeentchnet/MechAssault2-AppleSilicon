#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <epoxy/gl.h>
#include <openssl/ssl.h>
#include <stdio.h>

int main(void)
{
    SDL_version sdl;
    int (*volatile epoxy_symbol)(void) = &epoxy_gl_version;

    SDL_GetVersion(&sdl);
    if (sdl.major != 2 || OPENSSL_version_major() != 3 ||
        !OPENSSL_init_ssl(0, NULL) || !epoxy_symbol) {
        fputs("Dependency runtime probe failed\n", stderr);
        return 1;
    }
    printf("SDL %u.%u.%u; %s; libepoxy symbol linked\n",
           (unsigned)sdl.major, (unsigned)sdl.minor, (unsigned)sdl.patch,
           OpenSSL_version(OPENSSL_VERSION));
    return 0;
}

#include "kernel/nv2a_backend.h"
#include "kernel/nv2a_backend_dispatch.h"
#include "kernel/nv2a_pb_objects.h"
#include "kernel/nv2a_vsh_interp.h"
#include "kernel/xbox_memory_layout.h"
#include "video/framebuffer_host.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void nv2a_pb_exec_method(uint32_t subchannel, uint32_t method, uint32_t value);
void nv2a_pb_exec_report(void);
void *xbox_FramebufferWindowHandle(void);
enum { RENDER = 0x80010000u, SCANOUT = 0x80180000u, PITCH = 640u * 4u };
typedef struct Fixture { char calls[16]; unsigned n; int fail_readback; } Fixture;
static void record(Fixture *f, char c) { f->calls[f->n++] = c; }
static int clear_cb(void *context, const Nv2aGpuState *s, uint32_t mask, uint32_t color, float depth)
{
    (void)depth;
    Fixture *f = context;
    record(f, 'C');
    return s->color_va == RENDER && s->pitch == PITCH && s->clip_w == 640 &&
        s->clip_h == 480 && s->textures[3].offset == 0x123400 &&
        s->rc.alpha_icw[6] == 0x87654321 && mask == 0xf0 && color == 0xff102030;
}
static int draw_cb(void *context, const Nv2aGpuState *s, const Nv2aVshOutput *v,
                   uint32_t n, uint32_t primitive, Nv2aGpuTexel texel)
{ (void)context; (void)s; (void)v; (void)n; (void)primitive; (void)texel; return 0; }
static int sync_cb(void *context) { record(context, 'S'); return 1; }
static void invalidate_cb(void *context, uint32_t va) { (void)va; record(context, 'I'); }
static int readback_cb(void *context, uint32_t va)
{
    Fixture *f = context;
    record(f, 'R');
    if (f->fail_readback || va != SCANOUT) return 0;
    uint32_t *pixels = xbox_TranslateMappedRam(va, (size_t)PITCH * 480);
    if (!pixels) return 0;
    for (unsigned i = 0; i < 640 * 480; ++i) pixels[i] = 0xff00ff00;
    return 1;
}
static int present_cb(void *context, uint32_t va, uint32_t pitch)
{
    record(context, 'P');
    return va == SCANOUT && pitch == PITCH;
}
static int surface_info_cb(void *context, uint32_t va, uint32_t *width, uint32_t *height,
                           uint32_t *pitch, uint32_t *bpp)
{
    (void)context;
    if (va != SCANOUT) return 0;
    *width = 640; *height = 480; *pitch = PITCH; *bpp = 4;
    return 1;
}
static void flip_cb(void *context, uint32_t va, uint32_t pitch)
{
    Fixture *f = context;
    record(f, 'F');
    if (va != SCANOUT || pitch != PITCH) { record(f, '!'); return; }
    uint32_t *pixels = xbox_TranslateMappedRam(va, (size_t)PITCH * 480);
    for (unsigned i = 0; i < 640 * 480; ++i) pixels[i] = 0xffff0000;
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #expr, SDL_GetError()); return 1; } } while (0)

static size_t morton64(unsigned x, unsigned y)
{
    size_t index = 0;
    for (unsigned bit = 0; bit < 6; ++bit) {
        index |= (size_t)((x >> bit) & 1u) << (bit * 2);
        index |= (size_t)((y >> bit) & 1u) << (bit * 2 + 1);
    }
    return index;
}

static int test_swizzled_surface(void)
{
    enum { W = 64, H = 64, POS = RENDER + W * H * 4, DIFF = POS + 0x100 };
    static const float positions[6][3] = {
        {8, 8, 0}, {48, 8, 0}, {48, 48, 0},
        {8, 8, 0}, {48, 48, 0}, {8, 48, 0}
    };
    uint32_t *surface = xbox_TranslateMappedRam(RENDER, W * H * 4);
    float *position = xbox_TranslateMappedRam(POS, sizeof(positions));
    uint32_t *diffuse = xbox_TranslateMappedRam(DIFF, 6 * sizeof(*diffuse));
    CHECK(surface && position && diffuse);
    CHECK(setenv("RECOMP_SWIZZLED_RT", "1", 1) == 0);

    memset(surface, 0, W * H * 4);
    nv2a_pb_exec_method(0, 0x0200, W << 16);
    nv2a_pb_exec_method(0, 0x0204, H << 16);
    nv2a_pb_exec_method(0, 0x0208, (6u << 24) | (6u << 16) | (2u << 8) | 8u);
    nv2a_pb_exec_method(0, 0x020c, W * 4);
    nv2a_pb_exec_method(0, 0x0210, RENDER);
    nv2a_pb_exec_method(0, 0x1d98, 1u | (2u << 16));
    nv2a_pb_exec_method(0, 0x1d9c, 1u | (2u << 16));
    nv2a_pb_exec_method(0, 0x1d90, 0xff123456u);
    nv2a_pb_exec_method(0, 0x1d94, 0xf0);
    for (unsigned i = 0; i < W * H; ++i) {
        int cleared = i == 3 || i == 6 || i == 9 || i == 12;
        CHECK(surface[i] == (cleared ? 0xff123456u : 0u));
    }

    memset(surface, 0, W * H * 4);
    memcpy(position, positions, sizeof(positions));
    for (unsigned i = 0; i < 6; ++i)
        diffuse[i] = 0xff123456u;
    nv2a_pb_exec_method(0, 0x1720, POS);
    nv2a_pb_exec_method(0, 0x1760, 2u | (3u << 4) | (12u << 8));
    nv2a_pb_exec_method(0, 0x172c, DIFF);
    nv2a_pb_exec_method(0, 0x176c, 4u << 4 | 4u << 8);
    nv2a_pb_exec_method(0, 0x17fc, 5);
    nv2a_pb_exec_method(0, 0x1810, 5u << 24);
    nv2a_pb_exec_method(0, 0x17fc, 0);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            uint32_t expected = x >= 8 && x < 48 && y >= 8 && y < 48
                              ? 0xff123456u : 0u;
            CHECK(surface[morton64(x, y)] == expected);
        }
    return 0;
}

static void vsh_mov(uint32_t slot, uint32_t output, uint32_t input, int final)
{
    uint32_t words[4] = {0};
    words[1] = (1u << 21) | (input << 9) | 0x1bu;
    words[2] = 2u << 26;
    words[3] = (0xfu << 12) | (1u << 11) | (output << 3) | (final ? 1u : 0u);
    nv2a_vsh_set_instruction(slot, words);
}

static int test_strip_culling(void)
{
    enum { W = 64, H = 64, POS = RENDER + W * H * 4, DIFF = POS + 0x100 };
    static const float positions[4][4] = {
        {8, 8, 0, 1}, {8, 48, 0, 1}, {48, 8, 0, 1}, {48, 48, 0, 1}
    };
    uint32_t *surface = xbox_TranslateMappedRam(RENDER, W * H * 4);
    float *position = xbox_TranslateMappedRam(POS, sizeof(positions));
    uint32_t *diffuse = xbox_TranslateMappedRam(DIFF, 4 * sizeof(*diffuse));
    CHECK(surface && position && diffuse);
    CHECK(setenv("RECOMP_STRIP_ALT_WINDING", "1", 1) == 0);

    Fixture fixture = {{0}, 0, 0};
    Nv2aBackend backend = {
        .version = NV2A_BACKEND_VERSION, .struct_size = sizeof(backend), .context = &fixture,
        .clear = clear_cb, .draw = draw_cb, .sync = sync_cb, .readback = readback_cb,
        .invalidate = invalidate_cb, .flip = flip_cb, .present_sync = present_cb,
        .surface_info = surface_info_cb
    };
    CHECK(nv2a_backend_register(&backend));
    memset(surface, 0, W * H * 4);
    memcpy(position, positions, sizeof(positions));
    for (unsigned i = 0; i < 4; ++i)
        diffuse[i] = 0xff123456u;

    nv2a_pb_exec_method(0, 0x0200, W << 16);
    nv2a_pb_exec_method(0, 0x0204, H << 16);
    nv2a_pb_exec_method(0, 0x0208, 8);
    nv2a_pb_exec_method(0, 0x020c, W * 4);
    nv2a_pb_exec_method(0, 0x0210, RENDER);
    nv2a_pb_exec_method(0, 0x0308, 1);
    nv2a_pb_exec_method(0, 0x039c, 0x405);
    nv2a_pb_exec_method(0, 0x03a0, 0x901);
    nv2a_pb_exec_method(0, 0x1720, POS);
    nv2a_pb_exec_method(0, 0x1760, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x172c, DIFF);
    nv2a_pb_exec_method(0, 0x176c, 4u << 4 | 4u << 8);
    vsh_mov(0, 0, 0, 0);
    vsh_mov(1, 3, 3, 1);
    nv2a_pb_exec_method(0, 0x1e94, 2);
    nv2a_pb_exec_method(0, 0x17fc, 6);
    nv2a_pb_exec_method(0, 0x1810, 3u << 24);
    nv2a_pb_exec_method(0, 0x17fc, 0);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            uint32_t expected = x >= 8 && x < 48 && y >= 8 && y < 48
                              ? 0xff123456u : 0u;
            CHECK(surface[(size_t)y * W + x] == expected);
        }
    nv2a_backend_register(NULL);
    return 0;
}

static uint32_t draw_addressed_texel(uint32_t address, float u)
{
    enum {
        W = 64, H = 64, POS = RENDER + W * H * 4,
        DIFF = POS + 0x100, UV = DIFF + 0x100, TEX = UV + 0x100
    };
    static const float positions[4][4] = {
        {8, 8, 0, 1}, {56, 8, 0, 1}, {56, 56, 0, 1}, {8, 56, 0, 1}
    };
    uint32_t *surface = xbox_TranslateMappedRam(RENDER, W * H * 4);
    float *position = xbox_TranslateMappedRam(POS, sizeof(positions));
    uint32_t *diffuse = xbox_TranslateMappedRam(DIFF, 4 * sizeof(*diffuse));
    float *texcoord = xbox_TranslateMappedRam(UV, 4 * 4 * sizeof(*texcoord));
    uint32_t *texture = xbox_TranslateMappedRam(TEX, 4 * sizeof(*texture));
    CHECK(surface && position && diffuse && texcoord && texture);
    memset(surface, 0, W * H * 4);
    memcpy(position, positions, sizeof(positions));
    for (unsigned i = 0; i < 4; ++i) {
        diffuse[i] = 0xffffffffu;
        texcoord[i * 4] = u;
        texcoord[i * 4 + 1] = 0.0f;
        texcoord[i * 4 + 2] = 0.0f;
        texcoord[i * 4 + 3] = 1.0f;
    }
    texture[0] = 0xff112233u;
    texture[1] = 0xff445566u;
    texture[2] = 0xff778899u;
    texture[3] = 0xffaabbccu;

    nv2a_pb_exec_method(0, 0x0200, W << 16);
    nv2a_pb_exec_method(0, 0x0204, H << 16);
    nv2a_pb_exec_method(0, 0x0208, 8);
    nv2a_pb_exec_method(0, 0x020c, W * 4);
    nv2a_pb_exec_method(0, 0x0210, RENDER);
    nv2a_pb_exec_method(0, 0x1720, POS);
    nv2a_pb_exec_method(0, 0x1760, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x172c, DIFF);
    nv2a_pb_exec_method(0, 0x176c, (4u << 4) | (4u << 8));
    nv2a_pb_exec_method(0, 0x1744, UV);
    nv2a_pb_exec_method(0, 0x1784, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x1b00, TEX);
    nv2a_pb_exec_method(0, 0x1b04, (0x12u << 8) | (2u << 4) | (1u << 3));
    nv2a_pb_exec_method(0, 0x1b08, address | (3u << 8) | (3u << 16));
    nv2a_pb_exec_method(0, 0x1b0c, 1u << 30);
    nv2a_pb_exec_method(0, 0x1b10, 8u << 16);
    nv2a_pb_exec_method(0, 0x1b1c, (2u << 16) | 2u);
    nv2a_pb_exec_method(0, 0x1b24, 0xffcc8844u);
    vsh_mov(0, 0, 0, 0);
    vsh_mov(1, 3, 3, 0);
    vsh_mov(2, 9, 9, 1);
    nv2a_pb_exec_method(0, 0x1e94, 2);
    nv2a_pb_exec_method(0, 0x17fc, 8);
    nv2a_pb_exec_method(0, 0x1810, 3u << 24);
    nv2a_pb_exec_method(0, 0x17fc, 0);
    return surface[32 * W + 32];
}

static int test_texture_addressing(void)
{
    CHECK(draw_addressed_texel(4, -1.0f) == 0xffcc8844u);
    CHECK(draw_addressed_texel(2, 2.0f) == 0xff445566u);
    return 0;
}

static void bind_linear_texture(unsigned stage, uint32_t va)
{
    uint32_t base = 0x1b00u + stage * 0x40u;
    nv2a_pb_exec_method(0, base, va);
    nv2a_pb_exec_method(0, base + 0x04, (0x12u << 8) | (2u << 4));
    nv2a_pb_exec_method(0, base + 0x08, (3u << 8) | (3u << 16));
    nv2a_pb_exec_method(0, base + 0x0c, 1u << 30);
    nv2a_pb_exec_method(0, base + 0x10, 8u << 16);
    nv2a_pb_exec_method(0, base + 0x1c, (2u << 16) | 2u);
}

static uint32_t draw_dependent_texel(uint32_t stage_program, uint32_t output_stage,
                                     uint32_t source_texel)
{
    enum {
        W = 64, H = 64, POS = RENDER + W * H * 4, DIFF = POS + 0x100,
        UV0 = DIFF + 0x100, UV1 = UV0 + 0x100, UV2 = UV1 + 0x100,
        UV3 = UV2 + 0x100, TEX0 = UV3 + 0x100, TEX1 = TEX0 + 0x100,
        TEX2 = TEX1 + 0x100, TEX3 = TEX2 + 0x100
    };
    static const float positions[4][4] = {
        {8, 8, 0, 1}, {56, 8, 0, 1}, {56, 56, 0, 1}, {8, 56, 0, 1}
    };
    uint32_t *surface = xbox_TranslateMappedRam(RENDER, W * H * 4);
    float *position = xbox_TranslateMappedRam(POS, sizeof(positions));
    uint32_t *diffuse = xbox_TranslateMappedRam(DIFF, 4 * sizeof(*diffuse));
    float *uv0 = xbox_TranslateMappedRam(UV0, 4 * 4 * sizeof(*uv0));
    float *uv1 = xbox_TranslateMappedRam(UV1, 4 * 4 * sizeof(*uv1));
    float *uv2 = xbox_TranslateMappedRam(UV2, 4 * 4 * sizeof(*uv2));
    float *uv3 = xbox_TranslateMappedRam(UV3, 4 * 4 * sizeof(*uv3));
    uint32_t *tex0 = xbox_TranslateMappedRam(TEX0, 4 * sizeof(*tex0));
    uint32_t *tex1 = xbox_TranslateMappedRam(TEX1, 4 * sizeof(*tex1));
    uint32_t *tex2 = xbox_TranslateMappedRam(TEX2, 4 * sizeof(*tex2));
    uint32_t *tex3 = xbox_TranslateMappedRam(TEX3, 4 * sizeof(*tex3));
    CHECK(surface && position && diffuse && uv0 && uv1 && uv2 && uv3 &&
          tex0 && tex1 && tex2 && tex3);
    memset(surface, 0, W * H * 4);
    memcpy(position, positions, sizeof(positions));
    for (unsigned i = 0; i < 4; ++i) {
        diffuse[i] = 0xffffffffu;
        uv0[i * 4 + 3] = 1.0f;
        uv1[i * 4 + 3] = 1.0f;
        uv2[i * 4] = 1.0f;
        uv2[i * 4 + 3] = 1.0f;
        uv3[i * 4] = 1.0f;
        uv3[i * 4 + 3] = 1.0f;
    }
    tex0[0] = tex0[1] = tex0[2] = tex0[3] = source_texel;
    tex1[0] = 0xff112233u; tex1[1] = 0xff334455u;
    tex1[2] = 0xff55aa22u; tex1[3] = 0xff778899u;
    memcpy(tex2, tex1, 4 * sizeof(*tex2));
    memcpy(tex3, tex1, 4 * sizeof(*tex3));

    nv2a_pb_exec_method(0, 0x0200, W << 16);
    nv2a_pb_exec_method(0, 0x0204, H << 16);
    nv2a_pb_exec_method(0, 0x0208, 8);
    nv2a_pb_exec_method(0, 0x020c, W * 4);
    nv2a_pb_exec_method(0, 0x0210, RENDER);
    nv2a_pb_exec_method(0, 0x1720, POS);
    nv2a_pb_exec_method(0, 0x1760, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x172c, DIFF);
    nv2a_pb_exec_method(0, 0x176c, (4u << 4) | (4u << 8));
    nv2a_pb_exec_method(0, 0x1744, UV0);
    nv2a_pb_exec_method(0, 0x1784, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x1748, UV1);
    nv2a_pb_exec_method(0, 0x1788, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x174c, UV2);
    nv2a_pb_exec_method(0, 0x178c, 2u | (4u << 4) | (16u << 8));
    nv2a_pb_exec_method(0, 0x1750, UV3);
    nv2a_pb_exec_method(0, 0x1790, 2u | (4u << 4) | (16u << 8));
    bind_linear_texture(0, TEX0);
    bind_linear_texture(1, TEX1);
    bind_linear_texture(2, TEX2);
    bind_linear_texture(3, TEX3);
    if (stage_program == 0x000c1u)
        nv2a_pb_exec_method(0, 0x1b6c, 0x3fa00000u);
    nv2a_pb_exec_method(0, 0x1e60, 1);
    nv2a_pb_exec_method(0, 0x1e70, stage_program);
    nv2a_pb_exec_method(0, 0x0288, output_stage + 8u);
    nv2a_pb_exec_method(0, 0x028c, (output_stage + 0x18u) << 8);
    vsh_mov(0, 0, 0, 0);
    vsh_mov(1, 3, 3, 0);
    vsh_mov(2, 9, 9, 0);
    vsh_mov(3, 10, 10, 0);
    vsh_mov(4, 11, 11, 0);
    vsh_mov(5, 12, 12, 1);
    nv2a_pb_exec_method(0, 0x1e94, 2);
    nv2a_pb_exec_method(0, 0x17fc, 8);
    nv2a_pb_exec_method(0, 0x1810, 3u << 24);
    nv2a_pb_exec_method(0, 0x17fc, 0);
    return surface[32 * W + 32];
}

static int test_texture_dependencies(void)
{
    CHECK(draw_dependent_texel(0x02621u, 2, 0xffff0000u) == 0xff55aa22u);
    CHECK(draw_dependent_texel(0x001e1u, 1, 0x00ff0000u) == 0xff55aa22u);
    CHECK(draw_dependent_texel(0x000c1u, 1, 0xff00007fu) == 0xff55aa22u);
    CHECK(draw_dependent_texel(1u | (17u << 5) | (17u << 10) | (13u << 15),
                               3, 0xffff0000u) == 0xff55aa22u);
    return 0;
}

static int test_parallel_raster(void)
{
    enum { W = 192, H = 192, POS = RENDER + W * H * 4, DIFF = POS + 0x100 };
    static const float positions[4][4] = {
        {8, 8, 0, 1}, {184, 8, 0, 1}, {184, 184, 0, 1}, {8, 184, 0, 1}
    };
    uint32_t *surface=xbox_TranslateMappedRam(RENDER,W*H*4);
    float *position=xbox_TranslateMappedRam(POS,sizeof positions);
    uint32_t *diffuse=xbox_TranslateMappedRam(DIFF,4*sizeof(*diffuse));
    CHECK(surface && position && diffuse);
    CHECK(setenv("RECOMP_RASTER_THREADS","4",1)==0);
    memset(surface,0,W*H*4);
    memcpy(position,positions,sizeof positions);
    for(unsigned i=0;i<4;i++)diffuse[i]=0xff336699u;
    nv2a_pb_exec_method(0,0x0200,W<<16);
    nv2a_pb_exec_method(0,0x0204,H<<16);
    nv2a_pb_exec_method(0,0x0208,8);
    nv2a_pb_exec_method(0,0x020c,W*4);
    nv2a_pb_exec_method(0,0x0210,RENDER);
    nv2a_pb_exec_method(0,0x030c,0);
    nv2a_pb_exec_method(0,0x0358,0x01010101u);
    nv2a_pb_exec_method(0,0x039c,0);
    nv2a_pb_exec_method(0,0x1720,POS);
    nv2a_pb_exec_method(0,0x1760,2u|(4u<<4)|(16u<<8));
    nv2a_pb_exec_method(0,0x172c,DIFF);
    nv2a_pb_exec_method(0,0x176c,(4u<<4)|(4u<<8));
    vsh_mov(0,0,0,0);
    vsh_mov(1,3,3,1);
    nv2a_pb_exec_method(0,0x1e94,2);
    for(unsigned draw=0;draw<8;draw++){
        nv2a_pb_exec_method(0,0x17fc,8);
        nv2a_pb_exec_method(0,0x1810,3u<<24);
        nv2a_pb_exec_method(0,0x17fc,0);
    }
    for(unsigned y=0;y<H;y++)
        for(unsigned x=0;x<W;x++)
            CHECK(surface[y*W+x]==
                  (x>=8 && x<184 && y>=8 && y<184 ? 0xff336699u : 0));
    nv2a_pb_exec_report();
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t xbe[0x1000] = {0};
    put32(xbe + 0x104, XBOX_BASE_ADDRESS);
    put32(xbe + 0x108, sizeof(xbe));
    CHECK(xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
    CHECK(xbox_FramebufferHostOpen("NV2A synthetic fixture", 64, 48));
    if (argc == 2 && strcmp(argv[1], "swizzled-surface") == 0) {
        CHECK(test_swizzled_surface() == 0);
        xbox_FramebufferHostClose();
        xbox_MemoryLayoutShutdown();
        puts("NV2A executor: swizzled partial clear and draw passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "strip-culling") == 0) {
        CHECK(test_strip_culling() == 0);
        xbox_FramebufferHostClose();
        xbox_MemoryLayoutShutdown();
        puts("NV2A executor: alternating strip winding under culling passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "texture-addressing") == 0) {
        CHECK(test_texture_addressing() == 0);
        xbox_FramebufferHostClose();
        xbox_MemoryLayoutShutdown();
        puts("NV2A executor: mirrored and border texture addressing passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "texture-dependencies") == 0) {
        CHECK(test_texture_dependencies() == 0);
        xbox_FramebufferHostClose();
        xbox_MemoryLayoutShutdown();
        puts("NV2A executor: dependent texture stage programs passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "parallel-raster") == 0) {
        CHECK(test_parallel_raster() == 0);
        xbox_FramebufferHostClose();
        xbox_MemoryLayoutShutdown();
        puts("NV2A executor: POSIX parallel raster rows passed");
        return 0;
    }
    Fixture fixture = {{0}, 0, 0};
    Nv2aBackend backend = {
        .version = NV2A_BACKEND_VERSION, .struct_size = sizeof(backend), .context = &fixture,
        .clear = clear_cb, .draw = draw_cb, .sync = sync_cb, .readback = readback_cb,
        .invalidate = invalidate_cb, .flip = flip_cb, .present_sync = present_cb,
        .surface_info = surface_info_cb
    };
    const char *marker = "ma2-backend-test-enable";
    const char *dump = "ma2-backend-test-frame-00000.bmp";
    CHECK(access(marker, F_OK) != 0 && access(dump, F_OK) != 0);
    CHECK(setenv("RECOMP_FB_DUMP", "ma2-backend-test-frame-", 1) == 0);
    CHECK(setenv("RECOMP_FB_DUMP_FLIPS", marker, 1) == 0);
    CHECK(nv2a_backend_register(&backend));
    nv2a_pb_exec_method(0, 0x0200, 640u << 16);
    nv2a_pb_exec_method(0, 0x0204, 480u << 16);
    nv2a_pb_exec_method(0, 0x0208, 8);
    nv2a_pb_exec_method(0, 0x020c, PITCH);
    nv2a_pb_exec_method(0, 0x0210, RENDER);
    nv2a_pb_exec_method(0, 0x1bc0, 0x123400);
    nv2a_pb_exec_method(0, 0x0260 + 6 * 4, 0x87654321);
    nv2a_pb_exec_method(0, 0x1d90, 0xff102030);
    nv2a_pb_exec_method(0, 0x1d94, 0xf0);
    CHECK(strcmp(fixture.calls, "C") == 0);

    nv2a_pb_set_scanout(SCANOUT, PITCH);
    nv2a_pb_exec_method(0, 0x0130, 0);
    CHECK(strcmp(fixture.calls, "CPRF") == 0);
    CHECK(xbox_FramebufferHostPump(10) == 1);
    uint32_t pixels[64 * 48];
    SDL_Renderer *renderer = SDL_GetRenderer((SDL_Window *)xbox_FramebufferWindowHandle());
    CHECK(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, 64 * 4) == 0);
    CHECK((pixels[0] & 0x00ffffff) == 0x0000ff00);

    FILE *enable = fopen(marker, "wx");
    CHECK(enable && fclose(enable) == 0);
    fixture.fail_readback = 1;
    nv2a_pb_exec_method(0, 0x0130, 0);
    int failed_as_expected = strcmp(fixture.calls, "CPRFPR") == 0 && nv2a_dispatch_failed();
    int dumped = access(dump, F_OK) == 0;
    remove(marker);
    if (dumped) remove(dump);
    CHECK(failed_as_expected);
    CHECK(!dumped);
    nv2a_backend_register(NULL);

    xbox_FramebufferHostClose();
    xbox_MemoryLayoutShutdown();
    puts("NV2A executor: full state, QueueFlip scanout, snapshot ordering, and failure passed");
    return 0;
}

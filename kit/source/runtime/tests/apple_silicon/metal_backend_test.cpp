extern "C" {
#include "kernel/nv2a_combiner.h"
#include "kernel/nv2a_pb_objects.h"
#include "kernel/nv2a_texture.h"
#include "kernel/nv2a_vsh_interp.h"
}
#include "nv2a_metal.h"
#include "kernel/nv2a_backend_dispatch.h"
#include "kernel/xbox_memory_layout.h"
#include "video/framebuffer_host.h"
#include <SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

extern "C" void nv2a_pb_exec_method(uint32_t, uint32_t, uint32_t);
extern "C" void *xbox_FramebufferWindowHandle(void);
extern "C" uint32_t xbox_ContiguousAllocatedBytes(void);

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expr); \
        return false; \
    } \
} while (0)

enum : uint32_t {
    COLOR = 0x80020000u, DEPTH = 0x80040000u, DEPTH2 = 0x80060000u,
    OTHER = 0x80080000u, VERTICES = 0x800a0000u,
    FRAME_RENDER = 0x80100000u, FRAME_SCANOUT = 0x80300000u
};

/* The renderer keeps surfaces on the GPU. Like the executor around CPU
 * access, publish them before touching guest RAM and have the renderer
 * reload anything the test then writes. */
static uint8_t *memory(uint32_t va, size_t size)
{
    nv2a_dispatch_sync_all();
    for (uint32_t surface : {COLOR, DEPTH, DEPTH2, OTHER, FRAME_RENDER, FRAME_SCANOUT})
        nv2a_dispatch_invalidate(surface);
    return static_cast<uint8_t *>(xbox_TranslateMappedRam(va, size));
}

/* Submissions after completing everything encoded so far. */
static uint64_t submitted(Ma2MetalRenderer *renderer)
{
    nv2a_dispatch_sync_all();
    return ma2_metal_test_submission_count(renderer);
}

static uint32_t read32(uint32_t va, uint32_t pitch, unsigned x, unsigned y)
{
    uint32_t value = 0;
    std::memcpy(&value, memory(va, (size_t)(y + 1) * pitch) + (size_t)y * pitch + x * 4, 4);
    return value;
}

static uint16_t read16(uint32_t va, uint32_t pitch, unsigned x, unsigned y)
{
    uint16_t value = 0;
    std::memcpy(&value, memory(va, (size_t)(y + 1) * pitch) + (size_t)y * pitch + x * 2, 2);
    return value;
}

static Nv2aGpuState state()
{
    Nv2aGpuState s{};
    s.color_va = COLOR; s.pitch = 40; s.bpp = 4;
    s.depth_va = DEPTH; s.depth_pitch = 48;
    s.format = 0x128; s.clip_x = 2; s.clip_y = 1; s.clip_w = 5; s.clip_h = 4;
    s.color_mask = 0x01010101; s.depth_func = 0x201; s.depth_mask = 1;
    s.front_face = 0x900; s.cull_face = 0x405;
    return s;
}

static void make_triangle(Nv2aVshOutput (&v)[3], float z, float r, float g, float b, float a)
{
    std::memset(v, 0, sizeof(v));
    const float xy[3][2] = {{3, 2}, {13, 2}, {3, 10}};
    for (unsigned i = 0; i < 3; ++i) {
        v[i].pos[0] = xy[i][0]; v[i].pos[1] = xy[i][1];
        v[i].pos[2] = z; v[i].pos[3] = 1;
        v[i].d0[0] = r; v[i].d0[1] = g; v[i].d0[2] = b; v[i].d0[3] = a;
    }
}

static bool clear_tests()
{
    Nv2aGpuState s = state();
    std::memset(memory(COLOR, 40 * 8), 0x11, 40 * 8);
    std::memset(memory(DEPTH, 48 * 8), 0x77, 48 * 8);
    std::memset(memory(DEPTH2, 48 * 8), 0x44, 48 * 8);
    s.stencil_clear = 0x5a;
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff123456, 12345));
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 10; ++x) {
            bool inside = x >= 2 && x < 7 && y >= 1 && y < 5;
            CHECK(read32(COLOR, 40, x, y) == (inside ? 0xff123456u : 0x11111111u));
            CHECK(read32(DEPTH, 48, x, y) == (inside ? (12345u << 8) | 0x5au : 0x77777777u));
        }
        CHECK(read32(DEPTH, 48, 10, y) == 0x77777777u);
        CHECK(read32(DEPTH, 48, 11, y) == 0x77777777u);
    }
    uint32_t w, h, pitch, bpp;
    CHECK(!nv2a_dispatch_surface_info(COLOR, &w, &h, &pitch, &bpp));

    s.clip_x = 3; s.clip_y = 2; s.clip_w = 2; s.clip_h = 2;
    CHECK(nv2a_dispatch_clear(&s, 0x50, 0xaabbccdd, 0));
    CHECK(read32(COLOR, 40, 3, 2) == 0xffbb34ddu);
    CHECK(read32(COLOR, 40, 2, 2) == 0xff123456u);
    CHECK(read32(DEPTH, 48, 3, 2) == ((12345u << 8) | 0x5au));
    CHECK(!nv2a_dispatch_surface_info(COLOR, &w, &h, &pitch, &bpp));

    s.depth_va = DEPTH2; s.stencil_clear = 0x39;
    CHECK(nv2a_dispatch_clear(&s, 3, 0, 900));
    CHECK(read32(DEPTH2, 48, 3, 2) == ((900u << 8) | 0x39u));
    CHECK(read32(DEPTH, 48, 3, 2) == ((12345u << 8) | 0x5au));
    CHECK(read32(COLOR, 40, 3, 2) == 0xffbb34ddu);

    s = state();
    s.color_va = OTHER; s.pitch = 24; s.bpp = 2; s.format = 0x113;
    s.clip_x = 1; s.clip_y = 1; s.clip_w = 3; s.clip_h = 2;
    std::memset(memory(OTHER, 24 * 5), 0x33, 24 * 5);
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xffff8000, 0));
    CHECK(read16(OTHER, 24, 1, 1) == 0xfc00u);
    CHECK(read16(OTHER, 24, 0, 1) == 0x3333u);
    CHECK(nv2a_dispatch_clear(&s, 0x20, 0xff00ff00, 0));
    CHECK(read16(OTHER, 24, 1, 1) == 0xffe0u);

    s = state();
    s.clip_x = 3; s.clip_y = 2; s.clip_w = 2; s.clip_h = 2;
    s.stencil_clear = 0xc3;
    CHECK(nv2a_dispatch_clear(&s, 2, 0, 0));
    CHECK(read32(DEPTH, 48, 3, 2) == ((12345u << 8) | 0xc3u));
    CHECK(nv2a_dispatch_clear(&s, 1, 0, 777));
    CHECK(read32(DEPTH, 48, 3, 2) == ((777u << 8) | 0xc3u));
    s.format = 0x118; s.depth_pitch = 24;
    CHECK(nv2a_dispatch_clear(&s, 1, 0, 40000));
    CHECK(read16(DEPTH, 24, 3, 2) == 40000);
    CHECK(nv2a_dispatch_clear(&s, 2, 0, 0));
    CHECK(read16(DEPTH, 24, 3, 2) == 40000);
    return true;
}

/* Stencil stays resident beside Z24S8 depth and round-trips through guest
 * RAM: mark a triangle with INCR_WRAP, then draw only where stencil == 1. */
static bool stencil_tests()
{
    Nv2aGpuState s = state();
    s.pitch = 80; s.depth_pitch = 96; s.clip_x = 2; s.clip_y = 1; s.clip_w = 12; s.clip_h = 10;
    s.stencil_clear = 0;
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff000000, 16777215.0f));
    Nv2aVshOutput v[3];
    make_triangle(v, 1000, 1, 1, 1, 1);
    s.stencil_test = 1; s.stencil_func = 0x207; s.stencil_ref = 0;
    s.stencil_read_mask = s.stencil_write_mask = 0xff;
    s.stencil_fail = s.stencil_zfail = 0x1e00; s.stencil_zpass = 0x8507;
    s.color_mask = 0;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK((read32(DEPTH, 96, 4, 3) & 255u) == 1);
    CHECK((read32(DEPTH, 96, 12, 9) & 255u) == 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff000000u);

    Nv2aVshOutput quad[4];
    std::memset(quad, 0, sizeof(quad));
    const float corners[4][2] = {{2, 1}, {14, 1}, {14, 11}, {2, 11}};
    for (unsigned i = 0; i < 4; ++i) {
        quad[i].pos[0] = corners[i][0]; quad[i].pos[1] = corners[i][1]; quad[i].pos[3] = 1;
        quad[i].d0[0] = quad[i].d0[3] = 1;
    }
    s.color_mask = 0x01010101u;
    s.stencil_func = 0x202; s.stencil_ref = 1; s.stencil_zpass = 0x1e00;
    CHECK(nv2a_dispatch_draw(&s, quad, 4, 8, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    CHECK(read32(COLOR, 80, 12, 9) == 0xff000000u);
    CHECK(!nv2a_dispatch_failed());
    return true;
}

static bool draw_tests()
{
    Nv2aGpuState s = state();
    s.pitch = 80; s.depth_pitch = 96; s.clip_x = 2; s.clip_y = 1; s.clip_w = 12; s.clip_h = 10;
    s.depth_test = 1; s.stencil_clear = 0x5a;
    std::memset(memory(COLOR, 80 * 12), 0x17, 80 * 12);
    std::memset(memory(DEPTH, 96 * 12), 0x27, 96 * 12);
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff102030, 16777215.0f));
    Nv2aVshOutput v[3];
    make_triangle(v, 1000, 1, 0, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    CHECK(read32(DEPTH, 96, 4, 3) == ((1000u << 8) | 0x5au));
    CHECK(read32(COLOR, 80, 1, 3) == 0x17171717u);
    CHECK(read32(DEPTH, 96, 14, 3) == 0x27272727u);

    make_triangle(v, 2000, 0, 0, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    s.depth_func = 0x204;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff0000ffu);
    CHECK(read32(DEPTH, 96, 4, 3) == ((2000u << 8) | 0x5au));
    s.depth_mask = 0;
    make_triangle(v, 2500, 0, 1, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff00ff00u);
    CHECK(read32(DEPTH, 96, 4, 3) == ((2000u << 8) | 0x5au));

    s.depth_test = 0; s.color_mask = 0x00010000u;
    make_triangle(v, 0, 1, 0, 1, 0);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffffff00u);

    s.color_mask = 0x01010101u;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff0000ff, 0));
    s.blend_enable = 1; s.blend_src = 0x302; s.blend_dst = 0x303; s.blend_equation = 0x8006;
    make_triangle(v, 0, 1, 0, 0, 0.5f);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    uint32_t blended = read32(COLOR, 80, 4, 3);
    CHECK(std::abs((int)((blended >> 16) & 255) - 128) <= 1);
    CHECK(std::abs((int)(blended & 255) - 128) <= 1);
    CHECK(std::abs((int)(blended >> 24) - 191) <= 1);
    s.blend_enable = 0;

    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    s.blend_enable = 1; s.blend_src = 0x8001; s.blend_dst = 0x8002;
    s.blend_color = 0x80402080u;
    make_triangle(v, 0, 1, 1, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    blended = read32(COLOR, 80, 4, 3);
    CHECK(std::abs((int)((blended >> 16) & 255) - 64) <= 1);
    CHECK(std::abs((int)((blended >> 8) & 255) - 32) <= 1);
    CHECK(std::abs((int)(blended & 255) - 128) <= 1);
    CHECK((blended >> 24) == 255);
    s.blend_src = s.blend_dst = 0;
    s.blend_equation = 0x8007;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff203040, 0));
    make_triangle(v, 0, 128.0f / 255, 96.0f / 255, 16.0f / 255, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff203010u);
    s.blend_equation = 0x8008;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff203040, 0));
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff806040u);
    s.blend_enable = 0;

    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    s.cull_enable = 1; s.front_face = 0x900; s.cull_face = 0x404;
    make_triangle(v, 0, 1, 0, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff000000u);
    s.cull_face = 0x405;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    s.front_face = 0x901; s.cull_face = 0x404;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    s.cull_enable = 0;

    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    make_triangle(v, 0, 0, 0, 0, 1);
    v[0].d0[0] = 1; v[1].d0[1] = 1; v[2].d0[2] = 1;
    v[1].pos[3] = 2; v[2].pos[3] = 4;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    uint32_t gradient = read32(COLOR, 80, 4, 3);
    CHECK(std::abs((int)((gradient >> 16) & 255) - 215) <= 2);
    CHECK(std::abs((int)((gradient >> 8) & 255) - 24) <= 2);
    CHECK(std::abs((int)(gradient & 255) - 15) <= 2);
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    make_triangle(v, 0, 0, 0, 0, 1);
    v[0].d0[0] = 2;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(std::abs((int)((read32(COLOR, 80, 4, 3) >> 16) & 255) - 169) <= 1);

    s.depth_test = 1; s.depth_func = 0x202; s.depth_mask = 0;
    for (uint32_t z : {1u, 32767u, 50000u, 0x800000u, 0xfffffdu}) {
        CHECK(nv2a_dispatch_clear(&s, 0xf1, 0xff000000, (float)z));
        make_triangle(v, (float)z, 1, 0, 0, 1);
        CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
        CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
        CHECK((read32(DEPTH, 96, 4, 3) >> 8) == z);
    }
    s.depth_func = 0x203;
    CHECK(nv2a_dispatch_clear(&s, 0xf1, 0xff000000, 1000));
    make_triangle(v, 1000.4f, 1, 0, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    make_triangle(v, 1000.6f, 0, 0, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);

    s.format = 0x118; s.depth_pitch = 48; s.depth_test = 1; s.depth_func = 0x201; s.depth_mask = 1;
    CHECK(nv2a_dispatch_clear(&s, 3, 0, 65535));
    make_triangle(v, 32768, 0, 1, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff00ffffu);
    CHECK(std::abs((int)read16(DEPTH, 48, 4, 3) - 32768) <= 1);

    s.format = 0x128; s.depth_va = DEPTH2; s.depth_pitch = 96;
    s.depth_test = 1; s.depth_func = 0x204; s.depth_mask = 1;
    s.z_clamp = 1; s.clip_min = 0; s.clip_max = 16777215.0f;
    uint32_t first_depth = read32(DEPTH, 96, 4, 3);
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff102030, 0));
    s.color_mask = 0;
    make_triangle(v, 20000000, 1, 0, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xff102030u);
    CHECK((read32(DEPTH2, 96, 4, 3) >> 8) == 0xffffffu);
    CHECK(read32(DEPTH, 96, 4, 3) == first_depth);
    s.color_mask = 0x01010101u; s.z_clamp = 0;

    s.depth_test = 0; s.format = 0x128; s.depth_pitch = 96;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    s.clip_x = 5; s.clip_y = 3; s.clip_w = 3; s.clip_h = 4;
    make_triangle(v, 0, 1, 0, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 5, 3) == 0xffff0000u);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff000000u);
    CHECK(read32(COLOR, 80, 8, 3) == 0xff000000u);

    s.clip_x = 2; s.clip_y = 1; s.clip_w = 12; s.clip_h = 10;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    make_triangle(v, 0, 1, 1, 0, 1);
    v[0].pos[0] = v[2].pos[0] = 4.51f;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffffff00u);

    for (uint32_t primitive : {6u, 7u, 8u, 9u, 10u}) {
        Nv2aVshOutput quad[4] = {};
        const bool strip = primitive == 6 || primitive == 9;
        const float xy[4][2] = {{4, 2}, {strip ? 4.0f : 12.0f, strip ? 8.0f : 2.0f},
                                {12, strip ? 2.0f : 8.0f}, {strip ? 12.0f : 4.0f, 8}};
        for (unsigned i = 0; i < 4; ++i) {
            quad[i].pos[0] = xy[i][0]; quad[i].pos[1] = xy[i][1]; quad[i].pos[3] = 1;
            quad[i].d0[1] = quad[i].d0[3] = 1;
        }
        CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
        CHECK(nv2a_dispatch_draw(&s, quad, 4, primitive, nullptr));
        CHECK(read32(COLOR, 80, 5, 3) == 0xff00ff00u);
        CHECK(read32(COLOR, 80, 10, 6) == 0xff00ff00u);
    }
    Nv2aVshOutput flat[4] = {};
    const float flat_xy[4][2] = {{4, 2}, {4, 8}, {12, 2}, {12, 8}};
    for (unsigned i = 0; i < 4; ++i) {
        flat[i].pos[0] = flat_xy[i][0]; flat[i].pos[1] = flat_xy[i][1]; flat[i].pos[3] = 1;
        flat[i].d0[3] = 1;
    }
    flat[0].d0[0] = 1; flat[1].d0[1] = 1;
    flat[2].d0[2] = 1; flat[3].d0[0] = flat[3].d0[1] = 1;
    s.legacy = 1;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    CHECK(nv2a_dispatch_draw(&s, flat, 4, 6, nullptr));
    CHECK(read32(COLOR, 80, 5, 3) == 0xffff0000u);
    CHECK(read32(COLOR, 80, 10, 6) == 0xff00ff00u);
    s.legacy = 0;
    uint32_t previous = read32(COLOR, 80, 5, 3);
    s.color_va = OTHER;
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000000, 0));
    make_triangle(v, 0, 0, 0, 1, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read32(OTHER, 80, 4, 3) == 0xff0000ffu);
    CHECK(read32(COLOR, 80, 5, 3) == previous);
    return true;
}

static unsigned fixture_texel_calls[5];

static int fixture_texel(const Nv2aGpuTexture *texture, uint32_t face,
                         uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    if (!texture || !argb || face || z || x >= texture->width || y >= texture->height ||
        texture->offset < 1 || texture->offset > 4)
        return 0;
    ++fixture_texel_calls[texture->offset];
    if (texture->offset == 1) {
        *argb = x == 1 && y == 1 ? 0xc04080c0u : 0xff102030u;
    } else {
        uint32_t blue = (texture->offset - 1) * 0x11;
        *argb = 0xff000000u | ((x * 64u) << 16) | ((y * 64u) << 8) | blue;
    }
    return 1;
}

static Nv2aGpuTexture fixture_texture(uint32_t id)
{
    Nv2aGpuTexture t{};
    t.offset = id; t.width = t.height = 4;
    t.color = 0x06; t.addr_u = t.addr_v = 3;
    t.levels = 1; t.filter = 0x01010000; t.dimensionality = 2; t.valid = 1;
    return t;
}

static int indexed_texel(const Nv2aGpuTexture *t, uint32_t face,
                         uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    if (t && t->offset == 5 && t->width == 4096 && t->height == 1 &&
        !face && !z && !y && x < t->width && argb) {
        *argb = 0xff000055u | ((x >> 8) << 16) | ((x & 255) << 8);
        return 1;
    }
    return fixture_texel(t, face, x, y, z, argb);
}

static Nv2aGpuState fixture_state()
{
    Nv2aGpuState s = state();
    s.pitch = 80; s.clip_x = 2; s.clip_y = 1; s.clip_w = 12; s.clip_h = 10;
    s.use_rc = 1;
    for (unsigned i = 0; i < 4; ++i) s.textures[i] = fixture_texture(i + 1);
    /* LINEAR fog with px = py = 1 makes the factor equal oFog, so fixtures
     * can state the combiner's fog input directly. */
    s.fog_enable = 1; s.fog_mode = 0x2601;
    s.fog_params[0] = s.fog_params[1] = 1;
    return s;
}

static void fixture_vertices(Nv2aVshOutput (&v)[3])
{
    make_triangle(v, 0, 1, 0.5f, 0, 1);
    for (auto &vertex : v) {
        vertex.tex[0][0] = vertex.tex[0][1] = 0.5f;
        vertex.tex[0][3] = 2;
        vertex.tex[1][2] = 1;
        vertex.tex[2][0] = -1;
    }
}

static bool expect_texture_draw(Ma2MetalRenderer *renderer, Nv2aGpuState &s,
                                Nv2aVshOutput (&v)[3], Nv2aGpuTexel texel, uint32_t expected)
{
    uint64_t before = submitted(renderer);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, texel));
    CHECK(submitted(renderer) == before + 1);
    uint32_t actual = read32(COLOR, 80, 4, 3);
    if (actual != expected)
        std::fprintf(stderr, "pixel: got %08x, expected %08x\n", actual, expected);
    CHECK(actual == expected);
    return true;
}

static bool texture_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    std::memset(memory(COLOR, 80 * 12), 0, 80 * 12);
    std::memset(fixture_texel_calls, 0, sizeof(fixture_texel_calls));
    s.rc.stage_program = 0x02621;
    s.rc.other_stage_input = 0x11;
    s.rc.final0 = 0x0000000a;
    s.rc.final1 = 0x00001a00;
    uint64_t before = submitted(renderer);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    CHECK(submitted(renderer) == before + 1);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff808022u);
    CHECK(fixture_texel_calls[1] == 16 && fixture_texel_calls[2] == 0 &&
          fixture_texel_calls[3] == 16);

    s.rc = {};
    s.rc.stage_program = 0x001e1;
    s.rc.control = 1;
    s.rc.color_icw[0] = 0x09040000;
    s.rc.alpha_icw[0] = 0x19140000;
    s.rc.color_ocw[0] = s.rc.alpha_ocw[0] = 0xc0;
    std::memset(fixture_texel_calls, 0, sizeof(fixture_texel_calls));
    before = submitted(renderer);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    CHECK(submitted(renderer) == before + 1);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffc02000u);
    /* Stage 0 reuses the first draw's texture: same descriptor, same guest
     * bytes. Stage 1 is new. */
    CHECK(fixture_texel_calls[1] == 0 && fixture_texel_calls[2] == 16 &&
          fixture_texel_calls[3] == 0);

    /* Changing the guest bytes behind a cached texture must decode again. */
    uint8_t *texture_bytes = memory(1, 64);
    CHECK(texture_bytes);
    texture_bytes[0] ^= 0xff;
    std::memset(fixture_texel_calls, 0, sizeof(fixture_texel_calls));
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    CHECK(read32(COLOR, 80, 4, 3) == 0xffc02000u);
    CHECK(fixture_texel_calls[1] == 16 && fixture_texel_calls[2] == 0);
    texture_bytes = memory(1, 64);
    texture_bytes[0] ^= 0xff;
    return true;
}

static bool texture_stage_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.rc.stage_program = 0x02621;
    s.rc.final0 = 0x0a;
    s.rc.final1 = 0x1a00;
    for (auto &vertex : v) {
        vertex.tex[1][2] = -1;
        vertex.tex[2][0] = 1;
    }
    const uint32_t mapped[] = {0xff004022u, 0xff000022u, 0xff408022u,
                               0xff808022u, 0xff00c022u};
    for (uint32_t mode = 0; mode <= 4; ++mode) {
        s.rc.other_stage_input = mode | (mode << 4);
        CHECK(expect_texture_draw(renderer, s, v, fixture_texel, mapped[mode]));
    }
    s.rc.other_stage_input = 0x55;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, mapped[0]));
    s.textures[2] = fixture_texture(5);
    s.textures[2].width = 4096; s.textures[2].height = 1;
    s.rc.other_stage_input = 4;
    for (auto &vertex : v) {
        vertex.tex[1][0] = 1; vertex.tex[1][2] = 0;
        vertex.tex[2][0] = 0;
    }
    CHECK(expect_texture_draw(renderer, s, v, indexed_texel, 0xff0c0455u));
    s.textures[2] = fixture_texture(3);

    for (auto &vertex : v) {
        vertex.tex[1][0] = 0;
        vertex.tex[1][2] = 1;
        vertex.tex[2][0] = -1;
    }
    s.rc.other_stage_input = 0x11;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff808022u));
    s.rc.other_stage_input |= 1u << 16;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff80c022u));
    s.rc.other_stage_input = 0x11 | (15u << 16);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff80c022u));

    s.rc.stage_program = 0x22621;
    s.rc.other_stage_input = 0x11;
    s.rc.final0 = 0x0b;
    s.rc.final1 = 0x1b00;
    for (auto &vertex : v) {
        vertex.tex[3][0] = 2; vertex.tex[3][1] = -1;
        vertex.tex[3][2] = 0.5f; vertex.tex[3][3] = 0.25f;
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x40ff0080u));

    s.rc.stage_program = 1 | (4u << 5) | (4u << 10) | (15u << 15);
    for (auto &vertex : v) {
        vertex.tex[1][0] = 0.75f; vertex.tex[1][3] = 0.25f;
        vertex.tex[2][0] = 0.5f; vertex.tex[2][3] = 0.5f;
    }
    const uint32_t selected[] = {0xffc04033u, 0xff40c033u, 0xff808033u, 0xff808033u};
    const uint32_t selectors[] = {0, 1, 2, 15};
    for (unsigned i = 0; i < 4; ++i) {
        s.rc.other_stage_input = selectors[i] << 20;
        CHECK(expect_texture_draw(renderer, s, v, fixture_texel, selected[i]));
    }

    fixture_vertices(v);
    s.rc = {};
    s.rc.final0 = 9; s.rc.final1 = 0x1900;
    s.textures[1] = fixture_texture(3);
    s.bump_matrix[1][0] = 1;
    for (auto &vertex : v) {
        vertex.tex[1][0] = 0.75f; vertex.tex[1][1] = 0.25f;
    }
    s.rc.stage_program = 1 | (6u << 5);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff004022u));
    s.rc.stage_program = 1 | (7u << 5);
    s.bump_offset[1] = 0.5f;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x80002011u));
    s.bump_offset[1] = 0; s.bump_scale[1] = 0.5f;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x20000804u));
    s.textures[1] = fixture_texture(2);
    s.rc.stage_program = 1 | (16u << 5);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff80c011u));

    s.textures[0] = fixture_texture(3);
    s.rc = {};
    s.rc.stage_program = 3;
    s.rc.final0 = 8; s.rc.final1 = 0x1800;
    for (auto &vertex : v) {
        vertex.tex[0][0] = 1; vertex.tex[0][1] = 0.25f;
        vertex.tex[0][2] = -0.5f;
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff804022u));
    s.textures[0] = fixture_texture(1);
    fixture_vertices(v);
    s.rc.stage_program = 2;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));

    s = fixture_state();
    fixture_vertices(v);
    s.rc.final0 = 11; s.rc.final1 = 0x1b00;
    for (auto &vertex : v) {
        vertex.tex[1][0] = 4; vertex.tex[1][2] = 0; vertex.tex[1][3] = 0;
        vertex.tex[2][0] = 0; vertex.tex[2][1] = 1.2f; vertex.tex[2][3] = 0;
        std::memset(vertex.tex[3], 0, sizeof(vertex.tex[3]));
        vertex.tex[3][3] = 1;
    }
    s.rc.stage_program = 1 | (17u << 5) | (17u << 10) | (13u << 15);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xffc08033u));
    s.rc.stage_program = 1 | (17u << 5) | (10u << 10) | (14u << 15);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff008033u));
    s.rc.stage_program = 1 | (17u << 5) | (17u << 10) | (12u << 15);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff000033u));
    s.rc.stage_program = 1 | (17u << 5) | (17u << 10) | (18u << 15);
    s.eye_vector[2] = 1;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff000033u));
    s.rc.stage_program = 1 | (17u << 5) | (11u << 10) | (4u << 15);
    s.rc.final0 = 10; s.rc.final1 = 0x1a00;
    for (auto &vertex : v) vertex.tex[3][2] = -0.5f;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff408022u));
    s.rc.other_stage_input = 1u << 20;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff008022u));

    s = fixture_state();
    fixture_vertices(v);
    s.rc.stage_program = 1 | (5u << 5);
    s.rc.final0 = 8; s.rc.final1 = 0x1800;
    for (auto &vertex : v) std::memset(vertex.tex[1], 0, sizeof(vertex.tex[1]));
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    s.clip_plane_mode = 1u << 4;
    uint32_t sentinel = 0x12345678;
    std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &sentinel, 4);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, sentinel));

    s = fixture_state();
    fixture_vertices(v);
    s.rc.final0 = 4; s.rc.final1 = 0x1400;
    CHECK(expect_texture_draw(renderer, s, v, nullptr, 0xffff8000u));
    s.rc = {};
    s.rc.stage_program = 0x001e1; s.rc.final0 = 9; s.rc.final1 = 0x1900;
    for (auto &vertex : v) {
        vertex.tex[1][0] = std::numeric_limits<float>::quiet_NaN();
        vertex.tex[2][0] = std::numeric_limits<float>::quiet_NaN();
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xffc04011u));
    return true;
}

static uint32_t reference_combiner(const Nv2aGpuState &s, const Nv2aVshOutput &v)
{
    const float t[4][4] = {
        {64.0f / 255, 128.0f / 255, 192.0f / 255, 192.0f / 255},
        {192.0f / 255, 64.0f / 255, 17.0f / 255, 1},
        {0, 0, 0, 1}, {0, 0, 0, 1}
    };
    float fog[4] = {(s.fog_color & 255) / 255.0f, ((s.fog_color >> 8) & 255) / 255.0f,
                    ((s.fog_color >> 16) & 255) / 255.0f, v.fog[0]};
    float out[4];
    nv2a_rc_eval(&s.rc, v.d0, v.d1, fog, t, out);
    uint32_t r = (uint32_t)std::lround(out[0] * 255);
    uint32_t g = (uint32_t)std::lround(out[1] * 255);
    uint32_t b = (uint32_t)std::lround(out[2] * 255);
    uint32_t a = (uint32_t)std::lround(out[3] * 255);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool texture_combiner_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    for (auto &vertex : v) {
        const float color[] = {0.8f, 0.3f, 0.6f, 0.75f};
        const float secondary[] = {0.2f, 0.6f, 0.4f, 0.25f};
        std::memcpy(vertex.d0, color, sizeof(color));
        std::memcpy(vertex.d1, secondary, sizeof(secondary));
        vertex.fog[0] = 0.7f;
    }
    s.fog_color = 0xff203040;
    s.rc.stage_program = 0x001e1;
    s.rc.control = 1 | (1u << 8);
    s.rc.factor0[0] = 0xffcc8033;
    s.rc.factor1[0] = 0xff204080;
    s.rc.color_icw[0] = 0x09040801;
    s.rc.alpha_icw[0] = 0x19141811;
    s.rc.color_ocw[0] = s.rc.alpha_ocw[0] = (4u << 12) | (12u << 8);
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, reference_combiner(s, v[0])));
    s.rc.control = 1;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, reference_combiner(s, v[0])));

    s.rc.control = 2 | (0x111u << 8);
    s.rc.factor0[1] = 0xff336699;
    s.rc.factor1[1] = 0xff80a040;
    s.rc.color_icw[1] = 0xcc012902;
    s.rc.alpha_icw[1] = 0x1c113912;
    s.rc.color_ocw[1] = (0x96u << 12) | (12u << 8) | (13u << 4) | 12u;
    s.rc.alpha_ocw[1] = (0x08u << 12) | (12u << 8) | (13u << 4) | 12u;
    s.rc.final_c0 = 0xff20c080;
    s.rc.final_c1 = 0xff4080c0;
    s.rc.final0 = 0x010d0e0f;
    s.rc.final1 = 0x08021ce0;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, reference_combiner(s, v[0])));

    s.rc = {};
    s.rc.stage_program = 0x001e1;
    s.rc.final0 = 5; s.rc.final1 = 0x1500;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x40339966u));
    s.rc.final0 = 3; s.rc.final1 = 0x1300;
    for (auto &vertex : v) vertex.fog[0] = 0.5f;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x80403020u));

    s.rc = {};
    s.rc.stage_program = 0x001e1;
    s.rc.control = 9 | (0x110u << 8);
    for (unsigned i = 0; i < 8; ++i) {
        s.rc.factor0[i] = s.rc.factor1[i] = 0xffffffff;
        s.rc.color_icw[i] = ((i ? 12u : 8u) << 24) | (1u << 16);
        s.rc.alpha_icw[i] = ((i ? 0x1cu : 0x18u) << 24) | (0x12u << 16);
        s.rc.color_ocw[i] = s.rc.alpha_ocw[i] = 0xc0;
    }
    s.rc.factor0[4] = 0xff80ff00;
    s.rc.factor1[7] = 0x80ffffff;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, reference_combiner(s, v[0])));

    s.rc = {};
    s.rc.stage_program = 0x001e1;
    s.rc.control = 1;
    const uint32_t scales[] = {0, 8, 16, 24, 32, 48};
    for (uint32_t mapping = 0; mapping < 8; ++mapping) {
        s.rc.color_icw[0] = ((mapping * 32 + 8) << 24) | (0x20u << 16);
        s.rc.alpha_icw[0] = ((mapping * 32 + 0x18) << 24) | (0x20u << 16);
        for (uint32_t scale : scales) {
            s.rc.color_ocw[0] = s.rc.alpha_ocw[0] = (scale << 12) | (12u << 4);
            CHECK(expect_texture_draw(renderer, s, v, fixture_texel, reference_combiner(s, v[0])));
        }
    }
    fixture_vertices(v);
    for (auto &vertex : v) {
        vertex.d0[0] = vertex.d0[1] = vertex.d0[2] = 0;
        vertex.d0[3] = 1;
    }
    v[0].d0[0] = v[1].d0[1] = v[2].d0[2] = 1;
    s.legacy = 1;
    s.rc.color_icw[0] = 0x04200000;
    s.rc.alpha_icw[0] = 0x14200000;
    s.rc.color_ocw[0] = s.rc.alpha_ocw[0] = 0xc0;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xffa92630u));
    return true;
}

static unsigned mip_calls[3];

static int mip_texel(const Nv2aGpuTexture *t, uint32_t face,
                     uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    if (!t || !argb || face || z || x >= t->width || y >= t->height) return 0;
    if (t->offset == 0x1000 && t->width == 4 && t->height == 4) {
        ++mip_calls[0]; *argb = 0xffc00000;
    } else if (t->offset == 0x1040 && t->width == 2 && t->height == 2) {
        ++mip_calls[1]; *argb = 0xff00c000;
    } else if (t->offset == 0x1050 && t->width == 1 && t->height == 1) {
        ++mip_calls[2]; *argb = 0xff0000c0;
    } else return 0;
    return 1;
}

static int surface_texel(const Nv2aGpuTexture *t, uint32_t face,
                         uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    if (!t || !argb || face || z || x >= t->width || y >= t->height ||
        t->offset != COLOR || t->color != 0x12) return 0;
    uint8_t *pixel = memory(t->offset + y * t->pitch + x * 4, 4);
    if (!pixel) return 0;
    std::memcpy(argb, pixel, 4);
    return 1;
}

static bool texture_sampler_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.use_rc = 0; s.textured = 1; s.legacy = 1;
    s.textures[0].color = 0x12; s.textures[0].pitch = 16;
    for (auto &vertex : v) {
        vertex.tex[0][0] = vertex.tex[0][1] = 1;
        vertex.tex[0][3] = 1;
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    s.legacy = 0;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc0404000u));
    s.textured = 0;
    CHECK(expect_texture_draw(renderer, s, v, nullptr, 0xffff8000u));
    s.textured = 1;
    s.legacy = 1;
    s.textures[0].filter = 0x02010000;
    for (auto &vertex : v) vertex.tex[0][0] = vertex.tex[0][1] = 2;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xef1c3854u));

    s.textures[0] = fixture_texture(3);
    s.textures[0].color = 0x12; s.textures[0].pitch = 16;
    for (auto &vertex : v) {
        vertex.tex[0][0] = 5.2f; vertex.tex[0][1] = 1.2f;
    }
    s.textures[0].addr_u = 1;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff404022u));
    s.textures[0].addr_u = 2;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff804022u));
    s.textures[0].addr_u = 3;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xffc04022u));
    s.textures[0].addr_u = 4;
    s.textures[0].border_color = 0xff000000;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff000000u));
    s.textures[0].border_color = 0xffffffff;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xffffffffu));
    s.textures[0].border_color = 0;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0u));
    // Any other border colour: blended in the shader by filter coverage.
    s.textures[0].border_color = 0x80402010;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0x80402010u));
    s.textures[0].filter = 0x02020000;
    for (auto &vertex : v) {
        vertex.tex[0][0] = 4.0f; vertex.tex[0][1] = 1.5f;  // half on texel (3,1), half border
    }
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    uint32_t half = read32(COLOR, 80, 4, 3);
    auto channel = [&](unsigned shift) { return (int)((half >> shift) & 255); };
    if (std::abs(channel(24) - 191) > 1 || std::abs(channel(16) - 128) > 1 ||
        std::abs(channel(8) - 48) > 1 || std::abs(channel(0) - 25) > 1)
        std::fprintf(stderr, "half border: got %08x\n", half);
    CHECK(std::abs(channel(24) - 191) <= 1 && std::abs(channel(16) - 128) <= 1);
    CHECK(std::abs(channel(8) - 48) <= 1 && std::abs(channel(0) - 25) <= 1);

    s.textures[0] = fixture_texture(3);
    fixture_vertices(v);
    const float uv[3][2] = {{0.125f, 0.125f}, {0.875f, 0.125f}, {0.125f, 0.875f}};
    const float w[] = {1, 2, 4};
    for (unsigned i = 0; i < 3; ++i) {
        v[i].pos[3] = w[i]; v[i].tex[0][0] = uv[i][0]; v[i].tex[0][1] = uv[i][1];
        v[i].tex[0][3] = 1;
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff000022u));

    fixture_vertices(v);
    s.textures[0] = fixture_texture(1);
    for (auto &vertex : v) {
        vertex.tex[0][0] = vertex.tex[0][1] = 0.25f;
        vertex.tex[0][3] = 0;
    }
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));

    fixture_vertices(v);
    s.textures[0] = fixture_texture(1);
    s.textures[0].offset = 0x1000;
    s.textures[0].levels = 3;
    s.textures[0].filter = 0x01030000;
    const float mip_uv[3][2] = {{0, 0}, {10, 0}, {0, 8}};
    for (unsigned i = 0; i < 3; ++i) {
        v[i].tex[0][0] = mip_uv[i][0]; v[i].tex[0][1] = mip_uv[i][1]; v[i].tex[0][3] = 1;
    }
    std::memset(mip_calls, 0, sizeof(mip_calls));
    CHECK(expect_texture_draw(renderer, s, v, mip_texel, 0xff0000c0u));
    CHECK(mip_calls[0] == 16 && mip_calls[1] == 4 && mip_calls[2] == 1);
    s.textures[0].control0 = 1u << 6;
    CHECK(expect_texture_draw(renderer, s, v, mip_texel, 0xff00c000u));
    s.textures[0].control0 = 0;
    s.textures[0].filter = 0x01031f00;
    CHECK(expect_texture_draw(renderer, s, v, mip_texel, 0xff00c000u));
    s.textures[0].filter = 0x01051f80;
    CHECK(expect_texture_draw(renderer, s, v, mip_texel, 0xff006060u));

    fixture_vertices(v);
    s.textures[0] = fixture_texture(1);
    s.textures[0].offset = COLOR; s.textures[0].width = 20; s.textures[0].height = 11;
    s.textures[0].pitch = 80; s.textures[0].color = 0x12;
    for (auto &vertex : v) {
        vertex.tex[0][0] = 4.5f; vertex.tex[0][1] = 3.5f; vertex.tex[0][3] = 1;
    }
    std::memset(memory(COLOR, 80 * 12), 0, 80 * 12);
    uint32_t color = 0xffaa3322;
    std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &color, 4);
    CHECK(expect_texture_draw(renderer, s, v, surface_texel, color));
    CHECK(read32(COLOR, 80, 5, 3) == color);
    color = 0xff2255aa;
    std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &color, 4);
    CHECK(expect_texture_draw(renderer, s, v, surface_texel, color));
    CHECK(read32(COLOR, 80, 5, 3) == color);
    return true;
}

static int decoded_guest_texel(const Nv2aGpuTexture *t, uint32_t face,
                               uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    const uint8_t *base = memory(0, 1);
    return base && nv2a_texture_decode(base, t, face, x, y, z, argb);
}

static bool texture_decoder_tests(Ma2MetalRenderer *renderer)
{
    constexpr uint32_t source = 0x00180000, palette = 0x00181000;
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.rc.stage_program = 1; s.rc.final0 = 8; s.rc.final1 = 0x1800;
    Nv2aGpuTexture &t = s.textures[0];
    t.offset = source; t.color = 0x12; t.pitch = 16;
    for (auto &vertex : v) {
        vertex.tex[0][0] = vertex.tex[0][1] = 1; vertex.tex[0][3] = 1;
    }
    uint8_t *pixels = memory(source, 64);
    CHECK(pixels);
    std::memset(pixels, 0, 64);
    uint32_t color = 0xa1b2c3d4;
    std::memcpy(pixels + (1 * 4 + 1) * 4, &color, 4);
    CHECK(expect_texture_draw(renderer, s, v, decoded_guest_texel, color));

    t.color = 0x06; t.pitch = 0;
    for (auto &vertex : v) vertex.tex[0][0] = vertex.tex[0][1] = 0.25f;
    std::memset(pixels, 0, 64);
    std::memcpy(pixels + 3 * 4, &color, 4);
    CHECK(expect_texture_draw(renderer, s, v, decoded_guest_texel, color));

    t.color = 0x0c;
    const uint16_t endpoints[] = {0xf800, 0x07e0};
    std::memset(pixels, 0, 64);
    std::memcpy(pixels, endpoints, sizeof(endpoints));
    CHECK(expect_texture_draw(renderer, s, v, decoded_guest_texel, 0xffff0000u));

    t.color = 0x0b; t.palette = palette;
    std::memset(pixels, 0, 64);
    pixels[3] = 7;
    color = 0xaabbccdd;
    std::memset(memory(palette, 1024), 0, 1024);
    std::memcpy(memory(palette, 1024) + 7 * 4, &color, 4);
    CHECK(expect_texture_draw(renderer, s, v, decoded_guest_texel, color));
    return true;
}

static bool textured_alpha_depth_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.rc.stage_program = 1;
    s.rc.final0 = 8; s.rc.final1 = 0x1800;
    s.alpha_test = 1; s.alpha_ref = 192;
    const bool passes[] = {false, false, true, true, false, false, true, true};
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t sentinel = 0x12345678;
        std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &sentinel, 4);
        s.alpha_func = 0x200 + i;
        if (!expect_texture_draw(renderer, s, v, fixture_texel,
                                 passes[i] ? 0xc04080c0u : sentinel)) {
            std::fprintf(stderr, "alpha function %03x\n", s.alpha_func);
            return false;
        }
    }
    s.alpha_ref = 191; s.alpha_func = 0x204;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    s.use_rc = 0; s.textured = 1; s.legacy = 1; s.alpha_func = 0x202; s.alpha_ref = 192;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));

    s.use_rc = 1; s.textured = 0; s.legacy = 0;
    s.depth_test = 1; s.depth_pitch = 96; s.depth_func = 0x201; s.depth_mask = 1;
    s.stencil_clear = 0x5a;
    for (auto &vertex : v) vertex.pos[2] = 1000;
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff102030, 16777215.0f));
    s.alpha_func = 0x200;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff102030u));
    CHECK(read32(DEPTH, 96, 4, 3) == 0xffffff5au);
    s.alpha_func = 0x207;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    CHECK(read32(DEPTH, 96, 4, 3) == ((1000u << 8) | 0x5au));
    s.alpha_test = 0; s.depth_test = 0;
    uint32_t sentinel = 0x12345678;
    std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &sentinel, 4);
    s.clip_min = 1100; s.clip_max = 0;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, sentinel));
    s.clip_min = 900; s.clip_max = 1100;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    std::memcpy(memory(COLOR, 80 * 12) + 3 * 80 + 4 * 4, &sentinel, 4);
    s.clip_min = 0; s.clip_max = 900;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, sentinel));
    s.depth_test = 1; s.depth_func = 0x207;
    s.clip_min = 1100; s.clip_max = 0;
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff102030, 5000));
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xff102030u));
    CHECK(read32(DEPTH, 96, 4, 3) == ((5000u << 8) | 0x5au));
    s.clip_min = 900; s.clip_max = 1100;
    CHECK(expect_texture_draw(renderer, s, v, fixture_texel, 0xc04080c0u));
    CHECK(read32(DEPTH, 96, 4, 3) == ((1000u << 8) | 0x5au));
    return true;
}

static unsigned failing_texel_calls;

static int failing_texel(const Nv2aGpuTexture *t, uint32_t face,
                         uint32_t x, uint32_t y, uint32_t z, uint32_t *argb)
{
    if (++failing_texel_calls == 17) return 0;
    return fixture_texel(t, face, x, y, z, argb);
}

static bool texture_decline_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.rc.stage_program = 0x02621;
    s.rc.other_stage_input = 0x11;
    s.rc.final0 = 10; s.rc.final1 = 0x1a00;
    uint8_t before[80 * 12];
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    uint64_t submissions = submitted(renderer);
    uint64_t commands = ma2_metal_test_command_count(renderer);
    std::memset(fixture_texel_calls, 0, sizeof(fixture_texel_calls));
#define DECLINE_TEXTURE(...) do { Nv2aGpuState bad = s; __VA_ARGS__; \
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, fixture_texel)); } while (0)
    DECLINE_TEXTURE(bad.rc.stage_program = 1 | (8u << 5));
    CHECK(fixture_texel_calls[1] == 0);
    DECLINE_TEXTURE(bad.textures[0].valid = 0);
    DECLINE_TEXTURE(bad.textures[0].width = 0);
    DECLINE_TEXTURE(bad.textures[0].width = 4097);
    DECLINE_TEXTURE(bad.textures[0].cube = 1);
    DECLINE_TEXTURE(bad.textures[0].dimensionality = 3);
    DECLINE_TEXTURE(bad.textures[0].color = 0x12; bad.textures[0].pitch = 15);
    DECLINE_TEXTURE(bad.textures[0].addr_u = 4; bad.textures[0].border_color = 0xff123456;
                    bad.textures[0].filter = 0x01030000);
    DECLINE_TEXTURE(bad.textures[0].filter = 0x02020000; bad.textures[0].control0 = 0x10);
    DECLINE_TEXTURE(bad.textures[0].filter = 0x01030000; bad.textures[0].levels = 4);
    DECLINE_TEXTURE(bad.textures[2].offset = 0xfffffff0);
    DECLINE_TEXTURE(bad.alpha_test = 1; bad.alpha_func = 0x208);
    DECLINE_TEXTURE(bad.clip_max = std::numeric_limits<float>::quiet_NaN());
    DECLINE_TEXTURE(bad.rc.stage_program = 1 | (7u << 5);
                    bad.bump_matrix[1][0] = std::numeric_limits<float>::quiet_NaN());
#undef DECLINE_TEXTURE
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    v[0].tex[1][2] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    fixture_vertices(v);
    v[0].d1[0] = std::numeric_limits<float>::infinity();
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    fixture_vertices(v);
    // New guest bytes behind both stages, so neither is a cache hit.
    memory(4, 1)[0] ^= 0x5a;
    failing_texel_calls = 0;
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, failing_texel));
    CHECK(failing_texel_calls == 17);
    memory(4, 1)[0] ^= 0x5a;
    CHECK(submitted(renderer) == submissions);
    CHECK(ma2_metal_test_command_count(renderer) == commands);
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);
    CHECK(!nv2a_dispatch_failed());
    return true;
}

static bool near16(uint16_t value, unsigned g, unsigned b)
{
    return std::abs((int)(value >> 8) - (int)g) <= 1 && std::abs((int)(value & 255) - (int)b) <= 1;
}

/* G8B8 (colour format 0xA) stays on the GPU: 16-bit pixels with G high and
 * B low; red and alpha are never written. Matches the D3D11 backend. */
static bool g8b8_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = state();
    s.color_va = OTHER; s.pitch = 40; s.bpp = 2; s.format = 0x12a;
    s.clip_x = 1; s.clip_y = 1; s.clip_w = 14; s.clip_h = 10;
    std::memset(memory(OTHER, 40 * 12), 0x33, 40 * 12);
    uint64_t count = submitted(renderer);

    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff123456, 0));
    CHECK(read16(OTHER, 40, 4, 3) == 0x3456u);
    CHECK(read16(OTHER, 40, 0, 3) == 0x3333u);
    CHECK(read16(OTHER, 40, 15, 3) == 0x3333u);
    CHECK(read16(OTHER, 40, 4, 11) == 0x3333u);
    CHECK(nv2a_dispatch_clear(&s, 0x20, 0xffffab00, 0));
    CHECK(read16(OTHER, 40, 4, 3) == 0xab56u);
    // Red and alpha only: nothing in a G8B8 surface changes.
    CHECK(nv2a_dispatch_clear(&s, 0x90, 0xffffffff, 0));
    CHECK(read16(OTHER, 40, 4, 3) == 0xab56u);

    Nv2aVshOutput v[3];
    make_triangle(v, 0, 1, 0.5f, 0.25f, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(near16(read16(OTHER, 40, 4, 3), 128, 64));
    CHECK(read16(OTHER, 40, 0, 3) == 0x3333u);
    CHECK(read16(OTHER, 40, 12, 9) == 0xab56u);  // inside the clip, outside the triangle

    // Blend against the stored G:B (dst alpha reads as 1).
    CHECK(nv2a_dispatch_clear(&s, 0xf0, 0xff000080, 0));
    s.blend_enable = 1; s.blend_src = 0x302; s.blend_dst = 0x303; s.blend_equation = 0x8006;
    make_triangle(v, 0, 0, 1, 0, 0.5f);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(near16(read16(OTHER, 40, 4, 3), 128, 64));
    s.blend_enable = 0;

    // Guest writes reload into the texture: blue-only draw keeps the guest's G.
    uint8_t *pixel = memory(OTHER, 40 * 12) + 3 * 40 + 4 * 2;
    uint16_t guest = 0x12ff;
    std::memcpy(pixel, &guest, 2);
    s.color_mask = 0x00000001u;
    make_triangle(v, 0, 1, 1, 0, 1);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(read16(OTHER, 40, 4, 3) == 0x1200u);

    CHECK(submitted(renderer) > count);
    CHECK(!nv2a_dispatch_failed());
    return true;
}

/* Render to texture: a linear texture that is exactly a resident colour
 * surface is copied on the GPU, with no write-back wait and no texel
 * decode (the texel callback fails if it is ever called). */
static bool render_to_texture_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState target = state();
    target.color_va = OTHER; target.pitch = 16; target.bpp = 4; target.format = 0x128;
    target.clip_x = 0; target.clip_y = 0; target.clip_w = 4; target.clip_h = 4;
    CHECK(nv2a_dispatch_clear(&target, 0xf0, 0x80402010, 0));

    Nv2aGpuState s = fixture_state();
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    s.use_rc = 0; s.textured = 1; s.legacy = 1;
    s.textures[0] = fixture_texture(1);
    s.textures[0].offset = OTHER; s.textures[0].pitch = 16; s.textures[0].color = 0x12;
    for (auto &vertex : v) {
        vertex.tex[0][0] = vertex.tex[0][1] = 1;
        vertex.tex[0][3] = 1;
    }
    CHECK(expect_texture_draw(renderer, s, v, failing_texel, 0x80402010u));
    s.textures[0].color = 0x1E;
    CHECK(expect_texture_draw(renderer, s, v, failing_texel, 0xff402010u));
    // New contents reach the next sample (the copy follows the surface).
    CHECK(nv2a_dispatch_clear(&target, 0xf0, 0x11223344, 0));
    s.textures[0].color = 0x12;
    CHECK(expect_texture_draw(renderer, s, v, failing_texel, 0x11223344u));

    target.color_va = DEPTH2; target.pitch = 8; target.bpp = 2; target.format = 0x12a;
    CHECK(nv2a_dispatch_clear(&target, 0xf0, 0xff123456, 0));
    s.textures[0].offset = DEPTH2; s.textures[0].pitch = 8; s.textures[0].color = 0x17;
    CHECK(expect_texture_draw(renderer, s, v, failing_texel, 0xff003456u));

    // A Z24S8 depth surface read as LIN_A8R8G8B8: the guest's z << 8 | stencil.
    Nv2aGpuState z = state();
    z.color_va = OTHER; z.pitch = 16; z.depth_va = DEPTH; z.depth_pitch = 16; z.format = 0x128;
    z.clip_x = 0; z.clip_y = 0; z.clip_w = 4; z.clip_h = 4; z.stencil_clear = 0x5a;
    s.textures[0].offset = DEPTH; s.textures[0].pitch = 16; s.textures[0].color = 0x12;
    for (uint32_t value : {12345u, 8388607u, 16777215u, 1u}) {
        CHECK(nv2a_dispatch_clear(&z, 3, 0, (float)value));
        CHECK(expect_texture_draw(renderer, s, v, failing_texel, (value << 8) | 0x5au));
    }
    s.textures[0].color = 0x1E;
    CHECK(expect_texture_draw(renderer, s, v, failing_texel, 0xff00015au));
    CHECK(!nv2a_dispatch_failed());
    return true;
}

static bool unsupported_tests(Ma2MetalRenderer *renderer)
{
    Nv2aGpuState s = state();
    Nv2aVshOutput v[3];
    make_triangle(v, 0, 1, 1, 1, 1);
    uint8_t before[40 * 8];
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    uint64_t count = submitted(renderer);
#define DECLINE(field, value) do { Nv2aGpuState bad = s; bad.field = value; \
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr)); } while (0)
    DECLINE(textured, 1);
    DECLINE(stencil_test, 1);
    DECLINE(alpha_test, 1);
    DECLINE(polygon_offset_enable, 1);
    DECLINE(format, 0x228);
    DECLINE(pitch, 39);
    DECLINE(color_va, 0xfe800000u);
    DECLINE(color_mask, 0xffffffffu);
#undef DECLINE
    Nv2aGpuState bad = s;
    bad.use_rc = 1; bad.rc.stage_program = 8;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad = s;
    bad.depth_test = 1; bad.depth_va = COLOR;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad.depth_va = 0xf0020000u;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad.color_va = 0x00020000u; bad.depth_va = 0x08020000u;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad = s; bad.blend_enable = 1; bad.blend_src = 0xdead;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad = s; bad.z_clamp = 1; bad.clip_min = 1;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad.clip_min = -1;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    bad.clip_min = 0; bad.clip_max = 16777216.0f;
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 2, nullptr));
    v[0].pos[0] = std::numeric_limits<float>::quiet_NaN();
    v[0].d0[0] = std::numeric_limits<float>::infinity();
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    v[0].d0[0] = 1;
    CHECK(!nv2a_dispatch_clear(&s, 0x100, 0, 0));
    bad = s; bad.format = 0x228;
    CHECK(!nv2a_dispatch_clear(&bad, 0xf0, 0, 0));
    CHECK(!nv2a_dispatch_clear(&s, 1, 0, std::numeric_limits<float>::quiet_NaN()));
    v[0].pos[0] = 3;
    uint32_t old_lo = g_xbox_image_lo, old_hi = g_xbox_image_hi;
    g_xbox_image_lo = 0x10000; g_xbox_image_hi = 0x11000;
    bad = s; bad.color_va = 0x08010000;
    CHECK(!nv2a_dispatch_clear(&bad, 0xf0, 0, 0));
    CHECK(!nv2a_dispatch_draw(&bad, v, 3, 5, nullptr));
    g_xbox_image_lo = old_lo; g_xbox_image_hi = old_hi;
    CHECK(submitted(renderer) == count);
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);

    /* The CPU path culls a triangle whose position is not finite: handled,
     * with no GPU work. A vertex on or behind the eye plane is clipped by
     * Metal as the CPU's homogeneous clipper would, so it is not declined. */
    v[0].pos[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(submitted(renderer) == count);
    v[0].pos[0] = 3; v[0].pos[3] = 0;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    v[0].pos[3] = -1;
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, nullptr));
    CHECK(!nv2a_dispatch_failed());
    return true;
}

static void pb(uint32_t method, uint32_t value)
{
    nv2a_pb_exec_method(0, method, value);
}

static void submit_program_vertices(const float vertices[3][8])
{
    std::memcpy(memory(VERTICES, sizeof(float) * 24), vertices, sizeof(float) * 24);
    pb(0x17fc, 5);
    pb(0x1810, 2u << 24);
    pb(0x17fc, 0);
}

static void submit_program_triangle(float z, float r, float g, float b, float left = 3)
{
    float vertices[3][8] = {
        {left, 2, z, 1, r, g, b, 1}, {13, 2, z, 1, r, g, b, 1},
        {left, 10, z, 1, r, g, b, 1}
    };
    submit_program_vertices(vertices);
}

/* GET_REPORT through the executor; returns the reported sample count. */
static uint32_t report_count()
{
    constexpr uint32_t offset = 0x000c0000u;
    uint32_t va = offset < xbox_ContiguousAllocatedBytes() ? 0x80000000u + offset : offset;
    std::memset(memory(va, 16), 0xff, 16);
    pb(0x17d0, offset);
    uint32_t count = 0, status = 0;
    std::memcpy(&count, memory(va, 16) + 8, 4);
    std::memcpy(&status, memory(va, 16) + 12, 4);
    return status == 0 ? count : UINT32_MAX;
}

static void setup_program()
{
    /* MOV oPos,v0; MOV oD0,v3, with identity swizzles. */
    const uint32_t position[] = {0, (1u << 21) | 0x1b, 2u << 26, (15u << 12) | (1u << 11)};
    const uint32_t diffuse[] = {0, (1u << 21) | (3u << 9) | 0x1b, 2u << 26,
                                (15u << 12) | (1u << 11) | (3u << 3) | 1u};
    nv2a_vsh_set_instruction(0, position);
    nv2a_vsh_set_instruction(1, diffuse);
    nv2a_vsh_set_start_slot(0);
    pb(0x030c, 1); pb(0x0354, 0x201); pb(0x035c, 1);
    pb(0x0358, 0x01010101); pb(0x1e94, 2);
    pb(0x1720, VERTICES); pb(0x172c, VERTICES + 16);
    pb(0x1760, 2u | (4u << 4) | (32u << 8));
    pb(0x176c, 2u | (4u << 4) | (32u << 8));
}

static bool executor_tests(Ma2MetalRenderer *renderer)
{
    pb(0x0200, (12u << 16) | 2); pb(0x0204, (10u << 16) | 1);
    pb(0x0208, 0x128); pb(0x020c, (96u << 16) | 80u);
    pb(0x0210, COLOR); pb(0x0214, DEPTH);
    pb(0x1d90, 0xff000000); pb(0x1d8c, 0xffffff5a);
    pb(0x1d94, 0xf3);
    setup_program();
    uint64_t before = submitted(renderer);
    submit_program_triangle(1000, 1, 0, 0);
    CHECK(submitted(renderer) == before + 1);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    CHECK(read32(DEPTH, 96, 4, 3) == ((1000u << 8) | 0x5au));

    /* Alpha testing is deliberately unsupported by Metal; its software
     * fallback must see the GPU's depth and publish its own Z writes. */
    pb(0x0300, 1); pb(0x033c, 0x207); pb(0x0340, 0);
    before = submitted(renderer);
    submit_program_triangle(2000, 0, 0, 1);
    CHECK(submitted(renderer) == before);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    submit_program_triangle(500, 0, 0, 1);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff0000ffu);
    CHECK(read32(DEPTH, 96, 4, 3) == ((500u << 8) | 0x5au));
    pb(0x0300, 0);
    submit_program_triangle(750, 0, 1, 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff0000ffu);
    submit_program_triangle(250, 0, 1, 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff00ff00u);

    pb(0x1d90, 0xff000000); pb(0x1d8c, (1000u << 8) | 0x5a); pb(0x1d94, 0xf3);
    pb(0x0354, 0x203); pb(0x0300, 1);
    submit_program_triangle(1000.4f, 1, 0, 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    CHECK(read32(DEPTH, 96, 4, 3) == ((1000u << 8) | 0x5au));
    submit_program_triangle(1000.6f, 0, 0, 1);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    pb(0x0300, 0); pb(0x0354, 0x201);

    pb(0x030c, 0); pb(0x1d90, 0xff000000); pb(0x1d94, 0xf0);
    pb(0x0300, 1);
    submit_program_triangle(0, 1, 1, 0, 4.51f);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffffff00u);
    pb(0x1d94, 0xf0);
    const float bright[3][8] = {
        {3, 2, 0, 1, 2, 0, 0, 1}, {13, 2, 0, 1, 0, 0, 0, 1},
        {3, 10, 0, 1, 0, 0, 0, 1}
    };
    submit_program_vertices(bright);
    CHECK(std::abs((int)((read32(COLOR, 80, 4, 3) >> 16) & 255) - 169) <= 1);
    pb(0x1d94, 0xf0);
    pb(0x0308, 1); pb(0x03a0, 0x900); pb(0x039c, 0x404);
    submit_program_triangle(0, 1, 0, 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff000000u);
    pb(0x039c, 0x405);
    submit_program_triangle(0, 1, 0, 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff0000u);
    pb(0x0308, 0);
    pb(0x0300, 0); pb(0x030c, 1);

    /* Visibility tests run on Metal with exact sample counts. The report
     * must equal the pixels the counted draw changed, and a draw that the
     * depth test rejects everywhere must count nothing. */
    pb(0x1d90, 0xff000000); pb(0x1d8c, 0xffffff5a); pb(0x1d94, 0xf3);
    pb(0x17c8, 0); pb(0x17cc, 1);
    before = submitted(renderer);
    submit_program_triangle(100, 1, 0, 1);
    CHECK(submitted(renderer) == before + 1);
    pb(0x17cc, 0);
    CHECK(read32(DEPTH, 96, 4, 3) == ((100u << 8) | 0x5au));
    uint32_t covered = 0;
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 20; ++x)
            covered += read32(COLOR, 80, x, y) == 0xffff00ffu;
    CHECK(covered > 0);
    CHECK(report_count() == covered);
    pb(0x17c8, 0); pb(0x17cc, 1);
    submit_program_triangle(100, 0, 1, 1);
    pb(0x17cc, 0);
    CHECK(report_count() == 0);
    CHECK(read32(COLOR, 80, 4, 3) == 0xffff00ffu);
    uint32_t outside_color = read32(COLOR, 80, 6, 3);
    uint32_t outside_depth = read32(DEPTH, 96, 6, 3);
    pb(0x1d98, (5u << 16) | 3); pb(0x1d9c, (4u << 16) | 2);
    pb(0x1d90, 0xff220000); pb(0x1d8c, (600u << 8) | 0x5a);
    pb(0x1d94, 0x11);
    CHECK(read32(COLOR, 80, 4, 3) == 0xff2200ffu);
    CHECK(read32(DEPTH, 96, 4, 3) == ((600u << 8) | 0x5au));
    CHECK(read32(COLOR, 80, 6, 3) == outside_color);
    CHECK(read32(DEPTH, 96, 6, 3) == outside_depth);
    pb(0x1d98, 639u << 16); pb(0x1d9c, 479u << 16);
    CHECK(!nv2a_dispatch_failed());
    return true;
}

static bool interop_failure_test(Ma2MetalRenderer *renderer, int error_fd)
{
    pb(0x0200, (12u << 16) | 2); pb(0x0204, (10u << 16) | 1);
    pb(0x0208, 0x128); pb(0x020c, (96u << 16) | 80u);
    pb(0x0210, COLOR); pb(0x0214, 0x08010000u);
    setup_program();
    pb(0x0300, 1); pb(0x033c, 0x207);
    uint8_t before[4096];
    std::memcpy(before, memory(0x10000, sizeof(before)), sizeof(before));
    g_xbox_image_lo = 0x10000; g_xbox_image_hi = 0x11000;
    uint64_t count = submitted(renderer);
    submit_program_triangle(500, 0, 0, 1);
    CHECK(submitted(renderer) == count);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "depth mapping"));
    CHECK(std::memcmp(before, memory(0x10000, sizeof(before)), sizeof(before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_interop_failed"));
    return true;
}

static bool color_failure_test(Ma2MetalRenderer *renderer, int error_fd)
{
    uint8_t before[4096];
    std::memcpy(before, memory(0x10000, sizeof(before)), sizeof(before));
    g_xbox_image_lo = 0x10000; g_xbox_image_hi = 0x11000;
    pb(0x0200, (12u << 16) | 2); pb(0x0204, (10u << 16) | 1);
    pb(0x0208, 0x128); pb(0x020c, (96u << 16) | 80u);
    pb(0x0210, 0x08010000u); pb(0x1d90, 0xffabcdef);
    uint64_t count = submitted(renderer);
    pb(0x1d94, 0xf0);
    CHECK(submitted(renderer) == count);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "color mapping"));
    CHECK(std::memcmp(before, memory(0x10000, sizeof(before)), sizeof(before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_interop_failed"));
    return true;
}

static bool alias_failure_test(Ma2MetalRenderer *renderer, int error_fd, bool draw)
{
    uint8_t before[96 * 12];
    std::memset(memory(COLOR, sizeof(before)), 0x37, sizeof(before));
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    pb(0x0200, (12u << 16) | 2); pb(0x0204, (10u << 16) | 1);
    pb(0x0208, 0x128); pb(0x020c, (96u << 16) | 80u);
    pb(0x0210, COLOR); pb(0x0214, 0xf0020000u);
    uint64_t count = submitted(renderer);
    if (draw) {
        setup_program();
        submit_program_triangle(500, 1, 0, 0);
    } else {
        pb(0x1d90, 0xffabcdef); pb(0x1d8c, 0x12345678);
        pb(0x1d94, 0xf3);
    }
    CHECK(submitted(renderer) == count);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "surface overlap"));
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_interop_failed"));
    return true;
}

static bool mixed_depth_test(Ma2MetalRenderer *renderer, int error_fd, bool nonlinear_first)
{
    pb(0x0200, (12u << 16) | 2); pb(0x0204, (10u << 16) | 1);
    pb(0x020c, (96u << 16) | 80u); pb(0x0210, COLOR); pb(0x0214, DEPTH);
    std::memset(memory(DEPTH, 96 * 12), 0x45, 96 * 12);
    constexpr uint32_t linear = 0x128, swizzled = 0x04040228;
    pb(0x0208, nonlinear_first ? swizzled : linear);
    pb(0x1d8c, 0x12345678); pb(0x1d94, 1);
    CHECK(!nv2a_dispatch_failed());
    uint64_t count = submitted(renderer);
    uint8_t before[96 * 12];
    std::memcpy(before, memory(DEPTH, sizeof(before)), sizeof(before));
    pb(0x0208, nonlinear_first ? linear : swizzled);
    pb(0x1d8c, 0xabcdef12); pb(0x1d94, 1);
    CHECK(submitted(renderer) == count);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "mixed linear"));
    CHECK(std::memcmp(before, memory(DEPTH, sizeof(before)), sizeof(before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_interop_failed"));
    return true;
}

static bool presenter_test()
{
    constexpr uint32_t pitch = 640 * 4;
    uint32_t *scanout = reinterpret_cast<uint32_t *>(memory(FRAME_SCANOUT, pitch * 480));
    CHECK(scanout);
    for (unsigned i = 0; i < 640 * 480; ++i) scanout[i] = 0xff00ff00u;
    CHECK(xbox_FramebufferHostOpen("Metal synthetic fixture", 64, 48));
    pb(0x0200, 640u << 16); pb(0x0204, 480u << 16);
    pb(0x0208, 8); pb(0x020c, pitch); pb(0x0210, FRAME_RENDER);
    pb(0x1d90, 0xffff0000); pb(0x1d94, 0xf0);
    CHECK(read32(FRAME_RENDER, pitch, 0, 0) == 0xffff0000u);
    nv2a_pb_set_scanout(FRAME_SCANOUT, pitch);
    pb(0x0130, 0);
    CHECK(xbox_FramebufferHostPump(10) == 1);
    SDL_Renderer *sdl = SDL_GetRenderer(static_cast<SDL_Window *>(xbox_FramebufferWindowHandle()));
    int width = 0, height = 0;
    CHECK(sdl && SDL_GetRendererOutputSize(sdl, &width, &height) == 0 && width > 0 && height > 0);
    // The output is larger than the logical 64x48 window on a HiDPI display.
    std::vector<uint32_t> pixels((size_t)width * height);
    CHECK(SDL_RenderReadPixels(sdl, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), width * 4) == 0);
    std::fprintf(stderr, "output %dx%d first %08x centre %08x\n", width, height, pixels[0], pixels[(size_t)(height / 2) * width + width / 2]);
    CHECK((pixels[(size_t)(height / 2) * width + width / 2] & 0x00ffffffu) == 0x0000ff00u);
    xbox_FramebufferHostClose();
    return true;
}

static bool failure_test(Ma2MetalRenderer *renderer, Nv2aBackend &backend, int error_fd)
{
    Nv2aGpuState s = state();
    uint8_t before[40 * 8];
    uint8_t depth_before[48 * 8];
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    std::memcpy(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before));
    ma2_metal_test_fail_next_submission(renderer);
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xffabcdef, 500));
    // Work is encoded now and fails when it is completed.
    nv2a_dispatch_sync_all();
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(!backend.sync(backend.context));
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "command"));
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);
    CHECK(std::memcmp(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_command_failed"));
    return true;
}

static bool readback_failure_test(Ma2MetalRenderer *renderer, int error_fd)
{
    Nv2aGpuState s = state();
    s.stencil_clear = 0x5a;
    CHECK(nv2a_dispatch_clear(&s, 0xf3, 0xff334455, 1000));
    uint8_t before[40 * 8], depth_before[48 * 8];
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    std::memcpy(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before));
    Nv2aVshOutput vertices[3];
    make_triangle(vertices, 10, 0, 1, 0, 1);
    s.depth_test = 1;
    uint64_t submissions = submitted(renderer);
    ma2_metal_test_fail_next_depth_readback(renderer);
    CHECK(nv2a_dispatch_draw(&s, vertices, 3, 5, nullptr));
    CHECK(submitted(renderer) == submissions + 1);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);
    CHECK(std::memcmp(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before)) == 0);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_command_failed"));
    return true;
}

static bool textured_failure_test(Ma2MetalRenderer *renderer, int error_fd)
{
    Nv2aGpuState s = fixture_state();
    s.rc.stage_program = 0x001e1;
    s.rc.final0 = 9; s.rc.final1 = 0x1900;
    s.depth_test = 1; s.depth_pitch = 96;
    Nv2aVshOutput v[3];
    fixture_vertices(v);
    uint8_t before[80 * 12], depth_before[96 * 12];
    std::memcpy(before, memory(COLOR, sizeof(before)), sizeof(before));
    std::memcpy(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before));
    uint64_t submissions = submitted(renderer);
    uint64_t commands = ma2_metal_test_command_count(renderer);
    std::memset(fixture_texel_calls, 0, sizeof(fixture_texel_calls));
    ma2_metal_test_fail_next_submission(renderer);
    CHECK(nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    CHECK(submitted(renderer) == submissions + 1);
    CHECK(ma2_metal_test_command_count(renderer) == commands + 1);
    CHECK(fixture_texel_calls[1] == 16 && fixture_texel_calls[2] == 16);
    CHECK(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    CHECK(ma2_metal_last_error(renderer) && std::strstr(ma2_metal_last_error(renderer), "command"));
    CHECK(std::memcmp(before, memory(COLOR, sizeof(before)), sizeof(before)) == 0);
    CHECK(std::memcmp(depth_before, memory(DEPTH, sizeof(depth_before)), sizeof(depth_before)) == 0);
    CHECK(!nv2a_dispatch_draw(&s, v, 3, 5, fixture_texel));
    CHECK(ma2_metal_test_command_count(renderer) == commands + 1);
    char record[512] = {};
    ssize_t n = read(error_fd, record, sizeof(record) - 1);
    CHECK(n > 0 && std::strstr(record, "guest_metal_command_failed"));
    return true;
}

int main(int argc, char **argv)
{
    int errors[2];
    if (pipe(errors) != 0) return 1;
    if (fcntl(errors[0], F_SETFL, O_NONBLOCK) != 0) return 1;
    Ma2MetalRenderer *renderer = ma2_metal_create(-1, errors[1]);
    close(errors[1]);
    if (!renderer) { close(errors[0]); return 1; }
    Nv2aBackend backend;
    ma2_metal_backend(renderer, &backend);
    if (!nv2a_backend_register(&backend)) return 1;
    uint8_t xbe[0x1000] = {};
    uint32_t base = XBOX_BASE_ADDRESS, size = sizeof(xbe);
    std::memcpy(xbe + 0x104, &base, 4); std::memcpy(xbe + 0x108, &size, 4);
    if (!xbox_MemoryLayoutInit(xbe, sizeof(xbe))) return 1;

    bool ok, expect_failure = true;
    if (argc == 2 && std::strcmp(argv[1], "interop-failure") == 0)
        ok = interop_failure_test(renderer, errors[0]);
    else if (argc == 2 && std::strcmp(argv[1], "color-failure") == 0)
        ok = color_failure_test(renderer, errors[0]);
    else if (argc == 2 && std::strcmp(argv[1], "depth-readback-failure") == 0)
        ok = readback_failure_test(renderer, errors[0]);
    else if (argc == 2 && std::strcmp(argv[1], "textured-failure") == 0)
        ok = textured_failure_test(renderer, errors[0]);
    else if (argc == 2 && std::strcmp(argv[1], "clean-shutdown") == 0) {
        expect_failure = false;
        ok = clear_tests() && !nv2a_dispatch_failed();
    }
    else if (argc == 2 && std::strcmp(argv[1], "textures") == 0) {
        expect_failure = false;
        ok = texture_tests(renderer) && texture_stage_tests(renderer) &&
             texture_combiner_tests(renderer) && texture_sampler_tests(renderer) &&
             texture_decoder_tests(renderer) && textured_alpha_depth_tests(renderer) &&
             texture_decline_tests(renderer) && render_to_texture_tests(renderer) &&
             !nv2a_dispatch_failed();
    }
    else if (argc == 2 && std::strcmp(argv[1], "alias-clear") == 0)
        ok = alias_failure_test(renderer, errors[0], false);
    else if (argc == 2 && std::strcmp(argv[1], "alias-draw") == 0)
        ok = alias_failure_test(renderer, errors[0], true);
    else if (argc == 2 && std::strcmp(argv[1], "linear-then-swizzled") == 0)
        ok = mixed_depth_test(renderer, errors[0], false);
    else if (argc == 2 && std::strcmp(argv[1], "swizzled-then-linear") == 0)
        ok = mixed_depth_test(renderer, errors[0], true);
    else
        ok = clear_tests() && stencil_tests() && draw_tests() && g8b8_tests(renderer) &&
             unsupported_tests(renderer) &&
             executor_tests(renderer) && presenter_test() && failure_test(renderer, backend, errors[0]);
    if (!xbox_MemoryLayoutShutdownChecked()) ok = false;
    if (!nv2a_backend_register(nullptr)) ok = false;
    if ((ma2_metal_destroy(renderer) != 0) == expect_failure) ok = false;
    close(errors[0]);
    if (!ok) return 1;
    std::puts("Metal NV2A: clear, triangles, depth interop, scanout and failures passed");
    return 0;
}

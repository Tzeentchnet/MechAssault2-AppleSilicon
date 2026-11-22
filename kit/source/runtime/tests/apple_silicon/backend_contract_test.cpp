#include "nv2a_backend.h"
#include "nv2a_backend_dispatch.h"
#include <cassert>
#include <cstdio>
#include <cstring>

struct Recorder {
    char events[32]{};
    unsigned n = 0;
    uint32_t scanout = 0, pitch = 0;
    bool readback_ok = true;
    void add(char c) { events[n++] = c; }
};
static int clear_cb(void *p, const Nv2aGpuState *s, uint32_t mask, uint32_t color, float)
{
    auto &r = *static_cast<Recorder *>(p);
    r.add('C');
    assert(mask == 0xf0 && color == 0xff102030 && s->textures[3].offset == 0x123400);
    assert(s->rc.alpha_icw[6] == 0x87654321 && s->stencil_ref == 7);
    return 1;
}
static int draw_cb(void *p, const Nv2aGpuState *s, const Nv2aVshOutput *vertices,
                   uint32_t count, uint32_t prim, Nv2aGpuTexel)
{
    static_cast<Recorder *>(p)->add('D');
    assert(s->textures[3].offset == 0x123400 && count == 3 && prim == 4 && vertices);
    return 1;
}
static int sync_cb(void *p) { static_cast<Recorder *>(p)->add('S'); return 1; }
static int readback_cb(void *p, uint32_t va)
{
    auto &r = *static_cast<Recorder *>(p);
    r.add('R'); assert(va == 0x102000);
    return r.readback_ok;
}
static void invalidate_cb(void *p, uint32_t) { static_cast<Recorder *>(p)->add('I'); }
static void flip_cb(void *p, uint32_t va, uint32_t pitch)
{
    auto &r = *static_cast<Recorder *>(p);
    r.add('F'); r.scanout = va; r.pitch = pitch;
}
static void failure_cb(void *p, const char *operation)
{
    assert(nv2a_dispatch_failed());
    assert(std::strcmp(operation, "software depth import") == 0);
    static_cast<Recorder *>(p)->add('E');
}
static int program_cb(void *, const Nv2aGpuState *, const Nv2aGpuRawVertex *,
                      uint32_t, const uint32_t *, uint32_t, uint32_t, Nv2aGpuTexel,
                      const Nv2aVshProgram *, const float [NV2A_VSH_CONSTANTS][4],
                      uint32_t, uint32_t, const float [2]) { return 1; }

int main()
{
    assert(nv2a_backend_register(nullptr));
    assert(!nv2a_dispatch_enabled());
    Recorder recorder;
    Nv2aBackend b{};
    b.version = NV2A_BACKEND_VERSION;
    b.struct_size = sizeof(b);
    b.context = &recorder;
    assert(!nv2a_backend_register(&b));
    b.clear = clear_cb; b.draw = draw_cb; b.sync = sync_cb;
    b.readback = readback_cb; b.invalidate = invalidate_cb; b.flip = flip_cb;
    assert(nv2a_backend_register(&b) && nv2a_dispatch_enabled());

    Nv2aGpuState state{};
    state.textures[3].offset = 0x123400;
    state.rc.alpha_icw[6] = 0x87654321;
    state.stencil_ref = 7;
    Nv2aVshOutput vertices[3]{};
    assert(nv2a_dispatch_clear(&state, 0xf0, 0xff102030, 0.5f));
    assert(nv2a_dispatch_draw(&state, vertices, 3, 4, nullptr));
    nv2a_dispatch_sync_all();
    nv2a_dispatch_readback(0x102000);
    nv2a_dispatch_flip(0x102000, 2560);
    assert(std::strcmp(recorder.events, "CDSRF") == 0);
    assert(recorder.scanout == 0x102000 && recorder.pitch == 2560);

    Nv2aBackend invalid = b;
    invalid.version++;
    assert(!nv2a_backend_register(&invalid));
    assert(nv2a_dispatch_enabled());

    /* A truncated optional callback must never become a partial function pointer. */
    b.draw_program_indexed = program_cb;
    b.struct_size = offsetof(Nv2aBackend, draw_program_indexed) + sizeof(b.draw_program_indexed) - 1;
    assert(nv2a_backend_register(&b));
    assert(!nv2a_dispatch_draw_program_indexed(&state, nullptr, 0, nullptr, 0, 0,
                                                nullptr, nullptr, nullptr, 0, 0, nullptr));
    b.struct_size = sizeof(b);
    assert(nv2a_backend_register(&b));
    assert(nv2a_dispatch_draw_program_indexed(&state, nullptr, 0, nullptr, 0, 0,
                                               nullptr, nullptr, nullptr, 0, 0, nullptr));
    recorder.readback_ok = false;
    nv2a_dispatch_readback(0x102000);
    assert(nv2a_dispatch_failed() && nv2a_dispatch_discard_frame());
    unsigned previous = recorder.n;
    nv2a_dispatch_flip(0x102000, 2560);
    assert(recorder.n == previous);
    b.report_failure = failure_cb;
    b.struct_size = offsetof(Nv2aBackend, report_failure) + sizeof(b.report_failure) - 1;
    assert(nv2a_backend_register(&b));
    previous = recorder.n;
    nv2a_dispatch_report_failure("software depth import");
    assert(nv2a_dispatch_failed() && recorder.n == previous);
    b.struct_size = sizeof(b);
    assert(nv2a_backend_register(&b));
    nv2a_dispatch_report_failure("software depth import");
    assert(nv2a_dispatch_failed() && recorder.n == previous + 1 && recorder.events[previous] == 'E');
    nv2a_dispatch_report_failure("software depth import");
    assert(recorder.n == previous + 1);
    assert(nv2a_backend_register(nullptr) && !nv2a_dispatch_failed() && !nv2a_dispatch_enabled());
    std::puts("NV2A backend: extended state, registration, ordering, and failure passed");
    return 0;
}

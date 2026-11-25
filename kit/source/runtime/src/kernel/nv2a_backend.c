#include "nv2a_backend_dispatch.h"
#include <stdio.h>
#include <string.h>

static Nv2aBackend s_backend;
static int s_registered, s_failed;
#if defined(__APPLE__)
static uint64_t s_registration_epoch;
#endif

int nv2a_backend_register(const Nv2aBackend *backend)
{
    const size_t required = offsetof(Nv2aBackend, flip) + sizeof(backend->flip);
    if (!backend) {
        memset(&s_backend, 0, sizeof(s_backend));
        s_registered = s_failed = 0;
#if defined(__APPLE__)
        ++s_registration_epoch;
#endif
        return 1;
    }
    if (backend->version != NV2A_BACKEND_VERSION || backend->struct_size < required ||
        !backend->clear || !backend->draw || !backend->sync ||
        !backend->readback || !backend->invalidate || !backend->flip)
        return 0;
    memset(&s_backend, 0, sizeof(s_backend));
    memcpy(&s_backend, backend, required);
#define COPY_OPTIONAL(field) do { \
    if (backend->struct_size >= offsetof(Nv2aBackend, field) + sizeof(backend->field)) \
        s_backend.field = backend->field; \
} while (0)
    COPY_OPTIONAL(draw_program_indexed);
    COPY_OPTIONAL(draw_buffers);
    COPY_OPTIONAL(present_sync);
    COPY_OPTIONAL(surface_info);
    COPY_OPTIONAL(report_failure);
    COPY_OPTIONAL(visibility_reset);
    COPY_OPTIONAL(visibility_count);
    if (!s_backend.visibility_reset || !s_backend.visibility_count)
        s_backend.visibility_reset = NULL, s_backend.visibility_count = NULL;
#undef COPY_OPTIONAL
    s_registered = 1;
    s_failed = 0;
#if defined(__APPLE__)
    ++s_registration_epoch;
#endif
    return 1;
}

static void backend_error(const char *operation)
{
    if (!s_failed) fprintf(stderr, "[NV2A] registered backend %s failed; discarding frame\n", operation);
    s_failed = 1;
}

void nv2a_dispatch_report_failure(const char *operation)
{
    if (s_registered && !s_failed) {
        const char *name = operation ? operation : "operation";
        backend_error(name);
        if (s_backend.report_failure)
            s_backend.report_failure(s_backend.context, name);
    }
}

int nv2a_dispatch_enabled(void) { return s_registered || nv2a_gpu_enabled(); }
int nv2a_dispatch_registered(void) { return s_registered; }
#if defined(__APPLE__)
uint64_t nv2a_dispatch_registration_epoch(void) { return s_registration_epoch; }
#endif
int nv2a_dispatch_failed(void) { return s_registered && s_failed; }
int nv2a_dispatch_visibility(void) { return s_registered && s_backend.visibility_count != NULL; }
void nv2a_dispatch_visibility_reset(void)
{
    if (s_registered && !s_failed && s_backend.visibility_reset)
        s_backend.visibility_reset(s_backend.context);
}
uint32_t nv2a_dispatch_visibility_count(void)
{
    uint32_t count = 0;
    if (!s_registered || s_failed || !s_backend.visibility_count) return 0;
    if (!s_backend.visibility_count(s_backend.context, &count)) {
        backend_error("visibility result");
        return 0;
    }
    return count;
}
int nv2a_dispatch_discard_frame(void) { return s_registered ? s_failed : nv2a_gpu_discard_frame(); }
int nv2a_dispatch_program_pending(void) { return s_registered ? 0 : nv2a_gpu_program_pending(); }
int nv2a_dispatch_clear(const Nv2aGpuState *s, uint32_t mask, uint32_t color, float depth)
{
    return s_registered ? (!s_failed && s_backend.clear(s_backend.context, s, mask, color, depth))
                        : nv2a_gpu_clear(s, mask, color, depth);
}
int nv2a_dispatch_draw(const Nv2aGpuState *s, const Nv2aVshOutput *v,
                       uint32_t count, uint32_t primitive, Nv2aGpuTexel texel)
{
    return s_registered ? (!s_failed && s_backend.draw(s_backend.context, s, v, count, primitive, texel))
                        : nv2a_gpu_draw(s, v, count, primitive, texel);
}
int nv2a_dispatch_draw_program_indexed(const Nv2aGpuState *s, const Nv2aGpuRawVertex *v,
                       uint32_t count, const uint32_t *indices, uint32_t n,
                       uint32_t primitive, Nv2aGpuTexel texel, const Nv2aVshProgram *program,
                       const float constants[NV2A_VSH_CONSTANTS][4], uint32_t fog,
                       uint32_t mode, const float params[2])
{
    if (!s_registered) return nv2a_gpu_draw_program_indexed(s, v, count, indices, n,
        primitive, texel, program, constants, fog, mode, params);
    return !s_failed && s_backend.draw_program_indexed &&
        s_backend.draw_program_indexed(s_backend.context, s, v, count, indices, n,
                                       primitive, texel, program, constants, fog, mode, params);
}
int nv2a_dispatch_draw_buffers(const Nv2aGpuState *s, const Nv2aGpuVertexAttr attrs[16],
                       const float immediate[16][4], const uint32_t *indices, uint32_t n,
                       uint32_t primitive, Nv2aGpuTexel texel, const Nv2aVshProgram *program,
                       const float constants[NV2A_VSH_CONSTANTS][4], uint32_t fog,
                       uint32_t mode, const float params[2])
{
    if (!s_registered) return nv2a_gpu_draw_buffers(s, attrs, immediate, indices, n,
        primitive, texel, program, constants, fog, mode, params);
    return !s_failed && s_backend.draw_buffers &&
        s_backend.draw_buffers(s_backend.context, s, attrs, immediate, indices, n,
                               primitive, texel, program, constants, fog, mode, params);
}
void nv2a_dispatch_sync_all(void)
{
    if (!s_registered) nv2a_gpu_sync_all();
    else if (!s_failed && !s_backend.sync(s_backend.context)) backend_error("synchronization");
}
void nv2a_dispatch_readback(uint32_t va)
{
    if (!s_registered) nv2a_gpu_readback(va);
    else if (!s_failed && !s_backend.readback(s_backend.context, va)) backend_error("readback");
}
void nv2a_dispatch_invalidate(uint32_t va)
{
    if (!s_registered) nv2a_gpu_invalidate(va);
    else if (!s_failed) s_backend.invalidate(s_backend.context, va);
}
int nv2a_dispatch_present(uint32_t va, uint32_t pitch, void *window)
{
    if (!s_registered) return nv2a_gpu_present(va, window);
    if (!s_failed && s_backend.present_sync &&
        !s_backend.present_sync(s_backend.context, va, pitch))
        backend_error("present synchronization");
    return 0;
}
int nv2a_dispatch_surface_info(uint32_t va, uint32_t *w, uint32_t *h, uint32_t *pitch, uint32_t *bpp)
{
    if (!s_registered) return nv2a_gpu_surface_info(va, w, h, pitch, bpp);
    return !s_failed && s_backend.surface_info &&
        s_backend.surface_info(s_backend.context, va, w, h, pitch, bpp);
}
void nv2a_dispatch_flip(uint32_t va, uint32_t pitch)
{
    if (s_registered && !s_failed) s_backend.flip(s_backend.context, va, pitch);
}

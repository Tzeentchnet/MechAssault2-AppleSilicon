/*
 * Renderer-neutral backend seam, adapted from the Nv2aBackend direction in
 * xboxrecomp v0.18.0 (37b61d589168adee3680fabd51805ee81427bf07).
 *
 * MA2 needs more than upstream's stage-0 triangle contract: these descriptors
 * carry four texture stages, register combiners, vertex programs, guest buffer
 * attributes and the complete MA2 surface state. The structures in
 * nv2a_gpu_d3d11.h contain no D3D types despite that historical filename.
 */
#ifndef XBOX_NV2A_BACKEND_H
#define XBOX_NV2A_BACKEND_H

#include "nv2a_gpu_d3d11.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_BACKEND_VERSION 1u

/* All calls run synchronously, one at a time, on the pushbuffer executor.
 * State, vertices, indices, programs, constants and texture callbacks remain
 * valid only during the call; a backend retaining them must copy them.
 * clear/draw return 1 when handled, 0 only if no commands were issued and
 * the existing MA2 CPU fallback may run. sync and readback return 0 on error;
 * readback must finish and expose the requested surface in guest memory.
 * present_sync, when supplied, prepares the selected scanout before readback;
 * it returns 0 on failure and never owns the host window. flip runs after the
 * snapshot/dump and receives QueueFlip's authoritative scanout, not the
 * current render target. */
typedef struct Nv2aBackend {
    uint32_t version;
    size_t struct_size;
    void *context;
    int (*clear)(void *, const Nv2aGpuState *, uint32_t mask, uint32_t color, float depth);
    int (*draw)(void *, const Nv2aGpuState *, const Nv2aVshOutput *,
                uint32_t count, uint32_t primitive, Nv2aGpuTexel);
    int (*sync)(void *);
    int (*readback)(void *, uint32_t guest_va);
    void (*invalidate)(void *, uint32_t guest_va);
    void (*flip)(void *, uint32_t scanout_va, uint32_t pitch);
    int (*draw_program_indexed)(void *, const Nv2aGpuState *, const Nv2aGpuRawVertex *,
                uint32_t vertex_count, const uint32_t *, uint32_t index_count,
                uint32_t primitive, Nv2aGpuTexel, const Nv2aVshProgram *,
                const float constants[NV2A_VSH_CONSTANTS][4],
                uint32_t fog_enable, uint32_t fog_mode, const float fog_params[2]);
    int (*draw_buffers)(void *, const Nv2aGpuState *, const Nv2aGpuVertexAttr attrs[16],
                const float immediate[16][4], const uint32_t *, uint32_t count,
                uint32_t primitive, Nv2aGpuTexel, const Nv2aVshProgram *,
                const float constants[NV2A_VSH_CONSTANTS][4],
                uint32_t fog_enable, uint32_t fog_mode, const float fog_params[2]);
    int (*present_sync)(void *, uint32_t guest_va, uint32_t pitch);
    int (*surface_info)(void *, uint32_t guest_va, uint32_t *width, uint32_t *height,
                        uint32_t *pitch, uint32_t *bytes_per_pixel);
    /* Optional notification for executor-side interop failures. The
     * dispatcher has already marked the frame as failed when this is called. */
    void (*report_failure)(void *, const char *operation);
    /* Optional exact visibility results. A backend supplying both callbacks
     * receives draws issued while SET_ZPASS_PIXEL_COUNT_ENABLE is on, counts
     * their depth-passing samples, and adds them to the executor's own CPU
     * count. visibility_reset runs at CLEAR_REPORT_VALUE. visibility_count
     * runs at GET_REPORT: it completes outstanding work and stores the samples
     * counted since the last reset; it returns 0 on failure. Without both,
     * counted draws stay on the CPU path. */
    void (*visibility_reset)(void *);
    int (*visibility_count)(void *, uint32_t *count);
} Nv2aBackend;

/* Register before starting the executor; NULL restores MA2's existing backend.
 * Registration or removal while the executor is running is not supported.
 * Returns 0 for an incompatible or incomplete contract without changing the
 * previous registration. */
int nv2a_backend_register(const Nv2aBackend *backend);

#ifdef __cplusplus
}
#endif
#endif

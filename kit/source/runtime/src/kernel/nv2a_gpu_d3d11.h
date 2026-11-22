#ifndef NV2A_GPU_D3D11_H
#define NV2A_GPU_D3D11_H
#include <stdint.h>
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"

typedef struct {
    uint32_t offset, width, height, pitch, color, addr_u, addr_v, palette, levels, filter;
    uint32_t addr_w, control0, border_color, depth, dimensionality;
    uint32_t border_source_color;
    int cube, valid;
} Nv2aGpuTexture;

typedef struct {
    uint32_t color_va, pitch, bpp, format, depth_va, depth_pitch;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t z_clamp;
    float clip_min,clip_max;
    uint32_t cull_enable, cull_face, front_face, polygon_offset_enable;
    float polygon_offset_scale, polygon_offset_bias;
    uint32_t stencil_test, stencil_func, stencil_ref, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass, stencil_clear;
    uint32_t blend_enable, blend_src, blend_dst, blend_equation, blend_color, color_mask;
    uint32_t alpha_test, alpha_func, alpha_ref, clip_plane_mode;
    uint32_t fog_color;
    float bump_matrix[4][4], bump_scale[4], bump_offset[4], eye_vector[3];
    int use_rc, textured, legacy;
    Nv2aCombiner rc;
    Nv2aGpuTexture textures[4];
} Nv2aGpuState;

typedef int (*Nv2aGpuTexel)(const Nv2aGpuTexture *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
typedef struct { float attributes[16][4]; } Nv2aGpuRawVertex;
typedef struct {uint32_t offset,type,size,stride;} Nv2aGpuVertexAttr;

/* Optional CPU wall times, owned by the calling renderer thread. Nested work
 * is not additive: render phases are inside the PB draw bracket, and shader
 * resource creation is inside its cache-miss phase. No GPU execution times.
 * begin/end are used only on actual work paths, never on a hot cache hit. */
enum Nv2aCpuPhase {
    NV2A_CPU_TEXTURE_PREPARE, NV2A_CPU_TEXTURE_RESOURCE, NV2A_CPU_TEXTURE_SRV,
    NV2A_CPU_PIXEL_MISS, NV2A_CPU_PROGRAM_MISS, NV2A_CPU_SHADER_RESOURCE,
    NV2A_CPU_BUFFER_UPLOAD, NV2A_CPU_FETCH_RESOURCE, NV2A_CPU_VERTEX_FALLBACK,
    NV2A_CPU_PACK_DIGEST, NV2A_CPU_PACK_READ, NV2A_CPU_PACK_RESOURCE,
    NV2A_CPU_PACK_SRV, NV2A_CPU_PACK_SELECT, NV2A_CPU_PRESENT, NV2A_CPU_PACER,
    NV2A_CPU_PHASE_COUNT
};
enum Nv2aCpuEvent {
    NV2A_CPU_TEXTURE_EVICT, NV2A_CPU_TEXTURE_UPDATE, NV2A_CPU_PIXEL_EVICT,
    NV2A_CPU_PROGRAM_EVICT, NV2A_CPU_PIXEL_PENDING, NV2A_CPU_PROGRAM_PENDING,
    NV2A_CPU_FETCH_EVICT, NV2A_CPU_FETCH_UPDATE, NV2A_CPU_PACK_EVICT,
    NV2A_CPU_UPLOAD_DISCARD, NV2A_CPU_UPLOAD_WRAP, NV2A_CPU_PRESENT_OK,
    NV2A_CPU_EVENT_COUNT
};
typedef struct {
    uint64_t calls, bytes;
    double total_ms, max_ms;
} Nv2aCpuCounter;
typedef struct {
    Nv2aCpuCounter phases[NV2A_CPU_PHASE_COUNT];
    uint64_t events[NV2A_CPU_EVENT_COUNT];
    uint64_t draw_serial, present_serial;
    uint32_t owner_thread;
} Nv2aCpuSnapshot;
#ifdef __cplusplus
extern "C" {
#endif
int nv2a_gpu_cpu_enabled(void);
double nv2a_gpu_cpu_begin(void);
void nv2a_gpu_cpu_end(enum Nv2aCpuPhase, double start, uint64_t bytes);
void nv2a_gpu_cpu_count(enum Nv2aCpuEvent, uint64_t count);
/* Copies and resets only this thread's interval counters. Lifetime serials
 * and current program identity survive the reset. Never read GPU state here. */
void nv2a_gpu_cpu_take(Nv2aCpuSnapshot *);
uint64_t nv2a_gpu_cpu_draw_serial(void);
/* A key is known only if an actual draw occurred after before_serial and
 * the native program was recorded for that same draw. Fixed draws and an
 * early return cannot inherit the previous program's identity. */
int nv2a_gpu_cpu_program_since(uint64_t before_serial, uint64_t *hash);
#ifdef __cplusplus
}
#endif
int nv2a_gpu_enabled(void);
/* Keep draining the FIFO/fence commands while a failed hardware frame is
 * discarded. Never launch a full software rasterizer during device recovery. */
int nv2a_gpu_discard_frame(void);
uint64_t nv2a_gpu_device_generation(void);
/* Requires explicit private RECOMP_GPU_RECOVERY_TEST diagnostics. */
int nv2a_gpu_debug_device_loss(void);
int nv2a_gpu_program_pending(void);
int nv2a_gpu_draw(const Nv2aGpuState *, const Nv2aVshOutput *, uint32_t count,
                  uint32_t primitive, Nv2aGpuTexel);
int nv2a_gpu_draw_program(const Nv2aGpuState *, const Nv2aGpuRawVertex *, uint32_t,
                          uint32_t, Nv2aGpuTexel, const Nv2aVshProgram *,
                          const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable, uint32_t fog_mode, const float fog_params[2]);
int nv2a_gpu_draw_program_indexed(const Nv2aGpuState *, const Nv2aGpuRawVertex *, uint32_t,
                          const uint32_t *, uint32_t, uint32_t, Nv2aGpuTexel, const Nv2aVshProgram *,
                          const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable, uint32_t fog_mode, const float fog_params[2]);
int nv2a_gpu_draw_buffers(const Nv2aGpuState *,const Nv2aGpuVertexAttr [16],const float [16][4],
                          const uint32_t *,uint32_t,uint32_t,Nv2aGpuTexel,const Nv2aVshProgram *,
                          const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable,uint32_t fog_mode,const float fog_params[2]);
int nv2a_gpu_clear(const Nv2aGpuState *, uint32_t mask, uint32_t color, float depth);
void nv2a_gpu_readback(uint32_t color_va);
/* Read one unscaled guest pixel during an explicitly armed frame trace. */
int nv2a_gpu_debug_pixel(uint32_t color_va,uint32_t x,uint32_t y,uint32_t *argb);
int nv2a_gpu_debug_depth(uint32_t depth_va,uint32_t x,uint32_t y,uint32_t *packed);
int nv2a_gpu_present(uint32_t color_va, void *window);
/* Full allocation dimensions, independent of a draw's temporary clip. */
int nv2a_gpu_surface_info(uint32_t color_va, uint32_t *width, uint32_t *height,
                          uint32_t *pitch, uint32_t *bpp);
void nv2a_gpu_sync_all(void);
void nv2a_gpu_invalidate(uint32_t color_va);
#endif

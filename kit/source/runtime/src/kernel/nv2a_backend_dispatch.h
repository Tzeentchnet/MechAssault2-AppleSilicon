#ifndef XBOX_NV2A_BACKEND_DISPATCH_H
#define XBOX_NV2A_BACKEND_DISPATCH_H
#include "nv2a_backend.h"

#ifdef __cplusplus
extern "C" {
#endif
int nv2a_dispatch_enabled(void);
int nv2a_dispatch_registered(void);
#if defined(__APPLE__)
uint64_t nv2a_dispatch_registration_epoch(void);
#endif
int nv2a_dispatch_failed(void);
/* A backend that has issued work and cannot complete it must discard the
 * frame rather than returning an unsupported-state result to the CPU path. */
void nv2a_dispatch_report_failure(const char *operation);
int nv2a_dispatch_discard_frame(void);
int nv2a_dispatch_program_pending(void);
int nv2a_dispatch_clear(const Nv2aGpuState *, uint32_t, uint32_t, float);
int nv2a_dispatch_draw(const Nv2aGpuState *, const Nv2aVshOutput *,
                       uint32_t, uint32_t, Nv2aGpuTexel);
int nv2a_dispatch_draw_program_indexed(const Nv2aGpuState *, const Nv2aGpuRawVertex *,
                       uint32_t, const uint32_t *, uint32_t, uint32_t, Nv2aGpuTexel,
                       const Nv2aVshProgram *, const float [NV2A_VSH_CONSTANTS][4],
                       uint32_t, uint32_t, const float [2]);
int nv2a_dispatch_draw_buffers(const Nv2aGpuState *, const Nv2aGpuVertexAttr [16],
                       const float [16][4], const uint32_t *, uint32_t, uint32_t,
                       Nv2aGpuTexel, const Nv2aVshProgram *,
                       const float [NV2A_VSH_CONSTANTS][4], uint32_t, uint32_t, const float [2]);
void nv2a_dispatch_sync_all(void);
void nv2a_dispatch_readback(uint32_t);
void nv2a_dispatch_invalidate(uint32_t);
int nv2a_dispatch_present(uint32_t, uint32_t, void *);
int nv2a_dispatch_surface_info(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
void nv2a_dispatch_flip(uint32_t, uint32_t);
#ifdef __cplusplus
}
#endif
#endif

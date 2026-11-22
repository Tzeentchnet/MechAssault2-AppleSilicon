#ifndef MA2_NV2A_METAL_H
#define MA2_NV2A_METAL_H

#include "kernel/nv2a_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ma2MetalRenderer Ma2MetalRenderer;

/* Clears pitched A8R8G8B8/RGB565 and draws transformed triangles, including
 * supported 2D textures and register combiners, into pitched A8R8G8B8 with
 * linear Z16/Z24S8. Unsupported states decline
 * before submission; the existing software fallback retains its own limits.
 * Work and guest-RAM readback are synchronous, and SDL owns the window. The
 * contract does not expose a full pitched allocation height, so optional
 * surface_info is omitted rather than reporting a clipped extent as full.
 *
 * Register before the executor starts; unregister only after it has stopped.
 * Log descriptors may be -1; otherwise they are duplicated for durable
 * errors. last_error remains valid until destroy, which returns 0 after a
 * fatal renderer/interop or descriptor-close failure. */
Ma2MetalRenderer *ma2_metal_create(int console_fd, int log_fd);
void ma2_metal_backend(Ma2MetalRenderer *, Nv2aBackend *);
const char *ma2_metal_last_error(const Ma2MetalRenderer *);
int ma2_metal_destroy(Ma2MetalRenderer *);

#ifdef MA2_METAL_TESTING
void ma2_metal_test_fail_next_submission(Ma2MetalRenderer *);
void ma2_metal_test_fail_next_depth_readback(Ma2MetalRenderer *);
uint64_t ma2_metal_test_submission_count(const Ma2MetalRenderer *);
uint64_t ma2_metal_test_command_count(const Ma2MetalRenderer *);
#endif

#ifdef __cplusplus
}
#endif
#endif

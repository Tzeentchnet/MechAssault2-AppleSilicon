#ifndef MA2_NV2A_METAL_MEMORY_H
#define MA2_NV2A_METAL_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *ma2_metal_translate(uint32_t va, size_t bytes);
int ma2_metal_backing(uint32_t va, size_t bytes, unsigned *domain, uint64_t *offset);
int ma2_metal_writable(uint32_t va, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif

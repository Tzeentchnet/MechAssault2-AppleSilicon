#ifndef NV2A_TEXTURE_H
#define NV2A_TEXTURE_H
#include <stddef.h>
#include "nv2a_gpu_d3d11.h"

uint32_t nv2a_texture_texel_bytes(uint32_t format);
/* Byte offset of the containing DXT block. Compressed volumes group up to
 * four Z slices per XY tile; they are not arrays of flat compressed images. */
size_t nv2a_texture_dxt_block_offset(const Nv2aGpuTexture *texture,
                                    uint32_t x,uint32_t y,uint32_t z);
/* Coordinates are already addressed; face_offset is a byte offset. Volume
 * swizzling interleaves Z with X/Y, rather than laying out ordinary 2D slices. */
int nv2a_texture_decode(const uint8_t *memory,const Nv2aGpuTexture *texture,
                        uint32_t face_offset,uint32_t x,uint32_t y,uint32_t z,
                        uint32_t *argb);
/* Pack canonicalization matches the independent RGBA8 exporter. */
int nv2a_texture_decode_canonical(const uint8_t *memory,const Nv2aGpuTexture *texture,
                        uint32_t face_offset,uint32_t x,uint32_t y,uint32_t z,uint32_t *argb);
/* The whole row y (x = 0..width-1) as nv2a_texture_decode_canonical returns it, texel for
 * texel, with the per-row and per-block work done once. 1 = decoded, 0 = a texel failed
 * (as the per-texel decode would), -1 = layout not handled here (use the per-texel path). */
int nv2a_texture_decode_row_canonical(const uint8_t *memory,const Nv2aGpuTexture *texture,
                        uint32_t face_offset,uint32_t y,uint32_t z,uint32_t *argb);
/* T2-3b H1: row y as nv2a_texture_decode returns it (x = 0..width-1); same return codes.
 * DXT and swizzled volumes return -1 (per-texel path). */
int nv2a_texture_decode_row(const uint8_t *memory,const Nv2aGpuTexture *texture,
                        uint32_t face_offset,uint32_t y,uint32_t z,uint32_t *argb);
#endif

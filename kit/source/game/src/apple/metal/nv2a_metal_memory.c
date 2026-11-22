#include "nv2a_metal_memory.h"
#include "kernel/xbox_memory_layout.h"
#include "kernel/kernel.h"

void *ma2_metal_translate(uint32_t va, size_t bytes)
{
    return xbox_TranslateMappedRam(va, bytes);
}

int ma2_metal_backing(uint32_t va, size_t bytes, unsigned *domain, uint64_t *offset)
{
    if (!bytes || bytes > UINT64_C(0x100000000) - va || !domain || !offset ||
        !xbox_TranslateMappedRam(va, bytes))
        return 0;
    uint64_t end = (uint64_t)va + bytes;
    if (va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE) {
        *domain = 1;
        *offset = va - XBOX_CONTIG_BASE;
        return 1;
    }
    if (va >= XBOX_TILED_BASE && end <= (uint64_t)XBOX_TILED_BASE + XBOX_CONTIG_SIZE) {
        *domain = 1;
        *offset = va - XBOX_TILED_BASE;
        return 1;
    }
    size_t size = xbox_GetMappedSize();
    if (!size || end > (uint64_t)size * (XBOX_NUM_MIRRORS + 1u) ||
        va / size != (end - 1) / size)
        return 0;
    *domain = 0;
    *offset = va % size;
    return 1;
}

int ma2_metal_writable(uint32_t va, size_t bytes)
{
    unsigned domain;
    uint64_t offset;
    if (!ma2_metal_backing(va, bytes, &domain, &offset)) return 0;
    return domain != 0 || !g_xbox_image_hi ||
           offset >= g_xbox_image_hi || offset + bytes <= g_xbox_image_lo;
}

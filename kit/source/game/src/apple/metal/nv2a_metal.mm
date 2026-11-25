#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "nv2a_metal.h"
#include "nv2a_metal_memory.h"
#include "nv2a_metal_shader.h"
#include "kernel/nv2a_backend_dispatch.h"
#include "d3d/d3d8_swizzle.h"

extern "C" {
#include "kernel/nv2a_texture.h"
}

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>
#include <errno.h>
#include <unistd.h>

/*
 * GPU-resident NV2A renderer.
 *
 * Guest colour and depth surfaces live in Metal textures keyed by guest
 * address. Draws and clears are encoded into batched command buffers without
 * waiting; guest RAM is updated only when the executor asks for it (sync,
 * readback, present) or when a draw samples memory a resident surface has
 * written (render to texture). invalidate() marks a surface stale after the
 * CPU path writes guest RAM, and the next use reloads it. Textures are cached
 * by descriptor and content hash, and visibility tests use Metal's exact
 * sample counting.
 */

namespace {

constexpr uint32_t kMaxExtent = 4096;
constexpr uint32_t kMaxVertices = 4096;
constexpr uint64_t kMaxTextureBytes = UINT64_C(64) * 1024 * 1024;
constexpr size_t kUploadBlock = size_t(16) << 20;
constexpr size_t kUploadBlocksBeforeSync = 8;
constexpr uint32_t kVisibilitySlots = 4096;
constexpr uint32_t kDrawsPerCommit = 256;
constexpr uint64_t kTextureCacheBytes = UINT64_C(512) << 20;
constexpr size_t kMaxTargets = 64;
constexpr uint32_t kMinTargetExtent = 1024;

struct Surface {
    uint32_t va, pitch, bpp;
    size_t bytes;
    uint8_t *base;
};

struct Geometry {
    uint32_t width, height, x, y, w, h;
};

struct Vertex {
    float position[4];
    float color[4];
    float secondary[4];
    float fog[4];
    float tex[4][4];
};

struct Viewport {
    float width, height, depth_max, unused;
};

struct Clear {
    uint32_t width, height, color_format, depth_format;
    uint32_t mask, color, depth, stencil;
};

struct ClearValue {
    float color[4];
    float depth[4];
};

struct alignas(16) Float4 { float value[4]; };
struct alignas(16) Uint4 { uint32_t value[4]; };

struct Guest {
    Uint4 control, final_words;
    Float4 fog_color, final_c0, final_c1;
    Uint4 programs[8];
    Float4 factor0[8], factor1[8], tex_info[4];
    Uint4 texture_control;
    Float4 bump_matrix[4], bump_luminance[4], eye_vector, texture_bias, misc;
};

static_assert(sizeof(Guest) == 45 * 16, "Metal Guest layout");
static_assert(sizeof(Vertex) == 8 * 16, "Metal Vertex layout");

struct TextureLevel {
    Nv2aGpuTexture descriptor;
    uint64_t bytes;
};

struct TextureUpload {
    bool sampled = false, mipmapped = false, min_linear = false, mag_linear = false;
    bool cube = false;
    float min_lod = 0, max_lod = 0;
    uint64_t span = 0;         // guest bytes behind every level (and face)
    uint64_t face_stride = 0;  // cube: one face with its levels, 128-byte aligned
    std::vector<TextureLevel> levels;
};

enum class Failure { Device, Shader, Resource, Pipeline, Command, Memory, Interop };

/* Why an operation stayed on the CPU path; reported with RECOMP_PERF. */
enum Decline : unsigned {
    DeclineSurface, DeclineState, DeclineStencil, DeclinePolygonOffset, DeclineAlias,
    DeclinePrimitive, DeclineVertexCount, DeclineNonFinite, DeclineBehindEye,
    DeclineTexture, DeclineTextureStage, DeclineTextureFormat, DeclineTextureDecode,
    DeclineCount
};
const char *const kDeclineNames[DeclineCount] = {
    "surface", "state", "stencil", "polygon_offset", "alias",
    "primitive", "vertex_count", "non_finite", "clip_overflow",
    "texture", "texture_stage", "texture_format", "texture_decode"
};

/* Half-open pixel rectangle; empty when nothing is pending. */
struct Rect {
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    void clear() { *this = Rect(); }
    void add(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x + w); y1 = std::max(y1, y + h);
    }
};

/* A guest surface mirrored by a GPU texture. width/height is the guest extent
 * the texture mirrors; the texture itself may be larger so colour and depth
 * attachments of one pass can share dimensions. */
struct Resident {
    uint32_t va = 0, pitch = 0, bpp = 0, format = 0, width = 0, height = 0;
    unsigned domain = 0;
    uint64_t offset = 0;
    size_t bytes = 0;
    bool depth = false;
    bool stale = false;   // guest RAM is newer than the texture
    Rect dirty;           // texture is newer than guest RAM here
    __strong id<MTLTexture> texture = nil;
};

struct Upload {
    __strong id<MTLBuffer> buffer = nil;
    size_t used = 0;
};

struct CachedTexture {
    __strong id<MTLTexture> texture = nil;
    uint64_t bytes = 0, used = 0;
};

struct ContentMemo {
    uint64_t content = 0, epoch = 0;
};

static inline uint64_t rotl(uint64_t v, unsigned r) { return (v << r) | (v >> (64 - r)); }

static inline uint64_t mix64(uint64_t h)
{
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33; h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}

/* Fast non-cryptographic content hash for cache keys. */
static uint64_t hash_bytes(const void *data, size_t n, uint64_t seed)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    const uint64_t k1 = UINT64_C(0x9e3779b97f4a7c15), k2 = UINT64_C(0xc2b2ae3d27d4eb4f);
    uint64_t a = seed ^ k1, b = seed + n, c = rotl(seed, 17) ^ k2, d = ~seed;
    while (n >= 32) {
        uint64_t w[4];
        memcpy(w, p, sizeof(w));
        a = rotl(a + w[0] * k2, 31) * k1;
        b = rotl(b + w[1] * k2, 31) * k1;
        c = rotl(c + w[2] * k2, 31) * k1;
        d = rotl(d + w[3] * k2, 31) * k1;
        p += 32; n -= 32;
    }
    uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18);
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = rotl(h ^ mix64(w), 27) * k1 + k2;
        p += 8; n -= 8;
    }
    uint64_t tail = 0;
    memcpy(&tail, p, n);
    return mix64(h ^ mix64(tail ^ (uint64_t)n));
}

} // namespace

struct Ma2MetalRenderer {
    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> queue = nil;
    __strong id<MTLLibrary> library = nil;
    __strong id<MTLFunction> vertex = nil;
    __strong id<MTLFunction> color = nil;
    __strong id<MTLFunction> color_depth = nil;
    __strong id<MTLFunction> shaded = nil;
    __strong id<MTLFunction> shaded_depth = nil;
    __strong id<MTLFunction> clear_vertex = nil;
    __strong id<MTLFunction> clear_color = nil;
    __strong id<MTLFunction> clear_depth = nil;
    __strong id<MTLComputePipelineState> clear = nil;
    __strong NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *pipelines = nil;
    __strong NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *samplers = nil;
    __strong NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *depth_states = nil;
    __strong id<MTLTexture> white = nil;
    __strong id<MTLTexture> white_cube = nil;
    __strong id<MTLSamplerState> white_sampler = nil;

    std::vector<std::unique_ptr<Resident>> surfaces;

    __strong id<MTLCommandBuffer> command = nil;
    __strong id<MTLRenderCommandEncoder> encoder = nil;
    __strong NSMutableArray<id<MTLCommandBuffer>> *inflight = nil;
    Resident *pass_color = nullptr, *pass_depth = nullptr;
    uint32_t command_draws = 0;

    std::vector<Upload> uploads;
    size_t upload_index = 0;

    __strong id<MTLBuffer> visibility = nil;
    uint32_t visibility_next = 0;
    int visibility_slot = -1;
    bool visibility_counting = false;
    std::vector<uint32_t> visibility_pending;
    uint64_t visibility_resolved = 0;

    std::unordered_map<uint64_t, CachedTexture> textures;
    std::unordered_map<uint64_t, ContentMemo> memo;
    uint64_t texture_bytes = 0, epoch = 1, texture_clock = 0;

    uint64_t submissions = 0, waits = 0;
    uint64_t clear_accepted = 0, clear_declined = 0;
    uint64_t draw_accepted = 0, draw_declined = 0;
    uint64_t declines[DeclineCount] = {};
    uint64_t color_loads = 0, depth_loads = 0, color_writebacks = 0, depth_writebacks = 0;
    uint64_t texture_hits = 0, texture_misses = 0;
    int console_fd = -1, log_fd = -1;
    bool perf = false;
    bool failed = false;
    char error[128] = {};
#ifdef MA2_METAL_TESTING
    bool fail_next_submission = false;
    bool fail_next_depth_readback = false;
    uint64_t command_buffers = 0;
#endif
};

namespace {

static void write_record(int fd, const char *text)
{
    if (fd < 0) return;
    size_t left = strlen(text);
    while (left) {
        ssize_t n = write(fd, text, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text += n;
        left -= (size_t)n;
    }
}

static void fail(Ma2MetalRenderer *m, Failure kind, const char *detail,
                 NSError *error = nil, bool notify_dispatch = true)
{
    if (!m || m->failed) return;
    m->failed = true;
    const char *name = "unknown";
    const char *record = nullptr;
    switch (kind) {
    case Failure::Device:
        name = "device";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_device_failed\",\"message\":\"Metal device or command queue initialization failed.\"}\n";
        break;
    case Failure::Shader:
        name = "shader";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_shader_failed\",\"message\":\"Metal shader initialization failed.\"}\n";
        break;
    case Failure::Resource:
        name = "resource";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_resource_failed\",\"message\":\"Metal renderer resource allocation failed; the frame was discarded.\"}\n";
        break;
    case Failure::Pipeline:
        name = "pipeline";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_pipeline_failed\",\"message\":\"Metal render pipeline creation failed; the frame was discarded.\"}\n";
        break;
    case Failure::Command:
        name = "command";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_command_failed\",\"message\":\"Metal command execution failed; the frame was discarded.\"}\n";
        break;
    case Failure::Memory:
        name = "memory";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_memory_failed\",\"message\":\"Metal renderer host memory allocation failed; the frame was discarded.\"}\n";
        break;
    case Failure::Interop:
        name = "interop";
        record = "{\"level\":\"error\",\"event\":\"guest_metal_interop_failed\",\"message\":\"Metal/software surface interop failed; the frame was discarded.\"}\n";
        break;
    }
    snprintf(m->error, sizeof(m->error), "%s: %s", name, detail ? detail : "failure");
    write_record(m->console_fd, record);
    if (m->log_fd != m->console_fd) write_record(m->log_fd, record);
    fprintf(stderr, "[NV2A-METAL] %s%s%s\n", m->error,
            error ? ": " : "", error ? error.localizedDescription.UTF8String : "");
    if (notify_dispatch) nv2a_dispatch_report_failure("Metal renderer");
}

static bool geometry(const Nv2aGpuState *s, Geometry &g)
{
    if (!s || (s->format & 0xf000u) || ((s->format >> 8) & 15u) > 1u)
        return false;
    uint32_t format = s->format & 15u;
    if (!((format == 8 && s->bpp == 4) || (format == 3 && s->bpp == 2)) ||
        !s->pitch || s->pitch % s->bpp)
        return false;
    uint64_t right = (uint64_t)s->clip_x + s->clip_w;
    uint64_t bottom = (uint64_t)s->clip_y + s->clip_h;
    uint32_t width = s->pitch / s->bpp;
    if (!width || width > kMaxExtent || !s->clip_w || !s->clip_h ||
        right > width || bottom > kMaxExtent)
        return false;
    g = {width, (uint32_t)bottom, s->clip_x, s->clip_y, s->clip_w, s->clip_h};
    return true;
}

static bool mapped_surface(uint32_t va, uint32_t pitch, uint32_t bpp,
                           const Geometry &g, Surface &out)
{
    if (!va || !pitch || !bpp || pitch % bpp ||
        ((uint64_t)g.x + g.w) * bpp > pitch)
        return false;
    uint64_t bytes = ((uint64_t)g.y + g.h - 1) * pitch
                   + ((uint64_t)g.x + g.w) * bpp;
    if (!bytes || bytes > SIZE_MAX || bytes > UINT64_C(0x100000000) - va)
        return false;
    if (!ma2_metal_writable(va, (size_t)bytes)) return false;
    void *base = ma2_metal_translate(va, (size_t)bytes);
    if (!base) return false;
    out = {va, pitch, bpp, (size_t)bytes, static_cast<uint8_t *>(base)};
    return true;
}

static bool overlap(const Surface &a, const Surface &b)
{
    unsigned da, db;
    uint64_t aa, bb;
    if (!ma2_metal_backing(a.va, a.bytes, &da, &aa) ||
        !ma2_metal_backing(b.va, b.bytes, &db, &bb))
        return true;
    return da == db && aa < bb + b.bytes && bb < aa + a.bytes;
}

static bool depth_pitch(const Nv2aGpuState *s, const Geometry &g, uint32_t &bpp, uint32_t &pitch)
{
    uint32_t zformat = (s->format >> 4) & 15u;
    if (zformat != 1 && zformat != 2) return false;
    bpp = zformat == 1 ? 2 : 4;
    uint64_t value = s->depth_pitch ? s->depth_pitch : (uint64_t)g.width * bpp;
    if (value > UINT32_MAX) return false;
    pitch = (uint32_t)value;
    return true;
}

static bool depth_surface(const Nv2aGpuState *s, const Geometry &g, Surface &depth)
{
    uint32_t bpp, pitch;
    return depth_pitch(s, g, bpp, pitch) && mapped_surface(s->depth_va, pitch, bpp, g, depth);
}

static void copy_from_guest(void *dst, const Surface &surface, const Geometry &g)
{
    size_t row = (size_t)g.w * surface.bpp;
    for (uint32_t y = 0; y < g.h; ++y)
        memcpy(static_cast<uint8_t *>(dst) + (size_t)y * row,
               surface.base + (size_t)(g.y + y) * surface.pitch + (size_t)g.x * surface.bpp,
               row);
}

static void copy_to_guest(const Surface &surface, const Geometry &g, const void *src)
{
    size_t row = (size_t)g.w * surface.bpp;
    for (uint32_t y = 0; y < g.h; ++y)
        memcpy(surface.base + (size_t)(g.y + y) * surface.pitch + (size_t)g.x * surface.bpp,
               static_cast<const uint8_t *>(src) + (size_t)y * row, row);
}

// ---------------------------------------------------------------------------
// Command submission

static void end_encoder(Ma2MetalRenderer *m)
{
    if (m->encoder) [m->encoder endEncoding];
    m->encoder = nil;
    m->pass_color = m->pass_depth = nullptr;
    m->visibility_slot = -1;
    m->visibility_counting = false;
}

static bool ensure_command(Ma2MetalRenderer *m)
{
    if (m->command) return true;
#ifdef MA2_METAL_TESTING
    ++m->command_buffers;
#endif
    m->command = [m->queue commandBuffer];
    m->command_draws = 0;
    if (!m->command) {
        fail(m, Failure::Resource, "command buffer");
        return false;
    }
    return true;
}

/* Submit encoded work without waiting for it. */
static void commit(Ma2MetalRenderer *m)
{
    end_encoder(m);
    if (!m->command) return;
    [m->command commit];
    ++m->submissions;
    [m->inflight addObject:m->command];
    m->command = nil;
}

/* Submit and complete all work, then resolve visibility and recycle uploads. */
static bool finish(Ma2MetalRenderer *m)
{
    commit(m);
    if (!m->inflight.count) return !m->failed;
    ++m->waits;
    for (id<MTLCommandBuffer> command in m->inflight) {
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted)
            fail(m, Failure::Command, "command buffer", command.error);
    }
    [m->inflight removeAllObjects];
#ifdef MA2_METAL_TESTING
    if (m->fail_next_submission) {
        m->fail_next_submission = false;
        fail(m, Failure::Command, "injected submission failure");
    }
#endif
    const uint64_t *values = static_cast<const uint64_t *>(m->visibility.contents);
    for (uint32_t slot : m->visibility_pending) m->visibility_resolved += values[slot];
    m->visibility_pending.clear();
    m->visibility_next = 0;
    for (Upload &upload : m->uploads) upload.used = 0;
    m->upload_index = 0;
    return !m->failed;
}

static bool upload(Ma2MetalRenderer *m, const void *data, size_t bytes,
                   __strong id<MTLBuffer> &buffer, NSUInteger &offset)
{
    size_t aligned = (bytes + 255) & ~size_t(255);
    while (m->upload_index < m->uploads.size() &&
           m->uploads[m->upload_index].used + aligned > m->uploads[m->upload_index].buffer.length)
        ++m->upload_index;
    if (m->upload_index == m->uploads.size()) {
        if (m->uploads.size() >= kUploadBlocksBeforeSync) {
            if (!finish(m)) return false;
        } else {
            id<MTLBuffer> block = [m->device newBufferWithLength:std::max(kUploadBlock, aligned)
                                                         options:MTLResourceStorageModeShared |
                                                                 MTLResourceCPUCacheModeWriteCombined];
            if (!block) {
                fail(m, Failure::Resource, "vertex upload buffer");
                return false;
            }
            m->uploads.push_back({block, 0});
        }
        while (m->upload_index < m->uploads.size() &&
               m->uploads[m->upload_index].used + aligned > m->uploads[m->upload_index].buffer.length)
            ++m->upload_index;
        if (m->upload_index == m->uploads.size()) {
            fail(m, Failure::Resource, "vertex upload capacity");
            return false;
        }
    }
    Upload &target = m->uploads[m->upload_index];
    memcpy(static_cast<uint8_t *>(target.buffer.contents) + target.used, data, bytes);
    buffer = target.buffer;
    offset = target.used;
    target.used += aligned;
    return true;
}

// ---------------------------------------------------------------------------
// Resident surfaces

static double depth_maximum(uint32_t zformat) { return zformat == 1 ? 65535.0 : 16777215.0; }

/* Z24S8 keeps its stencil resident beside the depth. */
static bool has_stencil(const Resident &r) { return r.depth && r.format == 2; }

static id<MTLBlitCommandEncoder> blit_encoder(Ma2MetalRenderer *m)
{
    end_encoder(m);
    if (!ensure_command(m)) return nil;
    id<MTLBlitCommandEncoder> blit = [m->command blitCommandEncoder];
    if (!blit) fail(m, Failure::Resource, "blit encoder");
    return blit;
}

static id<MTLBuffer> staging(Ma2MetalRenderer *m, size_t bytes)
{
    id<MTLBuffer> buffer = [m->device newBufferWithLength:std::max<size_t>(bytes, 4)
                                                  options:MTLResourceStorageModeShared];
    if (!buffer) fail(m, Failure::Resource, "surface staging buffer");
    return buffer;
}

/* A depth readback encoded into the current command buffer, converted to
 * guest RAM once that buffer completes. */
struct DepthReadback {
    Resident *surface = nullptr;
    Rect rect;
    __strong id<MTLBuffer> depth = nil, stencil = nil;
};

static Rect dirty_extent(const Resident &r)
{
    Rect d = r.dirty;
    d.x1 = std::min(d.x1, r.width);
    d.y1 = std::min(d.y1, r.height);
    return d;
}

static bool encode_depth_readback(Ma2MetalRenderer *m, Resident &r, DepthReadback &out)
{
    Rect d = dirty_extent(r);
    out = DepthReadback();
    out.surface = &r;
    out.rect = d;
    if (d.empty()) return true;
    uint32_t w = d.x1 - d.x0, h = d.y1 - d.y0;
    bool stencil = has_stencil(r);
    out.depth = staging(m, (size_t)w * h * sizeof(float));
    out.stencil = stencil ? staging(m, (size_t)w * h) : nil;
    if (!out.depth || (stencil && !out.stencil)) return false;
    id<MTLBlitCommandEncoder> blit = blit_encoder(m);
    if (!blit) return false;
    MTLOrigin origin = {d.x0, d.y0, 0};
    MTLSize size = {w, h, 1};
    [blit copyFromTexture:r.texture sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size
                 toBuffer:out.depth destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4
  destinationBytesPerImage:(NSUInteger)w * h * 4
                  options:stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    if (stencil)
        [blit copyFromTexture:r.texture sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size
                     toBuffer:out.stencil destinationOffset:0 destinationBytesPerRow:w
      destinationBytesPerImage:(NSUInteger)w * h options:MTLBlitOptionStencilFromDepthStencil];
    [blit endEncoding];
    return true;
}

/* After the readback's command buffer completed. */
static bool complete_depth_readback(Ma2MetalRenderer *m, const DepthReadback &readback)
{
    Resident &r = *readback.surface;
    const Rect &d = readback.rect;
    if (d.empty()) {
        r.dirty.clear();
        return true;
    }
#ifdef MA2_METAL_TESTING
    if (m->fail_next_depth_readback) {
        m->fail_next_depth_readback = false;
        fail(m, Failure::Command, "injected depth readback failure");
        return false;
    }
#endif
    uint8_t *base = static_cast<uint8_t *>(ma2_metal_translate(r.va, r.bytes));
    if (!base) {
        fail(m, Failure::Interop, "resident surface writeback mapping");
        return false;
    }
    ++m->depth_writebacks;
    uint32_t w = d.x1 - d.x0, h = d.y1 - d.y0;
    const float *values = static_cast<const float *>(readback.depth.contents);
    const uint8_t *stencils = readback.stencil ? static_cast<const uint8_t *>(readback.stencil.contents) : nullptr;
    double maximum = depth_maximum(r.format);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = base + (size_t)(d.y0 + y) * r.pitch + (size_t)d.x0 * r.bpp;
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            float value = std::max(0.0f, std::min(1.0f, values[i]));
            uint32_t z = (uint32_t)std::round((double)value * maximum);
            uint32_t packed = r.format == 2 ? (z << 8) | stencils[i] : z;
            memcpy(row + (size_t)x * r.bpp, &packed, r.bpp);
        }
    }
    r.dirty.clear();
    ++m->epoch;
    return true;
}

static bool write_back_color(Ma2MetalRenderer *m, Resident &r)
{
    Rect d = dirty_extent(r);
    if (d.empty()) {
        r.dirty.clear();
        return true;
    }
    uint8_t *base = static_cast<uint8_t *>(ma2_metal_translate(r.va, r.bytes));
    if (!base) {
        fail(m, Failure::Interop, "resident surface writeback mapping");
        return false;
    }
    ++m->color_writebacks;
    [r.texture getBytes:base + (size_t)d.y0 * r.pitch + (size_t)d.x0 * 4
            bytesPerRow:r.pitch fromRegion:MTLRegionMake2D(d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0)
            mipmapLevel:0];
    r.dirty.clear();
    ++m->epoch;
    return true;
}

static bool write_back(Ma2MetalRenderer *m, Resident &r)
{
    if (r.dirty.empty()) return true;
    if (!r.depth) return finish(m) && write_back_color(m, r);
    DepthReadback readback;
    return encode_depth_readback(m, r, readback) && finish(m) &&
           complete_depth_readback(m, readback);
}

/* Copy guest RAM into the texture. Colour textures have no pending GPU work;
 * depth and stencil are uploaded in command order with a blit. */
static bool load(Ma2MetalRenderer *m, Resident &r)
{
    const uint8_t *base = static_cast<const uint8_t *>(ma2_metal_translate(r.va, r.bytes));
    if (!base) {
        fail(m, Failure::Interop, "resident surface load mapping");
        return false;
    }
    if (!r.depth) {
        ++m->color_loads;
        [r.texture replaceRegion:MTLRegionMake2D(0, 0, r.width, r.height) mipmapLevel:0
                       withBytes:base bytesPerRow:r.pitch];
        r.stale = false;
        return true;
    }
    ++m->depth_loads;
    bool stencil = has_stencil(r);
    size_t pixels = (size_t)r.width * r.height;
    id<MTLBuffer> depth_values = staging(m, pixels * sizeof(float));
    id<MTLBuffer> stencil_values = stencil ? staging(m, pixels) : nil;
    if (!depth_values || (stencil && !stencil_values)) return false;
    float *values = static_cast<float *>(depth_values.contents);
    uint8_t *stencils = stencil ? static_cast<uint8_t *>(stencil_values.contents) : nullptr;
    double maximum = depth_maximum(r.format);
    for (uint32_t y = 0; y < r.height; ++y) {
        const uint8_t *row = base + (size_t)y * r.pitch;
        for (uint32_t x = 0; x < r.width; ++x) {
            uint32_t packed = 0;
            memcpy(&packed, row + (size_t)x * r.bpp, r.bpp);
            size_t i = (size_t)y * r.width + x;
            values[i] = (float)((double)(r.format == 1 ? packed : packed >> 8) / maximum);
            if (stencils) stencils[i] = (uint8_t)packed;
        }
    }
    id<MTLBlitCommandEncoder> blit = blit_encoder(m);
    if (!blit) return false;
    MTLSize size = {r.width, r.height, 1};
    [blit copyFromBuffer:depth_values sourceOffset:0 sourceBytesPerRow:(NSUInteger)r.width * 4
       sourceBytesPerImage:pixels * 4 sourceSize:size toTexture:r.texture destinationSlice:0
          destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)
                   options:stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    if (stencil)
        [blit copyFromBuffer:stencil_values sourceOffset:0 sourceBytesPerRow:r.width
           sourceBytesPerImage:pixels sourceSize:size toTexture:r.texture destinationSlice:0
              destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)
                       options:MTLBlitOptionStencilFromDepthStencil];
    [blit endEncoding];
    r.stale = false;
    return true;
}

/* Depth first: a failed depth readback then leaves guest colour untouched,
 * so a discarded frame never publishes half of its results. */
static bool sync_surfaces(Ma2MetalRenderer *m)
{
    // Every depth readback rides in the frame's command buffer: one wait.
    std::vector<DepthReadback> readbacks;
    for (auto &surface : m->surfaces)
        if (surface->depth && !surface->dirty.empty()) {
            readbacks.emplace_back();
            if (!encode_depth_readback(m, *surface, readbacks.back())) return false;
        }
    if (!finish(m)) return false;
    for (const DepthReadback &readback : readbacks)
        if (!complete_depth_readback(m, readback)) return false;
    for (auto &surface : m->surfaces)
        if (!surface->depth && !write_back_color(m, *surface)) return false;
    return true;
}

static bool contains(const Resident &r, uint32_t va)
{
    return va >= r.va && (uint64_t)va < (uint64_t)r.va + r.bytes;
}

static bool aliases(const Resident &r, unsigned domain, uint64_t offset, size_t bytes)
{
    return r.domain == domain && r.offset < offset + bytes && offset < r.offset + r.bytes;
}

/* Write back and forget every resident surface sharing guest memory with the
 * range, except keep. Fails without side effects on keep if keep aliases. */
static bool evict_aliases(Ma2MetalRenderer *m, unsigned domain, uint64_t offset, size_t bytes,
                          const Resident *keep, bool &keep_aliased)
{
    keep_aliased = keep && aliases(*keep, domain, offset, bytes);
    if (keep_aliased) return true;
    bool any = false;
    for (auto &surface : m->surfaces)
        if (aliases(*surface, domain, offset, bytes)) any = true;
    if (!any) return true;
    if (!finish(m)) return false;
    for (size_t i = 0; i < m->surfaces.size();) {
        Resident &r = *m->surfaces[i];
        if (!aliases(r, domain, offset, bytes)) { ++i; continue; }
        if (!write_back(m, r)) return false;
        m->surfaces.erase(m->surfaces.begin() + (ptrdiff_t)i);
    }
    return true;
}

static MTLPixelFormat surface_format(bool depth, uint32_t format)
{
    if (!depth) return MTLPixelFormatBGRA8Unorm;
    return format == 2 ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
}

static id<MTLTexture> surface_texture(Ma2MetalRenderer *m, bool depth, uint32_t format,
                                      uint32_t width, uint32_t height)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:surface_format(depth, format)
                                    width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared;
    id<MTLTexture> texture = [m->device newTextureWithDescriptor:desc];
    if (!texture) fail(m, Failure::Resource, depth ? "resident depth texture" : "resident color texture");
    return texture;
}

static uint32_t texture_extent(uint32_t needed)
{
    return std::min(kMaxExtent, std::max(kMinTargetExtent, (needed + 63u) & ~63u));
}

/* Find or create the resident surface for a guest range. *declined is set
 * when the surface cannot be represented; no GPU command has been issued. */
static Resident *resident(Ma2MetalRenderer *m, bool depth, uint32_t va, uint32_t pitch,
                          uint32_t bpp, uint32_t format, uint32_t height,
                          const Resident *keep, bool &declined)
{
    declined = false;
    uint32_t width = pitch / bpp;
    for (auto &surface : m->surfaces) {
        Resident &r = *surface;
        if (r.depth != depth || r.va != va || r.pitch != pitch || r.format != format) continue;
        if (r.height >= height) {
            if (r.stale && !load(m, r)) return nullptr;
            return &r;
        }
        height = std::max(height, r.height);
        break;
    }
    Surface mapped;
    Geometry whole = {width, height, 0, 0, width, height};
    unsigned domain;
    uint64_t offset;
    if (!mapped_surface(va, pitch, bpp, whole, mapped) ||
        !ma2_metal_backing(va, mapped.bytes, &domain, &offset)) {
        declined = true;
        return nullptr;
    }
    bool keep_aliased = false;
    if (!evict_aliases(m, domain, offset, mapped.bytes, keep, keep_aliased)) return nullptr;
    if (keep_aliased) {
        declined = true;
        return nullptr;
    }
    if (m->surfaces.size() >= kMaxTargets) {
        if (!finish(m)) return nullptr;
        for (size_t i = 0; i < m->surfaces.size() && m->surfaces.size() >= kMaxTargets;) {
            if (m->surfaces[i].get() == keep) { ++i; continue; }
            if (!write_back(m, *m->surfaces[i])) return nullptr;
            m->surfaces.erase(m->surfaces.begin() + (ptrdiff_t)i);
        }
    }
    auto r = std::make_unique<Resident>();
    r->va = va; r->pitch = pitch; r->bpp = bpp; r->format = format;
    r->width = width; r->height = height;
    r->domain = domain; r->offset = offset; r->bytes = mapped.bytes;
    r->depth = depth;
    r->texture = surface_texture(m, depth, format, texture_extent(width), texture_extent(height));
    if (!r->texture || !load(m, *r)) return nullptr;
    m->surfaces.push_back(std::move(r));
    return m->surfaces.back().get();
}

/* Give a pass's colour and depth attachments identical texture dimensions. */
static bool match_attachments(Ma2MetalRenderer *m, Resident *color, Resident *depth)
{
    if (!color || !depth) return true;
    NSUInteger w = std::max(color->texture.width, depth->texture.width);
    NSUInteger h = std::max(color->texture.height, depth->texture.height);
    for (Resident *r : {color, depth}) {
        if (r->texture.width == w && r->texture.height == h) continue;
        if (!finish(m) || !write_back(m, *r)) return false;
        r->texture = surface_texture(m, r->depth, r->format, (uint32_t)w, (uint32_t)h);
        if (!r->texture || !load(m, *r)) return false;
    }
    return true;
}

static bool begin_pass(Ma2MetalRenderer *m, Resident *color, Resident *depth)
{
    if (m->encoder && m->pass_color == color && (!depth || m->pass_depth == depth)) return true;
    end_encoder(m);
    if (!match_attachments(m, color, depth) || !ensure_command(m)) return false;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color) {
        pass.colorAttachments[0].texture = color->texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        pass.depthAttachment.texture = depth->texture;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (has_stencil(*depth)) {
            pass.stencilAttachment.texture = depth->texture;
            pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    pass.visibilityResultBuffer = m->visibility;
    m->encoder = [m->command renderCommandEncoderWithDescriptor:pass];
    if (!m->encoder) {
        fail(m, Failure::Resource, "render encoder");
        return false;
    }
    m->pass_color = color;
    m->pass_depth = depth;
    return true;
}

/* Route this draw's depth-passing samples into the open visibility test. */
static void set_visibility(Ma2MetalRenderer *m, bool counted)
{
    if (counted) {
        if (m->visibility_slot < 0) {
            uint32_t slot = m->visibility_next++;
            static_cast<uint64_t *>(m->visibility.contents)[slot] = 0;
            m->visibility_pending.push_back(slot);
            m->visibility_slot = (int)slot;
            m->visibility_counting = false;
        }
        if (!m->visibility_counting) {
            [m->encoder setVisibilityResultMode:MTLVisibilityResultModeCounting
                                         offset:(NSUInteger)m->visibility_slot * sizeof(uint64_t)];
            m->visibility_counting = true;
        }
    } else if (m->visibility_counting) {
        [m->encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        m->visibility_counting = false;
    }
}

static void after_draw(Ma2MetalRenderer *m)
{
    if (++m->command_draws >= kDrawsPerCommit) commit(m);
}

// ---------------------------------------------------------------------------
// State objects

static bool blend_factor(uint32_t value, MTLBlendFactor &rgb, MTLBlendFactor &alpha,
                         bool destination)
{
    switch (value) {
    case 0: rgb = alpha = MTLBlendFactorZero; return true;
    case 1: rgb = alpha = MTLBlendFactorOne; return true;
    case 0x300: rgb = MTLBlendFactorSourceColor; alpha = MTLBlendFactorSourceAlpha; return true;
    case 0x301: rgb = MTLBlendFactorOneMinusSourceColor; alpha = MTLBlendFactorOneMinusSourceAlpha; return true;
    case 0x302: rgb = alpha = MTLBlendFactorSourceAlpha; return true;
    case 0x303: rgb = alpha = MTLBlendFactorOneMinusSourceAlpha; return true;
    case 0x304: rgb = alpha = MTLBlendFactorDestinationAlpha; return true;
    case 0x305: rgb = alpha = MTLBlendFactorOneMinusDestinationAlpha; return true;
    case 0x306: rgb = MTLBlendFactorDestinationColor; alpha = MTLBlendFactorDestinationAlpha; return true;
    case 0x307: rgb = MTLBlendFactorOneMinusDestinationColor; alpha = MTLBlendFactorOneMinusDestinationAlpha; return true;
    case 0x308:
        if (destination) return false;
        rgb = MTLBlendFactorSourceAlphaSaturated; alpha = MTLBlendFactorOne; return true;
    case 0x8001: rgb = MTLBlendFactorBlendColor; alpha = MTLBlendFactorBlendAlpha; return true;
    case 0x8002: rgb = MTLBlendFactorOneMinusBlendColor; alpha = MTLBlendFactorOneMinusBlendAlpha; return true;
    case 0x8003: rgb = alpha = MTLBlendFactorBlendAlpha; return true;
    case 0x8004: rgb = alpha = MTLBlendFactorOneMinusBlendAlpha; return true;
    default: return false;
    }
}

static bool blend_operation(uint32_t value, MTLBlendOperation &out)
{
    switch (value) {
    case 0: case 0x8006: out = MTLBlendOperationAdd; return true;
    case 0x800a: out = MTLBlendOperationSubtract; return true;
    case 0x800b: out = MTLBlendOperationReverseSubtract; return true;
    case 0x8007: out = MTLBlendOperationMin; return true;
    case 0x8008: out = MTLBlendOperationMax; return true;
    default: return false;
    }
}

static MTLColorWriteMask color_write_mask(uint32_t mask)
{
    NSUInteger result = 0;
    if (mask & 0x00010000u) result |= MTLColorWriteMaskRed;
    if (mask & 0x00000100u) result |= MTLColorWriteMaskGreen;
    if (mask & 0x00000001u) result |= MTLColorWriteMaskBlue;
    if (mask & 0x01000000u) result |= MTLColorWriteMaskAlpha;
    return (MTLColorWriteMask)result;
}

static id<MTLRenderPipelineState> make_pipeline(Ma2MetalRenderer *m, NSString *key,
                                                MTLRenderPipelineDescriptor *desc)
{
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [m->device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!result) {
        fail(m, Failure::Pipeline, "render pipeline", error);
        return nil;
    }
    if (m->pipelines.count >= 1024) [m->pipelines removeAllObjects];
    m->pipelines[key] = result;
    return result;
}

static void attachment_formats(MTLRenderPipelineDescriptor *desc, MTLPixelFormat depth)
{
    desc.depthAttachmentPixelFormat = depth;
    desc.stencilAttachmentPixelFormat =
        depth == MTLPixelFormatDepth32Float_Stencil8 ? depth : MTLPixelFormatInvalid;
}

static id<MTLRenderPipelineState> pipeline(Ma2MetalRenderer *m, const Nv2aGpuState *s,
                                           MTLPixelFormat depth)
{
    bool shaded = s->use_rc || s->textured;
    NSString *key = [NSString stringWithFormat:@"d%u:%u:%u:%u:%u:%u:%u:%lu",
        s->depth_test != 0, s->blend_enable != 0, s->blend_src, s->blend_dst,
        s->blend_equation, s->color_mask, shaded, (unsigned long)depth];
    id<MTLRenderPipelineState> cached = m->pipelines[key];
    if (cached) return cached;
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = m->vertex;
    desc.fragmentFunction = shaded ? (s->depth_test ? m->shaded_depth : m->shaded)
                                   : (s->depth_test ? m->color_depth : m->color);
    attachment_formats(desc, depth);
    MTLRenderPipelineColorAttachmentDescriptor *attachment = desc.colorAttachments[0];
    attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
    attachment.writeMask = color_write_mask(s->color_mask);
    attachment.blendingEnabled = s->blend_enable != 0;
    if (s->blend_enable) {
        MTLBlendFactor sr, sa, dr, da;
        MTLBlendOperation op;
        if (!blend_factor(s->blend_src, sr, sa, false) ||
            !blend_factor(s->blend_dst, dr, da, true) ||
            !blend_operation(s->blend_equation, op)) return nil;
        attachment.sourceRGBBlendFactor = sr;
        attachment.sourceAlphaBlendFactor = sa;
        attachment.destinationRGBBlendFactor = dr;
        attachment.destinationAlphaBlendFactor = da;
        attachment.rgbBlendOperation = attachment.alphaBlendOperation = op;
    }
    return make_pipeline(m, key, desc);
}

static id<MTLRenderPipelineState> clear_pipeline(Ma2MetalRenderer *m, bool has_color,
                                                 MTLColorWriteMask mask, MTLPixelFormat depth)
{
    NSString *key = [NSString stringWithFormat:@"c%u:%lu:%lu", has_color,
                     (unsigned long)mask, (unsigned long)depth];
    id<MTLRenderPipelineState> cached = m->pipelines[key];
    if (cached) return cached;
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = m->clear_vertex;
    desc.fragmentFunction = depth != MTLPixelFormatInvalid ? m->clear_depth : m->clear_color;
    attachment_formats(desc, depth);
    desc.colorAttachments[0].pixelFormat = has_color ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    desc.colorAttachments[0].writeMask = has_color ? mask : MTLColorWriteMaskNone;
    return make_pipeline(m, key, desc);
}

/* Depth/stencil test for one draw. func is 0..7 for NV2A's 0x200..0x207, or
 * 8 for "always" on a draw without a depth test. Ops are MTLStencilOperation. */
struct DepthStencil {
    uint32_t func = 8;
    bool write = false, stencil = false;
    uint32_t stencil_func = 7, fail = 0, zfail = 0, zpass = 0, read = 0xff, mask = 0xff;
};

static bool stencil_operation(uint32_t op, uint32_t &out)
{
    switch (op) {
    case 0x1e00: out = MTLStencilOperationKeep; return true;
    case 0: out = MTLStencilOperationZero; return true;
    case 0x1e01: out = MTLStencilOperationReplace; return true;
    case 0x1e02: out = MTLStencilOperationIncrementClamp; return true;
    case 0x1e03: out = MTLStencilOperationDecrementClamp; return true;
    case 0x150a: out = MTLStencilOperationInvert; return true;
    case 0x8507: out = MTLStencilOperationIncrementWrap; return true;
    case 0x8508: out = MTLStencilOperationDecrementWrap; return true;
    default: return false;
    }
}

/* The draw's depth/stencil test, or false if Metal cannot express it. */
static bool draw_depth_stencil(const Nv2aGpuState *s, DepthStencil &k)
{
    k = DepthStencil();
    if (s->depth_test) {
        if (s->depth_func < 0x200 || s->depth_func > 0x207) return false;
        k.func = s->depth_func - 0x200;
        k.write = s->depth_mask != 0;
    }
    if (!s->stencil_test) return true;
    if (s->stencil_func < 0x200 || s->stencil_func > 0x207 ||
        !stencil_operation(s->stencil_fail, k.fail) ||
        !stencil_operation(s->stencil_zfail, k.zfail) ||
        !stencil_operation(s->stencil_zpass, k.zpass))
        return false;
    k.stencil = true;
    k.stencil_func = s->stencil_func - 0x200;
    k.read = s->stencil_read_mask & 0xff;
    k.mask = s->stencil_write_mask & 0xff;
    return true;
}

static bool writes_stencil(const DepthStencil &k)
{
    return k.stencil && k.mask &&
           (k.fail != MTLStencilOperationKeep || k.zfail != MTLStencilOperationKeep ||
            k.zpass != MTLStencilOperationKeep);
}

static id<MTLDepthStencilState> depth_state(Ma2MetalRenderer *m, const DepthStencil &k)
{
    uint64_t packed = (uint64_t)k.func | (uint64_t)k.write << 4 | (uint64_t)k.stencil << 5;
    if (k.stencil)
        packed |= (uint64_t)k.stencil_func << 6 | (uint64_t)k.fail << 10 |
                  (uint64_t)k.zfail << 13 | (uint64_t)k.zpass << 16 |
                  (uint64_t)k.read << 19 | (uint64_t)k.mask << 27;
    NSNumber *key = @(packed);
    id<MTLDepthStencilState> cached = m->depth_states[key];
    if (cached) return cached;
    static const MTLCompareFunction compare[] = {
        MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
        MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways, MTLCompareFunctionAlways
    };
    MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = compare[k.func];
    desc.depthWriteEnabled = k.write;
    if (k.stencil) {
        MTLStencilDescriptor *stencil = [[MTLStencilDescriptor alloc] init];
        stencil.stencilCompareFunction = compare[k.stencil_func];
        stencil.stencilFailureOperation = (MTLStencilOperation)k.fail;
        stencil.depthFailureOperation = (MTLStencilOperation)k.zfail;
        stencil.depthStencilPassOperation = (MTLStencilOperation)k.zpass;
        stencil.readMask = k.read;
        stencil.writeMask = k.mask;
        desc.frontFaceStencil = stencil;
        desc.backFaceStencil = stencil;
    }
    id<MTLDepthStencilState> result = [m->device newDepthStencilStateWithDescriptor:desc];
    if (!result) {
        fail(m, Failure::Resource, "depth state");
        return nil;
    }
    m->depth_states[key] = result;
    return result;
}

static void report_operation(Ma2MetalRenderer *m, bool draw, int accepted)
{
    uint64_t &counter = draw
        ? (accepted ? m->draw_accepted : m->draw_declined)
        : (accepted ? m->clear_accepted : m->clear_declined);
    ++counter;
    uint64_t total = m->clear_accepted + m->clear_declined
                   + m->draw_accepted + m->draw_declined;
    if (!m->perf || (total != 1 && (total & 4095u) != 0)) return;
    char declines[256];
    size_t used = 0;
    declines[0] = 0;
    for (unsigned i = 0; i < DeclineCount; ++i)
        if (m->declines[i])
            used += (size_t)snprintf(declines + used, sizeof(declines) - used, " %s=%llu",
                                     kDeclineNames[i], (unsigned long long)m->declines[i]);
    fprintf(stderr,
            "[NV2A-METAL] operations=%llu submissions=%llu waits=%llu "
            "clear=%llu accepted/%llu declined draw=%llu accepted/%llu declined "
            "loads=%llu/%llu writebacks=%llu/%llu textures=%llu hit/%llu miss %.1fMB "
            "surfaces=%zu declines:%s\n",
            (unsigned long long)total, (unsigned long long)m->submissions,
            (unsigned long long)m->waits,
            (unsigned long long)m->clear_accepted, (unsigned long long)m->clear_declined,
            (unsigned long long)m->draw_accepted, (unsigned long long)m->draw_declined,
            (unsigned long long)m->color_loads, (unsigned long long)m->depth_loads,
            (unsigned long long)m->color_writebacks, (unsigned long long)m->depth_writebacks,
            (unsigned long long)m->texture_hits, (unsigned long long)m->texture_misses,
            m->texture_bytes / 1048576.0, m->surfaces.size(), used ? declines : " none");
}

// ---------------------------------------------------------------------------
// Clears

/* R5G6B5 surfaces are not resident: clear guest RAM directly after flushing
 * any resident surface that shares the memory. */
static int clear_unresident(Ma2MetalRenderer *m, const Nv2aGpuState *s, const Geometry &g,
                            uint32_t mask, uint32_t color, float depth_value,
                            bool color_write, bool depth_write, bool stencil_write)
{
    uint32_t cformat = s->format & 15u, zformat = (s->format >> 4) & 15u;
    Surface color_surface = {}, z_surface = {};
    if (color_write && !mapped_surface(s->color_va, s->pitch, s->bpp, g, color_surface)) return 0;
    if ((depth_write || stencil_write) && !depth_surface(s, g, z_surface)) return 0;
    if (color_write && (depth_write || stencil_write) && overlap(color_surface, z_surface)) return 0;
    for (const Surface *surface : {&color_surface, &z_surface}) {
        unsigned domain;
        uint64_t offset;
        bool keep_aliased = false;
        if (surface->bytes && ma2_metal_backing(surface->va, surface->bytes, &domain, &offset) &&
            !evict_aliases(m, domain, offset, surface->bytes, nullptr, keep_aliased))
            return 1;
    }
    if (!finish(m)) return 1;

    size_t pixels = (size_t)g.w * g.h;
    size_t color_bytes = color_write ? pixels * s->bpp : 4;
    size_t depth_bytes = depth_write || stencil_write ? pixels * z_surface.bpp : 4;
    id<MTLBuffer> colors = [m->device newBufferWithLength:color_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> depths = [m->device newBufferWithLength:depth_bytes options:MTLResourceStorageModeShared];
    if (!colors || !depths || !ensure_command(m)) {
        fail(m, Failure::Resource, "clear buffers or command buffer");
        return 1;
    }
    if (color_write) copy_from_guest(colors.contents, color_surface, g);
    if (depth_write || stencil_write) copy_from_guest(depths.contents, z_surface, g);
    Clear constants = {g.w, g.h, cformat, zformat, mask, color,
                       depth_write ? (uint32_t)depth_value : 0, s->stencil_clear & 255u};
    id<MTLComputeCommandEncoder> encoder = [m->command computeCommandEncoder];
    if (!encoder) {
        fail(m, Failure::Resource, "clear encoder");
        return 1;
    }
    [encoder setComputePipelineState:m->clear];
    [encoder setBuffer:colors offset:0 atIndex:0];
    [encoder setBuffer:depths offset:0 atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(g.w, g.h, 1)
       threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    if (!finish(m)) return 1;
    if (color_write) copy_to_guest(color_surface, g, colors.contents);
    if (depth_write || stencil_write) copy_to_guest(z_surface, g, depths.contents);
    return 1;
}

static int clear_impl(Ma2MetalRenderer *m, const Nv2aGpuState *s, uint32_t mask,
                      uint32_t color, float depth_value)
{
    Geometry g;
    if (!geometry(s, g) || (mask & ~0xf3u)) return 0;
    uint32_t cformat = s->format & 15u, zformat = (s->format >> 4) & 15u;
    bool color_write = (mask & (cformat == 3 ? 0x70u : 0xf0u)) != 0;
    bool depth_write = (mask & 1u) != 0;
    bool stencil_write = (mask & 2u) != 0;
    if (stencil_write && zformat == 1) stencil_write = false;
    if (!color_write && !depth_write && !stencil_write) return 1;
    if ((depth_write || stencil_write) && zformat != 1 && zformat != 2) return 0;
    float max_depth = zformat == 1 ? 65535.0f : 16777215.0f;
    if (depth_write && (!std::isfinite(depth_value) || depth_value < 0 ||
                        depth_value > max_depth || std::trunc(depth_value) != depth_value))
        return 0;
    if (cformat == 3)
        return clear_unresident(m, s, g, mask, color, depth_value,
                                color_write, depth_write, stencil_write);

    Surface color_surface = {}, z_surface = {};
    if (color_write && !mapped_surface(s->color_va, s->pitch, s->bpp, g, color_surface)) return 0;
    if ((depth_write || stencil_write) && !depth_surface(s, g, z_surface)) return 0;
    if (color_write && (depth_write || stencil_write) && overlap(color_surface, z_surface)) return 0;

    bool declined = false;
    Resident *target = nullptr, *depth = nullptr;
    if (color_write) {
        target = resident(m, false, s->color_va, s->pitch, 4, cformat, g.height, nullptr, declined);
        if (!target) return declined && !m->failed ? 0 : 1;
    }
    if (depth_write || stencil_write) {
        uint32_t bpp, pitch;
        depth_pitch(s, g, bpp, pitch);
        depth = resident(m, true, s->depth_va, pitch, bpp, zformat, g.height, target, declined);
        if (!depth) return declined && !m->failed ? 0 : 1;
    }

    if (!begin_pass(m, target, depth)) return 1;
    // A reused pass may carry a depth attachment this clear does not touch.
    MTLPixelFormat depth_format = m->pass_depth ? m->pass_depth->texture.pixelFormat
                                                : MTLPixelFormatInvalid;
    bool pass_depth = m->pass_depth != nullptr;
    MTLColorWriteMask write_mask = MTLColorWriteMaskNone;
    if (color_write) {
        NSUInteger bits = 0;
        if (mask & 0x10u) bits |= MTLColorWriteMaskRed;
        if (mask & 0x20u) bits |= MTLColorWriteMaskGreen;
        if (mask & 0x40u) bits |= MTLColorWriteMaskBlue;
        if (mask & 0x80u) bits |= MTLColorWriteMaskAlpha;
        write_mask = (MTLColorWriteMask)bits;
    }
    DepthStencil clear_test;
    clear_test.write = depth_write;
    if (stencil_write) {
        clear_test.stencil = true;
        clear_test.fail = clear_test.zfail = clear_test.zpass = MTLStencilOperationReplace;
    }
    id<MTLRenderPipelineState> state = clear_pipeline(m, m->pass_color != nullptr, write_mask, depth_format);
    id<MTLDepthStencilState> z_state = pass_depth ? depth_state(m, clear_test) : nil;
    if (!state || (pass_depth && !z_state)) return 1;
    ClearValue value = {};
    value.color[0] = ((color >> 16) & 255) / 255.0f;
    value.color[1] = ((color >> 8) & 255) / 255.0f;
    value.color[2] = (color & 255) / 255.0f;
    value.color[3] = (color >> 24) / 255.0f;
    value.depth[0] = depth_write ? (float)((double)depth_value / (double)max_depth) : 0.0f;
    set_visibility(m, false);
    [m->encoder setRenderPipelineState:state];
    if (z_state) [m->encoder setDepthStencilState:z_state];
    if (stencil_write) [m->encoder setStencilReferenceValue:s->stencil_clear & 255u];
    MTLViewport viewport = {(double)g.x, (double)g.y, (double)g.w, (double)g.h, 0, 1};
    MTLScissorRect scissor = {g.x, g.y, g.w, g.h};
    [m->encoder setViewport:viewport];
    [m->encoder setScissorRect:scissor];
    [m->encoder setCullMode:MTLCullModeNone];
    [m->encoder setDepthClipMode:MTLDepthClipModeClamp];
    [m->encoder setFragmentBytes:&value length:sizeof(value) atIndex:0];
    [m->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    if (target) target->dirty.add(g.x, g.y, g.w, g.h);
    if (depth) depth->dirty.add(g.x, g.y, g.w, g.h);
    after_draw(m);
    return 1;
}

// ---------------------------------------------------------------------------
// Draws

static void unpack_color(uint32_t color, Float4 &out)
{
    out.value[0] = ((color >> 16) & 255) / 255.0f;
    out.value[1] = ((color >> 8) & 255) / 255.0f;
    out.value[2] = (color & 255) / 255.0f;
    out.value[3] = (color >> 24) / 255.0f;
}

static void guest_constants(const Nv2aGpuState *s, Guest &g)
{
    g = {};
    g.control.value[0] = s->rc.control;
    g.control.value[1] = s->rc.stage_program;
    g.control.value[2] = s->clip_plane_mode;
    g.control.value[3] = s->use_rc != 0;
    g.final_words.value[0] = s->rc.final0;
    g.final_words.value[1] = s->rc.final1;
    g.final_words.value[2] = s->alpha_func;
    g.final_words.value[3] = s->textured ? (s->legacy ? 1 : 2) : 0;
    g.misc.value[0] = s->alpha_ref / 255.0f;
    float max_depth = ((s->format >> 4) & 15u) == 1 ? 65535.0f : 16777215.0f;
    g.misc.value[1] = s->clip_min / max_depth;
    g.misc.value[2] = s->clip_max > 0 ? s->clip_max / max_depth : 1.0f;
    g.misc.value[3] = !s->z_clamp && (g.misc.value[1] > 0 || g.misc.value[2] < 1);
    unpack_color(s->fog_color, g.fog_color);
    std::swap(g.fog_color.value[0], g.fog_color.value[2]);
    unpack_color(s->rc.final_c0, g.final_c0);
    unpack_color(s->rc.final_c1, g.final_c1);
    g.texture_control.value[0] = s->rc.other_stage_input;
    g.texture_control.value[1] = s->alpha_test != 0;
    for (unsigned i = 0; i < 8; ++i) {
        g.programs[i].value[0] = s->rc.color_icw[i];
        g.programs[i].value[1] = s->rc.alpha_icw[i];
        g.programs[i].value[2] = s->rc.color_ocw[i];
        g.programs[i].value[3] = s->rc.alpha_ocw[i];
        unpack_color(s->rc.factor0[i], g.factor0[i]);
        unpack_color(s->rc.factor1[i], g.factor1[i]);
    }
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(g.bump_matrix[i].value, s->bump_matrix[i], sizeof(s->bump_matrix[i]));
        g.bump_luminance[i].value[0] = s->bump_scale[i];
        g.bump_luminance[i].value[1] = s->bump_offset[i];
    }
    memcpy(g.eye_vector.value, s->eye_vector, sizeof(s->eye_vector));
}

static bool stage_texture(uint32_t mode, bool &sampled)
{
    switch (mode) {
    case 0: case 4: case 5: case 10: case 17:
        sampled = false;
        return true;
    case 1: case 2: case 3: case 6: case 7: case 9:
    case 11: case 12: case 13: case 14: case 15: case 16: case 18:
        sampled = true;
        return true;
    default:
        return false;
    }
}

/* Validate every stage and describe the guest levels; nothing is decoded. */
static bool prepare_textures(const Nv2aGpuState *s, Nv2aGpuTexel texel,
                             Guest &g, TextureUpload (&uploads)[4], Decline &reason)
{
    for (unsigned stage = 0; stage < 4; ++stage) {
        uint32_t mode = s->use_rc ? (s->rc.stage_program >> (stage * 5)) & 31u : 0;
        bool sampled = !s->use_rc && s->textured && stage == 0;
        reason = DeclineTextureStage;
        if (s->use_rc && !stage_texture(mode, sampled)) return false;
        if (mode == 6 || mode == 7) {
            for (float value : s->bump_matrix[stage])
                if (!std::isfinite(value)) return false;
            if (mode == 7 && (!std::isfinite(s->bump_scale[stage]) ||
                              !std::isfinite(s->bump_offset[stage]))) return false;
        }
        if (mode == 18)
            for (float value : s->eye_vector)
                if (!std::isfinite(value)) return false;
        if (!sampled) continue;

        const Nv2aGpuTexture &t = s->textures[stage];
        reason = DeclineTextureFormat;
        if (!texel || !t.valid || !t.width || !t.height ||
            t.width > kMaxExtent || t.height > kMaxExtent ||
            t.dimensionality > 2)
            return false;
        if ((t.addr_u == 4 || t.addr_v == 4) &&
            t.border_color != 0 && t.border_color != 0xff000000u &&
            t.border_color != 0xffffffffu)
            return false;

        uint32_t block = d3d8_format_dxt_block_bytes(t.color);
        bool packed = block || d3d8_format_is_swizzled(t.color);
        uint32_t texel_bytes = nv2a_texture_texel_bytes(t.color);
        uint64_t row_pixels = (t.color == 0x24 || t.color == 0x25)
            ? ((uint64_t)t.width + 1) & ~UINT64_C(1) : t.width;
        if (!packed && (!t.pitch || row_pixels * texel_bytes > t.pitch)) return false;
        // Cube maps: square swizzled or compressed faces, as the CPU samples them.
        if (t.cube && (!packed || t.width != t.height)) return false;

        uint32_t min_filter = (t.filter >> 16) & 255, mag_filter = (t.filter >> 24) & 15;
        TextureUpload &upload = uploads[stage];
        upload.sampled = true;
        upload.mipmapped = packed && min_filter >= 3 && min_filter <= 6;
        upload.min_linear = min_filter == 2 || min_filter == 4 || min_filter == 6 || min_filter == 7;
        upload.mag_linear = mag_filter == 2 || mag_filter == 4;
        if (((t.control0 >> 4) & 3) && upload.min_linear && upload.mag_linear) return false;
        uint32_t levels = upload.mipmapped && t.levels ? std::min(t.levels, 13u) : 1;
        uint32_t max_levels = 1;
        for (uint32_t extent = std::max(t.width, t.height); extent > 1; extent >>= 1)
            ++max_levels;
        if (levels > max_levels) return false;
        upload.max_lod = upload.mipmapped ? (float)(levels - 1) : 0;
        if (t.control0 && upload.mipmapped) {
            uint32_t lo = (t.control0 >> 18) & 4095, hi = (t.control0 >> 6) & 4095;
            upload.min_lod = (float)std::min(lo, levels - 1);
            upload.max_lod = (float)std::min(hi, levels - 1);
            if (upload.min_lod > upload.max_lod) upload.min_lod = upload.max_lod;
        }
        int32_t bias = (int32_t)(t.filter & 0x1fff);
        if (bias & 0x1000) bias -= 0x2000;
        g.texture_bias.value[stage] = bias / 256.0f;
        g.tex_info[stage].value[0] = packed ? 1.0f : 1.0f / t.width;
        g.tex_info[stage].value[1] = packed ? 1.0f : 1.0f / t.height;
        g.tex_info[stage].value[2] = 1;
        g.tex_info[stage].value[3] = t.cube ? 1.0f : 0.0f;

        uint64_t offset = 0, decoded_bytes = 0;
        upload.levels.reserve(levels);
        for (uint32_t level = 0; level < levels; ++level) {
            uint32_t width = std::max(t.width >> level, 1u);
            uint32_t height = std::max(t.height >> level, 1u);
            uint64_t bytes = block ? (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * block
                           : packed ? (uint64_t)width * height * texel_bytes
                                    : (uint64_t)t.pitch * height;
            decoded_bytes += (uint64_t)width * height * 4;
            if (bytes > kMaxTextureBytes || offset + bytes > kMaxTextureBytes ||
                decoded_bytes > kMaxTextureBytes ||
                offset + bytes > UINT64_C(0x100000000) - t.offset)
                return false;
            Nv2aGpuTexture descriptor = t;
            descriptor.offset += (uint32_t)offset;
            descriptor.width = width;
            descriptor.height = height;
            descriptor.depth = 1;
            upload.levels.push_back({descriptor, bytes});
            offset += bytes;
        }
        upload.span = offset;
        if (t.cube) {
            upload.cube = true;
            upload.face_stride = (offset + 127) & ~UINT64_C(127);
            upload.span = upload.face_stride * 5 + offset;
            if (upload.span > kMaxTextureBytes || decoded_bytes * 6 > kMaxTextureBytes ||
                upload.span > UINT64_C(0x100000000) - t.offset)
                return false;
        }
    }
    return true;
}

static MTLSamplerAddressMode texture_address(uint32_t mode)
{
    switch (mode) {
    case 1: return MTLSamplerAddressModeRepeat;
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

static id<MTLSamplerState> sampler(Ma2MetalRenderer *m, const Nv2aGpuTexture &t,
                                   const TextureUpload &upload)
{
    uint32_t min_filter = (t.filter >> 16) & 255;
    uint32_t mip = !upload.mipmapped ? 0 : min_filter >= 5 ? 2 : 1;
    uint32_t border = t.border_color == 0xff000000u ? 1 : t.border_color == 0xffffffffu ? 2 : 0;
    uint64_t key = (uint64_t)(t.addr_u & 7) | (uint64_t)(t.addr_v & 7) << 3 |
                   (uint64_t)upload.min_linear << 6 | (uint64_t)upload.mag_linear << 7 |
                   (uint64_t)mip << 8 | (uint64_t)border << 10 |
                   (uint64_t)(uint32_t)(upload.min_lod * 16) << 16 |
                   (uint64_t)(uint32_t)(upload.max_lod * 16) << 32;
    id<MTLSamplerState> cached = m->samplers[@(key)];
    if (cached) return cached;
    MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
    desc.normalizedCoordinates = YES;
    desc.sAddressMode = texture_address(t.addr_u);
    desc.tAddressMode = texture_address(t.addr_v);
    desc.minFilter = upload.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    desc.magFilter = upload.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    desc.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped :
                     mip == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    desc.lodMinClamp = upload.min_lod;
    desc.lodMaxClamp = upload.max_lod;
    desc.borderColor = border == 1 ? MTLSamplerBorderColorOpaqueBlack :
                       border == 2 ? MTLSamplerBorderColorOpaqueWhite :
                                     MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> result = [m->device newSamplerStateWithDescriptor:desc];
    if (!result) {
        fail(m, Failure::Resource, "sampler");
        return nil;
    }
    m->samplers[@(key)] = result;
    return result;
}

/* Bring guest RAM up to date wherever a resident surface wrote the range. */
static bool flush_writes(Ma2MetalRenderer *m, uint32_t va, uint64_t bytes)
{
    unsigned domain;
    uint64_t offset;
    if (!bytes || bytes > SIZE_MAX || !ma2_metal_backing(va, (size_t)bytes, &domain, &offset))
        return true;
    for (auto &surface : m->surfaces)
        if (!surface->dirty.empty() && aliases(*surface, domain, offset, (size_t)bytes) &&
            !write_back(m, *surface))
            return false;
    return true;
}

static void evict_textures(Ma2MetalRenderer *m)
{
    if (m->texture_bytes <= kTextureCacheBytes) return;
    std::vector<std::pair<uint64_t, uint64_t>> order;
    order.reserve(m->textures.size());
    for (auto &entry : m->textures) order.push_back({entry.second.used, entry.first});
    std::sort(order.begin(), order.end());
    for (auto &entry : order) {
        if (m->texture_bytes <= kTextureCacheBytes / 2) break;
        auto found = m->textures.find(entry.second);
        m->texture_bytes -= found->second.bytes;
        m->textures.erase(found);
    }
}

/* Returns 1 with the texture, 0 to decline (nothing issued), -1 on failure. */
static int resolve_texture(Ma2MetalRenderer *m, const TextureUpload &upload,
                           Nv2aGpuTexel texel, __strong id<MTLTexture> &out)
{
    const Nv2aGpuTexture &t = upload.levels[0].descriptor;
    bool paletted = t.color == 0x0B && t.palette;
    if (!flush_writes(m, t.offset, upload.span) ||
        (paletted && !flush_writes(m, t.palette, 1024)))
        return -1;
    uint32_t fields[] = {t.offset, t.width, t.height, t.pitch, t.color, t.palette,
                         (uint32_t)upload.levels.size(), (uint32_t)upload.span,
                         (uint32_t)upload.cube};
    uint64_t key = hash_bytes(fields, sizeof(fields), 0x6d61326d6574616cull);
    const void *bytes = ma2_metal_translate(t.offset, (size_t)upload.span);
    const void *palette = paletted ? ma2_metal_translate(t.palette, 1024) : nullptr;
    bool cacheable = bytes && (!paletted || palette);
    uint64_t content = 0;
    if (cacheable) {
        ContentMemo &memo = m->memo[key];
        if (memo.epoch != m->epoch) {
            memo.content = hash_bytes(bytes, (size_t)upload.span, key);
            if (palette) memo.content = hash_bytes(palette, 1024, memo.content);
            memo.epoch = m->epoch;
        }
        content = memo.content;
        auto found = m->textures.find(content);
        if (found != m->textures.end()) {
            found->second.used = ++m->texture_clock;
            ++m->texture_hits;
            out = found->second.texture;
            return 1;
        }
    }
    ++m->texture_misses;
    // Faces are consecutive in guest RAM; the texel callback takes a face's
    // byte offset. Slices follow Metal's +X, -X, +Y, -Y, +Z, -Z order.
    const uint32_t faces = upload.cube ? 6 : 1;
    size_t count = upload.levels.size();
    std::vector<std::vector<uint32_t>> pixels(count * faces);
    for (uint32_t face = 0; face < faces; ++face)
        for (size_t level = 0; level < count; ++level) {
            const Nv2aGpuTexture &d = upload.levels[level].descriptor;
            std::vector<uint32_t> &out = pixels[face * count + level];
            uint32_t face_offset = (uint32_t)(upload.face_stride * face);
            out.resize((size_t)d.width * d.height);
            for (uint32_t y = 0; y < d.height; ++y)
                for (uint32_t x = 0; x < d.width; ++x)
                    if (!texel(&d, face_offset, x, y, 0, &out[(size_t)y * d.width + x]))
                        return 0;
        }
    MTLTextureDescriptor *desc = upload.cube
        ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                size:t.width mipmapped:count > 1]
        : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:t.width height:t.height mipmapped:count > 1];
    desc.mipmapLevelCount = count;
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [m->device newTextureWithDescriptor:desc];
    if (!texture) {
        fail(m, Failure::Resource, "sampled texture");
        return -1;
    }
    uint64_t total = 0;
    for (uint32_t face = 0; face < faces; ++face)
        for (size_t level = 0; level < count; ++level) {
            const Nv2aGpuTexture &d = upload.levels[level].descriptor;
            [texture replaceRegion:MTLRegionMake2D(0, 0, d.width, d.height)
                       mipmapLevel:level slice:face withBytes:pixels[face * count + level].data()
                       bytesPerRow:(size_t)d.width * 4 bytesPerImage:(size_t)d.width * d.height * 4];
            total += (uint64_t)d.width * d.height * 4;
        }
    if (cacheable) {
        m->textures[content] = {texture, total, ++m->texture_clock};
        m->texture_bytes += total;
        evict_textures(m);
    }
    out = texture;
    return 1;
}

struct TextureResources {
    __strong id<MTLTexture> textures[4] = {};
    __strong id<MTLTexture> cubes[4] = {};
    __strong id<MTLSamplerState> samplers[4] = {};
};

static int bind_textures(Ma2MetalRenderer *m, const Nv2aGpuState *s, Nv2aGpuTexel texel,
                         const TextureUpload (&uploads)[4], TextureResources &resources)
{
    int first = -1;
    for (unsigned stage = 0; stage < 4; ++stage) {
        const TextureUpload &upload = uploads[stage];
        if (!upload.sampled) continue;
        __strong id<MTLTexture> &slot = upload.cube ? resources.cubes[stage] : resources.textures[stage];
        int status = resolve_texture(m, upload, texel, slot);
        if (status <= 0) return status;
        if (upload.cube) continue;
        resources.samplers[stage] = sampler(m, s->textures[stage], upload);
        if (!resources.samplers[stage]) return -1;
        if (first < 0) first = (int)stage;
    }
    for (unsigned stage = 0; stage < 4; ++stage) {
        if (!resources.cubes[stage]) resources.cubes[stage] = m->white_cube;
        if (uploads[stage].cube && !resources.samplers[stage])
            resources.samplers[stage] = sampler(m, s->textures[stage], uploads[stage]);
        if (!resources.textures[stage])
            resources.textures[stage] = first < 0 ? m->white : resources.textures[first];
        if (!resources.samplers[stage])
            resources.samplers[stage] = first < 0 ? m->white_sampler : resources.samplers[first];
    }
    return 1;
}

/* The executor's fog_factor(): oFog to the factor the final combiner uses.
 * Saturation happens after interpolation, in the shader. */
static float fog_factor(const Nv2aGpuState *s, float d)
{
    if (!s->fog_enable) return 1.0f;
    float f, px = s->fog_params[0], py = s->fog_params[1];
    switch (s->fog_mode) {
    case 0x800: case 0x802:
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:
        f = px + d * py - 1.0f;
        break;
    }
    if (s->fog_mode == 0x802 || s->fog_mode == 0x803 || s->fog_mode == 0x804) f = fabsf(f);
    if (std::isinf(d) || f != f) f = s->fog_mode == 0x800 || s->fog_mode == 0x2601 ? 1.0f : 0.0f;
    return std::max(-FLT_MAX, std::min(FLT_MAX, f));
}

static void add_triangle(std::vector<Vertex> &out, const Nv2aVshOutput *vertices,
                         uint32_t a, uint32_t b, uint32_t c, uint32_t flat,
                         const Nv2aGpuState *s, uint32_t tex_coords)
{
    bool legacy = s->legacy && !s->use_rc, use_rc = s->use_rc != 0;
    const uint32_t indices[] = {a, b, c};
    for (uint32_t index : indices) {
        Vertex v = {};
        memcpy(v.position, vertices[index].pos, sizeof(v.position));
        memcpy(v.color, vertices[legacy ? flat : index].d0, sizeof(v.color));
        if (use_rc) {
            memcpy(v.secondary, vertices[index].d1, sizeof(v.secondary));
            v.fog[0] = fog_factor(s, vertices[index].fog[0]);
        }
        for (unsigned stage = 0; stage < 4; ++stage)
            if (tex_coords & (1u << stage))
                memcpy(v.tex[stage], vertices[index].tex[stage], sizeof(v.tex[stage]));
        out.push_back(v);
    }
}

static bool triangles(std::vector<Vertex> &out, const Nv2aVshOutput *v,
                      uint32_t count, uint32_t primitive, const Nv2aGpuState *s,
                      Decline &reason)
{
    reason = DeclinePrimitive;
    if (primitive < 5 || primitive > 10) return false;
    reason = DeclineVertexCount;
    if (count > kMaxVertices || (count && !v)) return false;
    reason = DeclineNonFinite;
    std::vector<uint8_t> culled(count);
    uint32_t tex_coords = 0;
    if (s->use_rc) {
        for (unsigned stage = 0; stage < 4; ++stage) {
            uint32_t mode = (s->rc.stage_program >> (stage * 5)) & 31u;
            if (mode && mode != 15 && mode != 16) tex_coords |= 1u << stage;
            if (mode == 11) tex_coords |= 1u << 3;
            if (mode == 12) tex_coords |= 0xeu;
        }
    } else if (s->textured) tex_coords = 1;
    for (uint32_t i = 0; i < count; ++i) {
        for (unsigned j = 0; j < 4; ++j)
            if (!std::isfinite(v[i].d0[j])) return false;
        // The CPU path culls a triangle whose screen area is not finite.
        bool finite = true;
        for (float value : v[i].pos) finite = finite && std::isfinite(value);
        if (!finite) {
            culled[i] = 1;
            continue;
        }
        if (s->use_rc) {
            for (float value : v[i].d1)
                if (!std::isfinite(value)) return false;
        }
        for (unsigned stage = 0; stage < 4; ++stage)
            if (tex_coords & (1u << stage))
                for (float value : v[i].tex[stage])
                    if (!std::isfinite(value)) return false;
        /* Positions are screen coordinates; the shader multiplies them by w
         * to rebuild clip space, and Metal's clipper handles w <= 0 the way
         * the CPU's homogeneous near-plane clipper does. Values that would
         * overflow in that product stay on the CPU. */
        double w = v[i].pos[3];
        if (std::fabs((double)v[i].pos[0]) > FLT_MAX / 16.0 ||
            std::fabs((double)v[i].pos[1]) > FLT_MAX / 16.0 ||
            std::fabs((double)v[i].pos[0] * w) > FLT_MAX / 4.0 ||
            std::fabs((double)v[i].pos[1] * w) > FLT_MAX / 4.0 ||
            std::fabs((double)v[i].pos[2] * w) > FLT_MAX / 4.0) {
            reason = DeclineBehindEye;
            return false;
        }
    }
    out.reserve((size_t)count * 3);
    // Keep the old legacy color path flat; register combiners interpolate it.
#define TRI(a, b, c, flat) do { \
    if (!culled[a] && !culled[b] && !culled[c]) \
        add_triangle(out, v, a, b, c, flat, s, tex_coords); \
} while (0)
    switch (primitive) {
    case 5:
        for (uint32_t i = 0; i + 2 < count; i += 3) TRI(i, i + 1, i + 2, i);
        break;
    case 6:
        for (uint32_t i = 0; i + 2 < count; ++i) {
            if (i & 1) TRI(i + 1, i, i + 2, i);
            else TRI(i, i + 1, i + 2, i);
        }
        break;
    case 7: case 10:
        for (uint32_t i = 1; i + 1 < count; ++i) TRI(0, i, i + 1, 0);
        break;
    case 8:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            TRI(i, i + 1, i + 2, i); TRI(i, i + 2, i + 3, i);
        }
        break;
    case 9:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            TRI(i, i + 1, i + 3, i); TRI(i, i + 3, i + 2, i);
        }
        break;
    }
#undef TRI
    return true;
}

static int decline(Ma2MetalRenderer *m, Decline reason)
{
    ++m->declines[reason];
    return 0;
}

/* RECOMP_PERF: the first few declined draws of each kind, for triage. */
static int decline_draw(Ma2MetalRenderer *m, Decline reason, const Nv2aGpuState *s,
                        const Nv2aVshOutput *v, uint32_t count, uint32_t primitive)
{
    static unsigned samples[DeclineCount];
    if (m->perf && samples[reason] < 6) {
        ++samples[reason];
        fprintf(stderr, "[NV2A-METAL] decline %s prim=%u n=%u fmt=%08x pitch=%u clip=%u,%u+%ux%u "
                "color=%08x zeta=%08x/%u mask=%08x z=%u/%x/%u stencil=%u/%x/%x/%x/%x/%x/%x/%x "
                "blend=%u/%x/%x/%x alpha=%u/%x rc=%d tex=%d legacy=%d zpass=%u",
                kDeclineNames[reason], primitive, count, s->format, s->pitch,
                s->clip_x, s->clip_y, s->clip_w, s->clip_h, s->color_va, s->depth_va,
                s->depth_pitch, s->color_mask, s->depth_test, s->depth_func, s->depth_mask,
                s->stencil_test, s->stencil_func, s->stencil_ref, s->stencil_read_mask,
                s->stencil_write_mask, s->stencil_fail, s->stencil_zfail, s->stencil_zpass,
                s->blend_enable, s->blend_src, s->blend_dst, s->blend_equation,
                s->alpha_test, s->alpha_func, s->use_rc, s->textured, s->legacy, s->zpass_enable);
        if (reason == DeclineNonFinite && v)
            for (uint32_t i = 0; i < count && i < 4; ++i)
                fprintf(stderr, " v%u=pos(%g,%g,%g,%g)d0(%g,%g,%g,%g)d1(%g,%g,%g,%g)fog%g t0(%g,%g,%g,%g)",
                        i, v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3],
                        v[i].d0[0], v[i].d0[1], v[i].d0[2], v[i].d0[3],
                        v[i].d1[0], v[i].d1[1], v[i].d1[2], v[i].d1[3], v[i].fog[0],
                        v[i].tex[0][0], v[i].tex[0][1], v[i].tex[0][2], v[i].tex[0][3]);
        fputc('\n', stderr);
    }
    return decline(m, reason);
}

/* RECOMP_PERF: the first few declined texture descriptors, for triage. */
static void sample_texture_decline(Ma2MetalRenderer *m, const Nv2aGpuState *s, Decline reason)
{
    static unsigned samples;
    if (!m->perf || samples >= 16) return;
    ++samples;
    fprintf(stderr, "[NV2A-METAL] texture decline %s program=%05x", kDeclineNames[reason],
            s->use_rc ? s->rc.stage_program : 0u);
    for (unsigned i = 0; i < 4; ++i) {
        const Nv2aGpuTexture &t = s->textures[i];
        if (t.valid)
            fprintf(stderr, " t%u=fmt%02x/%ux%u/lv%u/dim%u/cube%d/addr%u%u/filter%08x/ctl%08x/border%08x/pitch%u",
                    i, t.color, t.width, t.height, t.levels, t.dimensionality, t.cube,
                    t.addr_u, t.addr_v, t.filter, t.control0, t.border_color, t.pitch);
    }
    fputc('\n', stderr);
}

static int draw_impl(Ma2MetalRenderer *m, const Nv2aGpuState *s, const Nv2aVshOutput *v,
                     uint32_t count, uint32_t primitive, Nv2aGpuTexel texel)
{
    Geometry g;
    if (!geometry(s, g) || (s->format & 15u) != 8)
        return decline_draw(m, DeclineSurface, s, v, count, primitive);
    DepthStencil test;
    uint32_t zformat_check = (s->format >> 4) & 15u;
    // Stencil lives only in Z24S8; NV2A stencil ops must map to Metal's.
    if (!draw_depth_stencil(s, test) || (s->stencil_test && zformat_check != 2))
        return decline_draw(m, s->stencil_test ? DeclineStencil : DeclineState, s, v, count, primitive);
    bool needs_depth = s->depth_test || s->stencil_test;
    if (s->polygon_offset_enable) return decline(m, DeclinePolygonOffset);
    if ((s->alpha_test && !(s->use_rc || s->textured)) ||
        (s->color_mask & ~0x01010101u))
        return decline(m, DeclineState);
    bool shaded = s->use_rc || s->textured;
    if (s->alpha_test && (s->alpha_func < 0x200 || s->alpha_func > 0x207)) return decline(m, DeclineState);
    if (shaded && (!std::isfinite(s->clip_min) || !std::isfinite(s->clip_max))) return decline(m, DeclineState);
    uint32_t zformat = (s->format >> 4) & 15u;
    float max_depth = zformat == 1 ? 65535.0f : 16777215.0f;
    if (s->z_clamp && (!std::isfinite(s->clip_min) || !std::isfinite(s->clip_max) ||
        s->clip_min != 0 || (s->clip_max > 0 && s->clip_max != max_depth)))
        return decline(m, DeclineState);
    if (s->cull_enable && ((s->front_face != 0x900 && s->front_face != 0x901) ||
        (s->cull_face != 0x404 && s->cull_face != 0x405 && s->cull_face != 0x408)))
        return decline(m, DeclineState);
    if (s->blend_enable) {
        MTLBlendFactor sr, sa, dr, da;
        MTLBlendOperation op;
        if (!blend_factor(s->blend_src, sr, sa, false) ||
            !blend_factor(s->blend_dst, dr, da, true) ||
            !blend_operation(s->blend_equation, op)) return decline(m, DeclineState);
    }
    Surface color_surface = {}, z_surface = {};
    if (!mapped_surface(s->color_va, s->pitch, s->bpp, g, color_surface)) return decline(m, DeclineSurface);
    if (needs_depth && !depth_surface(s, g, z_surface)) return decline(m, DeclineSurface);
    if (needs_depth && overlap(color_surface, z_surface)) return decline(m, DeclineAlias);
    std::vector<Vertex> vertices;
    Decline reason = DeclineState;
    if (!triangles(vertices, v, count, primitive, s, reason))
        return decline_draw(m, reason, s, v, count, primitive);
    Guest guest = {};
    TextureUpload uploads[4];
    if (shaded) {
        guest_constants(s, guest);
        if (!prepare_textures(s, texel, guest, uploads, reason)) {
            sample_texture_decline(m, s, reason);
            return decline(m, reason);
        }
    }
    if (vertices.empty() || (s->cull_enable && s->cull_face == 0x408)) return 1;

    // Texture resolution can write resident surfaces back, so do it first.
    TextureResources resources;
    if (shaded) {
        int status = bind_textures(m, s, texel, uploads, resources);
        if (status < 0 || m->failed) return 1;
        if (!status) {
            sample_texture_decline(m, s, DeclineTextureDecode);
            return decline(m, DeclineTextureDecode);
        }
    }
    bool declined = false;
    Resident *target = resident(m, false, s->color_va, s->pitch, 4, 8, g.height, nullptr, declined);
    if (!target) return declined && !m->failed ? decline(m, DeclineSurface) : 1;
    Resident *depth = nullptr;
    if (needs_depth) {
        uint32_t bpp, pitch;
        depth_pitch(s, g, bpp, pitch);
        depth = resident(m, true, s->depth_va, pitch, bpp, zformat, g.height, target, declined);
        if (!depth) return declined && !m->failed ? decline(m, DeclineAlias) : 1;
    }
    __strong id<MTLBuffer> vertex_buffer = nil;
    NSUInteger vertex_offset = 0;
    if (!upload(m, vertices.data(), vertices.size() * sizeof(Vertex), vertex_buffer, vertex_offset))
        return 1;
    if (s->zpass_enable && m->visibility_next >= kVisibilitySlots && !finish(m)) return 1;
    // finish() may have released the pass but never a resident surface.
    if (!begin_pass(m, target, depth)) return 1;
    bool pass_depth = m->pass_depth != nullptr;
    MTLPixelFormat depth_format = pass_depth ? m->pass_depth->texture.pixelFormat
                                             : MTLPixelFormatInvalid;
    id<MTLRenderPipelineState> state = pipeline(m, s, depth_format);
    id<MTLDepthStencilState> z_state = pass_depth ? depth_state(m, test) : nil;
    if (!state || (pass_depth && !z_state)) {
        if (!m->failed) fail(m, Failure::Pipeline, "triangle pipeline");
        return 1;
    }

    id<MTLRenderCommandEncoder> encoder = m->encoder;
    Viewport viewport = {(float)g.width, (float)g.height, max_depth, 0};
    set_visibility(m, s->zpass_enable != 0);
    [encoder setRenderPipelineState:state];
    if (z_state) [encoder setDepthStencilState:z_state];
    if (test.stencil) [encoder setStencilReferenceValue:s->stencil_ref & 255u];
    [encoder setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];
    [encoder setVertexBytes:&viewport length:sizeof(viewport) atIndex:1];
    if (shaded) {
        [encoder setFragmentBytes:&guest length:sizeof(guest) atIndex:1];
        for (unsigned stage = 0; stage < 4; ++stage) {
            [encoder setFragmentTexture:resources.textures[stage] atIndex:stage];
            [encoder setFragmentTexture:resources.cubes[stage] atIndex:4 + stage];
            [encoder setFragmentSamplerState:resources.samplers[stage] atIndex:stage];
        }
    }
    if (s->depth_test) [encoder setFragmentBytes:&viewport length:sizeof(viewport) atIndex:0];
    MTLViewport metal_viewport = {0, 0, (double)g.width, (double)g.height, 0, 1};
    MTLScissorRect scissor = {g.x, g.y, g.w, g.h};
    [encoder setViewport:metal_viewport];
    [encoder setScissorRect:scissor];
    [encoder setDepthClipMode:s->z_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    [encoder setFrontFacingWinding:s->front_face == 0x901 ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [encoder setCullMode:!s->cull_enable ? MTLCullModeNone :
                         s->cull_face == 0x404 ? MTLCullModeFront : MTLCullModeBack];
    if (s->blend_enable) {
        uint32_t c = s->blend_color;
        [encoder setBlendColorRed:((c >> 16) & 255) / 255.0f
                            green:((c >> 8) & 255) / 255.0f
                             blue:(c & 255) / 255.0f
                            alpha:(c >> 24) / 255.0f];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices.size()];
    if (s->color_mask & 0x01010101u) target->dirty.add(g.x, g.y, g.w, g.h);
    if (depth && ((s->depth_test && s->depth_mask) || writes_stencil(test)))
        depth->dirty.add(g.x, g.y, g.w, g.h);
    after_draw(m);
    return 1;
}

// ---------------------------------------------------------------------------
// Backend callbacks

static int clear_callback(void *context, const Nv2aGpuState *s, uint32_t mask,
                          uint32_t color, float depth)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    @autoreleasepool {
        if (m->failed) return 1;
        try {
            @try {
                int accepted = clear_impl(m, s, mask, color, depth);
                report_operation(m, false, accepted);
                return accepted;
            }
            @catch (NSException *exception) {
                fail(m, Failure::Command, exception.reason.UTF8String);
                return 1;
            }
        } catch (const std::bad_alloc &) {
            fail(m, Failure::Memory, "clear");
            return 1;
        }
    }
}

static int draw_callback(void *context, const Nv2aGpuState *s, const Nv2aVshOutput *vertices,
                         uint32_t count, uint32_t primitive, Nv2aGpuTexel texel)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    @autoreleasepool {
        if (m->failed) return 1;
        try {
            @try {
                int accepted = draw_impl(m, s, vertices, count, primitive, texel);
                report_operation(m, true, accepted);
                return accepted;
            }
            @catch (NSException *exception) {
                fail(m, Failure::Command, exception.reason.UTF8String);
                return 1;
            }
        } catch (const std::bad_alloc &) {
            fail(m, Failure::Memory, "triangle batch");
            return 1;
        }
    }
}

template <typename F>
static int guarded(Ma2MetalRenderer *m, const char *operation, F &&body)
{
    @autoreleasepool {
        if (m->failed) return 0;
        try {
            @try {
                return body() && !m->failed;
            }
            @catch (NSException *exception) {
                fail(m, Failure::Command, exception.reason.UTF8String);
                return 0;
            }
        } catch (const std::bad_alloc &) {
            fail(m, Failure::Memory, operation);
            return 0;
        }
    }
}

static int sync_callback(void *context)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    return guarded(m, "synchronization", [&] { return sync_surfaces(m); });
}

static bool write_back_address(Ma2MetalRenderer *m, uint32_t va)
{
    for (auto &surface : m->surfaces)
        if (contains(*surface, va) && !write_back(m, *surface)) return false;
    return true;
}

static int readback_callback(void *context, uint32_t va)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    return guarded(m, "readback", [&] {
        if (!va || !ma2_metal_translate(va, 1)) {
            fail(m, Failure::Resource, "unmapped readback");
            return false;
        }
        return write_back_address(m, va);
    });
}

/* The CPU path is about to write guest RAM at va. It may also rewrite the
 * bound depth buffer, which the executor does not name, so every resident
 * depth surface is reloaded on next use. */
static void invalidate_callback(void *context, uint32_t va)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    guarded(m, "invalidate", [&] {
        if (!finish(m)) return false;
        for (auto &surface : m->surfaces) {
            if (!surface->depth && !contains(*surface, va)) continue;
            if (!write_back(m, *surface)) return false;
            surface->stale = true;
        }
        ++m->epoch;
        return true;
    });
}

static void flip_callback(void *context, uint32_t, uint32_t)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    ++m->epoch;
    if (m->memo.size() > 16384) m->memo.clear();
}

static void failure_callback(void *context, const char *operation)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    fail(m, Failure::Interop, operation, nil, false);
}

static int present_callback(void *context, uint32_t va, uint32_t pitch)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    return guarded(m, "present", [&] {
        if (!pitch || !va || !ma2_metal_translate(va, pitch)) {
            fail(m, Failure::Resource, "unmapped scanout");
            return false;
        }
        return write_back_address(m, va);
    });
}

static void visibility_reset_callback(void *context)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    m->visibility_pending.clear();
    m->visibility_resolved = 0;
    // Counted draws after the reset need a fresh slot, even in this pass.
    m->visibility_slot = -1;
}

static int visibility_count_callback(void *context, uint32_t *count)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    return guarded(m, "visibility", [&] {
        if (!finish(m)) return false;
        *count = (uint32_t)std::min<uint64_t>(m->visibility_resolved, UINT32_MAX);
        return true;
    });
}

} // namespace

extern "C" Ma2MetalRenderer *ma2_metal_create(int console_fd, int log_fd)
{
    @autoreleasepool {
        Ma2MetalRenderer *m = new (std::nothrow) Ma2MetalRenderer;
        if (!m) return nullptr;
        m->perf = getenv("RECOMP_PERF") != nullptr;
        if ((console_fd >= 0 && (m->console_fd = dup(console_fd)) < 0) ||
            (log_fd >= 0 && (m->log_fd = dup(log_fd)) < 0)) {
            fail(m, Failure::Resource, "duplicate error log descriptors", nil, false);
            ma2_metal_destroy(m);
            return nullptr;
        }
        @try {
            m->device = MTLCreateSystemDefaultDevice();
            m->queue = [m->device newCommandQueue];
            if (!m->device || !m->queue) {
                fail(m, Failure::Device, "system device or command queue", nil, false);
                ma2_metal_destroy(m);
                return nullptr;
            }
            NSError *error = nil;
            MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
            if (@available(macOS 15.0, *)) {
                options.mathMode = MTLMathModeSafe;
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                options.fastMathEnabled = NO;
#pragma clang diagnostic pop
            }
            NSString *source = [NSString stringWithUTF8String:ma2_metal_shader_source];
            m->library = [m->device newLibraryWithSource:source options:options error:&error];
            if (!m->library) {
                fail(m, Failure::Shader, "compile", error, false);
                ma2_metal_destroy(m);
                return nullptr;
            }
            m->vertex = [m->library newFunctionWithName:@"ma2_vertex"];
            m->color = [m->library newFunctionWithName:@"ma2_color"];
            m->color_depth = [m->library newFunctionWithName:@"ma2_color_depth"];
            m->shaded = [m->library newFunctionWithName:@"ma2_shaded"];
            m->shaded_depth = [m->library newFunctionWithName:@"ma2_shaded_depth"];
            m->clear_vertex = [m->library newFunctionWithName:@"ma2_clear_vertex"];
            m->clear_color = [m->library newFunctionWithName:@"ma2_clear_color"];
            m->clear_depth = [m->library newFunctionWithName:@"ma2_clear_depth"];
            id<MTLFunction> clear = [m->library newFunctionWithName:@"ma2_clear"];
            if (clear) m->clear = [m->device newComputePipelineStateWithFunction:clear error:&error];
            m->pipelines = [[NSMutableDictionary alloc] init];
            m->samplers = [[NSMutableDictionary alloc] init];
            m->depth_states = [[NSMutableDictionary alloc] init];
            m->inflight = [[NSMutableArray alloc] init];
            m->visibility = [m->device newBufferWithLength:kVisibilitySlots * sizeof(uint64_t)
                                                   options:MTLResourceStorageModeShared];
            MTLTextureDescriptor *white = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            white.usage = MTLTextureUsageShaderRead;
            white.storageMode = MTLStorageModeShared;
            m->white = [m->device newTextureWithDescriptor:white];
            MTLTextureDescriptor *white_cube = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
            white_cube.usage = MTLTextureUsageShaderRead;
            white_cube.storageMode = MTLStorageModeShared;
            m->white_cube = [m->device newTextureWithDescriptor:white_cube];
            m->white_sampler = [m->device newSamplerStateWithDescriptor:[[MTLSamplerDescriptor alloc] init]];
            if (!m->vertex || !m->color || !m->color_depth || !m->shaded ||
                !m->shaded_depth || !m->clear_vertex || !m->clear_color || !m->clear_depth ||
                !m->clear || !m->pipelines || !m->samplers || !m->depth_states ||
                !m->inflight || !m->visibility || !m->white || !m->white_cube ||
                !m->white_sampler) {
                fail(m, Failure::Shader, "entry points or renderer resources", error, false);
                ma2_metal_destroy(m);
                return nullptr;
            }
            uint32_t opaque_white = 0xffffffffu;
            [m->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                          withBytes:&opaque_white bytesPerRow:4];
            for (NSUInteger face = 0; face < 6; ++face)
                [m->white_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face
                                   withBytes:&opaque_white bytesPerRow:4 bytesPerImage:4];
            return m;
        } @catch (NSException *exception) {
            fail(m, Failure::Device, exception.reason.UTF8String, nil, false);
            ma2_metal_destroy(m);
            return nullptr;
        }
    }
}

extern "C" void ma2_metal_backend(Ma2MetalRenderer *m, Nv2aBackend *backend)
{
    if (!backend) return;
    memset(backend, 0, sizeof(*backend));
    if (!m) return;
    backend->version = NV2A_BACKEND_VERSION;
    backend->struct_size = sizeof(*backend);
    backend->context = m;
    backend->clear = clear_callback;
    backend->draw = draw_callback;
    backend->sync = sync_callback;
    backend->readback = readback_callback;
    backend->invalidate = invalidate_callback;
    backend->flip = flip_callback;
    backend->present_sync = present_callback;
    backend->report_failure = failure_callback;
    backend->visibility_reset = visibility_reset_callback;
    backend->visibility_count = visibility_count_callback;
}

extern "C" const char *ma2_metal_last_error(const Ma2MetalRenderer *m)
{
    return m && m->failed ? m->error : nullptr;
}

extern "C" int ma2_metal_destroy(Ma2MetalRenderer *m)
{
    if (!m) return 1;
    @autoreleasepool {
        // Never leave the GPU writing textures that are about to be released.
        if (m->command || (m->inflight && m->inflight.count)) {
            end_encoder(m);
            if (m->command) [m->command commit];
            [m->command waitUntilCompleted];
            for (id<MTLCommandBuffer> command in m->inflight) [command waitUntilCompleted];
        }
    }
    int ok = !m->failed;
    if (m->console_fd >= 0 && close(m->console_fd) != 0) ok = 0;
    if (m->log_fd >= 0 && close(m->log_fd) != 0) ok = 0;
    delete m;
    return ok;
}

#ifdef MA2_METAL_TESTING
extern "C" void ma2_metal_test_fail_next_submission(Ma2MetalRenderer *m)
{
    if (m) m->fail_next_submission = true;
}

extern "C" void ma2_metal_test_fail_next_depth_readback(Ma2MetalRenderer *m)
{
    if (m) m->fail_next_depth_readback = true;
}

extern "C" uint64_t ma2_metal_test_submission_count(const Ma2MetalRenderer *m)
{
    return m ? m->submissions : 0;
}

extern "C" uint64_t ma2_metal_test_command_count(const Ma2MetalRenderer *m)
{
    return m ? m->command_buffers : 0;
}
#endif

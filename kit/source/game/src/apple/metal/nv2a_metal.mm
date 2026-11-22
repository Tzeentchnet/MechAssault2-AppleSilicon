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
#include <new>
#include <vector>
#include <errno.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMaxExtent = 4096;
constexpr uint32_t kMaxVertices = 4096;
constexpr uint64_t kMaxTextureBytes = UINT64_C(64) * 1024 * 1024;

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
    std::vector<uint32_t> pixels;
};

struct TextureUpload {
    bool sampled = false, mipmapped = false, min_linear = false, mag_linear = false;
    float min_lod = 0, max_lod = 0;
    std::vector<TextureLevel> levels;
};

enum class Failure { Device, Shader, Resource, Pipeline, Command, Memory, Interop };

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
    __strong id<MTLComputePipelineState> clear = nil;
    __strong NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *pipelines = nil;
    uint64_t submissions = 0;
    uint64_t clear_accepted = 0, clear_declined = 0;
    uint64_t draw_accepted = 0, draw_declined = 0;
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

static bool depth_surface(const Nv2aGpuState *s, const Geometry &g, Surface &depth)
{
    uint32_t zformat = (s->format >> 4) & 15u;
    if (zformat != 1 && zformat != 2) return false;
    uint32_t bpp = zformat == 1 ? 2 : 4;
    uint64_t pitch = s->depth_pitch ? s->depth_pitch : (uint64_t)g.width * bpp;
    return pitch <= UINT32_MAX && mapped_surface(s->depth_va, (uint32_t)pitch, bpp, g, depth);
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

static bool completed(Ma2MetalRenderer *m, id<MTLCommandBuffer> command)
{
    [command commit];
    ++m->submissions;
    [command waitUntilCompleted];
#ifdef MA2_METAL_TESTING
    if (m->fail_next_submission) {
        m->fail_next_submission = false;
        fail(m, Failure::Command, "injected submission failure");
        return false;
    }
#endif
    if (command.status != MTLCommandBufferStatusCompleted) {
        fail(m, Failure::Command, "command buffer", command.error);
        return false;
    }
    return true;
}

static void report_operation(Ma2MetalRenderer *m, bool draw, int accepted)
{
    uint64_t &counter = draw
        ? (accepted ? m->draw_accepted : m->draw_declined)
        : (accepted ? m->clear_accepted : m->clear_declined);
    ++counter;
    uint64_t total = m->clear_accepted + m->clear_declined
                   + m->draw_accepted + m->draw_declined;
    if (m->perf && (total == 1 || (total & 1023u) == 0))
        fprintf(stderr,
                "[NV2A-METAL] operations=%llu submissions=%llu "
                "clear=%llu accepted/%llu declined draw=%llu accepted/%llu declined\n",
                (unsigned long long)total, (unsigned long long)m->submissions,
                (unsigned long long)m->clear_accepted,
                (unsigned long long)m->clear_declined,
                (unsigned long long)m->draw_accepted,
                (unsigned long long)m->draw_declined);
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
    Surface color_surface = {}, z_surface = {};
    if (color_write && !mapped_surface(s->color_va, s->pitch, s->bpp, g, color_surface)) return 0;
    if ((depth_write || stencil_write) && !depth_surface(s, g, z_surface)) return 0;
    if (color_write && (depth_write || stencil_write) && overlap(color_surface, z_surface)) return 0;

    size_t pixels = (size_t)g.w * g.h;
    size_t color_bytes = color_write ? pixels * s->bpp : 4;
    size_t depth_bytes = depth_write || stencil_write ? pixels * z_surface.bpp : 4;
    id<MTLBuffer> colors = [m->device newBufferWithLength:color_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> depths = [m->device newBufferWithLength:depth_bytes options:MTLResourceStorageModeShared];
#ifdef MA2_METAL_TESTING
    ++m->command_buffers;
#endif
    id<MTLCommandBuffer> command = [m->queue commandBuffer];
    if (!colors || !depths || !command) {
        fail(m, Failure::Resource, "clear buffers or command buffer");
        return 1;
    }
    if (color_write) copy_from_guest(colors.contents, color_surface, g);
    if (depth_write || stencil_write) copy_from_guest(depths.contents, z_surface, g);
    Clear constants = {g.w, g.h, cformat, zformat, mask, color,
                       depth_write ? (uint32_t)depth_value : 0, s->stencil_clear & 255u};
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
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
    if (!completed(m, command)) return 1;
    if (color_write)
        copy_to_guest(color_surface, g, colors.contents);
    if (depth_write || stencil_write) copy_to_guest(z_surface, g, depths.contents);
    return 1;
}

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

static bool prepare_textures(const Nv2aGpuState *s, Nv2aGpuTexel texel,
                             Guest &g, TextureUpload (&uploads)[4])
{
    // Validate every stage before decoding; both phases precede any command buffer.
    for (unsigned stage = 0; stage < 4; ++stage) {
        uint32_t mode = s->use_rc ? (s->rc.stage_program >> (stage * 5)) & 31u : 0;
        bool sampled = !s->use_rc && s->textured && stage == 0;
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
        if (!texel || !t.valid || !t.width || !t.height ||
            t.width > kMaxExtent || t.height > kMaxExtent ||
            t.cube || t.dimensionality > 2)
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
            upload.levels.push_back({descriptor, {}});
            offset += bytes;
        }
    }

    for (TextureUpload &upload : uploads) {
        for (TextureLevel &level : upload.levels) {
            const Nv2aGpuTexture &t = level.descriptor;
            level.pixels.resize((size_t)t.width * t.height);
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                    if (!texel(&t, 0, x, y, 0, &level.pixels[(size_t)y * t.width + x]))
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

struct TextureResources {
    __strong id<MTLTexture> textures[4] = {};
    __strong id<MTLSamplerState> samplers[4] = {};
};

static bool upload_textures(Ma2MetalRenderer *m, const Nv2aGpuState *s,
                            const TextureUpload (&uploads)[4], TextureResources &resources)
{
    int first = -1;
    for (unsigned stage = 0; stage < 4; ++stage) {
        const TextureUpload &upload = uploads[stage];
        if (!upload.sampled) continue;
        const Nv2aGpuTexture &t = s->textures[stage];
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                        width:t.width height:t.height mipmapped:upload.levels.size() > 1];
        desc.mipmapLevelCount = upload.levels.size();
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        resources.textures[stage] = [m->device newTextureWithDescriptor:desc];
        MTLSamplerDescriptor *sampler = [[MTLSamplerDescriptor alloc] init];
        sampler.normalizedCoordinates = YES;
        sampler.sAddressMode = texture_address(t.addr_u);
        sampler.tAddressMode = texture_address(t.addr_v);
        sampler.minFilter = upload.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sampler.magFilter = upload.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        uint32_t min_filter = (t.filter >> 16) & 255;
        sampler.mipFilter = !upload.mipmapped ? MTLSamplerMipFilterNotMipmapped :
                            min_filter >= 5 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sampler.lodMinClamp = upload.min_lod;
        sampler.lodMaxClamp = upload.max_lod;
        if (t.border_color == 0xff000000u) sampler.borderColor = MTLSamplerBorderColorOpaqueBlack;
        else if (t.border_color == 0xffffffffu) sampler.borderColor = MTLSamplerBorderColorOpaqueWhite;
        else sampler.borderColor = MTLSamplerBorderColorTransparentBlack;
        resources.samplers[stage] = [m->device newSamplerStateWithDescriptor:sampler];
        if (!resources.textures[stage] || !resources.samplers[stage]) {
            fail(m, Failure::Resource, "sampled texture or sampler");
            return false;
        }
        for (size_t level = 0; level < upload.levels.size(); ++level) {
            const TextureLevel &data = upload.levels[level];
            [resources.textures[stage]
                replaceRegion:MTLRegionMake2D(0, 0, data.descriptor.width, data.descriptor.height)
                  mipmapLevel:level withBytes:data.pixels.data()
                  bytesPerRow:(size_t)data.descriptor.width * 4];
        }
        if (first < 0) first = (int)stage;
    }
    if (first < 0) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        resources.textures[0] = [m->device newTextureWithDescriptor:desc];
        MTLSamplerDescriptor *sampler = [[MTLSamplerDescriptor alloc] init];
        resources.samplers[0] = [m->device newSamplerStateWithDescriptor:sampler];
        if (!resources.textures[0] || !resources.samplers[0]) {
            fail(m, Failure::Resource, "fallback texture or sampler");
            return false;
        }
        uint32_t white = 0xffffffffu;
        [resources.textures[0] replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                 mipmapLevel:0 withBytes:&white bytesPerRow:4];
        first = 0;
    }
    for (unsigned stage = 0; stage < 4; ++stage) {
        if (!resources.textures[stage]) resources.textures[stage] = resources.textures[first];
        if (!resources.samplers[stage]) resources.samplers[stage] = resources.samplers[first];
    }
    return true;
}

static id<MTLRenderPipelineState> pipeline(Ma2MetalRenderer *m, const Nv2aGpuState *s)
{
    bool shaded = s->use_rc || s->textured;
    NSString *key = [NSString stringWithFormat:@"%u:%u:%u:%u:%u:%u:%u",
        s->depth_test != 0, s->blend_enable != 0, s->blend_src, s->blend_dst,
        s->blend_equation, s->color_mask, shaded];
    id<MTLRenderPipelineState> cached = m->pipelines[key];
    if (cached) return cached;
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = m->vertex;
    desc.fragmentFunction = shaded ? (s->depth_test ? m->shaded_depth : m->shaded)
                                   : (s->depth_test ? m->color_depth : m->color);
    desc.depthAttachmentPixelFormat = s->depth_test ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
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
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [m->device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!result) {
        fail(m, Failure::Pipeline, "triangle pipeline", error);
        return nil;
    }
    if (m->pipelines.count >= 128) [m->pipelines removeAllObjects];
    m->pipelines[key] = result;
    return result;
}

static void add_triangle(std::vector<Vertex> &out, const Nv2aVshOutput *vertices,
                         uint32_t a, uint32_t b, uint32_t c, uint32_t flat,
                         bool legacy, bool use_rc, uint32_t tex_coords)
{
    const uint32_t indices[] = {a, b, c};
    for (uint32_t index : indices) {
        Vertex v = {};
        memcpy(v.position, vertices[index].pos, sizeof(v.position));
        memcpy(v.color, vertices[legacy ? flat : index].d0, sizeof(v.color));
        if (use_rc) {
            memcpy(v.secondary, vertices[index].d1, sizeof(v.secondary));
            v.fog[0] = vertices[index].fog[0];
        }
        for (unsigned stage = 0; stage < 4; ++stage)
            if (tex_coords & (1u << stage))
                memcpy(v.tex[stage], vertices[index].tex[stage], sizeof(v.tex[stage]));
        out.push_back(v);
    }
}

static bool triangles(std::vector<Vertex> &out, const Nv2aVshOutput *v,
                      uint32_t count, uint32_t primitive, const Nv2aGpuState *s)
{
    if (primitive < 5 || primitive > 10 || count > kMaxVertices || (count && !v)) return false;
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
            if (!std::isfinite(v[i].pos[j]) || !std::isfinite(v[i].d0[j])) return false;
        if (s->use_rc) {
            for (float value : v[i].d1)
                if (!std::isfinite(value)) return false;
            if (!std::isfinite(v[i].fog[0])) return false;
        }
        for (unsigned stage = 0; stage < 4; ++stage)
            if (tex_coords & (1u << stage))
                for (float value : v[i].tex[stage])
                    if (!std::isfinite(value)) return false;
        if (v[i].pos[3] <= 0.001f) return false;
        double w = v[i].pos[3];
        if (std::fabs((double)v[i].pos[0]) > FLT_MAX / 16.0 ||
            std::fabs((double)v[i].pos[1]) > FLT_MAX / 16.0 ||
            std::fabs((double)v[i].pos[0] * w) > FLT_MAX / 4.0 ||
            std::fabs((double)v[i].pos[1] * w) > FLT_MAX / 4.0 ||
            std::fabs((double)v[i].pos[2] * w) > FLT_MAX / 4.0) return false;
    }
    out.reserve((size_t)count * 3);
    // Keep the old legacy color path flat; register combiners interpolate it.
#define TRI(a, b, c, flat) \
    add_triangle(out, v, a, b, c, flat, s->legacy && !s->use_rc, s->use_rc != 0, tex_coords)
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

static int draw_impl(Ma2MetalRenderer *m, const Nv2aGpuState *s, const Nv2aVshOutput *v,
                     uint32_t count, uint32_t primitive, Nv2aGpuTexel texel)
{
    Geometry g;
    if (!geometry(s, g) || (s->format & 15u) != 8 ||
        (s->alpha_test && !(s->use_rc || s->textured)) ||
        s->stencil_test || s->polygon_offset_enable ||
        (s->color_mask & ~0x01010101u))
        return 0;
    bool shaded = s->use_rc || s->textured;
    if (s->alpha_test && (s->alpha_func < 0x200 || s->alpha_func > 0x207)) return 0;
    if (shaded && (!std::isfinite(s->clip_min) || !std::isfinite(s->clip_max))) return 0;
    uint32_t zformat = (s->format >> 4) & 15u;
    float max_depth = zformat == 1 ? 65535.0f : 16777215.0f;
    if (s->z_clamp && (!std::isfinite(s->clip_min) || !std::isfinite(s->clip_max) ||
        s->clip_min != 0 || (s->clip_max > 0 && s->clip_max != max_depth)))
        return 0;
    if (s->depth_test && (s->depth_func < 0x200 || s->depth_func > 0x207)) return 0;
    if (s->cull_enable && ((s->front_face != 0x900 && s->front_face != 0x901) ||
        (s->cull_face != 0x404 && s->cull_face != 0x405 && s->cull_face != 0x408)))
        return 0;
    if (s->blend_enable) {
        MTLBlendFactor sr, sa, dr, da;
        MTLBlendOperation op;
        if (!blend_factor(s->blend_src, sr, sa, false) ||
            !blend_factor(s->blend_dst, dr, da, true) ||
            !blend_operation(s->blend_equation, op)) return 0;
    }
    Surface color_surface = {}, z_surface = {};
    if (!mapped_surface(s->color_va, s->pitch, s->bpp, g, color_surface)) return 0;
    if (s->depth_test && (!depth_surface(s, g, z_surface) || overlap(color_surface, z_surface))) return 0;
    std::vector<Vertex> vertices;
    if (!triangles(vertices, v, count, primitive, s)) return 0;
    Guest guest = {};
    TextureUpload uploads[4];
    if (shaded) {
        guest_constants(s, guest);
        if (!prepare_textures(s, texel, guest, uploads)) return 0;
    }
    if (vertices.empty() || (s->cull_enable && s->cull_face == 0x408)) return 1;

    id<MTLRenderPipelineState> state = pipeline(m, s);
    if (!state) return 1;
    TextureResources texture_resources;
    if (shaded && !upload_textures(m, s, uploads, texture_resources)) return 1;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:g.width height:g.height mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> color_texture = [m->device newTextureWithDescriptor:desc];
    size_t pixels = (size_t)g.w * g.h;
    id<MTLBuffer> color_buffer = [m->device newBufferWithLength:pixels * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> vertex_buffer = [m->device newBufferWithBytes:vertices.data()
        length:vertices.size() * sizeof(Vertex) options:MTLResourceStorageModeShared];
    if (!color_texture || !color_buffer || !vertex_buffer) {
        fail(m, Failure::Resource, "triangle color or vertices");
        return 1;
    }
    MTLRegion rect = MTLRegionMake2D(g.x, g.y, g.w, g.h);
    copy_from_guest(color_buffer.contents, color_surface, g);
    [color_texture replaceRegion:rect mipmapLevel:0 withBytes:color_buffer.contents bytesPerRow:(size_t)g.w * 4];

    id<MTLTexture> depth_texture = nil;
    id<MTLBuffer> depth_buffer = nil;
    id<MTLDepthStencilState> depth_state = nil;
    if (s->depth_test) {
        MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                        width:g.width height:g.height mipmapped:NO];
        depth_desc.usage = MTLTextureUsageRenderTarget;
        depth_desc.storageMode = MTLStorageModeShared;
        depth_texture = [m->device newTextureWithDescriptor:depth_desc];
        depth_buffer = [m->device newBufferWithLength:pixels * sizeof(float) options:MTLResourceStorageModeShared];
        MTLDepthStencilDescriptor *depth_desc_state = [[MTLDepthStencilDescriptor alloc] init];
        static const MTLCompareFunction compare[] = {
            MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
            MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
            MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways
        };
        depth_desc_state.depthCompareFunction = compare[s->depth_func - 0x200];
        depth_desc_state.depthWriteEnabled = s->depth_mask != 0;
        depth_state = [m->device newDepthStencilStateWithDescriptor:depth_desc_state];
        if (!depth_texture || !depth_buffer || !depth_state) {
            fail(m, Failure::Resource, "triangle depth resources");
            return 1;
        }
        float *values = static_cast<float *>(depth_buffer.contents);
        for (uint32_t y = 0; y < g.h; ++y) {
            const uint8_t *row = z_surface.base + (size_t)(g.y + y) * z_surface.pitch + (size_t)g.x * z_surface.bpp;
            for (uint32_t x = 0; x < g.w; ++x) {
                uint32_t packed = 0;
                memcpy(&packed, row + (size_t)x * z_surface.bpp, z_surface.bpp);
                uint32_t z = zformat == 1 ? packed : packed >> 8;
                values[(size_t)y * g.w + x] = (float)((double)z / (double)max_depth);
            }
        }
        [depth_texture replaceRegion:rect mipmapLevel:0 withBytes:values bytesPerRow:(size_t)g.w * sizeof(float)];
    }

#ifdef MA2_METAL_TESTING
    ++m->command_buffers;
#endif
    id<MTLCommandBuffer> command = [m->queue commandBuffer];
    if (!command) {
        fail(m, Failure::Resource, "triangle command buffer");
        return 1;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (depth_texture) {
        pass.depthAttachment.texture = depth_texture;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
        fail(m, Failure::Resource, "triangle encoder");
        return 1;
    }
    Viewport viewport = {(float)g.width, (float)g.height, max_depth, 0};
    [encoder setRenderPipelineState:state];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBytes:&viewport length:sizeof(viewport) atIndex:1];
    if (shaded) {
        [encoder setFragmentBytes:&guest length:sizeof(guest) atIndex:1];
        for (unsigned stage = 0; stage < 4; ++stage) {
            [encoder setFragmentTexture:texture_resources.textures[stage] atIndex:stage];
            [encoder setFragmentSamplerState:texture_resources.samplers[stage] atIndex:stage];
        }
    }
    if (depth_texture) {
        [encoder setDepthStencilState:depth_state];
        [encoder setFragmentBytes:&viewport length:sizeof(viewport) atIndex:0];
    }
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
    [encoder endEncoding];
    if (!completed(m, command)) return 1;
    if (s->color_mask & 0x01010101u) {
        [color_texture getBytes:color_buffer.contents bytesPerRow:(size_t)g.w * 4
                     fromRegion:rect mipmapLevel:0];
    }
    if (depth_texture && s->depth_mask) {
#ifdef MA2_METAL_TESTING
        if (m->fail_next_depth_readback) {
            m->fail_next_depth_readback = false;
            fail(m, Failure::Command, "injected depth readback failure");
            return 1;
        }
#endif
        float *values = static_cast<float *>(depth_buffer.contents);
        [depth_texture getBytes:values bytesPerRow:(size_t)g.w * sizeof(float)
                     fromRegion:rect mipmapLevel:0];
        for (uint32_t y = 0; y < g.h; ++y) {
            uint8_t *row = z_surface.base + (size_t)(g.y + y) * z_surface.pitch + (size_t)g.x * z_surface.bpp;
            for (uint32_t x = 0; x < g.w; ++x) {
                float d = std::max(0.0f, std::min(1.0f, values[(size_t)y * g.w + x]));
                uint32_t z = (uint32_t)std::round((double)d * (double)max_depth);
                uint32_t packed = 0;
                if (zformat == 2) {
                    memcpy(&packed, row + (size_t)x * 4, 4);
                    packed = (z << 8) | (packed & 255u);
                } else packed = z;
                memcpy(row + (size_t)x * z_surface.bpp, &packed, z_surface.bpp);
            }
        }
    }
    if (s->color_mask & 0x01010101u)
        copy_to_guest(color_surface, g, color_buffer.contents);
    return 1;
}

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

static int sync_callback(void *context)
{
    return !static_cast<Ma2MetalRenderer *>(context)->failed;
}

static int readback_callback(void *context, uint32_t va)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    if (m->failed) return 0;
    if (!va || !ma2_metal_translate(va, 1)) {
        fail(m, Failure::Resource, "unmapped readback");
        return 0;
    }
    return 1;
}

static void invalidate_callback(void *, uint32_t) {}
static void flip_callback(void *, uint32_t, uint32_t) {}

static void failure_callback(void *context, const char *operation)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    fail(m, Failure::Interop, operation, nil, false);
}

static int present_callback(void *context, uint32_t va, uint32_t pitch)
{
    Ma2MetalRenderer *m = static_cast<Ma2MetalRenderer *>(context);
    if (m->failed) return 0;
    if (!pitch || !va || !ma2_metal_translate(va, pitch)) {
        fail(m, Failure::Resource, "unmapped scanout");
        return 0;
    }
    return 1;
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
            id<MTLFunction> clear = [m->library newFunctionWithName:@"ma2_clear"];
            if (clear) m->clear = [m->device newComputePipelineStateWithFunction:clear error:&error];
            m->pipelines = [[NSMutableDictionary alloc] init];
            if (!m->vertex || !m->color || !m->color_depth || !m->shaded ||
                !m->shaded_depth || !m->clear || !m->pipelines) {
                fail(m, Failure::Shader, "entry points or clear pipeline", error, false);
                ma2_metal_destroy(m);
                return nullptr;
            }
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
}

extern "C" const char *ma2_metal_last_error(const Ma2MetalRenderer *m)
{
    return m && m->failed ? m->error : nullptr;
}

extern "C" int ma2_metal_destroy(Ma2MetalRenderer *m)
{
    if (!m) return 1;
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

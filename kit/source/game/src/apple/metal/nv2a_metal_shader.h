#ifndef MA2_NV2A_METAL_SHADER_H
#define MA2_NV2A_METAL_SHADER_H

static const char ma2_metal_shader_source[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
    float4 position;
    float4 color;
    float4 secondary;
    float4 fog;
    float4 tex0, tex1, tex2, tex3;
};
struct Viewport {
    float width, height, depth_max, unused;
};
struct Varying {
    float4 position [[position]];
    float4 color;
    float4 secondary;
    float fog;
    float4 tex0, tex1, tex2, tex3;
};

vertex Varying ma2_vertex(uint id [[vertex_id]],
                          const device Vertex *vertices [[buffer(0)]],
                          constant Viewport &viewport [[buffer(1)]])
{
    Vertex v = vertices[id];
    float2 screen = trunc(v.position.xy * 16.0) / 16.0;
    float2 ndc = screen / float2(viewport.width, viewport.height);
    ndc = ndc * float2(2.0, -2.0) + float2(-1.0, 1.0);
    Varying out;
    out.position = float4(ndc * v.position.w,
                          v.position.z / viewport.depth_max * v.position.w,
                          v.position.w);
    out.color = clamp(v.color, 0.0, 1.0);
    out.secondary = clamp(v.secondary, 0.0, 1.0);
    out.fog = v.fog.x;
    out.tex0 = v.tex0; out.tex1 = v.tex1;
    out.tex2 = v.tex2; out.tex3 = v.tex3;
    return out;
}

fragment float4 ma2_color(Varying in [[stage_in]])
{
    return in.color;
}

struct DepthOutput {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};
fragment DepthOutput ma2_color_depth(Varying in [[stage_in]],
                                     constant Viewport &viewport [[buffer(0)]])
{
    DepthOutput out;
    out.color = in.color;
    out.depth = round(clamp(in.position.z, 0.0, 1.0) * viewport.depth_max)
                / viewport.depth_max;
    return out;
}

struct Guest {
    uint4 control, finalWords;
    float4 fogColor, finalC0, finalC1;
    uint4 programs[8];
    float4 factor0[8], factor1[8], texInfo[4];
    uint4 textureControl;
    float4 bumpMatrix[4], bumpLuminance[4], eyeVector, textureBias, misc;
    float4 borderColor[4];
    uint4 borderMode;
};

// Mirrors the 2D texture-stage and combiner path in nv2a_gpu_shader.h.
// A stage holding a guest cube map binds it at texture(4 + stage) and sets
// texInfo.w; every other stage samples its 2D texture.
#define MA2_TEXTURE_PARAMS \
    texture2d<float> tex0, texture2d<float> tex1, texture2d<float> tex2, texture2d<float> tex3, \
    texturecube<float> cube0, texturecube<float> cube1, texturecube<float> cube2, \
    texturecube<float> cube3, \
    sampler smp0, sampler smp1, sampler smp2, sampler smp3
#define MA2_TEXTURE_ARGS tex0, tex1, tex2, tex3, cube0, cube1, cube2, cube3, \
    smp0, smp1, smp2, smp3
#define MA2_TEXTURE_BINDINGS \
    texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]], \
    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]], \
    texturecube<float> cube0 [[texture(4)]], texturecube<float> cube1 [[texture(5)]], \
    texturecube<float> cube2 [[texture(6)]], texturecube<float> cube3 [[texture(7)]], \
    sampler smp0 [[sampler(0)]], sampler smp1 [[sampler(1)]], \
    sampler smp2 [[sampler(2)]], sampler smp3 [[sampler(3)]]

float2 cube_as_2d(float3 c)
{
    float3 a = abs(c);
    if (a.x >= a.y && a.x >= a.z)
        return float2(c.x > 0 ? -c.z : c.z, c.y) / max(a.x, 1e-20f);
    if (a.y >= a.z)
        return float2(c.x, c.y > 0 ? -c.z : c.z) / max(a.y, 1e-20f);
    return float2(c.z > 0 ? c.x : -c.x, c.y) / max(a.z, 1e-20f);
}

float4 texture_sample(uint stage, float3 c, bool cube, constant Guest &g,
                      MA2_TEXTURE_PARAMS)
{
    if (!g.texInfo[stage].z) return 1.0f;
    if (cube && g.texInfo[stage].w != 0) {
        float3 d = all(c == 0.0f) ? float3(1, 0, 0) : c;
        if (stage == 0) return cube0.sample(smp0, d, bias(g.textureBias[0]));
        if (stage == 1) return cube1.sample(smp1, d, bias(g.textureBias[1]));
        if (stage == 2) return cube2.sample(smp2, d, bias(g.textureBias[2]));
        return cube3.sample(smp3, d, bias(g.textureBias[3]));
    }
    float2 uv = (cube ? cube_as_2d(c) : c.xy) * g.texInfo[stage].xy;
    float4 color;
    float2 size;
    if (stage == 0) { color = tex0.sample(smp0, uv, bias(g.textureBias[0])); size = float2(tex0.get_width(), tex0.get_height()); }
    else if (stage == 1) { color = tex1.sample(smp1, uv, bias(g.textureBias[1])); size = float2(tex1.get_width(), tex1.get_height()); }
    else if (stage == 2) { color = tex2.sample(smp2, uv, bias(g.textureBias[2])); size = float2(tex2.get_width(), tex2.get_height()); }
    else { color = tex3.sample(smp3, uv, bias(g.textureBias[3])); size = float2(tex3.get_width(), tex3.get_height()); }
    uint mode = g.borderMode[stage];
    if (!mode) return color;
    // The sampler's border is transparent black, so color holds only the
    // texels inside the texture; the border colour takes the remaining
    // filter weight, per texel as the CPU and D3D11 samplers apply it.
    float coverage = 1;
    for (uint axis = 0; axis < 2; ++axis) {
        if (!(mode & (1u << axis))) continue;
        float x = uv[axis] * size[axis], n = size[axis];
        if (mode & 4u) {
            float t = x - 0.5f, x0 = floor(t), f = t - x0;
            coverage *= (x0 >= 0 && x0 < n ? 1 - f : 0) + (x0 + 1 >= 0 && x0 + 1 < n ? f : 0);
        } else {
            coverage *= x >= 0 && x < n ? 1.0f : 0.0f;
        }
    }
    return color + g.borderColor[stage] * (1 - coverage);
}

float3 signed_dot_map(float3 v, uint mode)
{
    float3 b = v * 255.0f;
    if (mode == 1) return (b - 128.0f) / 127.0f;
    if (mode == 2)
        return float3(b.x >= 128 ? (b.x - 255.5f) / 127.5f : (b.x + .5f) / 127.5f,
                      b.y >= 128 ? (b.y - 255.5f) / 127.5f : (b.y + .5f) / 127.5f,
                      b.z >= 128 ? (b.z - 255.5f) / 127.5f : (b.z + .5f) / 127.5f);
    return float3(b.x >= 128 ? (b.x - 256.0f) / 127.0f : b.x / 127.0f,
                  b.y >= 128 ? (b.y - 256.0f) / 127.0f : b.y / 127.0f,
                  b.z >= 128 ? (b.z - 256.0f) / 127.0f : b.z / 127.0f);
}

float3 dot_map(float4 c, uint mode)
{
    if (mode == 0) return c.rgb;
    if (mode <= 3) return signed_dot_map(c.rgb, mode);
    if (mode == 4) {
        uint4 b = uint4(c * 255.0f);
        return float3(((b.a << 8) | b.r) / 65535.0f,
                      ((b.g << 8) | b.b) / 65535.0f, 1.0f);
    }
    return c.rgb;
}

uint stage_input(uint stage, constant Guest &g)
{
    return stage < 2 ? 0 : min((g.textureControl.x >> (stage == 2 ? 16 : 20)) & 15, stage - 1);
}

uint stage_dot_map(uint stage, constant Guest &g)
{
    return stage ? (g.textureControl.x >> ((stage - 1) * 4)) & 15 : 0;
}

float4 sample_stage(uint stage, thread float4 *coords, thread float4 *prior,
                    thread float4 &dots, uint mode, constant Guest &g, MA2_TEXTURE_PARAMS)
{
    float4 c = coords[stage];
    if (mode == 0) return float4(0, 0, 0, 1);
    if (mode == 4) return saturate(c);
    if (mode == 5) {
        for (uint j = 0; j < 4; ++j) {
            bool ge = ((g.control.z >> (stage * 4 + j)) & 1) != 0;
            if (ge ? c[j] >= 0 : c[j] < 0) discard_fragment();
        }
        return 0.0f;
    }
    if (mode == 1 || mode == 2)
        return texture_sample(stage, c.xyz / (c.w != 0 ? c.w : 1), false, g, MA2_TEXTURE_ARGS);
    if (mode == 3) return texture_sample(stage, c.xyz, true, g, MA2_TEXTURE_ARGS);
    float4 previous = prior[stage_input(stage, g)];
    if (mode == 6 || mode == 7) {
        float2 bump = signed_dot_map(previous.bgg, 3).xy;
        float4 m = g.bumpMatrix[stage];
        float2 dsdt = float2(m.x * bump.x + m.z * bump.y, m.y * bump.x + m.w * bump.y);
        float4 color = texture_sample(stage, float3(c.xy + dsdt, c.z), false, g, MA2_TEXTURE_ARGS);
        return mode == 7 ? color * (g.bumpLuminance[stage].x * previous.r
                                  + g.bumpLuminance[stage].y) : color;
    }
    if (mode == 15) return texture_sample(stage, float3(previous.ar, 0), false, g, MA2_TEXTURE_ARGS);
    if (mode == 16) return texture_sample(stage, float3(previous.gb, 0), false, g, MA2_TEXTURE_ARGS);
    if (mode == 17 || (mode >= 9 && mode <= 14) || mode == 18) {
        dots[stage] = dot(c.xyz, dot_map(previous, stage_dot_map(stage, g)));
        if (mode == 17 || mode == 10) return 0.0f;
        if (mode == 9)
            return texture_sample(stage, float3(dots[max(stage, 1u) - 1], dots[stage], 0),
                                  false, g, MA2_TEXTURE_ARGS);
        if (mode == 11) {
            float nextDot = dot(coords[3].xyz, dot_map(prior[stage_input(3, g)], stage_dot_map(3, g)));
            return texture_sample(stage, float3(dots[1], dots[2], nextDot), true, g, MA2_TEXTURE_ARGS);
        }
        float3 n = float3(dots[1], dots[2], dots[3]);
        if (mode == 12 || mode == 18) {
            float3 e = mode == 18 ? g.eyeVector.xyz : float3(coords[1].w, coords[2].w, coords[3].w);
            n = 2 * n * dot(n, e) / max(dot(n, n), 1e-20f) - e;
        }
        return texture_sample(stage, n, mode != 13, g, MA2_TEXTURE_ARGS);
    }
    return 0.0f;
}

float3 input_rgb(thread const float4 *r, uint b)
{
    float3 x = (b & 16) ? r[b & 15].aaa : r[b & 15].rgb;
    float3 p = max(x, 0.0f);
    switch ((b >> 5) & 7) {
    case 0: return p; case 1: return 1 - saturate(x);
    case 2: return 2 * p - 1; case 3: return 1 - 2 * p;
    case 4: return p - .5f; case 5: return .5f - p;
    case 6: return x; default: return -x;
    }
}

float input_alpha(thread const float4 *r, uint b)
{
    float x = (b & 16) ? r[b & 15].a : r[b & 15].b;
    float p = max(x, 0.0f);
    switch ((b >> 5) & 7) {
    case 0: return p; case 1: return 1 - saturate(x);
    case 2: return 2 * p - 1; case 3: return 1 - 2 * p;
    case 4: return p - .5f; case 5: return .5f - p;
    case 6: return x; default: return -x;
    }
}

float3 output_rgb(float3 x, uint mapping)
{
    switch (mapping) {
    case 8: x -= .5f; break; case 16: x *= 2; break;
    case 24: x = (x - .5f) * 2; break;
    case 32: x *= 4; break; case 48: x *= .5f; break;
    }
    return clamp(x, -1.0f, 1.0f);
}

bool alpha_pass(float a, constant Guest &g)
{
    float r = g.misc.x;
    if (g.finalWords.z == 0x200) return false;
    if (g.finalWords.z == 0x201) return a < r;
    if (g.finalWords.z == 0x202) return a == r;
    if (g.finalWords.z == 0x203) return a <= r;
    if (g.finalWords.z == 0x204) return a > r;
    if (g.finalWords.z == 0x205) return a != r;
    if (g.finalWords.z == 0x206) return a >= r;
    return true;
}

float4 pixel_color(Varying p, constant Guest &g, MA2_TEXTURE_PARAMS)
{
    float4 result;
    if (!g.control.w) {
        result = g.finalWords.w
            ? texture_sample(0, p.tex0.xyz / (p.tex0.w != 0 ? p.tex0.w : 1),
                             false, g, MA2_TEXTURE_ARGS)
            : p.color;
        if (g.finalWords.w == 2) result *= p.color;
    } else {
        float4 r[16];
        for (uint j = 0; j < 16; ++j) r[j] = 0.0f;
        r[3] = float4(g.fogColor.rgb, saturate(p.fog));
        r[4] = p.color; r[5] = p.secondary;
        float4 coords[4] = {p.tex0, p.tex1, p.tex2, p.tex3};
        float4 prior[4] = {float4(0), float4(0), float4(0), float4(0)};
        float4 dots = 0.0f;
        for (uint stage = 0; stage < 4; ++stage) {
            prior[stage] = sample_stage(stage, coords, prior, dots,
                (g.control.y >> (stage * 5)) & 31, g, MA2_TEXTURE_ARGS);
            r[8 + stage] = prior[stage];
        }
        r[12].a = (g.control.y & 31) ? r[8].a : 1;
        uint flags = g.control.x >> 8;
        for (uint stage = 0; stage < min(g.control.x & 255, 8u); ++stage) {
            uint icw = g.programs[stage].x, aicw = g.programs[stage].y;
            uint ocw = g.programs[stage].z, aocw = g.programs[stage].w;
            uint fl = ocw >> 12, afl = aocw >> 12;
            r[1] = g.factor0[(flags & 16) ? stage : 0];
            r[2] = g.factor1[(flags & 256) ? stage : 0];
            bool mux = (flags & 1) ? r[12].a >= .5f : ((int(r[12].a * 255) & 1) != 0);
            float3 A = input_rgb(r, icw >> 24), B = input_rgb(r, icw >> 16);
            float3 C = input_rgb(r, icw >> 8), D = input_rgb(r, icw);
            float aA = input_alpha(r, aicw >> 24), aB = input_alpha(r, aicw >> 16);
            float aC = input_alpha(r, aicw >> 8), aD = input_alpha(r, aicw);
            float3 ab = (fl & 2) ? float3(dot(A, B)) : A * B;
            float3 cd = (fl & 1) ? float3(dot(C, D)) : C * D;
            float3 ms = (fl & 4) ? (mux ? cd : ab) : ab + cd;
            float aab = aA * aB, acd = aC * aD, ams = (afl & 4) ? (mux ? acd : aab) : aab + acd;
            ab = output_rgb(ab, fl & 56); cd = output_rgb(cd, fl & 56);
            ms = output_rgb(ms, fl & 56);
            aab = output_rgb(float3(aab), afl & 56).x;
            acd = output_rgb(float3(acd), afl & 56).x;
            ams = output_rgb(float3(ams), afl & 56).x;
            if ((ocw >> 4) & 15) { r[(ocw >> 4) & 15].rgb = ab; if (fl & 128) r[(ocw >> 4) & 15].a = ab.b; }
            if (ocw & 15) { r[ocw & 15].rgb = cd; if (fl & 64) r[ocw & 15].a = cd.b; }
            if ((ocw >> 8) & 15) r[(ocw >> 8) & 15].rgb = ms;
            if ((aocw >> 4) & 15) r[(aocw >> 4) & 15].a = aab;
            if (aocw & 15) r[aocw & 15].a = acd;
            if ((aocw >> 8) & 15) r[(aocw >> 8) & 15].a = ams;
            r[0] = 0.0f;
        }
        uint f0 = g.finalWords.x, f1 = g.finalWords.y;
        if (f0 || f1) {
            r[1] = g.finalC0; r[2] = g.finalC1;
            float3 a = (f1 & 64) ? 1 - r[5].rgb : r[5].rgb;
            float3 b = (f1 & 32) ? 1 - r[12].rgb : r[12].rgb;
            r[14] = float4((f1 & 128) ? saturate(a + b) : a + b, 0);
            r[15] = float4(input_rgb(r, f1 >> 24) * input_rgb(r, f1 >> 16), 0);
            float3 A = input_rgb(r, f0 >> 24), B = input_rgb(r, f0 >> 16);
            float3 C = input_rgb(r, f0 >> 8), D = input_rgb(r, f0);
            result = float4(D + C * (1 - A) + B * A, input_alpha(r, f1 >> 8));
        } else result = r[12];
    }
    result = saturate(result);
    // Keep comparisons at float precision across fragment discard.
    thread volatile float exact_alpha = result.a;
    if (g.textureControl.y && !alpha_pass(exact_alpha, g)) discard_fragment();
    return result;
}

fragment float4 ma2_shaded(Varying in [[stage_in]], constant Guest &g [[buffer(1)]],
                           MA2_TEXTURE_BINDINGS)
{
    if (g.misc.w && (in.position.z < g.misc.y || in.position.z > g.misc.z))
        discard_fragment();
    return pixel_color(in, g, MA2_TEXTURE_ARGS);
}

fragment DepthOutput ma2_shaded_depth(Varying in [[stage_in]],
                                      constant Viewport &viewport [[buffer(0)]],
                                      constant Guest &g [[buffer(1)]], MA2_TEXTURE_BINDINGS)
{
    DepthOutput out;
    if (g.misc.w && (in.position.z < g.misc.y || in.position.z > g.misc.z))
        discard_fragment();
    out.color = pixel_color(in, g, MA2_TEXTURE_ARGS);
    out.depth = round(clamp(in.position.z, 0.0, 1.0) * viewport.depth_max)
                / viewport.depth_max;
    return out;
}

// Resident-target clears: one triangle covers the viewport, which the
// renderer sets to the clipped clear rectangle. Values arrive exact: colour
// as unorm8/255 and depth as integer/depth_max.
struct ClearVarying { float4 position [[position]]; };
vertex ClearVarying ma2_clear_vertex(uint id [[vertex_id]])
{
    float2 p = float2((id << 1) & 2, id & 2);
    ClearVarying out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}
struct ClearValue { float4 color; float4 depth; };
fragment float4 ma2_clear_color(ClearVarying in [[stage_in]],
                                constant ClearValue &c [[buffer(0)]])
{
    return c.color;
}
fragment DepthOutput ma2_clear_depth(ClearVarying in [[stage_in]],
                                     constant ClearValue &c [[buffer(0)]])
{
    DepthOutput out;
    out.color = c.color;
    out.depth = c.depth.x;
    return out;
}

// A Z24S8 surface as the guest's dwords (z << 8 | stencil), from the depth
// and stencil planes a blit wrote to buffers. round(d * (2^24 - 1)) is taken
// exactly: d * 2^24 is exact in float, so only the fraction is rounded.
struct DepthPack { uint width, height; };
kernel void ma2_pack_depth(device const float *depth [[buffer(0)]],
                           device const uchar *stencil [[buffer(1)]],
                           device uint *out [[buffer(2)]],
                           constant DepthPack &k [[buffer(3)]],
                           uint2 p [[thread_position_in_grid]])
{
    if (p.x >= k.width || p.y >= k.height) return;
    uint i = p.y * k.width + p.x;
    float d = clamp(depth[i], 0.0f, 1.0f);
    float x = d * 16777216.0f, whole = floor(x);
    float z = whole + floor((x - whole) - d + 0.5f);
    out[i] = (uint(clamp(z, 0.0f, 16777215.0f)) << 8) | stencil[i];
}

struct Clear {
    uint width, height, color_format, depth_format;
    uint mask, color, depth, stencil;
};
kernel void ma2_clear(device uint *colors [[buffer(0)]],
                      device uint *depths [[buffer(1)]],
                      constant Clear &c [[buffer(2)]],
                      uint2 p [[thread_position_in_grid]])
{
    if (p.x >= c.width || p.y >= c.height) return;
    uint i = p.y * c.width + p.x;
    if (c.color_format == 8 && (c.mask & 0xf0)) {
        uint bits = ((c.mask & 0x10) ? 0x00ff0000 : 0)
                  | ((c.mask & 0x20) ? 0x0000ff00 : 0)
                  | ((c.mask & 0x40) ? 0x000000ff : 0)
                  | ((c.mask & 0x80) ? 0xff000000 : 0);
        colors[i] = (colors[i] & ~bits) | (c.color & bits);
    } else if (c.color_format == 3 && (c.mask & 0x70)) {
        device ushort *pixels = (device ushort *)colors;
        uint bits = ((c.mask & 0x10) ? 0xf800 : 0)
                  | ((c.mask & 0x20) ? 0x07e0 : 0)
                  | ((c.mask & 0x40) ? 0x001f : 0);
        uint color = ((c.color >> 8) & 0xf800) | ((c.color >> 5) & 0x07e0)
                   | ((c.color >> 3) & 0x001f);
        pixels[i] = ushort((pixels[i] & ~bits) | (color & bits));
    }
    if (c.depth_format == 1 && (c.mask & 1)) {
        device ushort *pixels = (device ushort *)depths;
        pixels[i] = ushort(c.depth);
    } else if (c.depth_format == 2 && (c.mask & 3)) {
        uint old = depths[i];
        if (c.mask & 1) old = (old & 0xff) | (c.depth << 8);
        if (c.mask & 2) old = (old & 0xffffff00) | c.stencil;
        depths[i] = old;
    }
}
)metal";

#endif

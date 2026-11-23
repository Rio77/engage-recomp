#pragma once
/* Shared PipeKey definition and pure SeedPsoRec -> PipeKey conversion helpers,
 * shared by exec.c, prepare_shaders.c, and standalone unit tests.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../../../tools/shader_prep/seed_format.h"
#include "mtl_format_map.h"

struct Prog;

#define ZERO_BINDING 16

typedef struct {
    struct Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

static inline void pipe_key_strip_vi(PipeKey *k) {
    memset(k->attr_fmt, 0, sizeof k->attr_fmt);
    memset(k->attr_off, 0, sizeof k->attr_off);
    memset(k->attr_bind, 0, sizeof k->attr_bind);
    k->attr_mask = 0;
    memset(k->stride, 0, sizeof k->stride);
    memset(k->divisor, 0, sizeof k->divisor);
    k->used_streams = 0;
}

static inline int vk_nvn_attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static inline VkFormat vk_nvn_vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

typedef VkFormat (*VkVertexSupportFn)(VkFormat f);

/* Convert a SeedPsoRec + reflected shader masks into an exact PipeKey matching
 * exec.c:draw_(). Returns false if any active color attachment has an unknown
 * Metal pixel format.
 */
static inline bool vk_seed_to_pipe_key(const SeedPsoRec *rec, uint32_t vs_attr_mask, uint32_t fs_out_mask,
                                       VkFormat d24s8_vk, bool has_divisor, VkVertexSupportFn vfmt_support,
                                       PipeKey *out_key) {
    memset(out_key, 0, sizeof *out_key);

    uint32_t ncolor = 8;
    while (ncolor > 0 && rec->mtl_cfmt[ncolor - 1] == 0) ncolor--;
    out_key->ncolor = ncolor;
    for (uint32_t i = 0; i < ncolor; i++) {
        if (!rec->mtl_cfmt[i]) {
            out_key->color[i] = VK_FORMAT_UNDEFINED;
            continue;
        }
        VkFormat cf = mtl_to_vk_color_format(rec->mtl_cfmt[i]);
        if (cf == VK_FORMAT_UNDEFINED) return false;
        out_key->color[i] = cf;
    }

    if (rec->mtl_dfmt) {
        out_key->depth = mtl_to_vk_depth_format(rec->mtl_dfmt, d24s8_vk);
        if (out_key->depth == VK_FORMAT_UNDEFINED) return false;
    }
    if (rec->mtl_sfmt) {
        out_key->stencil = mtl_to_vk_stencil_format(rec->mtl_sfmt, d24s8_vk);
        if (out_key->stencil == VK_FORMAT_UNDEFINED) return false;
    }

    out_key->samples = rec->samples ? rec->samples : 1;

    for (int i = 0; i < 32; i++) {
        if (!(vs_attr_mask & (1u << i))) continue;
        out_key->attr_mask |= 1u << i;
        uint32_t fmt = rec->attr_fmt[i];
        uint32_t stream = rec->attr_bind[i];
        uint32_t off = rec->attr_off[i];
        if (fmt == 0 || stream >= 16) {
            out_key->attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            out_key->attr_bind[i] = ZERO_BINDING;
            out_key->used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        VkFormat vf = vk_nvn_vertex_format(fmt);
        out_key->attr_fmt[i] = vfmt_support ? vfmt_support(vf) : vf;
        out_key->attr_off[i] = off;
        out_key->attr_bind[i] = stream;
        out_key->used_streams |= 1u << stream;
    }

    for (int s = 0; s < 16; s++) {
        if (!(out_key->used_streams & (1u << s))) continue;
        out_key->stride[s] = rec->stream_stride[s];
        out_key->divisor[s] = out_key->stride[s] ? rec->stream_divisor[s] : 0;
        if (out_key->divisor[s] > 1 && !has_divisor) out_key->divisor[s] = 1;
    }

    for (uint32_t i = 0; i < ncolor && i < 8; i++) {
        if (out_key->color[i] == VK_FORMAT_UNDEFINED) continue;
        out_key->write_mask[i] = ((fs_out_mask >> (i * 4)) & 0xF) ? ((rec->chmask >> (i * 4)) & 0xF) : 0;
        if (((rec->blend_en >> i) & 1) && !mtl_rt_kind(rec->mtl_cfmt[i])) {
            out_key->blend_on |= (uint8_t)(1u << i);
            memcpy(out_key->blend[i], rec->blend[i], 8);
        }
    }

    out_key->a2c = rec->a2c ? 1 : 0;
    out_key->topo_class = rec->topo_class;
    out_key->poly_fill = rec->poly_fill;
    return true;
}

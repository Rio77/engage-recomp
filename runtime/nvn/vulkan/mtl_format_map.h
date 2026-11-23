#pragma once
/* Explicit mapping from NX Native / Metal MTLPixelFormat integer enums to
 * Engage Recomp's Vulkan attachment formats and shader rt_kind categories.
 * Returns VK_FORMAT_UNDEFINED for unknown or unsupported values.
 */
#include <stdint.h>
#include <vulkan/vulkan.h>

static inline VkFormat mtl_to_vk_color_format(uint32_t mtl_pf) {
    switch (mtl_pf) {
    case 10:  return VK_FORMAT_R8_UNORM;
    case 12:  return VK_FORMAT_R8_SNORM;
    case 13:  return VK_FORMAT_R8_UINT;
    case 14:  return VK_FORMAT_R8_SINT;
    case 20:  return VK_FORMAT_R16_UNORM;
    case 22:  return VK_FORMAT_R16_SNORM;
    case 23:  return VK_FORMAT_R16_UINT;
    case 24:  return VK_FORMAT_R16_SINT;
    case 25:  return VK_FORMAT_R16_SFLOAT;
    case 30:  return VK_FORMAT_R8G8_UNORM;
    case 32:  return VK_FORMAT_R8G8_SNORM;
    case 33:  return VK_FORMAT_R8G8_UINT;
    case 34:  return VK_FORMAT_R8G8_SINT;
    case 40:  return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 41:  return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 42:  return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 43:  return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 53:  return VK_FORMAT_R32_UINT;
    case 54:  return VK_FORMAT_R32_SINT;
    case 55:  return VK_FORMAT_R32_SFLOAT;
    case 60:  return VK_FORMAT_R16G16_UNORM;
    case 62:  return VK_FORMAT_R16G16_SNORM;
    case 63:  return VK_FORMAT_R16G16_UINT;
    case 64:  return VK_FORMAT_R16G16_SINT;
    case 65:  return VK_FORMAT_R16G16_SFLOAT;
    case 70:  return VK_FORMAT_R8G8B8A8_UNORM;
    case 71:  return VK_FORMAT_R8G8B8A8_SRGB;
    case 72:  return VK_FORMAT_R8G8B8A8_SNORM;
    case 73:  return VK_FORMAT_R8G8B8A8_UINT;
    case 74:  return VK_FORMAT_R8G8B8A8_SINT;
    case 80:  return VK_FORMAT_B8G8R8A8_UNORM;
    case 81:  return VK_FORMAT_B8G8R8A8_SRGB;
    case 90:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 91:  return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 92:  return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 93:  return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 103: return VK_FORMAT_R32G32_UINT;
    case 104: return VK_FORMAT_R32G32_SINT;
    case 105: return VK_FORMAT_R32G32_SFLOAT;
    case 110: return VK_FORMAT_R16G16B16A16_UNORM;
    case 112: return VK_FORMAT_R16G16B16A16_SNORM;
    case 113: return VK_FORMAT_R16G16B16A16_UINT;
    case 114: return VK_FORMAT_R16G16B16A16_SINT;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 123: return VK_FORMAT_R32G32B32A32_UINT;
    case 124: return VK_FORMAT_R32G32B32A32_SINT;
    case 125: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:  return VK_FORMAT_UNDEFINED;
    }
}

static inline int mtl_rt_kind(uint32_t mtl_pf) {
    switch (mtl_pf) {
    case 13: case 23: case 33: case 53: case 63:
    case 73: case 91: case 103: case 113: case 123:
        return 2; /* uint */
    case 14: case 24: case 34: case 54: case 64:
    case 74: case 104: case 114: case 124:
        return 1; /* sint */
    default:
        return 0; /* float / norm */
    }
}

static inline VkFormat mtl_to_vk_depth_format(uint32_t mtl_pf, VkFormat d24s8_vk) {
    switch (mtl_pf) {
    case 250: return VK_FORMAT_D16_UNORM;
    case 252: return VK_FORMAT_D32_SFLOAT;
    case 260: return d24s8_vk != VK_FORMAT_UNDEFINED ? d24s8_vk : VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:  return VK_FORMAT_UNDEFINED;
    }
}

static inline VkFormat mtl_to_vk_stencil_format(uint32_t mtl_pf, VkFormat d24s8_vk) {
    switch (mtl_pf) {
    case 253: return VK_FORMAT_S8_UINT;
    case 260: return d24s8_vk != VK_FORMAT_UNDEFINED ? d24s8_vk : VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:  return VK_FORMAT_UNDEFINED;
    }
}

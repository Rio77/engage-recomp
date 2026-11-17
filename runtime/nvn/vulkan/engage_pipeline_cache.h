#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <vulkan/vulkan.h>

static inline bool engage_pipeline_cache_compatible(const void *data, size_t size,
                                                    const VkPhysicalDeviceProperties *device) {
    VkPipelineCacheHeaderVersionOne header;
    if (!data || size < sizeof header) return false;
    memcpy(&header, data, sizeof header);
    return header.headerSize >= sizeof header && header.headerSize <= size &&
        header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        header.vendorID == device->vendorID && header.deviceID == device->deviceID &&
        !memcmp(header.pipelineCacheUUID, device->pipelineCacheUUID, VK_UUID_SIZE);
}

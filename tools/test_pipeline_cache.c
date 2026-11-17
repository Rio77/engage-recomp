#include <assert.h>
#include "pipeline_cache_header.h"

int main(void) {
    VkPhysicalDeviceProperties device = {.vendorID=0x1002, .deviceID=0x163f};
    memset(device.pipelineCacheUUID, 42, VK_UUID_SIZE);
    VkPipelineCacheHeaderVersionOne good = {.headerSize=sizeof good,
        .headerVersion=VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
        .vendorID=device.vendorID, .deviceID=device.deviceID};
    memcpy(good.pipelineCacheUUID, device.pipelineCacheUUID, VK_UUID_SIZE);
    assert(engage_pipeline_cache_compatible(&good, sizeof good, &device));
    assert(!engage_pipeline_cache_compatible(NULL, sizeof good, &device));
    assert(!engage_pipeline_cache_compatible(&good, sizeof good - 1, &device));
    VkPipelineCacheHeaderVersionOne bad = good;
    bad.headerSize++;
    assert(!engage_pipeline_cache_compatible(&bad, sizeof bad, &device));
    bad = good; bad.headerVersion = 0;
    assert(!engage_pipeline_cache_compatible(&bad, sizeof bad, &device));
    bad = good; bad.vendorID++;
    assert(!engage_pipeline_cache_compatible(&bad, sizeof bad, &device));
    bad = good; bad.deviceID++;
    assert(!engage_pipeline_cache_compatible(&bad, sizeof bad, &device));
    bad = good; bad.pipelineCacheUUID[0]++;
    assert(!engage_pipeline_cache_compatible(&bad, sizeof bad, &device));
    return 0;
}

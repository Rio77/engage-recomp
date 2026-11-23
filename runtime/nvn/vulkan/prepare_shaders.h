#pragma once
#include <stdbool.h>

typedef struct {
    const char *seed_path;
    const char *progress_path;  /* optional: atomic status file for installer UI */
    int jobs;                   /* 0 = auto: clamp(ncpu / 2, 1, 4) */
    bool no_pipelines;          /* Phase 2 prepares xlat + spirv only; Phase 3 adds pipelines */
} PrepareOpts;

/* Offline local shader preparation using the runtime's Vulkan capabilities,
 * Maxwell->GLSL translator (cache/xlat/), and shaderc SPIR-V compiler (cache/spirv/).
 * Returns 0 on completion, 130 if cancelled via SIGINT/SIGTERM, 1 on error.
 */
int vk_prepare_shaders(const PrepareOpts *opts);

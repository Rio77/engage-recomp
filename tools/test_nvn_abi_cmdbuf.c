// Thin command-buffer wrapper translation unit for tools/test_nvn_abi.c (Phase 2).
// Kept in its own TU so HLE_FUNC(__LINE__) labels in cmdbuf.c do not collide with objects.c.
#include "../runtime/nvn/cmdbuf.c"

void abi_cb_init(Cpu *c) { hle_nvnCommandBufferInitialize(c); }
void abi_cb_finalize(Cpu *c) { hle_nvnCommandBufferFinalize(c); }
void abi_cb_add_cmd_mem(Cpu *c) { hle_nvnCommandBufferAddCommandMemory(c); }
void abi_cb_add_ctrl_mem(Cpu *c) { hle_nvnCommandBufferAddControlMemory(c); }
void abi_cb_begin(Cpu *c) { hle_nvnCommandBufferBeginRecording(c); }
void abi_cb_end(Cpu *c) { hle_nvnCommandBufferEndRecording(c); }
void abi_cb_set_row_stride(Cpu *c) { hle_nvnCommandBufferSetCopyRowStride(c); }
void abi_cb_get_row_stride(Cpu *c) { hle_nvnCommandBufferGetCopyRowStride(c); }
void abi_cb_set_img_stride(Cpu *c) { hle_nvnCommandBufferSetCopyImageStride(c); }
void abi_cb_get_img_stride(Cpu *c) { hle_nvnCommandBufferGetCopyImageStride(c); }

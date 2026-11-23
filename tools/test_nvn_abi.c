// NVN ABI canary and layout verification suite (Phase 2).
// Verifies guest struct bounds with 64-byte 0xA5 guard bands before and after
// each NVN guest object and checks known ABI contracts against Engage 2.0.0 (NVN 55.0.0).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/objects.c"
#include "nvn_test_harness.h"

void abi_cb_init(Cpu *c);
void abi_cb_finalize(Cpu *c);
void abi_cb_add_cmd_mem(Cpu *c);
void abi_cb_add_ctrl_mem(Cpu *c);
void abi_cb_begin(Cpu *c);
void abi_cb_end(Cpu *c);
void abi_cb_set_row_stride(Cpu *c);
void abi_cb_get_row_stride(Cpu *c);
void abi_cb_set_img_stride(Cpu *c);
void abi_cb_get_img_stride(Cpu *c);

// Minimal stubs for non-NVN backend hooks
static unsigned g_destroyed;
static unsigned g_pool_written;
void backend_texture_destroyed(NvnTexture *t) { (void)t; g_destroyed++; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; g_pool_written++; }
void backend_present(NvnTexture *t) { (void)t; }
void backend_init(void) {}
void backend_execute(uint64_t handle) { (void)handle; }
void renderer_sync(void) {}
void renderer_flush(void) {}
void renderer_submit(void) {}
uint64_t renderer_fence_seq(void) { return 1; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
int renderer_wait_seq_timeout(uint64_t seq, uint64_t ns) { (void)seq; (void)ns; return 0; }
void engage_timing_apply(Cpu *c) { (void)c; }
void engage_present_pace(void) {}
uint64_t plat_now_ns(void) { return 1000; }
void guest_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; }
void rt_fatal_trace(void) { _exit(99); }

#define GUARD 64
#define CANARY 0xA5

typedef struct {
    uint8_t pre[GUARD];
    uint8_t body[512];
    uint8_t post[GUARD];
} GuardedBox;

static void box_init(GuardedBox *b, size_t abi_size) {
    assert(abi_size <= sizeof b->body);
    memset(b->pre, CANARY, GUARD);
    memset(b->body, 0, abi_size);
    memset(b->body + abi_size, CANARY, sizeof b->body - abi_size);
    memset(b->post, CANARY, GUARD);
}

static int box_canaries_intact(const GuardedBox *b, size_t abi_size) {
    return nvn_all_bytes_eq(b->pre, GUARD, CANARY) &&
           nvn_all_bytes_eq(b->body + abi_size, sizeof b->body - abi_size, CANARY) &&
           nvn_all_bytes_eq(b->post, GUARD, CANARY);
}

static uint64_t call1(void (*fn)(Cpu *), uint64_t a0) {
    Cpu c = {0};
    c.x[0] = a0;
    fn(&c);
    return c.x[0];
}
static uint64_t call2(void (*fn)(Cpu *), uint64_t a0, uint64_t a1) {
    Cpu c = {0};
    c.x[0] = a0; c.x[1] = a1;
    fn(&c);
    return c.x[0];
}
static uint64_t call3(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu c = {0};
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2;
    fn(&c);
    return c.x[0];
}
static uint64_t call4(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu c = {0};
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3;
    fn(&c);
    return c.x[0];
}
static uint64_t call5(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    Cpu c = {0};
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[4] = a4;
    fn(&c);
    return c.x[0];
}

// 1. NVNtextureView (ABI = 0x10 / 16 bytes in NVN 55.0.0 — proven in Engage main 0xa6fce0 & 0xa6f888)
static void test_abi_texture_view(void) {
    GuardedBox view;
    box_init(&view, 0x10);
    uint64_t v = (uintptr_t)view.body;
    call1(hle_nvnTextureViewSetDefaults, v);
    NVN_XFAIL_UNTIL(4, box_canaries_intact(&view, 0x10),
                    "NVNtextureView SetDefaults stays within 16-byte NVN 55 ABI");

    box_init(&view, 0x10);
    call1(hle_nvnTextureViewSetDefaults, v);
    call3(hle_nvnTextureViewSetLevels, v, 1, 3);
    call3(hle_nvnTextureViewSetLayers, v, 0, 2);
    call2(hle_nvnTextureViewSetFormat, v, 0x25);
    call5(hle_nvnTextureViewSetSwizzle, v, 2, 3, 4, 5);
    call2(hle_nvnTextureViewSetDepthStencilMode, v, 1);
    call2(hle_nvnTextureViewSetTarget, v, 1);
    NVN_XFAIL_UNTIL(4, box_canaries_intact(&view, 0x10),
                    "NVNtextureView all setters stay within 16-byte NVN 55 ABI");
    NVN_XFAIL_UNTIL(4, sizeof(NvnTextureView) <= 16,
                    "sizeof(NvnTextureView) <= 16 bytes (NVN 55 ABI)");
}

// 2. NVNmemoryPoolBuilder (ABI = 0x40 / 64 bytes) & preserves device@0 across SetDefaults
static void test_abi_memory_pool_and_builder(void) {
    GuardedBox bld, pool;
    static uint8_t storage[65536];
    box_init(&bld, 0x40);
    box_init(&pool, 0x100);
    uint64_t dev = 0xDEADBEEF12345678ull;

    call2(hle_nvnMemoryPoolBuilderSetDevice, (uintptr_t)bld.body, dev);
    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)bld.body);
    NVN_CHECK(box_canaries_intact(&bld, 0x40), "NVNmemoryPoolBuilder canaries intact");
    uint64_t got_dev = call1(hle_nvnMemoryPoolBuilderGetDevice, (uintptr_t)bld.body);
    NVN_XFAIL_UNTIL(4, got_dev == dev, "NVNmemoryPoolBuilderSetDefaults preserves device@0");

    call2(hle_nvnMemoryPoolBuilderSetFlags, (uintptr_t)bld.body, 0x09);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)bld.body, (uintptr_t)storage, sizeof storage);
    NVN_CHECK(call2(hle_nvnMemoryPoolInitialize, (uintptr_t)pool.body, (uintptr_t)bld.body) == 1,
              "NVNmemoryPoolInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&pool, 0x100), "NVNmemoryPool Init canaries intact");
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool.body);
    NVN_CHECK(box_canaries_intact(&pool, 0x100), "NVNmemoryPool Finalize canaries intact");
}

// 3. NVNbufferBuilder (0x40) & NVNbuffer (0x30)
static void test_abi_buffer_and_builder(void) {
    GuardedBox mp_bld, mp, b_bld, buf;
    static uint8_t storage[65536];
    box_init(&mp_bld, 0x40); box_init(&mp, 0x100);
    box_init(&b_bld, 0x40);  box_init(&buf, 0x30);

    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)mp_bld.body);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)mp_bld.body, (uintptr_t)storage, sizeof storage);
    call2(hle_nvnMemoryPoolInitialize, (uintptr_t)mp.body, (uintptr_t)mp_bld.body);

    uint64_t dev = 0x1122334455667788ull;
    call2(hle_nvnBufferBuilderSetDevice, (uintptr_t)b_bld.body, dev);
    call1(hle_nvnBufferBuilderSetDefaults, (uintptr_t)b_bld.body);
    NVN_CHECK(call1(hle_nvnBufferBuilderGetDevice, (uintptr_t)b_bld.body) == dev,
              "NVNbufferBuilderSetDefaults preserves device@0");
    call4(hle_nvnBufferBuilderSetStorage, (uintptr_t)b_bld.body, (uintptr_t)mp.body, 0x100, 0x1000);
    NVN_CHECK(box_canaries_intact(&b_bld, 0x40), "NVNbufferBuilder canaries intact");

    NVN_CHECK(call2(hle_nvnBufferInitialize, (uintptr_t)buf.body, (uintptr_t)b_bld.body) == 1,
              "NVNbufferInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&buf, 0x30), "NVNbuffer Init canaries intact");
    call1(hle_nvnBufferFinalize, (uintptr_t)buf.body);
    NVN_CHECK(box_canaries_intact(&buf, 0x30), "NVNbuffer Finalize canaries intact");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)mp.body);
}

// 4. NVNtextureBuilder (0x80) & NVNtexture (0x80)
static void test_abi_texture_and_builder(void) {
    GuardedBox mp_bld, mp, tb, tex;
    static uint8_t storage[1024 * 1024];
    box_init(&mp_bld, 0x40); box_init(&mp, 0x100);
    box_init(&tb, 0x80);     box_init(&tex, 0x80);

    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)mp_bld.body);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)mp_bld.body, (uintptr_t)storage, sizeof storage);
    call2(hle_nvnMemoryPoolInitialize, (uintptr_t)mp.body, (uintptr_t)mp_bld.body);

    call2(hle_nvnTextureBuilderSetDevice, (uintptr_t)tb.body, 0x1234);
    call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb.body);
    call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb.body, 1);  // 2D
    call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb.body, 64, 64);
    call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb.body, 4);
    call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb.body, 0x25);  // RGBA8
    call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb.body, (uintptr_t)mp.body, 0);
    NVN_CHECK(box_canaries_intact(&tb, 0x80), "NVNtextureBuilder canaries intact");

    NVN_CHECK(call2(hle_nvnTextureInitialize, (uintptr_t)tex.body, (uintptr_t)tb.body) == 1,
              "NVNtextureInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&tex, 0x80), "NVNtexture Init canaries intact");
    call1(hle_nvnTextureFinalize, (uintptr_t)tex.body);
    NVN_CHECK(box_canaries_intact(&tex, 0x80), "NVNtexture Finalize canaries intact");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)mp.body);
    texture_identities_reset();
}

// 5. NVNsamplerBuilder (0x60) & NVNsampler (0x60)
static void test_abi_sampler_and_builder(void) {
    GuardedBox sb, smp;
    box_init(&sb, 0x60); box_init(&smp, 0x60);

    call2(hle_nvnSamplerBuilderSetDevice, (uintptr_t)sb.body, 0xABCD);
    call1(hle_nvnSamplerBuilderSetDefaults, (uintptr_t)sb.body);
    call3(hle_nvnSamplerBuilderSetMinMagFilter, (uintptr_t)sb.body, 1, 1);
    call4(hle_nvnSamplerBuilderSetWrapMode, (uintptr_t)sb.body, 2, 2, 2);
    NVN_CHECK(box_canaries_intact(&sb, 0x60), "NVNsamplerBuilder canaries intact");

    NVN_CHECK(call2(hle_nvnSamplerInitialize, (uintptr_t)smp.body, (uintptr_t)sb.body) == 1,
              "NVNsamplerInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&smp, 0x60), "NVNsampler Init canaries intact");
    // Canonical sampler state bytes from +8 onward (nxnative nvn-objects.md:38-46)
    int canonical_nonzero = 0;
    for (size_t i = 8; i < 0x60; i++) if (smp.body[i] != 0) canonical_nonzero = 1;
    NVN_XFAIL_UNTIL(5, canonical_nonzero, "NVNsamplerInitialize writes canonical state bytes from +8");

    call1(hle_nvnSamplerFinalize, (uintptr_t)smp.body);
    NVN_CHECK(box_canaries_intact(&smp, 0x60), "NVNsampler Finalize canaries intact");
}

// 6. NVNtexturePool (0x20), NVNsamplerPool (0x20), NVNsync (0x40)
static void test_abi_pools_and_sync(void) {
    GuardedBox mp_bld, mp, tp, sp, sync;
    static uint8_t storage[65536];
    box_init(&mp_bld, 0x40); box_init(&mp, 0x100);
    box_init(&tp, 0x20);     box_init(&sp, 0x20);
    box_init(&sync, 0x40);

    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)mp_bld.body);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)mp_bld.body, (uintptr_t)storage, sizeof storage);
    call2(hle_nvnMemoryPoolInitialize, (uintptr_t)mp.body, (uintptr_t)mp_bld.body);

    NVN_CHECK(call4(hle_nvnTexturePoolInitialize, (uintptr_t)tp.body, (uintptr_t)mp.body, 0, 64) == 1,
              "NVNtexturePoolInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&tp, 0x20), "NVNtexturePool canaries intact");
    call1(hle_nvnTexturePoolFinalize, (uintptr_t)tp.body);
    NVN_CHECK(box_canaries_intact(&tp, 0x20), "NVNtexturePool Finalize canaries intact");

    NVN_CHECK(call4(hle_nvnSamplerPoolInitialize, (uintptr_t)sp.body, (uintptr_t)mp.body, 0x1000, 64) == 1,
              "NVNsamplerPoolInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&sp, 0x20), "NVNsamplerPool canaries intact");
    call1(hle_nvnSamplerPoolFinalize, (uintptr_t)sp.body);
    NVN_CHECK(box_canaries_intact(&sp, 0x20), "NVNsamplerPool Finalize canaries intact");

    NVN_CHECK(call2(hle_nvnSyncInitialize, (uintptr_t)sync.body, 0x1234) == 1,
              "NVNsyncInitialize returns 1");
    NVN_CHECK(box_canaries_intact(&sync, 0x40), "NVNsync Init canaries intact");
    call1(hle_nvnSyncFinalize, (uintptr_t)sync.body);
    NVN_CHECK(box_canaries_intact(&sync, 0x40), "NVNsync Finalize canaries intact");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)mp.body);
    desc_pools_reset();
}

// 7. NVNcommandBuffer (0xA0) & per-cmdbuf copy_row_stride / copy_image_stride isolation
static void test_abi_cmdbuf_and_strides(void) {
    GuardedBox mp_bld, mp, cb1, cb2;
    static uint8_t cmd_mem[65536];
    static uint8_t ctrl1[4096], ctrl2[4096];
    box_init(&mp_bld, 0x40); box_init(&mp, 0x100);
    box_init(&cb1, 0xA0);    box_init(&cb2, 0xA0);

    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)mp_bld.body);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)mp_bld.body, (uintptr_t)cmd_mem, sizeof cmd_mem);
    call2(hle_nvnMemoryPoolInitialize, (uintptr_t)mp.body, (uintptr_t)mp_bld.body);

    NVN_CHECK(call2(abi_cb_init, (uintptr_t)cb1.body, 0x1234) == 1,
              "NVNcommandBufferInitialize cb1");
    NVN_CHECK(call2(abi_cb_init, (uintptr_t)cb2.body, 0x1234) == 1,
              "NVNcommandBufferInitialize cb2");
    NVN_CHECK(box_canaries_intact(&cb1, 0xA0), "NVNcommandBuffer cb1 Init canaries intact");
    NVN_CHECK(box_canaries_intact(&cb2, 0xA0), "NVNcommandBuffer cb2 Init canaries intact");

    call4(abi_cb_add_cmd_mem, (uintptr_t)cb1.body, (uintptr_t)mp.body, 0, 16384);
    call3(abi_cb_add_ctrl_mem, (uintptr_t)cb1.body, (uintptr_t)ctrl1, sizeof ctrl1);
    call4(abi_cb_add_cmd_mem, (uintptr_t)cb2.body, (uintptr_t)mp.body, 16384, 16384);
    call3(abi_cb_add_ctrl_mem, (uintptr_t)cb2.body, (uintptr_t)ctrl2, sizeof ctrl2);

    call1(abi_cb_begin, (uintptr_t)cb1.body);
    call1(abi_cb_begin, (uintptr_t)cb2.body);

    call2(abi_cb_set_row_stride, (uintptr_t)cb1.body, 4096);
    call2(abi_cb_set_img_stride, (uintptr_t)cb1.body, 65536);
    call2(abi_cb_set_row_stride, (uintptr_t)cb2.body, 1024);
    call2(abi_cb_set_img_stride, (uintptr_t)cb2.body, 8192);

    uint64_t r1 = call1(abi_cb_get_row_stride, (uintptr_t)cb1.body);
    uint64_t i1 = call1(abi_cb_get_img_stride, (uintptr_t)cb1.body);
    NVN_XFAIL_UNTIL(4, r1 == 4096 && i1 == 65536,
                    "copy_row_stride/copy_image_stride isolated per NvnCmdBuf (not global)");

    uint64_t h1 = call1(abi_cb_end, (uintptr_t)cb1.body);
    uint64_t h2 = call1(abi_cb_end, (uintptr_t)cb2.body);
    nvn_command_finalize(h1);
    nvn_command_finalize(h2);

    call1(abi_cb_finalize, (uintptr_t)cb1.body);
    call1(abi_cb_finalize, (uintptr_t)cb2.body);
    NVN_CHECK(box_canaries_intact(&cb1, 0xA0), "NVNcommandBuffer cb1 Finalize canaries intact");
    NVN_CHECK(box_canaries_intact(&cb2, 0xA0), "NVNcommandBuffer cb2 Finalize canaries intact");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)mp.body);
}

int main(void) {
    printf("=== test_nvn_abi (NVN_TEST_PHASE=%d) ===\n", NVN_TEST_PHASE);
    test_abi_texture_view();
    test_abi_memory_pool_and_builder();
    test_abi_buffer_and_builder();
    test_abi_texture_and_builder();
    test_abi_sampler_and_builder();
    test_abi_pools_and_sync();
    test_abi_cmdbuf_and_strides();
    return nvn_summary("test_nvn_abi");
}

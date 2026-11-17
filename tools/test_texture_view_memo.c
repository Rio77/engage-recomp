#include <assert.h>
#include "../runtime/nvn/vulkan/exec.c"

Settings g_settings;
uint32_t vk_view_gen = 1;
static unsigned refreshes, uploads, views;
uint64_t vk_frame_counter(void) { return 9; }
void rt_fatal_trace(void) { abort(); }
TexBackend *vk_tex(NvnTexture *texture, bool render) {
    assert(!render);
    refreshes++;
    TexBackend *backend = texture->backend;
    if (backend->pending_rgba) { uploads++; backend->pending_rgba = NULL; backend->data_hash++; }
    return backend;
}
VkImageView vk_view(NvnTexture *texture, const NvnTextureView *v, bool has, bool render, int type) {
    (void)v; (void)has; (void)type;
    views++;
    TexBackend *backend = vk_tex(texture, render);
    return (VkImageView)(uintptr_t)(0x1000 + (uintptr_t)backend->image + texture->swizzle[0]);
}
VkImageView vk_dummy_view(int type) { (void)type; return (VkImageView)(uintptr_t)1; }
VkSampler vk_sampler(NvnSamplerDesc *desc) { (void)desc; return (VkSampler)(uintptr_t)1; }
VkSampler vk_default_sampler(bool linear) { (void)linear; return (VkSampler)(uintptr_t)1; }
void renderer_texture_scale(NvnTexture *texture, float *x, float *y) { (void)texture; *x = *y = 1; }

int main(void) {
    unsetenv("LARECOMP_VK_NO_TEX_MEMO");
    TexBackend backing = {.image = (VkImage)(uintptr_t)16, .pending_rgba = (void *)(uintptr_t)1};
    NvnTexture texture = {.width = 64, .height = 64, .backend = &backing};
    NvnTexEntry entries[2] = {0};
    entries[1].tex = &texture;
    NvnDescPool pool = {.count = 2, .entries = entries};
    st.tpool = &pool;
    st.tex[1][0] = 1;
    ShaderInfo info = {.ntex = 1};
    info.tex[0].handle_offset = 0x20;
    info.tex[0].type = TEXTYPE_2D;
    VkDescriptorImageInfo out;
    float scale[2]; uint32_t extent[2];
    texture_info(1, &info, 0, &out, scale, extent);
    assert(uploads == 1 && views == 1);
    VkImageView initial = out.imageView;
    backing.pending_rgba = (void *)(uintptr_t)1;
    texture_info(1, &info, 0, &out, scale, extent);
    assert(uploads == 2);  // same frame/view must still make producer writes visible
    assert(views == 1 && out.imageView == initial);  // retain the view fast path
    backing.image = (VkImage)(uintptr_t)32;
    texture_info(1, &info, 0, &out, scale, extent);
    assert(views == 2 && out.imageView != initial);
    texture.swizzle[0] = 2;
    VkImageView previous = out.imageView;
    texture_info(1, &info, 0, &out, scale, extent);
    assert(views == 3 && out.imageView != previous);
    assert(refreshes >= 4);
    puts("PASS: pending uploads remain visible through view memo hits; image/swizzle changes invalidate views");
}

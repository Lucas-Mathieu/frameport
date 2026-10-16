// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c after flip_vk.c. XR_FB_composition_layer_alpha_blend for emulated passthrough
// (setting alpha_blend_emul, Vulkan sessions).
//
// Passthrough is emulated by compositing the game's projection layer over the Frame's cameras with its texture alpha
// (ALPHA_BLEND environment). Unreal's mobile renderer keeps 1 - coverage in the scene alpha (0 where it drew, 1 where
// it drew nothing), and Meta's OVRPlugin compensates with an XrCompositionLayerAlphaBlendFB on the eye layer
// (source colour factor ONE_MINUS_SRC_ALPHA, destination SRC_ALPHA). The Frame's runtime ignores that struct, so the
// cameras showed through the scene and the empty background stayed black (GitHub #329, Star Wars: Beyond Victory).
// Blending src * (1 - a) + dst * a is the same as the runtime's standard blend with alpha 1 - a, so the alpha of the
// layer's image rectangles is inverted in place, once per acquired image, by a tiny render pass (blend: one minus
// destination alpha, zero; only alpha written) before the frame goes to the runtime. Our work is queued on the app's
// queue ahead of the runtime's xrEndFrame submission; command buffers come from a ring and are only waited for when
// reused, several frames later.
#include "alpha_blend_spv.h"

#define AB_RING 4
#define AB_TARGETS 32

static struct {
    VkDevice device;  // the session's device these objects belong to
    int state;        // 0 = not set up, 1 = ready, -1 = failed
    VkFormat format;  // the pipeline's colour format
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdSetViewport CmdSetViewport;
    PFN_vkCmdSetScissor CmdSetScissor;
    PFN_vkCmdDraw CmdDraw;
    VkShaderModule vs, fs;
    VkPipelineLayout layout;
    VkRenderPass pass;
    VkPipeline pipeline;
    VkCommandBuffer cmd[AB_RING];
    VkFence fence[AB_RING];
    int pending[AB_RING], next;
    struct { XrSwapchain swapchain; VkImage image; uint32_t layer, w, h; VkImageView view; VkFramebuffer fb; } targets[AB_TARGETS];
} ab;

static int ab_functions(void) {
#define AB_DEV(name) ab.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!ab.name) return 0;
    AB_DEV(CreateShaderModule) AB_DEV(DestroyShaderModule) AB_DEV(CreatePipelineLayout) AB_DEV(CreateRenderPass)
    AB_DEV(DestroyRenderPass) AB_DEV(CreateGraphicsPipelines) AB_DEV(DestroyPipeline) AB_DEV(CreateImageView)
    AB_DEV(DestroyImageView) AB_DEV(CreateFramebuffer) AB_DEV(DestroyFramebuffer) AB_DEV(CmdBeginRenderPass)
    AB_DEV(CmdEndRenderPass) AB_DEV(CmdBindPipeline) AB_DEV(CmdSetViewport) AB_DEV(CmdSetScissor) AB_DEV(CmdDraw)
#undef AB_DEV
    return 1;
}

// Command buffers, fences and shaders: once per session device.
static int ab_ready(void) {
    if (ab.device != vk.device) {  // a new session (the old device's objects went with it)
        memset(&ab, 0, sizeof(ab));
        ab.device = vk.device;
    }
    if (ab.state) return ab.state > 0;
    ab.state = -1;
    if (!vk_ready() || !ab_functions()) return 0;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, AB_RING};
    if (vk.AllocateCommandBuffers(vk.device, &alloc, ab.cmd) != VK_SUCCESS) return 0;
    for (int i = 0; i < AB_RING; ++i) {
        VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        if (vk.CreateFence(vk.device, &fence, NULL, &ab.fence[i]) != VK_SUCCESS) return 0;
    }
    VkShaderModuleCreateInfo vs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(alpha_blend_vert_spv),
                                   alpha_blend_vert_spv};
    VkShaderModuleCreateInfo fs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(alpha_blend_frag_spv),
                                   alpha_blend_frag_spv};
    if (ab.CreateShaderModule(vk.device, &vs, NULL, &ab.vs) != VK_SUCCESS ||
        ab.CreateShaderModule(vk.device, &fs, NULL, &ab.fs) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    if (ab.CreatePipelineLayout(vk.device, &layout, NULL, &ab.layout) != VK_SUCCESS) return 0;
    ab.state = 1;
    return 1;
}

// Render pass + pipeline for one colour format (the eye swapchains': rebuilt only if it changes).
static int ab_pipeline(VkFormat format) {
    if (ab.pipeline && ab.format == format) return 1;
    if (ab.pipeline) { ab.DestroyPipeline(vk.device, ab.pipeline, NULL); ab.pipeline = VK_NULL_HANDLE; }
    if (ab.pass) { ab.DestroyRenderPass(vk.device, ab.pass, NULL); ab.pass = VK_NULL_HANDLE; }
    VkAttachmentDescription color = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                                     VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkSubpassDependency deps[2] = {
        // the app's rendering into the image (any stage) before our read-modify-write
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        // and ours before whatever the runtime does with it next
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
    };
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &color, 1, &subpass, 2, deps};
    if (ab.CreateRenderPass(vk.device, &rp, NULL, &ab.pass) != VK_SUCCESS) { ab.pass = VK_NULL_HANDLE; return 0; }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, ab.vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ab.fs, "main", NULL},
    };
    VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL,
                                                       0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL,
                                                  1, NULL};
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {VK_TRUE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                                                 VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                                                 VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2,
                                                dynamic_states};
    VkGraphicsPipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blending;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = ab.layout;
    pipeline.renderPass = ab.pass;
    if (ab.CreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &ab.pipeline) != VK_SUCCESS) {
        ab.pipeline = VK_NULL_HANDLE;
        return 0;
    }
    ab.format = format;
    // framebuffers were made for the old render pass (compatible only with the same format): drop them
    for (int i = 0; i < AB_TARGETS; ++i)
        if (ab.targets[i].fb) {
            ab.DestroyFramebuffer(vk.device, ab.targets[i].fb, NULL);
            ab.DestroyImageView(vk.device, ab.targets[i].view, NULL);
            memset(&ab.targets[i], 0, sizeof(ab.targets[i]));
        }
    LOG("alpha_blend: Vulkan alpha inversion ready (format %d)", (int)format);
    return 1;
}

static void ab_wait_all(void) {
    for (int i = 0; i < AB_RING; ++i)
        if (ab.pending[i]) {
            vk.WaitForFences(vk.device, 1, &ab.fence[i], VK_TRUE, 1000000000ull);
            vk.ResetFences(vk.device, 1, &ab.fence[i]);
            ab.pending[i] = 0;
        }
}

// The view + framebuffer of one array layer of a swapchain image.
static VkFramebuffer ab_target(XrSwapchain swapchain, VkImage image, uint32_t layer, VkFormat format, uint32_t w, uint32_t h) {
    int free_slot = -1;
    for (int i = 0; i < AB_TARGETS; ++i) {
        if (ab.targets[i].fb && ab.targets[i].image == image && ab.targets[i].layer == layer &&
            ab.targets[i].swapchain == swapchain) return ab.targets[i].fb;
        if (!ab.targets[i].fb && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, format,
                                {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1}};
    VkImageView view;
    if (ab.CreateImageView(vk.device, &vi, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, ab.pass, 1, &view, w, h, 1};
    VkFramebuffer fb;
    if (ab.CreateFramebuffer(vk.device, &fi, NULL, &fb) != VK_SUCCESS) {
        ab.DestroyImageView(vk.device, view, NULL);
        return VK_NULL_HANDLE;
    }
    ab.targets[free_slot].swapchain = swapchain;
    ab.targets[free_slot].image = image;
    ab.targets[free_slot].layer = layer;
    ab.targets[free_slot].w = w;
    ab.targets[free_slot].h = h;
    ab.targets[free_slot].view = view;
    ab.targets[free_slot].fb = fb;
    return fb;
}

// xrDestroySwapchain (before the runtime's): our views and framebuffers of its images go first.
static void ab_on_destroy(XrSwapchain swapchain) {
    if (ab.state <= 0 || ab.device != vk.device) return;
    int any = 0;
    for (int i = 0; i < AB_TARGETS; ++i) any |= ab.targets[i].fb && ab.targets[i].swapchain == swapchain;
    if (!any) return;
    ab_wait_all();
    for (int i = 0; i < AB_TARGETS; ++i)
        if (ab.targets[i].fb && ab.targets[i].swapchain == swapchain) {
            ab.DestroyFramebuffer(vk.device, ab.targets[i].fb, NULL);
            ab.DestroyImageView(vk.device, ab.targets[i].view, NULL);
            memset(&ab.targets[i], 0, sizeof(ab.targets[i]));
        }
}

// Inverts the alpha of one projection view's rectangle in the image the app last acquired from its swapchain
// (Vulkan; once per acquire). Returns 1 when done or already done for this image, 0 when it couldn't be done.
static int ab_invert_view(const XrSwapchainSubImage *sub) {
    if (!vk.device) return 0;  // not a Vulkan session
    int ok = 0;
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(sub->swapchain, 0);
    uint32_t index = t ? t->last_acquired : 0, layer = sub->imageArrayIndex;
    XrRect2Di rect = sub->imageRect;
    if (!t || !t->image_count || index >= t->image_count || layer >= 32 || rect.extent.width <= 0 ||
        rect.extent.height <= 0 || t->info.sampleCount > 1 ||
        !(t->info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        goto done;
    if (t->alpha_inverted[index] & (1u << layer)) { ok = 1; goto done; }  // a static frame submitted again
    if (!ab_ready() || !ab_pipeline((VkFormat)t->info.format)) goto done;
    VkFramebuffer fb = ab_target(sub->swapchain, t->images[index], layer, (VkFormat)t->info.format, t->info.width,
                                 t->info.height);
    if (!fb) goto done;
    int slot = ab.next;
    if (ab.pending[slot]) {  // submitted AB_RING calls ago: normally long finished
        if (vk.WaitForFences(vk.device, 1, &ab.fence[slot], VK_TRUE, 100000000ull) != VK_SUCCESS) goto done;
        vk.ResetFences(vk.device, 1, &ab.fence[slot]);
        ab.pending[slot] = 0;
    }
    VkCommandBuffer cmd = ab.cmd[slot];
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vk.ResetCommandBuffer(cmd, 0);
    vk.BeginCommandBuffer(cmd, &begin);
    VkRect2D area = {{rect.offset.x, rect.offset.y}, {(uint32_t)rect.extent.width, (uint32_t)rect.extent.height}};
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, ab.pass, fb, area, 0, NULL};
    ab.CmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ab.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ab.pipeline);
    VkViewport vp = {(float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height,
                     0.0f, 1.0f};
    ab.CmdSetViewport(cmd, 0, 1, &vp);
    ab.CmdSetScissor(cmd, 0, 1, &area);
    ab.CmdDraw(cmd, 3, 1, 0, 0);
    ab.CmdEndRenderPass(cmd);
    vk.EndCommandBuffer(cmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vk.QueueSubmit(vk.queue, 1, &submit, ab.fence[slot]) != VK_SUCCESS) goto done;
    ab.pending[slot] = 1;
    ab.next = (slot + 1) % AB_RING;
    t->alpha_inverted[index] |= 1u << layer;
    ok = 1;
    static int logged;
    if (!logged++)
        LOG("alpha_blend: inverting the alpha of swapchain=%p (rect %dx%d at %d,%d, layer %u) for passthrough",
            (void *)(uintptr_t)sub->swapchain, rect.extent.width, rect.extent.height, rect.offset.x, rect.offset.y, layer);
done:
    pthread_mutex_unlock(&flip_lock);
    return ok;
}

// xrEndFrame, emulated passthrough: what the projection layer's XrCompositionLayerAlphaBlendFB asks for, done with
// the runtime's standard blend. 0 = nothing to do (no such struct, or factors that can't be expressed), 1 = straight
// alpha (inverted when asked), 2 = premultiplied colour (alpha inverted when asked).
static int alpha_blend_apply(const XrCompositionLayerProjection *p) {
    const XrCompositionLayerAlphaBlendFB *f = NULL;
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)p->next; n; n = n->next)
        if (n->type == XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB) f = (const XrCompositionLayerAlphaBlendFB *)n;
    if (!f) return 0;
    int src = f->srcFactorColor, dst = f->dstFactorColor;
    int straight = src == XR_BLEND_FACTOR_SRC_ALPHA_FB, premultiplied = src == XR_BLEND_FACTOR_ONE_FB,
        inverse = src == XR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA_FB;
    static int logged;
    if (!logged++)
        LOG("alpha_blend: projection layer asks for colour %d/%d alpha %d/%d (src/dst factors)", src, dst,
            f->srcFactorAlpha, f->dstFactorAlpha);
    if ((straight || premultiplied) && dst == XR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA_FB) return straight ? 1 : 2;
    if (!(inverse || premultiplied) || dst != XR_BLEND_FACTOR_SRC_ALPHA_FB) {
        static int warned;
        if (!warned++) LOG("alpha_blend: these factors can't be emulated, the layer is blended normally");
        return 0;
    }
    int done = 1;  // src * (1 - a) + dst * a: the standard blend with the alpha inverted
    for (uint32_t v = 0; v < p->viewCount && v < 2; ++v) done &= ab_invert_view(&p->views[v].subImage);
    if (!done) {
        static int warned;
        if (!warned++) LOG("alpha_blend: inverse alpha asked for, but the image couldn't be changed (not Vulkan?)");
        return 0;
    }
    return inverse ? 1 : 2;
}

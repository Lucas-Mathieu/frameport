// SPDX-License-Identifier: GPL-3.0-only
// Pipeline log for the Vulkan shim (adapter setting vk_pipeline_log=1, diagnostics; GitHub #349): when
// vkCreateGraphicsPipelines / vkCreateComputePipelines fail (a result other than VK_SUCCESS, or a NULL pipeline), the
// create infos are logged: per pipeline its flags, layout, render pass/subpass, the pNext structure types, sample count
// and vertex formats, and per stage its stage bit, shader module (as the vk_shader_dump file name <size>_<sha256>.spv,
// remembered at vkCreateShaderModule), entry point and pNext types. Sampler Y'CbCr conversions are followed through
// samplers, descriptor set layouts (immutable samplers) and pipeline layouts, so a failing pipeline says whether it
// samples through one ("ycbcr"). Conversions and samplers using one are logged when created.
// Unreal treats a NULL pipeline as "Shader compilation failures are Fatal." (PipelineStateCache.cpp) without saying
// why; this names the pipeline. Needs LOG(...), sha256() and readable() before inclusion (vkshim.c; the host test in
// tests/test_vk_pipeline_log.py provides its own).
#pragma once
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define PL_MODULES 16384  // shader modules remembered (a power of two)
#define PL_SET 4096       // handles per kind that involve a Y'CbCr conversion (a power of two)
static int pl_on;         // vk_pipeline_log
static pthread_mutex_t pl_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t handle; uint32_t size; uint8_t sha[32]; } pl_modules[PL_MODULES];
enum { PL_CONV, PL_SAMPLER, PL_SET_LAYOUT, PL_LAYOUT, PL_KINDS };
static uint64_t pl_sets[PL_KINDS][PL_SET];  // 0 = empty
static unsigned pl_created, pl_failed, pl_logged_ycbcr;

static unsigned pl_hash(uint64_t h, unsigned mask) { return (unsigned)((h * 0x9E3779B97F4A7C15ull) >> 40) & mask; }

static void pl_module_add(VkShaderModule module, const uint32_t *code, size_t size) {
    uint64_t h = (uint64_t)module;
    if (!h || !code) return;
    uint8_t digest[32];
    sha256((const uint8_t *)code, size, digest);
    pthread_mutex_lock(&pl_lock);
    for (unsigned i = pl_hash(h, PL_MODULES - 1), n = 0; n < PL_MODULES; i = (i + 1) & (PL_MODULES - 1), n++) {
        if (pl_modules[i].handle && pl_modules[i].handle != h) continue;
        pl_modules[i].handle = h;  // a handle reused after vkDestroyShaderModule is simply overwritten
        pl_modules[i].size = (uint32_t)size;
        memcpy(pl_modules[i].sha, digest, 32);
        break;
    }
    pthread_mutex_unlock(&pl_lock);
}

// "<size>_<sha256>.spv" of a module, else "unknown(0x...)" / "none"
static void pl_module_name(VkShaderModule module, char *out, size_t cap) {
    uint64_t h = (uint64_t)module;
    if (!h) { snprintf(out, cap, "none"); return; }
    snprintf(out, cap, "unknown(0x%" PRIx64 ")", h);
    pthread_mutex_lock(&pl_lock);
    for (unsigned i = pl_hash(h, PL_MODULES - 1), n = 0; n < PL_MODULES && pl_modules[i].handle;
         i = (i + 1) & (PL_MODULES - 1), n++) {
        if (pl_modules[i].handle != h) continue;
        int len = snprintf(out, cap, "%u_", pl_modules[i].size);
        for (int b = 0; b < 32 && len > 0 && (size_t)len < cap; b++)
            len += snprintf(out + len, cap - (size_t)len, "%02x", pl_modules[i].sha[b]);
        if (len > 0 && (size_t)len < cap) snprintf(out + len, cap - (size_t)len, ".spv");
        break;
    }
    pthread_mutex_unlock(&pl_lock);
}

static void pl_set_add(int kind, uint64_t h) {
    if (!h) return;
    pthread_mutex_lock(&pl_lock);
    for (unsigned i = pl_hash(h, PL_SET - 1), n = 0; n < PL_SET; i = (i + 1) & (PL_SET - 1), n++)
        if (!pl_sets[kind][i] || pl_sets[kind][i] == h) { pl_sets[kind][i] = h; break; }
    pthread_mutex_unlock(&pl_lock);
}

static int pl_set_has(int kind, uint64_t h) {
    if (!h) return 0;
    int found = 0;
    pthread_mutex_lock(&pl_lock);
    for (unsigned i = pl_hash(h, PL_SET - 1), n = 0; n < PL_SET && pl_sets[kind][i]; i = (i + 1) & (PL_SET - 1), n++)
        if (pl_sets[kind][i] == h) { found = 1; break; }
    pthread_mutex_unlock(&pl_lock);
    return found;
}

// The sTypes of a pNext chain, e.g. " 1000053000,1000044002" (at most 8; "!" marks an unreadable link)
static void pl_chain(const void *next, char *out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (int n = 0; next && n < 8 && len + 16 < cap; n++) {
        if (!readable(next, sizeof(VkBaseInStructure))) { snprintf(out + len, cap - len, "%s!", n ? "," : " "); return; }
        const VkBaseInStructure *b = next;
        len += (size_t)snprintf(out + len, cap - len, "%s%u", n ? "," : " ", (unsigned)b->sType);
        next = b->pNext;
    }
}

// A Y'CbCr conversion in a sampler's pNext chain (VkSamplerYcbcrConversionInfo), else 0
static uint64_t pl_sampler_conversion(const VkSamplerCreateInfo *ci) {
    for (const VkBaseInStructure *b = ci ? ci->pNext : NULL; b; b = b->pNext) {
        if (!readable(b, sizeof *b)) return 0;
        if (b->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO)
            return (uint64_t)((const VkSamplerYcbcrConversionInfo *)b)->conversion;
    }
    return 0;
}

static void pl_conversion_created(const VkSamplerYcbcrConversionCreateInfo *ci, VkResult r, VkSamplerYcbcrConversion c) {
    uint64_t external = 0;
    for (const VkBaseInStructure *b = ci ? ci->pNext : NULL; b; b = b->pNext) {
        if (!readable(b, sizeof *b)) break;
        if (b->sType == 1000129005 /* VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID */)
            external = ((const struct { VkStructureType s; const void *n; uint64_t f; } *)(const void *)b)->f;
    }
    if (r == VK_SUCCESS) pl_set_add(PL_CONV, (uint64_t)c);
    if (pl_logged_ycbcr++ < 20 && ci)
        LOG("vk shim: vkCreateSamplerYcbcrConversion format %d externalFormat 0x%" PRIx64 " model %d range %d "
            "chroma offsets %d/%d filter %d explicit %u: %d -> 0x%" PRIx64, ci->format, external, ci->ycbcrModel,
            ci->ycbcrRange, ci->xChromaOffset, ci->yChromaOffset, ci->chromaFilter, ci->forceExplicitReconstruction, r,
            r == VK_SUCCESS ? (uint64_t)c : 0);
}

static void pl_sampler_created(const VkSamplerCreateInfo *ci, VkResult r, VkSampler s) {
    uint64_t conv = pl_sampler_conversion(ci);
    if (!conv) return;
    if (r == VK_SUCCESS) pl_set_add(PL_SAMPLER, (uint64_t)s);
    if (pl_logged_ycbcr++ < 20)
        LOG("vk shim: vkCreateSampler with Y'CbCr conversion 0x%" PRIx64 "%s: %d", conv,
            pl_set_has(PL_CONV, conv) ? "" : " (not created through the shim)", r);
}

static void pl_set_layout_created(const VkDescriptorSetLayoutCreateInfo *ci, VkResult r, VkDescriptorSetLayout l) {
    if (r != VK_SUCCESS || !ci || !ci->pBindings) return;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        if (!b->pImmutableSamplers) continue;
        for (uint32_t j = 0; j < b->descriptorCount; j++)
            if (pl_set_has(PL_SAMPLER, (uint64_t)b->pImmutableSamplers[j])) {
                pl_set_add(PL_SET_LAYOUT, (uint64_t)l);
                return;
            }
    }
}

static void pl_layout_created(const VkPipelineLayoutCreateInfo *ci, VkResult r, VkPipelineLayout l) {
    if (r != VK_SUCCESS || !ci || !ci->pSetLayouts) return;
    for (uint32_t i = 0; i < ci->setLayoutCount; i++)
        if (pl_set_has(PL_SET_LAYOUT, (uint64_t)ci->pSetLayouts[i])) { pl_set_add(PL_LAYOUT, (uint64_t)l); return; }
}

static void pl_stage(int index, const VkPipelineShaderStageCreateInfo *s) {
    char module[96], chain[128];
    pl_module_name(s->module, module, sizeof module);
    pl_chain(s->pNext, chain, sizeof chain);
    LOG("vk shim:   [%d] stage 0x%x module %s entry %s flags 0x%x specialization %u pNext%s", index, s->stage, module,
        s->pName && readable(s->pName, 1) ? s->pName : "?", s->flags,
        s->pSpecializationInfo ? s->pSpecializationInfo->mapEntryCount : 0, chain[0] ? chain : " -");
}

// Called after vkCreateGraphicsPipelines; logs only failures (and a running count now and then)
static void pl_graphics(uint32_t count, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *out, VkResult r) {
    int bad = r != VK_SUCCESS;
    for (uint32_t i = 0; !bad && out && i < count; i++) bad = !out[i];
    pthread_mutex_lock(&pl_lock);
    unsigned created = bad ? pl_created : (pl_created += count), failed = bad ? ++pl_failed : pl_failed;
    pthread_mutex_unlock(&pl_lock);
    if (!bad) {
        if (created == count || created / 500 != (created - count) / 500)
            LOG("vk shim: %u pipeline(s) created so far, %u failed call(s)", created, failed);
        return;
    }
    if (failed > 50) return;
    LOG("vk shim: vkCreateGraphicsPipelines failed: %d (%u pipeline(s); failure %u)", r, count, failed);
    for (uint32_t i = 0; ci && i < count && i < 8; i++) {
        const VkGraphicsPipelineCreateInfo *p = &ci[i];
        char chain[128], formats[160] = "";
        pl_chain(p->pNext, chain, sizeof chain);
        const VkPipelineVertexInputStateCreateInfo *vi = p->pVertexInputState;
        size_t len = 0;
        for (uint32_t a = 0; vi && vi->pVertexAttributeDescriptions && a < vi->vertexAttributeDescriptionCount &&
                             len + 12 < sizeof formats; a++)
            len += (size_t)snprintf(formats + len, sizeof formats - len, "%s%d", a ? "," : "",
                                    vi->pVertexAttributeDescriptions[a].format);
        LOG("vk shim: pipeline %u: %s flags 0x%x layout 0x%" PRIx64 "%s renderPass 0x%" PRIx64 " subpass %u samples %d "
            "colour attachments %u vertex formats [%s] pNext%s", i, out && out[i] ? "ok" : "NULL", p->flags,
            (uint64_t)p->layout, pl_set_has(PL_LAYOUT, (uint64_t)p->layout) ? " (ycbcr)" : "", (uint64_t)p->renderPass,
            p->subpass, p->pMultisampleState ? (int)p->pMultisampleState->rasterizationSamples : 0,
            p->pColorBlendState ? p->pColorBlendState->attachmentCount : 0, formats, chain[0] ? chain : " -");
        for (uint32_t s = 0; p->pStages && s < p->stageCount && s < 6; s++) pl_stage((int)s, &p->pStages[s]);
    }
}

static void pl_compute(uint32_t count, const VkComputePipelineCreateInfo *ci, const VkPipeline *out, VkResult r) {
    int bad = r != VK_SUCCESS;
    for (uint32_t i = 0; !bad && out && i < count; i++) bad = !out[i];
    if (!bad) return;
    pthread_mutex_lock(&pl_lock);
    unsigned failed = ++pl_failed;
    pthread_mutex_unlock(&pl_lock);
    if (failed > 50) return;
    LOG("vk shim: vkCreateComputePipelines failed: %d (%u pipeline(s); failure %u)", r, count, failed);
    for (uint32_t i = 0; ci && i < count && i < 8; i++) {
        char chain[128];
        pl_chain(ci[i].pNext, chain, sizeof chain);
        LOG("vk shim: pipeline %u: %s flags 0x%x layout 0x%" PRIx64 "%s pNext%s", i, out && out[i] ? "ok" : "NULL",
            ci[i].flags, (uint64_t)ci[i].layout, pl_set_has(PL_LAYOUT, (uint64_t)ci[i].layout) ? " (ycbcr)" : "",
            chain[0] ? chain : " -");
        pl_stage(0, &ci[i].stage);
    }
}

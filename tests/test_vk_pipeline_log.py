"""vk_pipeline_log (GitHub #349): the Vulkan shim logs failing pipeline creations with their stages' modules and any
sampler Y'CbCr conversion behind their layout."""
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from frameport.patches import base
from frameport.patches.settings import UI

ROOT = Path(__file__).resolve().parents[1]

HOST_TEST = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static int readable(const void *p, size_t n) { (void)n; return p != NULL; }
#include "sha256.h"
#include "pipeline_log.h"

int main(void) {
    pl_on = 1;
    uint32_t vs[5] = {0x07230203, 0x00010000, 0, 10, 0}, fs[6] = {0x07230203, 0x00010300, 0, 20, 0, 7};
    pl_module_add((VkShaderModule)0x1001, vs, sizeof vs);
    pl_module_add((VkShaderModule)0x1002, fs, sizeof fs);

    struct { VkStructureType s; const void *n; uint64_t f; } ext = {(VkStructureType)1000129005, NULL, 0x7fa30c06};
    VkSamplerYcbcrConversionCreateInfo cci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, &ext};
    pl_conversion_created(&cci, VK_SUCCESS, (VkSamplerYcbcrConversion)0x2001);
    VkSamplerYcbcrConversionInfo info = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL,
                                         (VkSamplerYcbcrConversion)0x2001};
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &info};
    pl_sampler_created(&sci, VK_SUCCESS, (VkSampler)0x3001);
    VkSampler immutable = (VkSampler)0x3001;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, &immutable};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &binding};
    pl_set_layout_created(&dci, VK_SUCCESS, (VkDescriptorSetLayout)0x4001);
    VkDescriptorSetLayout sets[2] = {(VkDescriptorSetLayout)0x4000, (VkDescriptorSetLayout)0x4001};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sets};
    pl_layout_created(&lci, VK_SUCCESS, (VkPipelineLayout)0x5001);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
         (VkShaderModule)0x1001, "main_vs", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         (VkShaderModule)0x1009, "main_ps", NULL}};
    VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0,
                                               NULL, 2, attrs};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
                                               VK_SAMPLE_COUNT_4_BIT};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, 0, 2, stages, &vi};
    gp.pMultisampleState = &ms;
    gp.layout = (VkPipelineLayout)0x5001;
    gp.renderPass = (VkRenderPass)0x6001;
    gp.subpass = 1;
    VkPipeline out = VK_NULL_HANDLE;
    pl_graphics(1, &gp, &out, VK_ERROR_UNKNOWN);  // failed
    out = (VkPipeline)0x7001;
    gp.layout = (VkPipelineLayout)0x5002;
    pl_graphics(1, &gp, &out, VK_SUCCESS);  // quiet apart from the first count
    pl_graphics(1, &gp, &out, VK_SUCCESS);
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, stages[0],
                                      (VkPipelineLayout)0x5002};
    VkPipeline none = VK_NULL_HANDLE;
    pl_compute(1, &cp, &none, VK_SUCCESS);  // a NULL pipeline counts as a failure too
    return 0;
}
"""


def test_setting_registered_off_by_default():
    base.load_all()
    p = base.REGISTRY["adapter.vk_pipeline_log"]
    assert p.category == "adapter" and p.default == 0 and not p.default_on
    assert UI["vk_pipeline_log"]["group"] == "troubleshooting"


def test_artifact_reads_the_setting():
    so = (ROOT / "artifacts/arm64-v8a/libfp_vk.so").read_bytes()
    assert b"vk_pipeline_log=" in so and b"vkCreateGraphicsPipelines failed" in so


def test_failed_pipeline_is_described(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt" or not Path("/usr/include/vulkan/vulkan.h").exists():
        pytest.skip("needs a Linux host C compiler and the Vulkan headers")
    src = tmp_path / "t.c"
    src.write_text(HOST_TEST)
    exe = tmp_path / "t"
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I",
                    str(ROOT / "native/vkshim"), str(src), "-pthread", "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    log = run.stderr
    vs = b"".join(w.to_bytes(4, "little") for w in (0x07230203, 0x00010000, 0, 10, 0))
    assert "externalFormat 0x7fa30c06" in log and "vkCreateSampler with Y'CbCr conversion 0x2001: 0" in log
    assert "vkCreateGraphicsPipelines failed: -13 (1 pipeline(s); failure 1)" in log
    line = next(ln for ln in log.splitlines() if ln.startswith("vk shim: pipeline 0: NULL"))
    assert "layout 0x5001 (ycbcr)" in line and "renderPass 0x6001 subpass 1 samples 4" in line
    assert "vertex formats [106,37]" in line and "pNext 1000044002" in line
    assert f"stage 0x1 module {len(vs)}_{hashlib.sha256(vs).hexdigest()}.spv entry main_vs" in log
    assert "stage 0x10 module unknown(0x1009) entry main_ps" in log
    assert "1 pipeline(s) created so far" in log and "2 pipeline(s) created" not in log  # successes stay quiet
    assert "vkCreateComputePipelines failed: 0 (1 pipeline(s); failure 2)" in log

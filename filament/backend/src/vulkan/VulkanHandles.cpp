/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VulkanHandles.h"

#include "VulkanConstants.h"
#include "VulkanMemory.h"
#include "VulkanPlatform.h"

#include <utils/Panic.h>

using namespace bluevk;

namespace filament {
namespace backend {

static void flipVertically(VkRect2D* rect, uint32_t framebufferHeight) {
    rect->offset.y = framebufferHeight - rect->offset.y - rect->extent.height;
}

static void flipVertically(VkViewport* rect, uint32_t framebufferHeight) {
    rect->y = framebufferHeight - rect->y - rect->height;
}

static void clampToFramebuffer(VkRect2D* rect, uint32_t fbWidth, uint32_t fbHeight) {
    int32_t x = std::max(rect->offset.x, 0);
    int32_t y = std::max(rect->offset.y, 0);
    int32_t right = std::min(rect->offset.x + (int32_t) rect->extent.width, (int32_t) fbWidth);
    int32_t top = std::min(rect->offset.y + (int32_t) rect->extent.height, (int32_t) fbHeight);
    rect->offset.x = std::min(x, (int32_t) fbWidth);
    rect->offset.y = std::min(y, (int32_t) fbHeight);
    rect->extent.width = std::max(right - x, 0);
    rect->extent.height = std::max(top - y, 0);
}

VulkanProgram::VulkanProgram(VulkanContext& context, const Program& builder) noexcept :
        HwProgram(builder.getName()), context(context) {
    auto const& blobs = builder.getShadersSource();
    VkShaderModule* modules[2] = { &bundle.vertex, &bundle.fragment };
    bool missing = false;
    for (size_t i = 0; i < Program::SHADER_TYPE_COUNT; i++) {
        const auto& blob = blobs[i];
        VkShaderModule* module = modules[i];
        if (blob.empty()) {
            missing = true;
            continue;
        }
        VkShaderModuleCreateInfo moduleInfo = {};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.size();
        moduleInfo.pCode = (uint32_t*) blob.data();
        VkResult result = vkCreateShaderModule(context.device, &moduleInfo, VKALLOC, module);
        ASSERT_POSTCONDITION(result == VK_SUCCESS, "Unable to create shader module.");
    }

    // Output a warning because it's okay to encounter empty blobs, but it's not okay to use
    // this program handle in a draw call.
    if (missing) {
        utils::slog.w << "Missing SPIR-V shader: " << builder.getName().c_str() << utils::io::endl;
        return;
    }

    // Make a copy of the binding map
    samplerGroupInfo = builder.getSamplerGroupInfo();
#if FILAMENT_VULKAN_VERBOSE
    utils::slog.d << "Created VulkanProgram " << builder.getName().c_str()
                << ", variant = (" << utils::io::hex
                << (int) builder.getVariant() << utils::io::dec << "), "
                << "shaders = (" << bundle.vertex << ", " << bundle.fragment << ")"
                << utils::io::endl;
#endif
}

VulkanProgram::VulkanProgram(VulkanContext& context, VkShaderModule vs, VkShaderModule fs) noexcept :
        context(context) {
    bundle.vertex = vs;
    bundle.fragment = fs;
}

VulkanProgram::~VulkanProgram() {
    vkDestroyShaderModule(context.device, bundle.vertex, VKALLOC);
    vkDestroyShaderModule(context.device, bundle.fragment, VKALLOC);
}

static VulkanAttachment createAttachment(VulkanContext& context, VulkanAttachment spec) {
    if (spec.texture == nullptr) {
        return spec;
    }
    return {
        .format = spec.texture->getVkFormat(),
        .image = spec.texture->getVkImage(),
        .view = {},
        .memory = {},
        .texture = spec.texture,
        .layout = spec.texture->getVkLayout(spec.layer, spec.level),
        .level = spec.level,
        .layer = spec.layer
    };
}

// Creates a special "default" render target (i.e. associated with the swap chain)
// Note that the attachment structs are unused in this case in favor of VulkanSwapChain.
VulkanRenderTarget::VulkanRenderTarget(VulkanContext& context) : HwRenderTarget(0, 0),
        mOffscreen(false), mSamples(1) {}

VulkanRenderTarget::VulkanRenderTarget(VulkanContext& context, uint32_t width, uint32_t height,
            uint8_t samples, VulkanAttachment color[MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT],
            VulkanAttachment depthStencil[2], VulkanStagePool& stagePool) :
            HwRenderTarget(width, height), mOffscreen(true), mSamples(samples) {

    // For each color attachment, create (or fetch from cache) a VkImageView that selects a specific
    // miplevel and array layer.
    for (int index = 0; index < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; index++) {
        const VulkanAttachment& spec = color[index];
        mColor[index] = createAttachment(context, spec);
        VulkanTexture* texture = spec.texture;
        if (texture == nullptr) {
            continue;
        }
        mColor[index].view = texture->getAttachmentView(spec.level, spec.layer,
                VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // For the depth attachment, create (or fetch from cache) a VkImageView that selects a specific
    // miplevel and array layer.
    const VulkanAttachment& depthSpec = depthStencil[0];
    mDepth = createAttachment(context, depthSpec);
    VulkanTexture* depthTexture = mDepth.texture;
    if (depthTexture) {
        mDepth.view = depthTexture->getAttachmentView(mDepth.level, mDepth.layer,
                VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    if (samples == 1) {
        return;
    }

    // Constrain the sample count according to both kinds of sample count masks obtained from
    // VkPhysicalDeviceProperties. This is consistent with the VulkanTexture constructor.
    const auto& limits = context.physicalDeviceProperties.limits;
    mSamples = samples = reduceSampleCount(samples, limits.framebufferDepthSampleCounts &
            limits.framebufferColorSampleCounts);

    // The sidecar textures need to have only 1 miplevel and 1 array slice.
    const int level = 1;
    const int depth = 1;

    // Create sidecar MSAA textures for color attachments.
    for (int index = 0; index < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; index++) {
        const VulkanAttachment& spec = color[index];
        VulkanTexture* texture = spec.texture;
        if (texture && texture->samples == 1) {
            VulkanTexture* msTexture = texture->getSidecar();
            if (UTILS_UNLIKELY(msTexture == nullptr)) {
                msTexture = new VulkanTexture(context, texture->target, level,
                        texture->format, samples, width, height, depth, texture->usage, stagePool);
                texture->setSidecar(msTexture);
            }
            mMsaaAttachments[index] = createAttachment(context, { .texture = msTexture });
            mMsaaAttachments[index].view = msTexture->getAttachmentView(0, 0,
                    VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (texture && texture->samples > 1) {
            mMsaaAttachments[index] = mColor[index];
        }
    }

    if (depthTexture == nullptr) {
        return;
    }

    // There is no need for sidecar depth if the depth texture is already MSAA.
    if (depthTexture->samples > 1) {
        mMsaaDepthAttachment = mDepth;
        return;
    }

    // Create sidecar MSAA texture for the depth attachment.
    VulkanTexture* msTexture = depthTexture->getSidecar();
    if (UTILS_UNLIKELY(msTexture == nullptr)) {
        msTexture = new VulkanTexture(context, depthTexture->target, level,
                depthTexture->format, samples, width, height, depth, depthTexture->usage, stagePool);
        depthTexture->setSidecar(msTexture);
    }
    mMsaaDepthAttachment = createAttachment(context, {
        .format = {},
        .image = {},
        .view = {},
        .memory = {},
        .texture = msTexture,
        .layout = {},
        .level = depthSpec.level,
        .layer = depthSpec.layer,
    });
    mMsaaDepthAttachment.view = msTexture->getAttachmentView(depthSpec.level, depthSpec.layer,
            VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanRenderTarget::transformClientRectToPlatform(VulkanSwapChain* currentSurface, VkRect2D* bounds) const {
    const auto& extent = getExtent(currentSurface);
    flipVertically(bounds, extent.height);
    clampToFramebuffer(bounds, extent.width, extent.height);
}

void VulkanRenderTarget::transformClientRectToPlatform(VulkanSwapChain* currentSurface, VkViewport* bounds) const {
    flipVertically(bounds, getExtent(currentSurface).height);
}

VkExtent2D VulkanRenderTarget::getExtent(VulkanSwapChain* currentSurface) const {
    if (mOffscreen) {
        return {width, height};
    }
    return currentSurface->clientSize;
}

VulkanAttachment VulkanRenderTarget::getColor(VulkanSwapChain* currentSurface, int target) const {
    return (mOffscreen || target > 0) ? mColor[target] : currentSurface->getColor();
}

VulkanAttachment VulkanRenderTarget::getMsaaColor(int target) const {
    return mMsaaAttachments[target];
}

VulkanAttachment VulkanRenderTarget::getDepth(VulkanSwapChain* currentSurface) const {
    return mOffscreen ? mDepth : currentSurface->depth;
}

VulkanAttachment VulkanRenderTarget::getMsaaDepth() const {
    return mMsaaDepthAttachment;
}

int VulkanRenderTarget::getColorTargetCount(const VulkanRenderPass& pass) const {
    if (!mOffscreen) {
        return 1;
    }
    int count = 0;
    for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
        if (mColor[i].format == VK_FORMAT_UNDEFINED) {
            continue;
        }
        // NOTE: This must be consistent with VkRenderPass construction (see VulkanFboCache).
        if (!(pass.subpassMask & (1 << i)) || pass.currentSubpass == 1) {
            count++;
        }
    }
    return count;
}

VulkanVertexBuffer::VulkanVertexBuffer(VulkanContext& context, VulkanStagePool& stagePool,
        uint8_t bufferCount, uint8_t attributeCount,
        uint32_t elementCount, AttributeArray const& attribs) :
        HwVertexBuffer(bufferCount, attributeCount, elementCount, attribs),
        buffers(bufferCount, nullptr) {}


VulkanBufferObject::VulkanBufferObject(VulkanContext& context, VulkanStagePool& stagePool,
        uint32_t byteCount, BufferObjectBinding bindingType, BufferUsage usage)
        : HwBufferObject(byteCount),
          buffer(context, stagePool, getBufferObjectUsage(bindingType), byteCount),
          bindingType(bindingType) {
}

void VulkanRenderPrimitive::setPrimitiveType(backend::PrimitiveType pt) {
    this->type = pt;
    switch (pt) {
        case backend::PrimitiveType::NONE:
        case backend::PrimitiveType::POINTS:
            primitiveTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case backend::PrimitiveType::LINES:
            primitiveTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case backend::PrimitiveType::LINE_STRIP:
            primitiveTopology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case backend::PrimitiveType::TRIANGLES:
            primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        case backend::PrimitiveType::TRIANGLE_STRIP:
            primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
    }
}

void VulkanRenderPrimitive::setBuffers(VulkanVertexBuffer* vertexBuffer,
        VulkanIndexBuffer* indexBuffer) {
    this->vertexBuffer = vertexBuffer;
    this->indexBuffer = indexBuffer;
}

VulkanTimerQuery::VulkanTimerQuery(VulkanContext& context) : mContext(context) {
    std::unique_lock<utils::Mutex> lock(context.timestamps.mutex);
    utils::bitset32& bitset = context.timestamps.used;
    const size_t maxTimers = bitset.size();
    assert_invariant(bitset.count() < maxTimers);
    for (size_t timerIndex = 0; timerIndex < maxTimers; ++timerIndex) {
        if (!bitset.test(timerIndex)) {
            bitset.set(timerIndex);
            startingQueryIndex = timerIndex * 2;
            stoppingQueryIndex = timerIndex * 2 + 1;
            return;
        }
    }
    utils::slog.e << "More than " << maxTimers << " timers are not supported." << utils::io::endl;
    startingQueryIndex = 0;
    stoppingQueryIndex = 1;
}

VulkanTimerQuery::~VulkanTimerQuery() {
    std::unique_lock<utils::Mutex> lock(mContext.timestamps.mutex);
    mContext.timestamps.used.unset(startingQueryIndex / 2);
}

} // namespace filament
} // namespace backend

#include "rhi/VulkanRHI.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include "rhi/VulkanRHIVkUtil.h"
#include "rhi/RhiLog.h"
#include "rhi/RhiReadback.h"
#include "rhi/SpirvCompiler.h"

using namespace SFE::Render::VulkanVkUtil;

namespace SFE::Render {
	namespace {
		// The present blit belongs to the backend, so its shaders live here rather than in a content
		// directory the host may not have.
		constexpr const char* kBlitVertexSource = R"(#version 450 core
layout(location = 0) out vec2 vUV;
void main() {
    const vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const vec2 uvs[3] = vec2[](vec2(0.0, 1.0), vec2(2.0, 1.0), vec2(0.0, -1.0));
    vUV = uvs[gl_VertexIndex];
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)";

		constexpr const char* kBlitFragmentSource = R"(#version 450 core
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sourceTex;
void main() {
    outColor = vec4(texture(sourceTex, vUV).rgb, 1.0);
}
)";

		VkShaderModule createVkShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
			if (code.empty()) {
				RhiLog::fatal("VulkanRHI: empty SPIR-V");
				return VK_NULL_HANDLE;
			}
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = code.size() * sizeof(uint32_t);
			createInfo.pCode = code.data();
			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkCreateShaderModule failed");
			}
			return module;
		}
	}

	void VulkanRHI::setPresentBlitSource(const TextureHandle texture, const SamplerHandle sampler) {
		m_presentBlitTexture = texture;
		m_presentBlitSampler = sampler;
	}

	bool VulkanRHI::readbackTexture(TextureHandle handle, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t layer) {
		// Texture writes still waiting in the frame's upload ring (queueTextureUpload) copied first: what was written is read.
		if (!m_textureUploadOps.empty()) {
			executeImmediate([&](VkCommandBuffer cmd) { recordTextureUploads(cmd); });
		}
		auto* source = getLive(m_textures, handle.id);
		if (!source) {
			return false;
		}
		width = static_cast<int>(source->desc.width);
		height = static_cast<int>(source->desc.height);
		if (width <= 0 || height <= 0) {
			return false;
		}

		const bool isDepth = (aspectMaskForFormat(toVkFormat(source->desc.format)) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
		const int floatChannels = floatFormatChannels(source->desc.format);

		// Pass outputs come in many formats (RGBA16F and friends). Blit through a scratch RGBA8
		// image so one copy path serves all of them and the bytes match what OpenGL hands back.
		// Depth is copied straight out instead: it cannot be blitted into a colour image. So are float formats,
		// converted on the CPU by the rule OpenGL uses too (RhiReadback.h): the blit rounded some values to a
		// different byte than glReadPixels did.
		TextureHandle scratchHandle{};
		VkTextureResource* tex = source;
		if (!isDepth && floatChannels == 0 && source->desc.format != TextureFormat::RGBA8) {
			TextureDesc scratchDesc;
			scratchDesc.width = source->desc.width;
			scratchDesc.height = source->desc.height;
			scratchDesc.format = TextureFormat::RGBA8;
			scratchDesc.usage = TextureUsage::Sampled | TextureUsage::TransferSrc | TextureUsage::TransferDst;
			scratchHandle = createTexture(scratchDesc, nullptr);
			// createTexture can grow m_textures, which invalidates every pointer into it.
			source = getLive(m_textures, handle.id);
			auto* scratch = getLive(m_textures, scratchHandle.id);
			if (!source || !scratch) {
				return false;
			}
			executeImmediate([&](VkCommandBuffer cmd) {
				blitTextureToTexture(cmd, *source, *scratch);
			});
			tex = getLive(m_textures, scratchHandle.id);
			if (!tex) {
				return false;
			}
		}

		const VkDeviceSize texelBytes = floatChannels > 0 ? bytesPerPixel(source->desc.format) : 4u;
		const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * texelBytes;
		VkBuffer staging = VK_NULL_HANDLE;
		VmaAllocation stagingAlloc = VK_NULL_HANDLE;
		VmaAllocationInfo mappedInfo{};
		{
			VkBufferCreateInfo bufferInfo{};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = byteSize;
			bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocInfo{};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &staging, &stagingAlloc, &mappedInfo) != VK_SUCCESS) {
				if (scratchHandle) {
					destroyTexture(scratchHandle);
				}
				return false;
			}
		}

		const VkImageLayout restore = tex->layout;
		executeImmediate([&](VkCommandBuffer cmd) {
			ensureTextureLayout(cmd, *tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy region{};
			region.imageSubresource.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = 0;
			region.imageSubresource.baseArrayLayer = layer;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
			vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
			ensureTextureLayout(cmd, *tex, restore != VK_IMAGE_LAYOUT_UNDEFINED ? restore : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

		if (!mappedInfo.pMappedData) {
			destroyStagingBuffer(staging, stagingAlloc);
			if (scratchHandle) {
				destroyTexture(scratchHandle);
			}
			return false;
		}
		vmaInvalidateAllocation(m_allocator, stagingAlloc, 0, VK_WHOLE_SIZE);

		// The engine feeds Vulkan an OpenGL-convention projection (+Y up) and never flips the
		// viewport, so image row 0 holds the BOTTOM of the picture here, exactly as glReadPixels
		// does. Flip so callers always get rows in display order.
		const size_t rowBytes = static_cast<size_t>(width) * 4u;
		rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
		if (isDepth) {
			// D32 float. Packed the same way as the OpenGL path: 16 bits across R and G
			// (R = high byte), B kept as a greyscale preview. 8 bits cannot resolve a far
			// cascade, where the whole scene sits in the last fraction of the range.
			const auto* depth = static_cast<const float*>(mappedInfo.pMappedData);
			for (int y = 0; y < height; ++y) {
				const size_t srcRow = static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width);
				for (int x = 0; x < width; ++x) {
					const float d = std::clamp(depth[srcRow + static_cast<size_t>(x)], 0.f, 1.f);
					const auto q = static_cast<uint32_t>(d * 65535.f + 0.5f);
					uint8_t* out = rgba.data() + rowBytes * static_cast<size_t>(y) + static_cast<size_t>(x) * 4u;
					out[0] = static_cast<uint8_t>(q >> 8);
					out[1] = static_cast<uint8_t>(q & 0xFF);
					out[2] = static_cast<uint8_t>(q >> 8);
					out[3] = 255;
				}
			}
		}
		else if (floatChannels > 0) {
			const auto* src = static_cast<const uint8_t*>(mappedInfo.pMappedData);
			const size_t channelBytes = static_cast<size_t>(texelBytes) / static_cast<size_t>(floatChannels);
			for (int y = 0; y < height; ++y) {
				const size_t srcRow = static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width);
				for (int x = 0; x < width; ++x) {
					const uint8_t* texel = src + (srcRow + static_cast<size_t>(x)) * static_cast<size_t>(texelBytes);
					uint8_t* out = rgba.data() + rowBytes * static_cast<size_t>(y) + static_cast<size_t>(x) * 4u;
					for (int c = 0; c < 4; ++c) {
						float value = c == 3 ? 1.f : 0.f;
						if (c < floatChannels) {
							const uint8_t* bytes = texel + static_cast<size_t>(c) * channelBytes;
							if (channelBytes == 2) {
								uint16_t half = 0;
								std::memcpy(&half, bytes, 2);
								value = halfToFloat(half);
							}
							else {
								std::memcpy(&value, bytes, 4);
							}
						}
						out[c] = floatToUnorm8(value);
					}
				}
			}
		}
		else {
			const auto* src = static_cast<const uint8_t*>(mappedInfo.pMappedData);
			for (int y = 0; y < height; ++y) {
				std::memcpy(rgba.data() + rowBytes * static_cast<size_t>(y),
					src + rowBytes * static_cast<size_t>(height - 1 - y),
					rowBytes);
			}
		}
		destroyStagingBuffer(staging, stagingAlloc);
		if (scratchHandle) {
			destroyTexture(scratchHandle);
		}
		return true;
	}

	void VulkanRHI::beginFrame() {
		waitForInFlightFrame();
	}

	void VulkanRHI::blitTextureToTexture(VkCommandBuffer cmd, VkTextureResource& src, VkTextureResource& dst) {
		ensureTextureLayout(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		ensureTextureLayout(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageBlit blit{};
		blit.srcSubresource.aspectMask = aspectMaskForFormat(src.format);
		blit.srcSubresource.mipLevel = 0;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = {
			static_cast<int32_t>((std::max)(1u, src.desc.width)),
			static_cast<int32_t>((std::max)(1u, src.desc.height)),
			1
		};
		blit.dstSubresource.aspectMask = aspectMaskForFormat(dst.format);
		blit.dstSubresource.mipLevel = 0;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = {
			static_cast<int32_t>((std::max)(1u, dst.desc.width)),
			static_cast<int32_t>((std::max)(1u, dst.desc.height)),
			1
		};
		vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
		ensureTextureLayout(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ensureTextureLayout(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	void VulkanRHI::blitToSwapchain(VkCommandBuffer cmd, VkTextureResource& src) {
		if (!m_swapchainAcquired || m_acquiredImageIndex >= m_swapchainImages.size() || !src.image) {
			return;
		}
		if (!m_swapchainTransferDst || src.format != m_swapchainFormat) {
			return;
		}

		ensureTextureLayout(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		auto& layout = m_swapchainLayouts[m_acquiredImageIndex];
		transitionImageLayout(cmd, m_swapchainImages[m_acquiredImageIndex], m_swapchainFormat,
			layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		VkImageBlit blit{};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1] = {
			static_cast<int32_t>((std::max)(1u, src.desc.width)),
			static_cast<int32_t>((std::max)(1u, src.desc.height)),
			1
		};
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		const int32_t dstW = static_cast<int32_t>((std::max)(1u, m_swapchainExtent.width));
		const int32_t dstH = static_cast<int32_t>((std::max)(1u, m_swapchainExtent.height));
		blit.dstOffsets[0] = { 0, dstH, 0 };
		blit.dstOffsets[1] = { dstW, 0, 1 };
		vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			m_swapchainImages[m_acquiredImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_NEAREST);
		ensureTextureLayout(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_swapchainBlitted = true;
	}

	void VulkanRHI::ensureBackbufferHandle() {
		if (auto* fb = m_backbufferHandle ? getLive(m_framebuffers, m_backbufferHandle.id) : nullptr) {
			fb->swapchainTarget = true;
			fb->desc.width = m_swapchainExtent.width;
			fb->desc.height = m_swapchainExtent.height;
			return;
		}
		VkFramebufferResource fb{};
		fb.live = true;
		fb.swapchainTarget = true;
		fb.desc.width = m_swapchainExtent.width;
		fb.desc.height = m_swapchainExtent.height;
		m_backbufferHandle.id = storeResource(m_framebuffers, m_freeFramebuffers, std::move(fb));
	}

	void VulkanRHI::createSceneCommandPool() {
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
		if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_sceneCommandPool) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create scene command pool");
		}

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_sceneFence) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create scene fence");
		}
		if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_uploadFence) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create upload fence");
		}

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_sceneCommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 2;
		if (vkAllocateCommandBuffers(m_device, &allocInfo, m_sceneCmds) != VK_SUCCESS) {
			m_sceneCmds[0] = VK_NULL_HANDLE;
			m_sceneCmds[1] = VK_NULL_HANDLE;
			RhiLog::fatal("VulkanRHI: failed to allocate scene command buffers");
		}
	}

	std::vector<uint32_t> VulkanRHI::compileShader(const ShaderStage stage, const std::string& name, const std::string& source) {
		const RhiHost* h = host();
		auto code = compileSpirv(stage, name, source, h ? h->shaderCacheDir() : std::filesystem::path{}, [&] {
			if (h) {
				h->onShaderCompile(name, false);
			}
		});
		if (h) {
			h->onShaderCompile(name, true);
		}
		return code;
	}

	void VulkanRHI::createBlitPipeline() {
		const auto vertCode = compileShader(ShaderStage::Vertex, "vulkan_blit.vs", kBlitVertexSource);
		const auto fragCode = compileShader(ShaderStage::Fragment, "vulkan_blit.fs", kBlitFragmentSource);
		const VkShaderModule vertModule = createVkShaderModule(m_device, vertCode);
		const VkShaderModule fragModule = createVkShaderModule(m_device, fragCode);

		VkDescriptorSetLayoutBinding samplerBinding{};
		samplerBinding.binding = 0;
		samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		samplerBinding.descriptorCount = 1;
		samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings = &samplerBinding;
		if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_blitSetLayout) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: blit descriptor set layout failed");
		}

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &m_blitSetLayout;
		if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_blitPipelineLayout) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: blit pipeline layout failed");
		}

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_blitDefaultSampler) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: blit sampler failed");
		}

		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = m_descriptorPool;
		const VkDescriptorSetLayout blitLayouts[kMaxFramesInFlight] = { m_blitSetLayout, m_blitSetLayout };
		allocInfo.descriptorSetCount = kMaxFramesInFlight;
		allocInfo.pSetLayouts = blitLayouts;
		if (vkAllocateDescriptorSets(m_device, &allocInfo, m_blitDescriptorSets) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: blit descriptor set allocation failed");
		}

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertModule;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragModule;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.lineWidth = 1.f;

		VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo colorBlending{};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &blendAttachment;

		const std::array dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = stages;
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = m_blitPipelineLayout;
		pipelineInfo.renderPass = m_swapchainRenderPass;
		pipelineInfo.subpass = 0;

		if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &m_blitPipeline) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: blit pipeline creation failed");
		}

		vkDestroyShaderModule(m_device, vertModule, nullptr);
		vkDestroyShaderModule(m_device, fragModule, nullptr);
	}

	void VulkanRHI::destroyBlitPipeline() {
		if (m_blitPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(m_device, m_blitPipeline, nullptr);
			m_blitPipeline = VK_NULL_HANDLE;
		}
		if (m_blitPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(m_device, m_blitPipelineLayout, nullptr);
			m_blitPipelineLayout = VK_NULL_HANDLE;
		}
		if (m_blitSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(m_device, m_blitSetLayout, nullptr);
			m_blitSetLayout = VK_NULL_HANDLE;
		}
		if (m_blitDefaultSampler != VK_NULL_HANDLE) {
			vkDestroySampler(m_device, m_blitDefaultSampler, nullptr);
			m_blitDefaultSampler = VK_NULL_HANDLE;
		}
		for (auto& set : m_blitDescriptorSets) {
			set = VK_NULL_HANDLE;
		}
	}

	void VulkanRHI::recordPresentBlit(const VkCommandBuffer cmd, const uint32_t imageIndex) {
		auto* tex = getLive(m_textures, m_presentBlitTexture.id);
		if (!tex || !tex->view) {
			VkRenderPassBeginInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = m_swapchainRenderPass;
			renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
			renderPassInfo.renderArea.offset = { 0, 0 };
			renderPassInfo.renderArea.extent = m_swapchainExtent;
			VkClearValue clearValue{};
			clearValue.color = { { 0.05f, 0.08f, 0.22f, 1.f } };
			renderPassInfo.clearValueCount = 1;
			renderPassInfo.pClearValues = &clearValue;
			vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cmd);
			return;
		}

		const bool blitCompatible = m_swapchainTransferDst
			&& imageIndex < m_swapchainImages.size()
			&& tex->image
			&& tex->format == m_swapchainFormat;
		if (blitCompatible) {
			ensureTextureLayout(cmd, *tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			transitionImageLayout(cmd, m_swapchainImages[imageIndex], m_swapchainFormat,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkImageBlit blit{};
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.layerCount = 1;
			blit.srcOffsets[1] = {
				static_cast<int32_t>((std::max)(1u, tex->desc.width)),
				static_cast<int32_t>((std::max)(1u, tex->desc.height)),
				1
			};
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.layerCount = 1;
			const int32_t dstW = static_cast<int32_t>((std::max)(1u, m_swapchainExtent.width));
			const int32_t dstH = static_cast<int32_t>((std::max)(1u, m_swapchainExtent.height));
			blit.dstOffsets[0] = { 0, dstH, 0 };
			blit.dstOffsets[1] = { dstW, 0, 1 };
			vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				m_swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &blit, VK_FILTER_NEAREST);
			transitionImageLayout(cmd, m_swapchainImages[imageIndex], m_swapchainFormat,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			ensureTextureLayout(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			return;
		}

		ensureTextureLayout(cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		const VkDescriptorSet blitSet = m_blitDescriptorSets[m_currentFrame];
		if (m_blitPipeline == VK_NULL_HANDLE || blitSet == VK_NULL_HANDLE) {
			VkRenderPassBeginInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = m_swapchainRenderPass;
			renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
			renderPassInfo.renderArea.offset = { 0, 0 };
			renderPassInfo.renderArea.extent = m_swapchainExtent;
			VkClearValue clearValue{};
			clearValue.color = { { 0.05f, 0.08f, 0.22f, 1.f } };
			renderPassInfo.clearValueCount = 1;
			renderPassInfo.pClearValues = &clearValue;
			vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cmd);
			return;
		}

		auto* samplerRes = getLive(m_samplers, m_presentBlitSampler.id);
		VkSampler sampler = samplerRes ? samplerRes->sampler : m_blitDefaultSampler;

		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = tex->view;
		imageInfo.sampler = sampler;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = blitSet;
		write.dstBinding = 0;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.descriptorCount = 1;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = m_swapchainRenderPass;
		renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = m_swapchainExtent;
		VkClearValue clearValue{};
		clearValue.color = { { 0.05f, 0.08f, 0.22f, 1.f } };
		renderPassInfo.clearValueCount = 1;
		renderPassInfo.pClearValues = &clearValue;

		vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{};
		viewport.width = static_cast<float>(m_swapchainExtent.width);
		viewport.height = static_cast<float>(m_swapchainExtent.height);
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		VkRect2D scissor{ { 0, 0 }, m_swapchainExtent };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipelineLayout, 0, 1, &blitSet, 0, nullptr);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
	}

	void VulkanRHI::expandSetLayouts(const std::vector<DescriptorSetLayoutHandle>& handles, std::vector<VkDescriptorSetLayout>& out) const {
		out.clear();
		out.reserve(handles.size() * 2);
		for (const auto layoutHandle : handles) {
			const auto* layout = getLive(m_descriptorSetLayouts, layoutHandle.id);
			if (!layout) {
				continue;
			}
			out.push_back(layout->layout);
		}
	}

	uint32_t VulkanRHI::inferFramebufferLayers(const FramebufferDesc& desc) const {
		auto layersOf = [this](const TextureHandle handle) -> uint32_t {
			const auto* tex = getLive(m_textures, handle.id);
			if (!tex) {
				return 1u;
			}
			if (tex->desc.dimension == TextureDimension::Array) {
				return (std::max)(1u, static_cast<uint32_t>(tex->desc.depth));
			}
			if (tex->desc.dimension == TextureDimension::Cube) {
				return 6u;
			}
			return 1u;
		};

		uint32_t layers = (std::max)(1u, desc.layers);
		for (const auto& color : desc.colorAttachments) {
			if (color.layer != UINT32_MAX) {
				continue;
			}
			layers = (std::max)(layers, layersOf(color.texture));
		}
		if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->layer == UINT32_MAX) {
			layers = (std::max)(layers, layersOf(desc.depthStencilAttachment->texture));
		}
		return layers;
	}

	bool VulkanRHI::realizeFramebuffer(VkFramebufferResource& resource, VkRenderPass vkPass) {
		if (resource.swapchainTarget) {
			return false;
		}
		if (resource.framebuffer != VK_NULL_HANDLE) {
			return true;
		}
		if (vkPass == VK_NULL_HANDLE) {
			return false;
		}

		// A texture's own view spans the whole array, so attaching it ignores a requested layer and
		// every draw lands on layer 0. When an attachment pins a layer, give the framebuffer a view
		// of just that slice; it is owned here and destroyed with the framebuffer.
		auto viewFor = [this, &resource](const FramebufferAttachmentDesc& att, const VkTextureResource& tex) -> VkImageView {
			if (att.layer == UINT32_MAX || tex.desc.dimension == TextureDimension::Tex2D) {
				return tex.view;
			}
			VkImageViewCreateInfo viewInfo{};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = tex.image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = tex.format;
			viewInfo.subresourceRange.aspectMask = aspectMaskForFormat(tex.format);
			viewInfo.subresourceRange.baseMipLevel = att.mipLevel;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = att.layer;
			viewInfo.subresourceRange.layerCount = 1;
			VkImageView view = VK_NULL_HANDLE;
			if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkCreateImageView failed for layer %u", att.layer);
				return tex.view;
			}
			resource.ownedViews.push_back(view);
			return view;
		};

		std::vector<VkImageView> attachments;
		attachments.reserve(resource.desc.colorAttachments.size() + 1);
		for (const auto& color : resource.desc.colorAttachments) {
			const auto* tex = getLive(m_textures, color.texture.id);
			if (!tex || !tex->view) {
				RhiLog::fatal("VulkanRHI: invalid color attachment texture");
				return false;
			}
			attachments.push_back(viewFor(color, *tex));
		}
		if (resource.desc.depthStencilAttachment.has_value()) {
			const auto* tex = getLive(m_textures, resource.desc.depthStencilAttachment->texture.id);
			if (!tex || !tex->view) {
				RhiLog::fatal("VulkanRHI: invalid depth attachment texture");
				return false;
			}
			attachments.push_back(viewFor(*resource.desc.depthStencilAttachment, *tex));
		}

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = vkPass;
		framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		framebufferInfo.pAttachments = attachments.data();
		framebufferInfo.width = (std::max)(1u, resource.desc.width);
		framebufferInfo.height = (std::max)(1u, resource.desc.height);
		framebufferInfo.layers = inferFramebufferLayers(resource.desc);

		if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &resource.framebuffer) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateFramebuffer failed");
			return false;
		}
		resource.vkRenderPass = vkPass;
		return true;
	}

	RenderPassHandle VulkanRHI::synthesizeGraphicsRenderPass(const GraphicsPipelineDesc& desc) {
		RenderPassDesc rpDesc;
		const bool depthOnly = desc.depthStencilState.depthTest && desc.depthStencilState.depthWrite
			&& desc.cullMode == CullMode::Front && !desc.blend.enabled;
		if (depthOnly) {
			rpDesc.hasDepthStencil = true;
			rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Clear;
			rpDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
			rpDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT32;
		}
		else {
			rpDesc.colorAttachmentCount = 1;
			rpDesc.colorAttachments[0].loadOp = AttachmentLoadOp::Load;
			rpDesc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
			rpDesc.colorAttachments[0].format = TextureFormat::RGBA8;
			if (desc.depthStencilState.depthTest) {
				rpDesc.hasDepthStencil = true;
				rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Load;
				rpDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
				rpDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT24;
			}
		}
		rpDesc.sampleCount = desc.sampleCount;
		return createRenderPass(rpDesc);
	}

	static std::string spvNameOf(const std::string& name) {
		const auto pos = name.find_last_of("/\\");
		return pos == std::string::npos ? name : name.substr(pos + 1);
	}

	ShaderModuleHandle VulkanRHI::createShaderModule(const ShaderModuleDesc& desc) {
		const std::string spvName = spvNameOf(desc.name);
		if (const auto it = m_shaderByName.find(spvName); it != m_shaderByName.end()) {
			if (auto* shared = getLive(m_shaders, it->second.id)) {
				++shared->refs;
				return it->second;
			}
		}
		const auto code = compileShader(desc.stage, desc.name, desc.source);
		if (code.empty()) {
			RhiLog::fatal("VulkanRHI: shader %s did not compile", desc.name.c_str());
			return {};
		}
		VkShaderModuleResource resource{};
		resource.module = createVkShaderModule(m_device, code);
		resource.desc = desc;
		resource.refs = 1;
		resource.live = true;

		ShaderModuleHandle handle{};
		handle.id = storeResource(m_shaders, m_freeShaders, std::move(resource));
		m_shaderByName[spvName] = handle;
		return handle;
	}

	void VulkanRHI::destroyShaderModule(const ShaderModuleHandle handle) {
		auto* shader = getLive(m_shaders, handle.id);
		if (!shader || --shader->refs > 0) {
			return;
		}
		if (const auto it = m_shaderByName.find(spvNameOf(shader->desc.name)); it != m_shaderByName.end() && it->second.id == handle.id) {
			m_shaderByName.erase(it);
		}
		if (shader->module != VK_NULL_HANDLE) {
			vkDestroyShaderModule(m_device, shader->module, nullptr);
		}
		recycleResource(m_shaders, m_freeShaders, handle.id);
	}

	RenderPassHandle VulkanRHI::createRenderPass(const RenderPassDesc& desc) {
		std::vector<VkAttachmentDescription> attachments;
		std::vector<VkAttachmentReference> colorRefs;
		attachments.reserve(desc.colorAttachmentCount + (desc.hasDepthStencil ? 1u : 0u));
		colorRefs.reserve(desc.colorAttachmentCount);

		for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
			const auto& src = desc.colorAttachments[i];
			VkAttachmentDescription attachment{};
			attachment.format = toVkFormat(src.format);
			attachment.samples = toVkSampleCount(desc.sampleCount);
			attachment.loadOp = toVkLoadOp(src.loadOp);
			attachment.storeOp = toVkStoreOp(src.storeOp);
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = src.loadOp == AttachmentLoadOp::Load
				? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_UNDEFINED;
			attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			attachments.push_back(attachment);

			VkAttachmentReference ref{};
			ref.attachment = i;
			ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorRefs.push_back(ref);
		}

		VkAttachmentReference depthRef{};
		const VkAttachmentReference* depthRefPtr = nullptr;
		if (desc.hasDepthStencil) {
			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = toVkFormat(desc.depthAttachment.format);
			depthAttachment.samples = toVkSampleCount(desc.sampleCount);
			depthAttachment.loadOp = toVkLoadOp(desc.depthAttachment.loadOp);
			depthAttachment.storeOp = toVkStoreOp(desc.depthAttachment.storeOp);
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = desc.depthAttachment.loadOp == AttachmentLoadOp::Load
				? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			depthRef.attachment = desc.colorAttachmentCount;
			depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments.push_back(depthAttachment);
			depthRefPtr = &depthRef;
		}

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
		subpass.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
		subpass.pDepthStencilAttachment = depthRefPtr;

		std::array<VkSubpassDependency, 2> dependencies{};
		// Same dependency masks for Load and Clear so overlay RPs (Load on Final)
		// stay compatible with the framebuffer created from the Clear pass.
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassInfo.pDependencies = dependencies.data();

		VkRenderPassResource resource{};
		resource.desc = desc;
		resource.live = true;
		if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &resource.renderPass) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: createRenderPass failed");
		}

		RenderPassHandle handle{};
		handle.id = storeResource(m_renderPasses, m_freeRenderPasses, std::move(resource));
		return handle;
	}

	void VulkanRHI::destroyRenderPass(const RenderPassHandle renderPass) {
		if (auto* pass = getLive(m_renderPasses, renderPass.id)) {
			if (pass->renderPass != VK_NULL_HANDLE) {
				vkDestroyRenderPass(m_device, pass->renderPass, nullptr);
			}
			recycleResource(m_renderPasses, m_freeRenderPasses, renderPass.id);
		}
	}

	FramebufferHandle VulkanRHI::createFramebuffer(const FramebufferDesc& desc) {
		VkFramebufferResource resource{};
		resource.desc = desc;
		resource.desc.layers = inferFramebufferLayers(desc);
		resource.live = true;

		if (const auto* pass = getLive(m_renderPasses, desc.renderPass.id)) {
			if (!realizeFramebuffer(resource, pass->renderPass)) {
				return {};
			}
		}

		FramebufferHandle handle{};
		handle.id = storeResource(m_framebuffers, m_freeFramebuffers, std::move(resource));
		return handle;
	}

	void VulkanRHI::destroyFramebuffer(const FramebufferHandle framebuffer) {
		if (auto* fb = getLive(m_framebuffers, framebuffer.id)) {
			if (fb->framebuffer != VK_NULL_HANDLE) {
				vkDestroyFramebuffer(m_device, fb->framebuffer, nullptr);
			}
			for (VkImageView view : fb->ownedViews) {
				vkDestroyImageView(m_device, view, nullptr);
			}
			fb->ownedViews.clear();
			if (fb->ownedRenderPass) {
				destroyRenderPass(fb->ownedRenderPass);
			}
			recycleResource(m_framebuffers, m_freeFramebuffers, framebuffer.id);
		}
	}

	FramebufferHandle VulkanRHI::getBackbufferFramebuffer() {
		return m_backbufferHandle;
	}

	PipelineHandle VulkanRHI::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
		VkGraphicsPipelineResource resource{};
		if (!buildGraphicsPipeline(desc, resource)) {
			return {};
		}
		PipelineHandle handle{};
		handle.id = storeResource(m_pipelines, m_freePipelines, std::move(resource));
		return handle;
	}

	bool VulkanRHI::buildGraphicsPipeline(const GraphicsPipelineDesc& desc, VkGraphicsPipelineResource& out) {
		const auto* vert = getLive(m_shaders, desc.shaders.vertex.id);
		const auto* frag = getLive(m_shaders, desc.shaders.fragment.id);
		const auto* geom = desc.shaders.geometry.id != UINT32_MAX ? getLive(m_shaders, desc.shaders.geometry.id) : nullptr;
		const auto* tessCtrl = desc.shaders.tessControl.id != UINT32_MAX ? getLive(m_shaders, desc.shaders.tessControl.id) : nullptr;
		const auto* tessEval = desc.shaders.tessEval.id != UINT32_MAX ? getLive(m_shaders, desc.shaders.tessEval.id) : nullptr;
		if (!vert || !frag) {
			RhiLog::fatal("VulkanRHI: createGraphicsPipeline requires vertex/fragment shaders");
			return false;
		}

		GraphicsPipelineDesc local = desc;
		RenderPassHandle ownedRp{};
		const auto* renderPass = getLive(m_renderPasses, local.renderPass.id);
		if (!renderPass) {
			ownedRp = synthesizeGraphicsRenderPass(local);
			local.renderPass = ownedRp;
			renderPass = getLive(m_renderPasses, local.renderPass.id);
		}
		if (!renderPass) {
			RhiLog::fatal("VulkanRHI: createGraphicsPipeline requires renderPass");
			return false;
		}

		std::vector<VkDescriptorSetLayout> setLayouts;
		expandSetLayouts(local.descriptorSetLayouts, setLayouts);

		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		{
			VkPushConstantRange pushRange{};
			if (desc.pushConstantSize > 0) {
				pushRange.stageFlags = toVkShaderStage(desc.pushConstantStages);
				pushRange.offset = 0;
				pushRange.size = desc.pushConstantSize;
			}

			VkPipelineLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
			layoutInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
			layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
			layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
			if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
				if (ownedRp) {
					destroyRenderPass(ownedRp);
				}
				RhiLog::fatal("VulkanRHI: pipeline layout creation failed");
				return false;
			}
		}

		std::vector<VkVertexInputBindingDescription> bindings(desc.layout.bindings.size());
		for (size_t i = 0; i < desc.layout.bindings.size(); ++i) {
			bindings[i].binding = desc.layout.bindings[i].binding;
			bindings[i].stride = desc.layout.bindings[i].stride;
			bindings[i].inputRate = desc.layout.bindings[i].inputRate == VertexInputRate::PerInstance
				? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
		}

		std::vector<VkVertexInputAttributeDescription> attributes(desc.layout.attributes.size());
		for (size_t i = 0; i < desc.layout.attributes.size(); ++i) {
			const auto& attr = desc.layout.attributes[i];
			attributes[i].location = attr.location;
			attributes[i].binding = attr.binding;
			attributes[i].format = vertexFormatToVk(attr.format);
			attributes[i].offset = attr.offset;
		}

		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
		vertexInput.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
		vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
		vertexInput.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		const bool useTess = tessEval != nullptr;
		inputAssembly.topology = useTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : toVkTopology(desc.topology);

		VkPipelineTessellationStateCreateInfo tessellation{};
		tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
		tessellation.patchControlPoints = (std::max)(1u, desc.patchControlPoints);

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		VkPipelineViewportDepthClipControlCreateInfoEXT clipControl{};
		if (m_negativeOneToOneDepth) {
			clipControl.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
			clipControl.negativeOneToOne = VK_TRUE;
			viewportState.pNext = &clipControl;
		}

		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = toVkPolygonMode(desc.fillMode);
		rasterizer.cullMode = toVkCullMode(desc.cullMode);
		rasterizer.frontFace = toVkFrontFace(desc.frontFace);
		rasterizer.lineWidth = 1.f;

		VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisampling.rasterizationSamples = toVkSampleCount(renderPass->desc.sampleCount);

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = toVkBool(desc.depthStencilState.depthTest);
		depthStencil.depthWriteEnable = toVkBool(desc.depthStencilState.depthWrite);
		depthStencil.depthCompareOp = toVkCompareOp(desc.depthStencilState.depthCompareOp);

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable = toVkBool(desc.blend.enabled);
		blendAttachment.srcColorBlendFactor = toVkBlendFactor(desc.blend.srcColor);
		blendAttachment.dstColorBlendFactor = toVkBlendFactor(desc.blend.dstColor);
		blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = toVkBlendFactor(desc.blend.srcAlpha.value_or(desc.blend.srcColor));
		blendAttachment.dstAlphaBlendFactor = toVkBlendFactor(desc.blend.dstAlpha.value_or(desc.blend.dstColor));
		blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.colorWriteMask = desc.colorWrite
			? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT : 0;

		// Must match the render pass subpass (0 for depth-only CSM). Forcing 1 breaks
		// vkCreateGraphicsPipelines / produces a pipeline that cannot draw into layered depth FBOs.
		const uint32_t colorAttachmentCount = renderPass->desc.colorAttachmentCount;
		std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
			colorAttachmentCount, blendAttachment);

		VkPipelineColorBlendStateCreateInfo colorBlending{};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.attachmentCount = colorAttachmentCount;
		colorBlending.pAttachments = colorAttachmentCount ? blendAttachments.data() : nullptr;

		const std::array dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		std::vector<VkPipelineShaderStageCreateInfo> stages;
		stages.reserve(5u);
		{
			VkPipelineShaderStageCreateInfo stage{};
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
			stage.module = vert->module;
			stage.pName = "main";
			stages.push_back(stage);
		}
		if (tessCtrl) {
			VkPipelineShaderStageCreateInfo stage{};
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			stage.module = tessCtrl->module;
			stage.pName = "main";
			stages.push_back(stage);
		}
		if (tessEval) {
			VkPipelineShaderStageCreateInfo stage{};
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			stage.module = tessEval->module;
			stage.pName = "main";
			stages.push_back(stage);
		}
		if (geom) {
			VkPipelineShaderStageCreateInfo stage{};
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
			stage.module = geom->module;
			stage.pName = "main";
			stages.push_back(stage);
		}
		{
			VkPipelineShaderStageCreateInfo stage{};
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stage.module = frag->module;
			stage.pName = "main";
			stages.push_back(stage);
		}

		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
		pipelineInfo.pStages = stages.data();
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pTessellationState = useTess ? &tessellation : nullptr;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = pipelineLayout;
		pipelineInfo.renderPass = renderPass->renderPass;
		pipelineInfo.subpass = desc.subpassIndex;

		out = {};
		out.desc = local;
		out.label = pipelineLabel(vert->desc.name, frag->desc.name);
		out.ownedRenderPass = ownedRp;
		out.layout = pipelineLayout;
		out.live = true;
		if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &out.pipeline) != VK_SUCCESS) {
			vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
			if (ownedRp) {
				destroyRenderPass(ownedRp);
			}
			RhiLog::fatal("VulkanRHI: vkCreateGraphicsPipelines failed for %s", vert->desc.name.c_str());
			return false;
		}
		return true;
	}

	PipelineHandle VulkanRHI::createComputePipeline(const ComputePipelineDesc& desc) {
		VkGraphicsPipelineResource resource{};
		if (!buildComputePipeline(desc, resource)) {
			return {};
		}
		PipelineHandle handle{};
		handle.id = storeResource(m_pipelines, m_freePipelines, std::move(resource));
		return handle;
	}

	bool VulkanRHI::buildComputePipeline(const ComputePipelineDesc& desc, VkGraphicsPipelineResource& out) {
		const auto* compute = getLive(m_shaders, desc.compute.id);
		if (!compute) {
			RhiLog::fatal("VulkanRHI: createComputePipeline requires compute shader");
			return false;
		}

		std::vector<VkDescriptorSetLayout> setLayouts;
		expandSetLayouts(desc.descriptorSetLayouts, setLayouts);

		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		{
			const VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.pushConstantSize };
			VkPipelineLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
			layoutInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
			layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
			layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
			if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: compute pipeline layout creation failed");
				return false;
			}
		}

		VkPipelineShaderStageCreateInfo stage{};
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = compute->module;
		stage.pName = "main";

		VkComputePipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipelineInfo.stage = stage;
		pipelineInfo.layout = pipelineLayout;

		out = {};
		out.computeDesc = desc;
		out.label = pipelineLabel(compute->desc.name);
		out.layout = pipelineLayout;
		out.isCompute = true;
		out.live = true;
		if (vkCreateComputePipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &out.pipeline) != VK_SUCCESS) {
			vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
			RhiLog::fatal("VulkanRHI: vkCreateComputePipelines failed");
			return false;
		}
		return true;
	}

	size_t VulkanRHI::reloadShaders() {
		const RhiHost* h = host();
		if (!h) {
			return 0;
		}
		// The modules whose text has moved on, compiled again; one that fails keeps what it had.
		std::vector<std::pair<uint32_t, VkShaderModule>> fresh;
		for (uint32_t id = 0; id < m_shaders.size(); ++id) {
			auto& shader = m_shaders[id];
			if (!shader.live || shader.desc.name.empty()) {
				continue;
			}
			auto source = h->loadShaderSource(shader.desc.name);
			if (source.empty() || source == shader.desc.source) {
				continue;
			}
			const auto code = compileShader(shader.desc.stage, shader.desc.name, source);
			if (code.empty()) {
				RhiLog::error("VulkanRHI: %s does not compile; the old one stays", shader.desc.name.c_str());
				continue;
			}
			fresh.emplace_back(id, createVkShaderModule(m_device, code));
			shader.desc.source = std::move(source);
		}
		if (fresh.empty()) {
			return 0;
		}
		vkDeviceWaitIdle(m_device);
		std::vector<uint32_t> remade;
		for (const auto& [id, module] : fresh) {
			vkDestroyShaderModule(m_device, m_shaders[id].module, nullptr);
			m_shaders[id].module = module;
			remade.push_back(id);
		}
		const auto uses = [&](const ShaderModuleHandle handle) {
			return handle.id != UINT32_MAX && std::find(remade.begin(), remade.end(), handle.id) != remade.end();
		};
		// Every pipeline made with one of them, made again from its own desc; its render pass (its own, if it made one)
		// is the one it had.
		for (auto& pipeline : m_pipelines) {
			if (!pipeline.live) {
				continue;
			}
			const auto& shaders = pipeline.desc.shaders;
			const bool affected = pipeline.isCompute ? uses(pipeline.computeDesc.compute)
				: uses(shaders.vertex) || uses(shaders.fragment) || uses(shaders.geometry) || uses(shaders.tessControl) || uses(shaders.tessEval);
			if (!affected) {
				continue;
			}
			VkGraphicsPipelineResource rebuilt{};
			if (!(pipeline.isCompute ? buildComputePipeline(pipeline.computeDesc, rebuilt) : buildGraphicsPipeline(pipeline.desc, rebuilt))) {
				continue;
			}
			vkDestroyPipeline(m_device, pipeline.pipeline, nullptr);
			vkDestroyPipelineLayout(m_device, pipeline.layout, nullptr);
			pipeline.pipeline = rebuilt.pipeline;
			pipeline.layout = rebuilt.layout;
		}
		return remade.size();
	}

	void VulkanRHI::destroyGraphicsPipeline(const PipelineHandle pipeline) {
		if (auto* gp = getLive(m_pipelines, pipeline.id)) {
			if (gp->pipeline != VK_NULL_HANDLE) {
				vkDestroyPipeline(m_device, gp->pipeline, nullptr);
			}
			if (gp->layout != VK_NULL_HANDLE) {
				vkDestroyPipelineLayout(m_device, gp->layout, nullptr);
			}
			if (gp->ownedRenderPass) {
				destroyRenderPass(gp->ownedRenderPass);
			}
			recycleResource(m_pipelines, m_freePipelines, pipeline.id);
		}
	}

	void VulkanRHI::cmdBeginRenderPass(const CommandListHandle cmd, const RenderPassBeginInfo& beginInfo) {
		auto* list = getLive(m_commandLists, cmd.id);
		const auto* pass = getLive(m_renderPasses, beginInfo.renderPass.id);
		auto* fb = getLive(m_framebuffers, beginInfo.framebuffer.id);
		if (!list) {
			return;
		}
		if (!pass || !fb || fb->swapchainTarget) {
			RhiLog::error("VulkanRHI: cmdBeginRenderPass rejected: %s",
				fb && fb->swapchainTarget ? "the backbuffer cannot be a render pass target" : "invalid render pass or framebuffer");
			return;
		}
		if (!fb->framebuffer && !realizeFramebuffer(*fb, pass->renderPass)) {
			return;
		}

		for (uint32_t i = 0; i < fb->desc.colorAttachments.size(); ++i) {
			if (auto* tex = getLive(m_textures, fb->desc.colorAttachments[i].texture.id)) {
				if (i < pass->desc.colorAttachmentCount && pass->desc.colorAttachments[i].loadOp == AttachmentLoadOp::Load) {
					ensureTextureLayout(list->cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				}
			}
		}
		if (fb->desc.depthStencilAttachment.has_value()) {
			if (auto* tex = getLive(m_textures, fb->desc.depthStencilAttachment->texture.id)) {
				if (pass->desc.hasDepthStencil && pass->desc.depthAttachment.loadOp == AttachmentLoadOp::Load) {
					ensureTextureLayout(list->cmd, *tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				}
			}
		}
		list->activeFramebuffer = beginInfo.framebuffer;
		list->passWidth = beginInfo.width;
		list->passHeight = beginInfo.height;

		std::array<VkClearValue, std::size(RenderPassDesc{}.colorAttachments) + 1> clearValues{};
		uint32_t clearCount = 0;
		for (uint32_t i = 0; i < pass->desc.colorAttachmentCount && clearCount < clearValues.size(); ++i) {
			const RhiVec4 color = beginInfo.overrideClearColors ? beginInfo.overrideClearColors[i] : pass->desc.colorAttachments[i].clearColor;
			clearValues[clearCount++].color = { { color.x, color.y, color.z, color.w } };
		}
		if (pass->desc.hasDepthStencil) {
			auto& value = clearValues[clearCount++];
			value.depthStencil.depth = beginInfo.overrideClearDepth ? *beginInfo.overrideClearDepth : pass->desc.depthAttachment.clearDepth;
			value.depthStencil.stencil = beginInfo.overrideClearStencil ? *beginInfo.overrideClearStencil : pass->desc.depthAttachment.clearStencil;
		}

		VkRenderPassBeginInfo rpBegin{};
		rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rpBegin.renderPass = pass->renderPass;
		rpBegin.framebuffer = fb->framebuffer;
		rpBegin.renderArea.offset = { beginInfo.x, beginInfo.y };
		rpBegin.renderArea.extent = {
			static_cast<uint32_t>((std::max)(1, beginInfo.width)),
			static_cast<uint32_t>((std::max)(1, beginInfo.height))
		};
		rpBegin.clearValueCount = clearCount;
		rpBegin.pClearValues = clearCount ? clearValues.data() : nullptr;
		vkCmdBeginRenderPass(list->cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{};
		viewport.x = static_cast<float>(beginInfo.x);
		viewport.y = static_cast<float>(beginInfo.y);
		viewport.width = static_cast<float>((std::max)(1, beginInfo.width));
		viewport.height = static_cast<float>((std::max)(1, beginInfo.height));
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		VkRect2D scissor{
			{ beginInfo.x, beginInfo.y },
			{ static_cast<uint32_t>((std::max)(1, beginInfo.width)), static_cast<uint32_t>((std::max)(1, beginInfo.height)) }
		};
		vkCmdSetViewport(list->cmd, 0, 1, &viewport);
		vkCmdSetScissor(list->cmd, 0, 1, &scissor);
	}

	void VulkanRHI::cmdSetScissor(const CommandListHandle cmd, const int32_t x, const int32_t y, const uint32_t width, const uint32_t height) {
		auto* list = getLive(m_commandLists, cmd.id);
		if (!list || !list->recording || list->cmd == VK_NULL_HANDLE) {
			return;
		}
		const int fbW = list->passWidth > 0 ? list->passWidth : 1;
		const int fbH = list->passHeight > 0 ? list->passHeight : 1;
		// Same Y flip as OpenGLRHI: clip is ImGui top-left, RT is +Y-up.
		int x0 = x;
		int y0 = fbH - y - static_cast<int>(height);
		int x1 = x0 + static_cast<int>(width);
		int y1 = y0 + static_cast<int>(height);
		x0 = (std::max)(0, x0);
		y0 = (std::max)(0, y0);
		x1 = (std::min)(fbW, x1);
		y1 = (std::min)(fbH, y1);
		VkRect2D scissor{};
		if (x1 > x0 && y1 > y0) {
			scissor.offset.x = x0;
			scissor.offset.y = y0;
			scissor.extent.width = static_cast<uint32_t>(x1 - x0);
			scissor.extent.height = static_cast<uint32_t>(y1 - y0);
		} else {
			// An empty rectangle rejects everything, which is what OpenGLRHI does for the same
			// input. A 1x1 fallback here used to let the origin pixel through, so a scissor the
			// caller meant as "draw nothing" drew one pixel on Vulkan and none on OpenGL.
			scissor.offset = { 0, 0 };
			scissor.extent = { 0, 0 };
		}
		vkCmdSetScissor(list->cmd, 0, 1, &scissor);
	}

	void VulkanRHI::cmdEndRenderPass(const CommandListHandle cmd) {
		auto* list = getLive(m_commandLists, cmd.id);
		if (!list || !list->recording || !list->activeFramebuffer) {
			return;
		}
		vkCmdEndRenderPass(list->cmd);

		// Match createRenderPass finalLayout (SHADER_READ_ONLY) without extra barriers —
		// the render pass itself performed the transition.
		if (auto* fb = getLive(m_framebuffers, list->activeFramebuffer.id)) {
			for (const auto& color : fb->desc.colorAttachments) {
				if (auto* tex = getLive(m_textures, color.texture.id)) {
					tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			}
			if (fb->desc.depthStencilAttachment.has_value()) {
				if (auto* tex = getLive(m_textures, fb->desc.depthStencilAttachment->texture.id)) {
					tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			}
		}
		list->activeFramebuffer = {};
		list->passWidth = 0;
		list->passHeight = 0;
	}

	void VulkanRHI::cmdBindPipeline(const CommandListHandle cmd, const PipelineHandle pipeline) {
		auto* list = getLive(m_commandLists, cmd.id);
		const auto* gp = getLive(m_pipelines, pipeline.id);
		if (!list || !gp) {
			return;
		}
		pipelineBound(cmd, gp->label);
		list->boundLayout = gp->layout;
		list->boundBindPoint = gp->isCompute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
		vkCmdBindPipeline(list->cmd, list->boundBindPoint, gp->pipeline);
	}

	void VulkanRHI::cmdBindVertexBuffer(const CommandListHandle cmd, const uint32_t slot, const BufferHandle handle, const uint32_t offset) {
		const auto* list = getLive(m_commandLists, cmd.id);
		const auto* buffer = getLive(m_buffers, handle.id);
		if (!list || !buffer) {
			return;
		}
		const VkDeviceSize offsets[] = { offset };
		vkCmdBindVertexBuffers(list->cmd, slot, 1, &buffer->buffer, offsets);
	}

	void VulkanRHI::cmdBindIndexBuffer(const CommandListHandle cmd, const BufferHandle handle, const IndexType type, const uint32_t offset) {
		const auto* list = getLive(m_commandLists, cmd.id);
		const auto* buffer = getLive(m_buffers, handle.id);
		if (!list || !buffer) {
			return;
		}
		vkCmdBindIndexBuffer(list->cmd, buffer->buffer, offset,
			type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	}

	void VulkanRHI::cmdBindDescriptorSets(const CommandListHandle cmd, const PipelineHandle pipeline, const uint32_t firstSet,
		const DescriptorSetHandle* sets, const uint32_t setCount, const uint32_t* dynamicOffsets, const uint32_t dynamicOffsetCount)
	{
		auto* list = getLive(m_commandLists, cmd.id);
		if (!list || !sets || setCount == 0) {
			return;
		}

		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipelineBindPoint bindPoint = list->boundBindPoint;
		if (const auto* gp = getLive(m_pipelines, pipeline.id)) {
			layout = gp->layout;
			bindPoint = gp->isCompute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
		}
		else {
			layout = list->boundLayout;
		}
		if (layout == VK_NULL_HANDLE) {
			return;
		}

		std::array<VkDescriptorSet, 8> vkSets{};
		if (setCount > vkSets.size()) {
			RhiLog::error("cmdBindDescriptorSets: %u sets, at most %zu", setCount, vkSets.size());
			return;
		}
		for (uint32_t i = 0; i < setCount; ++i) {
			const auto* set = getLive(m_descriptorSets, sets[i].id);
			if (!set) {
				return;
			}
			vkSets[i] = set->set;
		}
		if (layout == VK_NULL_HANDLE || list->cmd == VK_NULL_HANDLE || !list->recording) {
			return;
		}
		vkCmdBindDescriptorSets(list->cmd, bindPoint, layout, firstSet,
			setCount, vkSets.data(), dynamicOffsets ? dynamicOffsetCount : 0, dynamicOffsets);
		list->boundSets.insert(list->boundSets.end(), vkSets.begin(), vkSets.begin() + setCount);
	}

	void VulkanRHI::cmdPushConstants(const CommandListHandle cmd, const PipelineHandle pipeline, const ShaderStage stages, const void* data, const uint32_t size) {
		auto* list = getLive(m_commandLists, cmd.id);
		const auto* gp = getLive(m_pipelines, pipeline.id);
		if (!list || !gp || !data || size == 0 || !list->recording || gp->layout == VK_NULL_HANDLE) {
			return;
		}
		vkCmdPushConstants(list->cmd, gp->layout, toVkShaderStage(stages), 0, size, data);
	}
	VkRecordedCommandList* VulkanRHI::listInPass(const CommandListHandle cmd, const char* command) {
		auto* list = getLive(m_commandLists, cmd.id);
		if (!list || !list->recording) {
			return nullptr;
		}
		if (!list->activeFramebuffer) {
			// A draw recorded outside a render pass faults in the driver instead of failing validation.
			RhiLog::error("VulkanRHI: %s outside a render pass, dropped", command);
			return nullptr;
		}
		return list;
	}

	void VulkanRHI::cmdDraw(const CommandListHandle cmd, const uint32_t vertexCount, const uint32_t instanceCount, const uint32_t firstVertex, const uint32_t firstInstance) {
		if (auto* list = listInPass(cmd, "cmdDraw")) {
			vkCmdDraw(list->cmd, vertexCount, instanceCount, firstVertex, firstInstance);
		}
	}
	void VulkanRHI::cmdDrawIndexed(const CommandListHandle cmd, const uint32_t indexCount, const uint32_t instanceCount,
		const uint32_t firstIndex, const int32_t vertexOffset, const uint32_t firstInstance)
	{
		if (auto* list = listInPass(cmd, "cmdDrawIndexed")) {
			vkCmdDrawIndexed(list->cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
		}
	}

	void VulkanRHI::cmdDrawIndirect(const CommandListHandle cmd, const BufferHandle buffer, const size_t offset,
		const uint32_t drawCount, const uint32_t stride)
	{
		auto* list = listInPass(cmd, "cmdDrawIndirect");
		const auto* buf = getLive(m_buffers, buffer.id);
		if (!list || !buf || drawCount == 0) {
			return;
		}
		vkCmdDrawIndirect(list->cmd, buf->buffer, offset, drawCount, stride ? stride : 16u);
	}

	void VulkanRHI::cmdDrawIndexedIndirect(const CommandListHandle cmd, const BufferHandle buffer, const size_t offset,
		const uint32_t drawCount, const uint32_t stride)
	{
		auto* list = listInPass(cmd, "cmdDrawIndexedIndirect");
		const auto* buf = getLive(m_buffers, buffer.id);
		if (!list || !buf || drawCount == 0) {
			return;
		}
		vkCmdDrawIndexedIndirect(list->cmd, buf->buffer, offset, drawCount, stride ? stride : 20u);
	}

	void VulkanRHI::cmdDrawIndexedIndirectCount(const CommandListHandle cmd, const BufferHandle buffer, const size_t offset,
		const BufferHandle countBuffer, const size_t countOffset, const uint32_t maxDrawCount, const uint32_t stride)
	{
		auto* list = listInPass(cmd, "cmdDrawIndexedIndirectCount");
		const auto* buf = getLive(m_buffers, buffer.id);
		const auto* countBuf = getLive(m_buffers, countBuffer.id);
		if (!list || !buf || !countBuf || maxDrawCount == 0) {
			return;
		}
		vkCmdDrawIndexedIndirectCount(list->cmd, buf->buffer, offset, countBuf->buffer, countOffset, maxDrawCount, stride ? stride : 20u);
	}

	void VulkanRHI::cmdDispatch(const CommandListHandle cmd, const uint32_t groupCountX, const uint32_t groupCountY, const uint32_t groupCountZ) {
		if (auto* list = getLive(m_commandLists, cmd.id)) {
			vkCmdDispatch(list->cmd, groupCountX, groupCountY, groupCountZ);
		}
	}
	void VulkanRHI::cmdBlit(const CommandListHandle cmd, const FramebufferHandle src, const FramebufferHandle dst,
		const BlitMask mask, const BlitFilter filter)
	{
		auto* list = getLive(m_commandLists, cmd.id);
		const auto* srcFb = getLive(m_framebuffers, src.id);
		if (!list || !list->recording || !srcFb) {
			return;
		}

		auto* dstFb = dst ? getLive(m_framebuffers, dst.id) : nullptr;
		if (!dstFb || dstFb->swapchainTarget) {
			if ((mask & BlitMask::Color) && !srcFb->desc.colorAttachments.empty()) {
				if (auto* srcTex = getLive(m_textures, srcFb->desc.colorAttachments[0].texture.id)) {
					if (m_swapchainTransferDst) {
						blitToSwapchain(list->cmd, *srcTex);
					}
					else {
						setPresentBlitSource(srcFb->desc.colorAttachments[0].texture, m_presentBlitSampler);
					}
					m_presentBlitTexture = srcFb->desc.colorAttachments[0].texture;
				}
			}
			return;
		}

		const VkFilter vkFilter = filter == BlitFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

		auto blitAttachment = [&](TextureHandle srcTexHandle, TextureHandle dstTexHandle, VkImageAspectFlags aspect) {
			auto* srcTex = getLive(m_textures, srcTexHandle.id);
			auto* dstTex = getLive(m_textures, dstTexHandle.id);
			if (!srcTex || !dstTex) {
				return;
			}
			ensureTextureLayout(list->cmd, *srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			ensureTextureLayout(list->cmd, *dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			VkImageBlit blit{};
			blit.srcSubresource.aspectMask = aspect;
			blit.srcSubresource.mipLevel = 0;
			blit.srcSubresource.baseArrayLayer = 0;
			blit.srcSubresource.layerCount = 1;
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = {
				static_cast<int32_t>((std::max)(1u, srcFb->desc.width)),
				static_cast<int32_t>((std::max)(1u, srcFb->desc.height)),
				1
			};
			blit.dstSubresource.aspectMask = aspect;
			blit.dstSubresource.mipLevel = 0;
			blit.dstSubresource.baseArrayLayer = 0;
			blit.dstSubresource.layerCount = 1;
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = {
				static_cast<int32_t>((std::max)(1u, dstFb->desc.width)),
				static_cast<int32_t>((std::max)(1u, dstFb->desc.height)),
				1
			};

			vkCmdBlitImage(list->cmd,
				srcTex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstTex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &blit, vkFilter);

			ensureTextureLayout(list->cmd, *srcTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			ensureTextureLayout(list->cmd, *dstTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		};

		if ((mask & BlitMask::Color) && !srcFb->desc.colorAttachments.empty() && !dstFb->desc.colorAttachments.empty()) {
			blitAttachment(srcFb->desc.colorAttachments[0].texture, dstFb->desc.colorAttachments[0].texture, VK_IMAGE_ASPECT_COLOR_BIT);
		}
		if ((mask & BlitMask::Depth)
			&& srcFb->desc.depthStencilAttachment.has_value()
			&& dstFb->desc.depthStencilAttachment.has_value()) {
			blitAttachment(srcFb->desc.depthStencilAttachment->texture,
				dstFb->desc.depthStencilAttachment->texture,
				VK_IMAGE_ASPECT_DEPTH_BIT);
		}
	}
}

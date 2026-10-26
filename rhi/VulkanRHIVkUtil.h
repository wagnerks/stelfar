#pragma once

#include <vulkan/vulkan.h>

#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render::VulkanVkUtil {
	inline VkFormat toVkFormat(TextureFormat format) {
		switch (format) {
		case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
		case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
		case TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
		case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
		case TextureFormat::SRGB8_ALPHA8: return VK_FORMAT_R8G8B8A8_SRGB;
		case TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
		case TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
		case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
		case TextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
		case TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case TextureFormat::DEPTH_COMPONENT16: return VK_FORMAT_D16_UNORM;
		case TextureFormat::DEPTH_COMPONENT24: return VK_FORMAT_D32_SFLOAT;
		case TextureFormat::DEPTH_COMPONENT32: return VK_FORMAT_D32_SFLOAT;
		case TextureFormat::DEPTH_COMPONENT32F: return VK_FORMAT_D32_SFLOAT;
		case TextureFormat::DEPTH24_STENCIL8: return VK_FORMAT_D24_UNORM_S8_UINT;
		case TextureFormat::DEPTH32F_STENCIL8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
		case TextureFormat::BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
		case TextureFormat::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
		default: return VK_FORMAT_R8G8B8A8_UNORM;
		}
	}

	inline VkImageAspectFlags aspectMaskForFormat(VkFormat format) {
		switch (format) {
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D32_SFLOAT:
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		default:
			return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	inline VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
		VkBufferUsageFlags flags = 0;
		if (usage & BufferUsage::Vertex) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (usage & BufferUsage::Index) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (usage & BufferUsage::Uniform) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (usage & BufferUsage::Storage) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (usage & BufferUsage::Indirect) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		return flags;
	}

	inline VkMemoryPropertyFlags toVkMemoryProperties(CpuAccess access) {
		switch (access) {
		case CpuAccess::Read:
			return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		case CpuAccess::ReadWrite:
		case CpuAccess::Write:
			return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		default:
			return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		}
	}

	inline VkDescriptorType toVkDescriptorType(DescriptorType type) {
		switch (type) {
		case DescriptorType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case DescriptorType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		case DescriptorType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		case DescriptorType::UniformBufferDynamic: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		case DescriptorType::StorageBufferDynamic: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		default: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		}
	}

	inline VkShaderStageFlags toVkShaderStage(ShaderStage stage) {
		VkShaderStageFlags flags = 0;
		if (stage & ShaderStage::Vertex) flags |= VK_SHADER_STAGE_VERTEX_BIT;
		if (stage & ShaderStage::Fragment) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if (stage & ShaderStage::Geometry) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
		if (stage & ShaderStage::TessControl) flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		if (stage & ShaderStage::TessEval) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		if (stage & ShaderStage::Compute) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
		if (flags == 0) flags = VK_SHADER_STAGE_ALL_GRAPHICS;
		return flags;
	}

	inline VkPipelineStageFlags toVkPipelineStage(PipelineStage stage) {
		VkPipelineStageFlags flags = 0;
		if (stage & PipelineStage::DrawIndirect) flags |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
		if (stage & PipelineStage::VertexInput) flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		if (stage & PipelineStage::VertexShader) flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		if (stage & PipelineStage::FragmentShader) flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if (stage & PipelineStage::ColorAttachmentOutput) flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		if (stage & PipelineStage::LateFragmentTests) flags |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		if (stage & PipelineStage::Transfer) flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		if (stage & PipelineStage::Host) flags |= VK_PIPELINE_STAGE_HOST_BIT;
		if (stage & PipelineStage::Compute) flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (stage & PipelineStage::AllGraphics) flags |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
		if (stage & PipelineStage::AllCommands) flags |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		if (flags == 0) flags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		return flags;
	}

	inline VkAccessFlags toVkAccess(AccessMask access) {
		VkAccessFlags flags = 0;
		if (access & AccessMask::IndirectCommandRead) flags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
		if (access & AccessMask::IndexRead) flags |= VK_ACCESS_INDEX_READ_BIT;
		if (access & AccessMask::VertexAttributeRead) flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
		if (access & AccessMask::UniformRead) flags |= VK_ACCESS_UNIFORM_READ_BIT;
		if (access & AccessMask::ShaderRead) flags |= VK_ACCESS_SHADER_READ_BIT;
		if (access & AccessMask::ShaderWrite) flags |= VK_ACCESS_SHADER_WRITE_BIT;
		if (access & AccessMask::ColorAttachmentRead) flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		if (access & AccessMask::ColorAttachmentWrite) flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		if (access & AccessMask::DepthStencilAttachmentRead) flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		if (access & AccessMask::DepthStencilAttachmentWrite) flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		if (access & AccessMask::TransferRead) flags |= VK_ACCESS_TRANSFER_READ_BIT;
		if (access & AccessMask::TransferWrite) flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
		if (access & AccessMask::HostRead) flags |= VK_ACCESS_HOST_READ_BIT;
		if (access & AccessMask::HostWrite) flags |= VK_ACCESS_HOST_WRITE_BIT;
		return flags;
	}

	inline VkFilter toVkFilter(FilterMode mode) {
		switch (mode) {
		case FilterMode::Nearest: return VK_FILTER_NEAREST;
		case FilterMode::LinearMipmapLinear:
		case FilterMode::Linear:
		default:
			return VK_FILTER_LINEAR;
		}
	}

	inline VkSamplerAddressMode toVkAddressMode(AddressMode mode) {
		switch (mode) {
		case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		case AddressMode::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		case AddressMode::MirrorClampToEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		case AddressMode::ClampToEdge:
		default:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		}
	}

	inline VkCompareOp toVkCompareOp(CompareOp op) {
		switch (op) {
		case CompareOp::Less: return VK_COMPARE_OP_LESS;
		case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
		case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
		case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
		case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
		case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
		case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
		case CompareOp::Never: return VK_COMPARE_OP_NEVER;
		default: return VK_COMPARE_OP_ALWAYS;
		}
	}

	inline VkImageUsageFlags toVkImageUsage(TextureUsage usage) {
		VkImageUsageFlags flags = 0;
		if (usage & TextureUsage::Sampled) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (usage & TextureUsage::ColorAttachment) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (usage & TextureUsage::DepthStencilAttachment) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		if (usage & TextureUsage::TransferSrc) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (usage & TextureUsage::TransferDst) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (flags == 0) flags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		return flags;
	}

	inline VkBlendFactor toVkBlendFactor(BlendFactor factor) {
		switch (factor) {
		case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
		case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
		case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
		case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		default: return VK_BLEND_FACTOR_ONE;
		}
	}

	inline VkFormat vertexFormatToVk(VertexFormat format) {
		switch (format) {
		case VertexFormat::Float: return VK_FORMAT_R32_SFLOAT;
		case VertexFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
		case VertexFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
		case VertexFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case VertexFormat::Int: return VK_FORMAT_R32_SINT;
		case VertexFormat::Int2: return VK_FORMAT_R32G32_SINT;
		case VertexFormat::Int3: return VK_FORMAT_R32G32B32_SINT;
		case VertexFormat::Int4: return VK_FORMAT_R32G32B32A32_SINT;
		case VertexFormat::UInt: return VK_FORMAT_R32_UINT;
		case VertexFormat::UInt2: return VK_FORMAT_R32G32_UINT;
		case VertexFormat::UInt3: return VK_FORMAT_R32G32B32_UINT;
		case VertexFormat::UInt4: return VK_FORMAT_R32G32B32A32_UINT;
		default: return VK_FORMAT_R32G32B32A32_SFLOAT;
		}
	}

	inline uint32_t vertexFormatComponentCount(VertexFormat format) {
		switch (format) {
		case VertexFormat::Float:
		case VertexFormat::Int:
		case VertexFormat::UInt: return 1;
		case VertexFormat::Float2:
		case VertexFormat::Int2:
		case VertexFormat::UInt2: return 2;
		case VertexFormat::Float3:
		case VertexFormat::Int3:
		case VertexFormat::UInt3: return 3;
		default: return 4;
		}
	}

	inline VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) {
		switch (topology) {
		case PrimitiveTopology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		case PrimitiveTopology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		case PrimitiveTopology::Patches: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
		case PrimitiveTopology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}
	}

	inline VkCullModeFlags toVkCullMode(CullMode mode) {
		switch (mode) {
		case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
		case CullMode::None: return VK_CULL_MODE_NONE;
		default: return VK_CULL_MODE_BACK_BIT;
		}
	}

	inline VkFrontFace toVkFrontFace(FrontFace face) {
		// Viewport is not Y-flipped; invert winding so GL CCW is front on Vulkan.
		return face == FrontFace::ClockWise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
	}

	inline bool isImageDescriptor(DescriptorType type) {
		return type == DescriptorType::CombinedImageSampler;
	}


	inline VkSampleCountFlagBits toVkSampleCount(uint32_t count) {
		if (count >= 8) return VK_SAMPLE_COUNT_8_BIT;
		if (count >= 4) return VK_SAMPLE_COUNT_4_BIT;
		if (count >= 2) return VK_SAMPLE_COUNT_2_BIT;
		return VK_SAMPLE_COUNT_1_BIT;
	}

	inline VkPolygonMode toVkPolygonMode(PolygonFillMode mode) {
		return mode == PolygonFillMode::Line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	}

	inline VkAttachmentLoadOp toVkLoadOp(AttachmentLoadOp op) {
		return op == AttachmentLoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	}

	inline VkAttachmentStoreOp toVkStoreOp(AttachmentStoreOp op) {
		return op == AttachmentStoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
	}

	inline VkBool32 toVkBool(bool value) {
		return value ? VK_TRUE : VK_FALSE;
	}
}

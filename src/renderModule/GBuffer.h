#pragma once

#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The G-buffer's targets as GeometryPass makes them, in order: material (RGBA8), normal, albedo, velocity and
	// emissive (RGBA16F), depth (24 bits). A pass drawing into it after GeometryPass loads them (`load`); every pipeline
	// drawing into it writes all five (sfe_gbuffer_surface.glsl).
	inline RenderPassDesc gBufferPassDesc(const bool load) {
		const auto op = load ? AttachmentLoadOp::Load : AttachmentLoadOp::Clear;
		RenderPassDesc desc;
		desc.colorAttachmentCount = 5;
		desc.colorAttachments[0] = { op, AttachmentStoreOp::Store, { 0.f, 0.f, 0.f, 1.f }, TextureFormat::RGBA8 };
		desc.colorAttachments[1] = { op, AttachmentStoreOp::Store, { 0.f, 0.f, 0.f, 1.f }, TextureFormat::RGBA16F };
		desc.colorAttachments[2] = { op, AttachmentStoreOp::Store, { 0.f, 0.f, 0.f, 1.f }, TextureFormat::RGBA16F };
		desc.colorAttachments[3] = { op, AttachmentStoreOp::Store, { 0.f, 0.f, 0.f, 0.f }, TextureFormat::RGBA16F };
		desc.colorAttachments[4] = { op, AttachmentStoreOp::Store, { 0.f, 0.f, 0.f, 0.f }, TextureFormat::RGBA16F };
		desc.hasDepthStencil = true;
		desc.depthAttachment = { op, AttachmentStoreOp::Store, 1.f, 0, false, TextureFormat::DEPTH_COMPONENT24 };
		return desc;
	}
}

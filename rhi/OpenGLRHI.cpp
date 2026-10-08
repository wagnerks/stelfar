#include <rhi/OpenGLRHI.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include <glad/glad.h> // should be included before GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>


#include "rhi/RhiLog.h"
#include "rhi/RhiReadback.h"

namespace SFE::Render
{
	static GLenum ToGLAttachmentFormat(TextureFormat f)
	{
		switch (f) {
		case TextureFormat::DEPTH_COMPONENT16:
		case TextureFormat::DEPTH_COMPONENT24:
		case TextureFormat::DEPTH_COMPONENT32:
		case TextureFormat::DEPTH_COMPONENT32F:
			return GL_DEPTH_ATTACHMENT;
		case TextureFormat::DEPTH24_STENCIL8:
		case TextureFormat::DEPTH32F_STENCIL8:
			return GL_DEPTH_STENCIL_ATTACHMENT;
		default:
			return GL_COLOR_ATTACHMENT0; // fallback
		}
	}

	static GLint ToGLInternalFormat(TextureFormat f)
	{
		switch (f) {
		case TextureFormat::R8: return GL_R8;
		case TextureFormat::R8_SNORM: return GL_R8_SNORM;
		case TextureFormat::R16: return GL_R16;
		case TextureFormat::R16_SNORM: return GL_R16_SNORM;

		case TextureFormat::RG8: return GL_RG8;
		case TextureFormat::RG8_SNORM: return GL_RG8_SNORM;
		case TextureFormat::RG16: return GL_RG16;
		case TextureFormat::RG16_SNORM: return GL_RG16_SNORM;

		case TextureFormat::R3_G3_B2: return GL_R3_G3_B2;
		case TextureFormat::RGB4: return GL_RGB4;
		case TextureFormat::RGB5: return GL_RGB5;
		case TextureFormat::RGB8: return GL_RGB8;
		case TextureFormat::RGB8_SNORM: return GL_RGB8_SNORM;
		case TextureFormat::RGB10: return GL_RGB10;
		case TextureFormat::RGB12: return GL_RGB12;
		case TextureFormat::RGB16_SNORM: return GL_RGB16_SNORM;

		case TextureFormat::RGBA2: return GL_RGBA2;
		case TextureFormat::RGBA4: return GL_RGBA4;
		case TextureFormat::RGB5_A1: return GL_RGB5_A1;
		case TextureFormat::RGBA8: return GL_RGBA8;
		case TextureFormat::RGBA8_SNORM: return GL_RGBA8_SNORM;
		case TextureFormat::RGB10_A2: return GL_RGB10_A2;
		case TextureFormat::RGB10_A2UI: return GL_RGB10_A2UI;
		case TextureFormat::RGBA12: return GL_RGBA12;
		case TextureFormat::RGBA16: return GL_RGBA16;

		case TextureFormat::SRGB8: return GL_SRGB8;
		case TextureFormat::SRGB8_ALPHA8: return GL_SRGB8_ALPHA8;

		case TextureFormat::R16F: return GL_R16F;
		case TextureFormat::RG16F: return GL_RG16F;
		case TextureFormat::RGB16F: return GL_RGB16F;
		case TextureFormat::RGBA16F: return GL_RGBA16F;

		case TextureFormat::R32F: return GL_R32F;
		case TextureFormat::RG32F: return GL_RG32F;
		case TextureFormat::RGB32F: return GL_RGB32F;
		case TextureFormat::RGBA32F: return GL_RGBA32F;

		case TextureFormat::R11F_G11F_B10F: return GL_R11F_G11F_B10F;
		case TextureFormat::RGB9_E5: return GL_RGB9_E5;

		case TextureFormat::R8I: return GL_R8I;
		case TextureFormat::R8UI: return GL_R8UI;
		case TextureFormat::R16I: return GL_R16I;
		case TextureFormat::R16UI: return GL_R16UI;
		case TextureFormat::R32I: return GL_R32I;
		case TextureFormat::R32UI: return GL_R32UI;

		case TextureFormat::RG8I: return GL_RG8I;
		case TextureFormat::RG8UI: return GL_RG8UI;
		case TextureFormat::RG16I: return GL_RG16I;
		case TextureFormat::RG16UI: return GL_RG16UI;
		case TextureFormat::RG32I: return GL_RG32I;
		case TextureFormat::RG32UI: return GL_RG32UI;

		case TextureFormat::RGB8I: return GL_RGB8I;
		case TextureFormat::RGB8UI: return GL_RGB8UI;
		case TextureFormat::RGB16I: return GL_RGB16I;
		case TextureFormat::RGB16UI: return GL_RGB16UI;
		case TextureFormat::RGB32I: return GL_RGB32I;
		case TextureFormat::RGB32UI: return GL_RGB32UI;

		case TextureFormat::RGBA8I: return GL_RGBA8I;
		case TextureFormat::RGBA8UI: return GL_RGBA8UI;
		case TextureFormat::RGBA16I: return GL_RGBA16I;
		case TextureFormat::RGBA16UI: return GL_RGBA16UI;
		case TextureFormat::RGBA32I: return GL_RGBA32I;
		case TextureFormat::RGBA32UI: return GL_RGBA32UI;

		case TextureFormat::DEPTH_COMPONENT16: return GL_DEPTH_COMPONENT16;
		// Float depth, as Vulkan: it has no 32-bit unorm depth and maps both to D32_SFLOAT. Integer depth
		// here resolved different depth differences than Vulkan, so shadow and z-tests disagreed.
		case TextureFormat::DEPTH_COMPONENT24: return GL_DEPTH_COMPONENT32F;
		case TextureFormat::DEPTH_COMPONENT32: return GL_DEPTH_COMPONENT32F;
		case TextureFormat::DEPTH_COMPONENT32F: return GL_DEPTH_COMPONENT32F;
		case TextureFormat::DEPTH24_STENCIL8:	return GL_DEPTH24_STENCIL8;
		case TextureFormat::DEPTH32F_STENCIL8:	return GL_DEPTH32F_STENCIL8;

		default:
			return GL_RGBA8;
		}
	}

	static GLenum ToGLFormat(TextureFormat f)
	{
		switch (f) {
		case TextureFormat::R8:
		case TextureFormat::R8_SNORM:
		case TextureFormat::R16:
		case TextureFormat::R16_SNORM:
		case TextureFormat::R16F:
		case TextureFormat::R32F:
		case TextureFormat::R8I:
		case TextureFormat::R8UI:
		case TextureFormat::R16I:
		case TextureFormat::R16UI:
		case TextureFormat::R32I:
		case TextureFormat::R32UI:
			return GL_RED;

		case TextureFormat::RG8:
		case TextureFormat::RG8_SNORM:
		case TextureFormat::RG16:
		case TextureFormat::RG16_SNORM:
		case TextureFormat::RG16F:
		case TextureFormat::RG32F:
		case TextureFormat::RG8I:
		case TextureFormat::RG8UI:
		case TextureFormat::RG16I:
		case TextureFormat::RG16UI:
		case TextureFormat::RG32I:
		case TextureFormat::RG32UI:
			return GL_RG;

		case TextureFormat::R3_G3_B2:
		case TextureFormat::RGB4:
		case TextureFormat::RGB5:
		case TextureFormat::RGB8:
		case TextureFormat::RGB8_SNORM:
		case TextureFormat::RGB10:
		case TextureFormat::RGB12:
		case TextureFormat::RGB16_SNORM:
		case TextureFormat::SRGB8:
		case TextureFormat::RGB16F:
		case TextureFormat::RGB32F:
		case TextureFormat::R11F_G11F_B10F:
		case TextureFormat::RGB9_E5:
		case TextureFormat::RGB8I:
		case TextureFormat::RGB8UI:
		case TextureFormat::RGB16I:
		case TextureFormat::RGB16UI:
		case TextureFormat::RGB32I:
		case TextureFormat::RGB32UI:
			return GL_RGB;

		case TextureFormat::RGBA2:
		case TextureFormat::RGBA4:
		case TextureFormat::RGB5_A1:
		case TextureFormat::RGBA8:
		case TextureFormat::RGBA8_SNORM:
		case TextureFormat::RGB10_A2:
		case TextureFormat::SRGB8_ALPHA8:
		case TextureFormat::RGBA12:
		case TextureFormat::RGBA16:
		case TextureFormat::RGBA16F:
		case TextureFormat::RGBA32F:
		case TextureFormat::RGBA8I:
		case TextureFormat::RGBA8UI:
		case TextureFormat::RGBA16I:
		case TextureFormat::RGBA16UI:
		case TextureFormat::RGBA32I:
		case TextureFormat::RGBA32UI:
			return GL_RGBA;

		case TextureFormat::DEPTH_COMPONENT16:
		case TextureFormat::DEPTH_COMPONENT24:
		case TextureFormat::DEPTH_COMPONENT32:
		case TextureFormat::DEPTH_COMPONENT32F:
			return GL_DEPTH_COMPONENT;

		case TextureFormat::DEPTH24_STENCIL8:
		case TextureFormat::DEPTH32F_STENCIL8:
			return GL_DEPTH_STENCIL;
		default:
			return GL_RGBA;
		}
	}

	static GLenum ToGLType(TextureFormat f)
	{
		switch (f) {

			// unsigned normalized
		case TextureFormat::R8:
		case TextureFormat::RG8:
		case TextureFormat::RGB8:
		case TextureFormat::RGBA8:
			return GL_UNSIGNED_BYTE;

			// signed normalized
		case TextureFormat::R8_SNORM:
		case TextureFormat::RG8_SNORM:
		case TextureFormat::RGB8_SNORM:
		case TextureFormat::RGBA8_SNORM:
			return GL_BYTE;

		case TextureFormat::R16:
		case TextureFormat::RG16:
			return GL_UNSIGNED_SHORT;

		case TextureFormat::R16_SNORM:
		case TextureFormat::RG16_SNORM:
			return GL_SHORT;

		case TextureFormat::RGB5:
		case TextureFormat::RGB4:
		case TextureFormat::RGBA4:
		case TextureFormat::RGB5_A1:
		case TextureFormat::R3_G3_B2:
			return GL_UNSIGNED_SHORT_5_6_5;

			// float formats
		case TextureFormat::R16F:
		case TextureFormat::RG16F:
		case TextureFormat::RGB16F:
		case TextureFormat::RGBA16F:
		case TextureFormat::R11F_G11F_B10F:
		case TextureFormat::RGB9_E5:
			return GL_HALF_FLOAT;

		case TextureFormat::R32F:
		case TextureFormat::RG32F:
		case TextureFormat::RGB32F:
		case TextureFormat::RGBA32F:
			return GL_FLOAT;

			// integer formats
		case TextureFormat::R8I:
		case TextureFormat::RG8I:
		case TextureFormat::RGB8I:
		case TextureFormat::RGBA8I:
			return GL_BYTE;

		case TextureFormat::R8UI:
		case TextureFormat::RG8UI:
		case TextureFormat::RGB8UI:
		case TextureFormat::RGBA8UI:
			return GL_UNSIGNED_BYTE;

		case TextureFormat::R16I:
		case TextureFormat::RG16I:
		case TextureFormat::RGB16I:
		case TextureFormat::RGBA16I:
			return GL_SHORT;

		case TextureFormat::R16UI:
		case TextureFormat::RG16UI:
		case TextureFormat::RGB16UI:
		case TextureFormat::RGBA16UI:
			return GL_UNSIGNED_SHORT;

		case TextureFormat::R32I:
		case TextureFormat::RG32I:
		case TextureFormat::RGB32I:
		case TextureFormat::RGBA32I:
			return GL_INT;

		case TextureFormat::R32UI:
		case TextureFormat::RG32UI:
		case TextureFormat::RGB32UI:
		case TextureFormat::RGBA32UI:
			return GL_UNSIGNED_INT;

			// depth formats
		case TextureFormat::DEPTH_COMPONENT16: return GL_UNSIGNED_SHORT;
		case TextureFormat::DEPTH_COMPONENT24: return GL_FLOAT;
		case TextureFormat::DEPTH_COMPONENT32: return GL_FLOAT;
		case TextureFormat::DEPTH_COMPONENT32F: return GL_FLOAT;

		case TextureFormat::DEPTH24_STENCIL8:
			return GL_UNSIGNED_INT_24_8;

		case TextureFormat::DEPTH32F_STENCIL8:
			return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
		default:
			return GL_UNSIGNED_BYTE;
		}
	}

	static GLenum ToOpenglTargetType(TextureDimension dimension)
	{
		switch (dimension) {
		case TextureDimension::Tex2D: return GL_TEXTURE_2D;
		case TextureDimension::Tex3D: return GL_TEXTURE_3D;
		case TextureDimension::Array: return GL_TEXTURE_2D_ARRAY;
		case TextureDimension::Cube: return GL_TEXTURE_CUBE_MAP;
		default:;
		}

		return GL_TEXTURE_2D;
	}

	static GLenum toGLShaderStage(ShaderStage stage)
	{
		switch (stage)
		{
		case ShaderStage::Vertex:      return GL_VERTEX_SHADER;
		case ShaderStage::Fragment:    return GL_FRAGMENT_SHADER;
		case ShaderStage::Geometry:    return GL_GEOMETRY_SHADER;
		case ShaderStage::Compute:     return GL_COMPUTE_SHADER;
		case ShaderStage::TessControl: return GL_TESS_CONTROL_SHADER;
		case ShaderStage::TessEval:    return GL_TESS_EVALUATION_SHADER;
		default:
			return 0;
		}
	}

	static GLenum ToGLTopology(PrimitiveTopology t)
	{
		switch (t) {
		case PrimitiveTopology::Lines:    return GL_LINES;
		case PrimitiveTopology::LineStrip:   return GL_LINE_STRIP;
		case PrimitiveTopology::Points:   return GL_POINTS;
		case PrimitiveTopology::TriangleStrip:   return GL_TRIANGLE_STRIP;
		case PrimitiveTopology::Patches: return GL_PATCHES;
		case PrimitiveTopology::Triangles:
		default:                          return GL_TRIANGLES;
		}
	}

	static GLenum ToGLCullMode(CullMode c)
	{
		switch (c) {
		case CullMode::Front: return GL_FRONT;
		case CullMode::Back:  return GL_BACK;
		case CullMode::None:
		default:              return 0; // 0 = отключить cull
		}
	}

	static GLenum ToGLFrontFace(FrontFace f)
	{
		return (f == FrontFace::ClockWise) ? GL_CW : GL_CCW;
	}

	static GLenum ToGLFillMode(PolygonFillMode f)
	{
		return (f == PolygonFillMode::Line) ? GL_LINE : GL_FILL;
	}

	static GLenum ToGLBlendFactor(BlendFactor f)
	{
		switch (f) {
		case BlendFactor::Zero:             return GL_ZERO;
		case BlendFactor::One:              return GL_ONE;
		case BlendFactor::SrcAlpha:         return GL_SRC_ALPHA;
		case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
		default:                            return GL_ONE;
		}
	}

	// Если ещё нет маппера CompareOp -> GLenum:
	static GLint ToGLCompareFunc(CompareOp op)
	{
		switch (op) {
		case CompareOp::Less:         return GL_LESS;
		case CompareOp::LessEqual:    return GL_LEQUAL;
		case CompareOp::Greater:      return GL_GREATER;
		case CompareOp::GreaterEqual: return GL_GEQUAL;
		case CompareOp::Equal:        return GL_EQUAL;
		case CompareOp::NotEqual:     return GL_NOTEQUAL;
		case CompareOp::Always:       return GL_ALWAYS;
		case CompareOp::Never:        return GL_NEVER;
		case CompareOp::None:
		default:                      return GL_ALWAYS;
		}
	}

	static GLenum ToGLIndexType(IndexType t)
	{
		switch (t) {
		case IndexType::Uint16: return GL_UNSIGNED_SHORT;
		case IndexType::Uint32:
		default:                return GL_UNSIGNED_INT;
		}
	}

	GLenum OpenGLRHI::toGLBufferTarget(BufferUsage usage) const
	{
		// Only where the buffer is created and mapped; every draw and descriptor bind names its own target,
		// so a multi-usage buffer serves all its roles whichever one is picked here.
		if (usage & BufferUsage::Vertex)   return GL_ARRAY_BUFFER;
		if (usage & BufferUsage::Index)    return GL_ELEMENT_ARRAY_BUFFER;
		if (usage & BufferUsage::Uniform)  return GL_UNIFORM_BUFFER;
		if (usage & BufferUsage::Storage)  return GL_SHADER_STORAGE_BUFFER;
		if (usage & BufferUsage::Indirect) return GL_DRAW_INDIRECT_BUFFER;
		return GL_ARRAY_BUFFER;
	}

	GLenum OpenGLRHI::toGLAccessFlags(CpuAccess access) const
	{
		// Используется как hint для glBufferData (usage)
		switch (access)
		{
		case CpuAccess::None:      return GL_STATIC_DRAW;
		case CpuAccess::Write:     return GL_DYNAMIC_DRAW;
		case CpuAccess::Read:      return GL_STATIC_READ;
		case CpuAccess::ReadWrite: return GL_DYNAMIC_READ;
		default:                   return GL_DYNAMIC_DRAW;
		}
	}

	GLbitfield OpenGLRHI::toGLMapFlags(MapMode mode, CpuAccess access) const
	{
		GLbitfield flags = 0;

		// Доступ по направлению
		switch (mode)
		{
		case MapMode::Read:
			flags |= GL_MAP_READ_BIT;
			break;
		case MapMode::Write:
			flags |= GL_MAP_WRITE_BIT;
			break;
		case MapMode::ReadWrite:
			flags |= (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
			break;
		}

		// Invalidate/unsynchronized only for a write-only mapping, and never for one that reads.
		// This used to key off the buffer's CpuAccess alone, so mapping a ReadWrite buffer for reading
		// asked GL for GL_MAP_READ_BIT together with GL_MAP_INVALIDATE_RANGE_BIT -- an error, which
		// made glMapBufferRange return null and the read silently impossible.
		(void)access;
		if (mode == MapMode::Write)
		{
			flags |= GL_MAP_INVALIDATE_RANGE_BIT;
			flags |= GL_MAP_UNSYNCHRONIZED_BIT;
		}

		return flags;
	}

	bool isInteger(VertexFormat f)
	{
		switch (f) {
		case VertexFormat::Float:
		case VertexFormat::Float2:
		case VertexFormat::Float3:
		case VertexFormat::Float4:
			return false;
		case VertexFormat::Int:
		case VertexFormat::Int2:
		case VertexFormat::Int3:
		case VertexFormat::Int4:
		case VertexFormat::UInt:
		case VertexFormat::UInt2:
		case VertexFormat::UInt3:
		case VertexFormat::UInt4:
			return true;
		default:
			return false;
		}
	}


	GLenum OpenGLRHI::toGLType(VertexFormat f) const
	{
		switch (f) {
		case VertexFormat::Float:
		case VertexFormat::Float2:
		case VertexFormat::Float3:
		case VertexFormat::Float4:
			return GL_FLOAT;
		case VertexFormat::Int:
		case VertexFormat::Int2:
		case VertexFormat::Int3:
		case VertexFormat::Int4:
			return GL_INT;
		case VertexFormat::UInt:
		case VertexFormat::UInt2:
		case VertexFormat::UInt3:
		case VertexFormat::UInt4:
			return GL_UNSIGNED_INT;
		default:
			return GL_FLOAT;
		}
	}

	GLint OpenGLRHI::toGLSize(VertexFormat f) const
	{
		switch (f)
		{
		case VertexFormat::Float:
		case VertexFormat::Int:
		case VertexFormat::UInt:

			return 1;
		case VertexFormat::Float2:
		case VertexFormat::Int2:
		case VertexFormat::UInt2:
			return 2;
		case VertexFormat::Float3:
		case VertexFormat::Int3:
		case VertexFormat::UInt3:
			return 3;
		case VertexFormat::Float4:
		case VertexFormat::Int4:
		case VertexFormat::UInt4:
			return 4;
		default:
			return 3;
		}
	}


	GLCommandList* OpenGLRHI::recordingList(CommandListHandle cmd)
	{
		GLCommandList* list = getLive(m_commandLists, cmd.id);
		if (!list || !list->recording) {
			return nullptr;
		}
		return list;
	}

	void OpenGLRHI::cmdEndRenderPass(CommandListHandle cmd)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdEndRenderPass{});
		}
	}

	void OpenGLRHI::execEndRenderPass()
	{
		glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, m_prevFramebuffer);
		m_prevFramebuffer = 0;
		m_currentPassHeight = 0;
	}

	void OpenGLRHI::cmdBindPipeline(CommandListHandle cmd, PipelineHandle pipeline)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			if (const auto* gp = getLive(m_pipelines, pipeline.id)) {
				pipelineBound(cmd, gp->label);
			}
			list->commands.emplace_back(CmdBindPipeline{ pipeline });
		}
	}

	void OpenGLRHI::execBindPipeline(PipelineHandle pipeline)
	{
		const GLGraphicsPipeline* gpPtr = getLive(m_pipelines, pipeline.id);
		if (!gpPtr) {
			return;
		}

		const GLGraphicsPipeline& gp = *gpPtr;

		glUseProgram(gp.programId);
		m_currentPipeline = pipeline;

		if (gp.isCompute) {
			glBindVertexArray(0);
			return;
		}

		glBindVertexArray(gp.vaoId);

		// topology
		m_currentPrimitive = gp.primitiveGL;

		// polygon mode
		glPolygonMode(GL_FRONT_AND_BACK, gp.fillModeGL);

		if (gp.patchControlPoints > 0) {
			glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(gp.patchControlPoints));
			m_currentPrimitive = GL_PATCHES;
		}

		// cull
		if (gp.cullModeGL == 0) {
			glDisable(GL_CULL_FACE);
		}
		else {
			glEnable(GL_CULL_FACE);
			glCullFace(gp.cullModeGL);
		}
		glFrontFace(gp.frontFaceGL);

		// depth
		if (gp.depthTestEnabled) {
			glEnable(GL_DEPTH_TEST);
		}
		else {
			glDisable(GL_DEPTH_TEST);
		}
		glDepthFunc(gp.depthFuncGL);
		glDepthMask(gp.depthWriteEnabled ? GL_TRUE : GL_FALSE);

		// blend
		if (gp.blendEnabled) {
			glEnable(GL_BLEND);
			glBlendFuncSeparate(gp.blendSrcGL, gp.blendDstGL, gp.blendSrcAlphaGL, gp.blendDstAlphaGL);
		}
		else {
			glDisable(GL_BLEND);
		}

		// colour: none for a depth-only pipeline
		const GLboolean colorWrite = gp.desc.colorWrite ? GL_TRUE : GL_FALSE;
		glColorMask(colorWrite, colorWrite, colorWrite, colorWrite);

		m_currentPipeline = pipeline;
	}

	void OpenGLRHI::cmdBindVertexBuffer(CommandListHandle cmd, uint32_t slot, BufferHandle handle, uint32_t offset)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdBindVertexBuffer{ slot, handle, offset });
		}
	}

	void OpenGLRHI::execBindVertexBuffer(uint32_t slot, BufferHandle handle, uint32_t offset)
	{
		const auto* gp = getLive(m_pipelines, m_currentPipeline.id);
		const auto* buf = getLive(m_buffers, handle.id);
		if (!gp || !buf)
			return;

		// защитимся от кривого slot
		if (slot >= gp->desc.layout.bindings.size())
			return;

		RhiLog::check(buf->desc.usage & BufferUsage::Vertex, "WRONG BUFFER TYPE BINDING");
		const auto& bindingDesc = gp->desc.layout.bindings[slot];
		const GLsizei stride = static_cast<GLsizei>(bindingDesc.stride);
		glVertexArrayVertexBuffer(
			gp->vaoId,
			bindingDesc.binding,                          // vertex binding
			buf->id,                        // VBO
			static_cast<GLintptr>(offset), // byte offset
			stride
		);
		glBindBuffer(GL_ARRAY_BUFFER, buf->id);
	}

	void OpenGLRHI::cmdBindIndexBuffer(CommandListHandle cmd, BufferHandle handle, IndexType type, uint32_t offset)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdBindIndexBuffer{ handle, type, offset });
		}
	}

	void OpenGLRHI::execBindIndexBuffer(BufferHandle handle, IndexType type, uint32_t offset)
	{
		const auto* gp = getLive(m_pipelines, m_currentPipeline.id);
		const auto* buf = getLive(m_buffers, handle.id);
		if (!gp || !buf) {
			m_currentIndexType = type;
			m_currentIndexOffset = 0;
			return;
		}

		RhiLog::check(buf->desc.usage & BufferUsage::Index, "WRONG BUFFER TYPE BINDING");
		glVertexArrayElementBuffer(gp->vaoId, buf->id);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf->id);
		m_currentIndexType = type;
		m_currentIndexOffset = offset;
	}

	void OpenGLRHI::cmdDraw(CommandListHandle cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDraw{ vertexCount, instanceCount, firstVertex, firstInstance });
		}
	}

	void OpenGLRHI::execDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		if (vertexCount == 0 || instanceCount == 0) {
			return;
		}

		if (instanceCount == 1 && firstInstance == 0) {
			glDrawArrays(m_currentPrimitive,
				static_cast<GLint>(firstVertex),
				static_cast<GLsizei>(vertexCount));
		}
		else if (firstInstance == 0) {
			glDrawArraysInstanced(m_currentPrimitive,
				static_cast<GLint>(firstVertex),
				static_cast<GLsizei>(vertexCount),
				static_cast<GLsizei>(instanceCount));
		}
		else {
			// Base instance shifts where per-instance attributes are fetched from, which is what
			// Vulkan's firstInstance does. glDrawArraysInstanced has no such argument, so taking
			// this branch off the plain instanced call is the difference between honouring the
			// parameter and silently ignoring it.
			glDrawArraysInstancedBaseInstance(m_currentPrimitive,
				static_cast<GLint>(firstVertex),
				static_cast<GLsizei>(vertexCount),
				static_cast<GLsizei>(instanceCount),
				static_cast<GLuint>(firstInstance));
		}
	}

	void OpenGLRHI::cmdDrawIndexed(CommandListHandle cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDrawIndexed{ indexCount, instanceCount, firstIndex, vertexOffset, firstInstance });
		}
	}

	void OpenGLRHI::cmdDrawIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDrawIndirect{ buffer, offset, drawCount, stride });
		}
	}

	void OpenGLRHI::cmdDrawIndexedIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDrawIndexedIndirect{ buffer, offset, drawCount, stride });
		}
	}

	void OpenGLRHI::execDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		if (indexCount == 0 || instanceCount == 0) {
			return;
		}

		const GLenum glIndexType = ToGLIndexType(m_currentIndexType);
		const GLsizei indexSize = (m_currentIndexType == IndexType::Uint16) ? sizeof(uint16_t) : sizeof(uint32_t);

		uintptr_t baseOffsetBytes = static_cast<uintptr_t>(m_currentIndexOffset);
		uintptr_t firstIndexBytes = static_cast<uintptr_t>(firstIndex) * indexSize;
		const void* indexOffset = reinterpret_cast<const void*>(baseOffsetBytes + firstIndexBytes);

		if (firstInstance != 0) {
			// Covers a non-zero vertexOffset too: base vertex is an argument here, so there is no
			// need for a fourth branch. See execDraw for why the base-instance call is not optional.
			glDrawElementsInstancedBaseVertexBaseInstance(
				m_currentPrimitive,
				static_cast<GLsizei>(indexCount),
				glIndexType,
				indexOffset,
				static_cast<GLsizei>(instanceCount),
				vertexOffset,
				static_cast<GLuint>(firstInstance)
			);
		}
		else if (vertexOffset == 0) {
			glDrawElementsInstanced(
				m_currentPrimitive,
				static_cast<GLsizei>(indexCount),
				glIndexType,
				indexOffset,
				static_cast<GLsizei>(instanceCount)
			);
		}
		else {
			glDrawElementsInstancedBaseVertex(
				m_currentPrimitive,
				static_cast<GLsizei>(indexCount),
				glIndexType,
				indexOffset,
				static_cast<GLsizei>(instanceCount),
				vertexOffset
			);
		}
	}

	void OpenGLRHI::execDrawIndirect(BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride)
	{
		const GLBuffer* buf = getLive(m_buffers, buffer.id);
		if (!buf || drawCount == 0) {
			return;
		}
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buf->id);
		if (drawCount == 1) {
			glDrawArraysIndirect(m_currentPrimitive, reinterpret_cast<const void*>(offset));
		}
		else {
			glMultiDrawArraysIndirect(m_currentPrimitive, reinterpret_cast<const void*>(offset),
				static_cast<GLsizei>(drawCount), static_cast<GLsizei>(stride ? stride : 16u));
		}
	}

	void OpenGLRHI::execDrawIndexedIndirect(BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride)
	{
		const GLBuffer* buf = getLive(m_buffers, buffer.id);
		if (!buf || drawCount == 0) {
			return;
		}
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buf->id);
		if (drawCount == 1) {
			glDrawElementsIndirect(m_currentPrimitive, ToGLIndexType(m_currentIndexType),
				reinterpret_cast<const void*>(offset));
		}
		else {
			glMultiDrawElementsIndirect(m_currentPrimitive, ToGLIndexType(m_currentIndexType),
				reinterpret_cast<const void*>(offset), static_cast<GLsizei>(drawCount),
				static_cast<GLsizei>(stride ? stride : 20u));
		}
	}

	void OpenGLRHI::cmdDrawIndexedIndirectCount(CommandListHandle cmd, BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDrawIndexedIndirectCount{ buffer, offset, countBuffer, countOffset, maxDrawCount, stride });
		}
	}

	void OpenGLRHI::execDrawIndexedIndirectCount(BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride)
	{
		const GLBuffer* buf = getLive(m_buffers, buffer.id);
		const GLBuffer* countBuf = getLive(m_buffers, countBuffer.id);
		if (!buf || !countBuf || maxDrawCount == 0) {
			return;
		}
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buf->id);
		glBindBuffer(GL_PARAMETER_BUFFER, countBuf->id);
		glMultiDrawElementsIndirectCount(m_currentPrimitive, ToGLIndexType(m_currentIndexType),
			reinterpret_cast<const void*>(offset), static_cast<GLintptr>(countOffset),
			static_cast<GLsizei>(maxDrawCount), static_cast<GLsizei>(stride ? stride : 20u));
		glBindBuffer(GL_PARAMETER_BUFFER, 0);
	}

	void OpenGLRHI::cmdDispatch(CommandListHandle cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdDispatch{ groupCountX, groupCountY, groupCountZ });
		}
	}

	void OpenGLRHI::execDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
	{
		if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) {
			return;
		}
		glDispatchCompute(groupCountX, groupCountY, groupCountZ);
	}

	RenderPassHandle OpenGLRHI::createRenderPass(const RenderPassDesc& desc)
	{
		// В OpenGL рендер-пасс — чисто описание: LoadOp/StoreOp/clear-значения.
		// Никаких GL-объектов создавать не нужно.
		GLRenderPass rp{};
		rp.desc = desc;
		rp.live = true;

		RenderPassHandle handle;
		handle.id = storeResource(m_renderPasses, m_freeRenderPasses, std::move(rp));
		return handle;
	}

	void OpenGLRHI::destroyRenderPass(RenderPassHandle renderPass)
	{
		recycleResource(m_renderPasses, m_freeRenderPasses, renderPass.id);
	}

	CommandListHandle OpenGLRHI::beginCommandList(CommandListType type)
	{
		if (m_openCommandList) {
			RhiLog::error("beginCommandList: the previous command list was never submitted");
		}
		GLCommandList list{};
		list.type = type;
		list.recording = true;
		list.live = true;
		list.commands.swap(m_spareCommands);
		list.pushData.swap(m_sparePushData);

		CommandListHandle handle;
		handle.id = storeResource(m_commandLists, m_freeCommandLists, std::move(list));
		m_openCommandList = handle;
		return handle;
	}

	void OpenGLRHI::endCommandList(CommandListHandle cmd)
	{
		if (GLCommandList* list = getLive(m_commandLists, cmd.id)) {
			list->recording = false;
		}
	}

	void OpenGLRHI::executeCommand(const GLCommand& command)
	{
		std::visit([this](const auto& recorded) {
			using T = std::decay_t<decltype(recorded)>;
			if constexpr (std::is_same_v<T, CmdBeginRenderPass>) {
				execBeginRenderPass(recorded);
			}
			else if constexpr (std::is_same_v<T, CmdEndRenderPass>) {
				execEndRenderPass();
			}
			else if constexpr (std::is_same_v<T, CmdBindPipeline>) {
				execBindPipeline(recorded.pipeline);
			}
			else if constexpr (std::is_same_v<T, CmdBindVertexBuffer>) {
				execBindVertexBuffer(recorded.slot, recorded.handle, recorded.offset);
			}
			else if constexpr (std::is_same_v<T, CmdBindIndexBuffer>) {
				execBindIndexBuffer(recorded.handle, recorded.type, recorded.offset);
			}
			else if constexpr (std::is_same_v<T, CmdBindDescriptorSets>) {
				execBindDescriptorSets(recorded);
			}
			else if constexpr (std::is_same_v<T, CmdPushConstants>) {
				execPushConstants(m_replayingList->pushData.data() + recorded.offset, recorded.size);
			}
			else if constexpr (std::is_same_v<T, CmdDraw>) {
				execDraw(recorded.vertexCount, recorded.instanceCount, recorded.firstVertex, recorded.firstInstance);
			}
			else if constexpr (std::is_same_v<T, CmdDrawIndexed>) {
				execDrawIndexed(recorded.indexCount, recorded.instanceCount, recorded.firstIndex, recorded.vertexOffset, recorded.firstInstance);
			}
			else if constexpr (std::is_same_v<T, CmdDrawIndirect>) {
				execDrawIndirect(recorded.buffer, recorded.offset, recorded.drawCount, recorded.stride);
			}
			else if constexpr (std::is_same_v<T, CmdDrawIndexedIndirect>) {
				execDrawIndexedIndirect(recorded.buffer, recorded.offset, recorded.drawCount, recorded.stride);
			}
			else if constexpr (std::is_same_v<T, CmdDrawIndexedIndirectCount>) {
				execDrawIndexedIndirectCount(recorded.buffer, recorded.offset, recorded.countBuffer, recorded.countOffset, recorded.maxDrawCount, recorded.stride);
			}
			else if constexpr (std::is_same_v<T, CmdDispatch>) {
				execDispatch(recorded.groupCountX, recorded.groupCountY, recorded.groupCountZ);
			}
			else if constexpr (std::is_same_v<T, CmdBlit>) {
				execBlit(recorded.src, recorded.dst, recorded.mask, recorded.filter);
			}
			else if constexpr (std::is_same_v<T, CmdSetScissor>) {
				execSetScissor(recorded.x, recorded.y, recorded.width, recorded.height);
			}
			else if constexpr (std::is_same_v<T, CmdPipelineBarrier>) {
				execPipelineBarrier(recorded.barrier);
			}
			else if constexpr (std::is_same_v<T, CmdGpuMark>) {
				auto& count = mGpuMarkCount[mGpuMarkWrite];
				if (count < (recorded.endsPass ? kMaxGpuMarks : kGpuShaderMarks)) {
					glQueryCounter(mGpuMarkQueries[mGpuMarkWrite][count], GL_TIMESTAMP);
					mGpuMarkLabels[mGpuMarkWrite][count] = { recorded.label, recorded.endsPass };
					++count;
				}
			}
		}, command);
	}

	void OpenGLRHI::submit(CommandListHandle handle)
	{
		if (handle.id == m_openCommandList.id) {
			m_openCommandList = {};
		}
		GLCommandList* list = getLive(m_commandLists, handle.id);
		if (!list) {
			return;
		}

		if (mGpuTimeQueries[0]) {
			glBeginQuery(GL_TIME_ELAPSED, mGpuTimeQueries[mGpuQueryWrite]);
		}

		list->recording = false;
		uploadTransient();
		m_replayingList = list;
		for (const auto& command : list->commands) {
			executeCommand(command);
		}
		m_replayingList = nullptr;
		resetTransient();
		list->commands.clear();
		list->pushData.clear();
		m_spareCommands.swap(list->commands);
		m_sparePushData.swap(list->pushData);
		recycleResource(m_commandLists, m_freeCommandLists, handle.id);
		for (const auto id : std::exchange(m_pendingBufferDestroys, {})) {
			destroyBuffer({ id });
		}

		if (mGpuTimeQueries[0]) {
			glEndQuery(GL_TIME_ELAPSED);
			// The oldest query in the ring, which the GPU has had every submit since to finish. Never waited on:
			// an unfinished one leaves the last reading in place.
			if (mGpuQueriesIssued >= kGpuTimeQueries) {
				const int oldest = (mGpuQueryWrite + 1) % kGpuTimeQueries;
				GLuint available = 0;
				glGetQueryObjectuiv(mGpuTimeQueries[oldest], GL_QUERY_RESULT_AVAILABLE, &available);
				if (available) {
					GLuint64 ns = 0;
					glGetQueryObjectui64v(mGpuTimeQueries[oldest], GL_QUERY_RESULT, &ns);
					mLastGpuMs = static_cast<double>(ns) / 1'000'000.0;
				}
			}
			else {
				++mGpuQueriesIssued;
			}
			mGpuQueryWrite = (mGpuQueryWrite + 1) % kGpuTimeQueries;
		}
		// A submit that marked moves the marks' ring on, to the oldest slot: read, if the GPU is done with it, before
		// it is written again. Never waited on either.
		if (mGpuMarkCount[mGpuMarkWrite] > 0) {
			mGpuMarkWrite = (mGpuMarkWrite + 1) % kGpuTimeQueries;
			auto& count = mGpuMarkCount[mGpuMarkWrite];
			GLuint available = 0;
			if (count > 1) {
				glGetQueryObjectuiv(mGpuMarkQueries[mGpuMarkWrite][count - 1], GL_QUERY_RESULT_AVAILABLE, &available);
			}
			GLuint64 first = 0;
			double atMs[kMaxGpuMarks]{};
			for (uint32_t mark = 0; available && mark < count; ++mark) {
				GLuint64 ns = 0;
				glGetQueryObjectui64v(mGpuMarkQueries[mGpuMarkWrite][mark], GL_QUERY_RESULT, &ns);
				first = mark == 0 ? ns : first;
				atMs[mark] = static_cast<double>(ns - first) / 1'000'000.0;
			}
			if (available) {
				publishGpuSpans(mGpuMarkLabels[mGpuMarkWrite], atMs, count);
			}
			count = 0;
		}
	}

	void OpenGLRHI::setPresentBlitSource(TextureHandle texture, SamplerHandle)
	{
		m_presentBlitTexture = texture;
	}

	bool OpenGLRHI::readbackTexture(TextureHandle handle, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t layer)
	{
		const GLTexture* tex = getLive(textures, handle.id);
		if (!tex) {
			return false;
		}
		const bool isArray = tex->desc.dimension == TextureDimension::Array;
		if (!isArray && tex->desc.dimension != TextureDimension::Tex2D) {
			return false; // cube reads would need a face convention; no pass outputs one
		}
		width = static_cast<int>(tex->desc.width);
		height = static_cast<int>(tex->desc.height);
		if (width <= 0 || height <= 0) {
			return false;
		}

		// One-shot resources: capture is a diagnostic path, not a per-frame one.
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		const GLenum attachment = ToGLAttachmentFormat(tex->desc.format);
		const bool isDepth = attachment != GL_COLOR_ATTACHMENT0;
		if (isArray) {
			glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, attachment, tex->id, 0, static_cast<GLint>(layer));
		}
		else {
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_2D, tex->id, 0);
		}
		if (isDepth) {
			glReadBuffer(GL_NONE);
			glDrawBuffer(GL_NONE);
		}

		std::vector<uint8_t> bottomUp;
		const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		if (complete) {
			glFinish();
			bottomUp.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			if (isDepth) {
				// Depth needs more than 8 bits to be useful: a far cascade packs its whole scene
				// into the last fraction of the range, where an 8-bit grey collapses everything to
				// 255. Pack 16 bits across R and G (R = high byte), keep B as a greyscale preview.
				std::vector<float> depth(static_cast<size_t>(width) * static_cast<size_t>(height));
				glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
				for (size_t i = 0; i < depth.size(); ++i) {
					const auto q = static_cast<uint32_t>(std::clamp(depth[i], 0.f, 1.f) * 65535.f + 0.5f);
					bottomUp[i * 4 + 0] = static_cast<uint8_t>(q >> 8);
					bottomUp[i * 4 + 1] = static_cast<uint8_t>(q & 0xFF);
					bottomUp[i * 4 + 2] = static_cast<uint8_t>(q >> 8);
					bottomUp[i * 4 + 3] = 255;
				}
			}
			else if (floatFormatChannels(tex->desc.format) > 0) {
				// As floats, converted by the same rule as Vulkan (RhiReadback.h). GL fills missing channels with
				// 0, 0, 1 itself.
				std::vector<float> texels(bottomUp.size());
				glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, texels.data());
				for (size_t i = 0; i < texels.size(); ++i) {
					bottomUp[i] = floatToUnorm8(texels[i]);
				}
			}
			else {
				glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
			}
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &fbo);
		if (!complete) {
			return false;
		}

		// glReadPixels starts at the bottom-left; Vulkan hands back top-down. Normalise here so
		// the two backends' captures can be diffed directly.
		const size_t rowBytes = static_cast<size_t>(width) * 4u;
		rgba.resize(bottomUp.size());
		for (int y = 0; y < height; ++y) {
			std::memcpy(rgba.data() + rowBytes * static_cast<size_t>(y),
				bottomUp.data() + rowBytes * static_cast<size_t>(height - 1 - y),
				rowBytes);
		}
		return true;
	}

	void OpenGLRHI::present()
	{
		const RhiSurface surface = hostSurface();
		if (!surface.window) {
			return;
		}
		if (hostPresentsToScreen() && surface.visible && m_presentBlitTexture) {
			glDisable(GL_SCISSOR_TEST);
			const GLTexture* tex = getLive(textures, m_presentBlitTexture.id);
			if (tex) {
				if (m_presentReadGlFbo == 0) {
					glGenFramebuffers(1, &m_presentReadGlFbo);
				}
				if (m_presentReadGlFbo != 0 && m_presentReadTexId != m_presentBlitTexture.id) {
					glBindFramebuffer(GL_READ_FRAMEBUFFER, m_presentReadGlFbo);
					glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->id, 0);
					m_presentReadTexId = m_presentBlitTexture.id;
				}
				const int dstW = surface.backbufferWidth > 0 ? surface.backbufferWidth : surface.renderWidth;
				const int dstH = surface.backbufferHeight > 0 ? surface.backbufferHeight : surface.renderHeight;
				const int srcW = static_cast<int>(tex->desc.width);
				const int srcH = static_cast<int>(tex->desc.height);
				if (srcW > 0 && srcH > 0 && dstW > 0 && dstH > 0 && m_presentReadGlFbo != 0) {
					glBindFramebuffer(GL_READ_FRAMEBUFFER, m_presentReadGlFbo);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
					glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, dstW, dstH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
				}
			}
		}
		glFlush();
		if (hostPresentsToScreen() && surface.visible) {
			glfwSwapBuffers(surface.window);
			return;
		}
		// Hidden, nothing swaps, and nothing else held the CPU back: it ran on ahead of the GPU by as many frames as the
		// driver would queue, so a frame's time said little of what it cost and the GPU's timings never came back. The
		// frame kHiddenFrames back is waited for instead, as a swap would wait.
		auto& fence = mHiddenFrames[mHiddenFrame];
		if (fence) {
			glClientWaitSync(static_cast<GLsync>(fence), GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000);
			glDeleteSync(static_cast<GLsync>(fence));
		}
		fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		mHiddenFrame = (mHiddenFrame + 1) % kHiddenFrames;
	}

	void OpenGLRHI::runOnWorkerThread(const std::function<void()>& work)
	{
		std::lock_guard lock(m_workerContextMutex);
		if (!m_workerContext) {
			RhiLog::error("OpenGLRHI: no worker context; running on the calling thread without one");
			work();
			return;
		}
		glfwMakeContextCurrent(m_workerContext);
		work();
		// Objects made here are used by the main context next; finish before handing them over.
		glFinish();
		glfwMakeContextCurrent(nullptr);
	}

	void OpenGLRHI::cmdBeginRenderPass(CommandListHandle cmd, const RenderPassBeginInfo& beginInfo)
	{
		GLCommandList* list = recordingList(cmd);
		if (!list) {
			return;
		}

		CmdBeginRenderPass rec;
		rec.info = beginInfo;
		rec.info.overrideClearColors = nullptr;
		rec.info.overrideClearDepth = nullptr;
		rec.info.overrideClearStencil = nullptr;
		if (beginInfo.overrideClearColors) {
			rec.hasOverrideColors = true;
			const GLRenderPass* rp = getLive(m_renderPasses, beginInfo.renderPass.id);
			const uint32_t n = rp ? rp->desc.colorAttachmentCount : 8u;
			for (uint32_t i = 0; i < n && i < 8u; ++i) {
				rec.clearColors[i] = beginInfo.overrideClearColors[i];
			}
		}
		if (beginInfo.overrideClearDepth) {
			rec.hasOverrideDepth = true;
			rec.clearDepth = *beginInfo.overrideClearDepth;
		}
		if (beginInfo.overrideClearStencil) {
			rec.hasOverrideStencil = true;
			rec.clearStencil = *beginInfo.overrideClearStencil;
		}
		list->commands.emplace_back(std::move(rec));
	}

	void OpenGLRHI::execBeginRenderPass(const CmdBeginRenderPass& recorded)
	{
		RenderPassBeginInfo beginInfo = recorded.info;
		if (recorded.hasOverrideColors) {
			beginInfo.overrideClearColors = recorded.clearColors;
		}
		if (recorded.hasOverrideDepth) {
			beginInfo.overrideClearDepth = &recorded.clearDepth;
		}
		if (recorded.hasOverrideStencil) {
			beginInfo.overrideClearStencil = &recorded.clearStencil;
		}

		const GLRenderPass* rpPtr = getLive(m_renderPasses, beginInfo.renderPass.id);
		if (!rpPtr) {
			RhiLog::fatal("OpenGLRHI::beginRenderPass: invalid RenderPass handle");
			return;
		}

		const GLRenderPass& rp = *rpPtr;


		// Сохраняем предыдущий FBO, чтобы вернуть в endRenderPass
		GLint prevFbo = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
		m_prevFramebuffer = static_cast<GLuint>(prevFbo);
		int w = 0;
		int h = 0;
		if (!beginInfo.framebuffer) {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			// No host means no default framebuffer to size against, so the caller's extent is all
			// there is. Only a headless RHI gets here with a null window.
			const RhiSurface surface = hostSurface();
			const auto screenW = surface.width;
			const auto screenH = surface.height;
			w = beginInfo.width > 0 ? beginInfo.width : screenW;
			h = beginInfo.height > 0 ? beginInfo.height : screenH;
		}
		else if (const GLFramebuffer* fb = getLive(framebuffers, beginInfo.framebuffer.id)) {
			glBindFramebuffer(GL_FRAMEBUFFER, fb->id);
			w = beginInfo.width > 0 ? beginInfo.width : static_cast<int>(fb->desc.width);
			h = beginInfo.height > 0 ? beginInfo.height : static_cast<int>(fb->desc.height);
		}
		else {
			RhiLog::fatal("OpenGLRHI::beginRenderPass: invalid Framebuffer handle");
			return;
		}


		// Вычисляем область
		int x = beginInfo.x;
		int y = beginInfo.y;
		//int w = beginInfo.width > 0 ? beginInfo.width : static_cast<int>(fb.desc.width);
		//int h = beginInfo.height > 0 ? beginInfo.height : static_cast<int>(fb.desc.height);

		glViewport(x, y, w, h);
		glDepthRange(0.f, 1.f);
		m_currentPassHeight = h;
		glDisable(GL_SCISSOR_TEST);
		// The clears obey the colour mask, which a depth-only pipeline (GraphicsPipelineDesc::colorWrite) leaves off.
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// --- Clear color attachments (если loadOp == Clear) ---
		for (uint32_t i = 0; i < rp.desc.colorAttachmentCount; ++i)
		{
			const auto& att = rp.desc.colorAttachments[i];
			if (att.loadOp != AttachmentLoadOp::Clear)
				continue;

			RhiVec4 color = att.clearColor;
			if (beginInfo.overrideClearColors) {
				// предполагаем, что вызывающий гарантирует нужный размер массива
				color = beginInfo.overrideClearColors[i];
			}

			float c[4] = { color.x, color.y, color.z, color.w };
			glClearBufferfv(GL_COLOR, static_cast<GLint>(i), c);
		}

		// --- Clear depth/stencil (если есть и loadOp == Clear) ---
		if (rp.desc.hasDepthStencil && rp.desc.depthAttachment.loadOp == AttachmentLoadOp::Clear)
		{
			float depth = rp.desc.depthAttachment.clearDepth;
			uint8_t stencil = rp.desc.depthAttachment.clearStencil;

			if (beginInfo.overrideClearDepth) {
				depth = *beginInfo.overrideClearDepth;
			}
			if (rp.desc.depthAttachment.hasStencil && beginInfo.overrideClearStencil) {
				stencil = *beginInfo.overrideClearStencil;
			}

			// На время clear depth буфера логично обеспечить включённую запись по глубине
			GLboolean oldDepthMask;
			glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
			glDepthMask(GL_TRUE);

			if (rp.desc.depthAttachment.hasStencil) {
				glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, static_cast<GLint>(stencil));
			}
			else {
				glClearBufferfv(GL_DEPTH, 0, &depth);
			}

			glDepthMask(oldDepthMask);
		}
	}

	void OpenGLRHI::cmdPushConstants(CommandListHandle cmd, PipelineHandle, ShaderStage, const void* data, uint32_t size)
	{
		GLCommandList* list = recordingList(cmd);
		if (!list || !data || size == 0) {
			return;
		}

		CmdPushConstants rec{ static_cast<uint32_t>(list->pushData.size()), size };
		list->pushData.insert(list->pushData.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
		list->commands.emplace_back(rec);
	}

	void OpenGLRHI::execPushConstants(const void* data, uint32_t size)
	{
		if (!data || size == 0) {
			return;
		}

		// emulation push constant in GL via UBO
		if (!m_pushConstantUBO) {
			glCreateBuffers(1, &m_pushConstantUBO);
			glNamedBufferData(m_pushConstantUBO, 256, nullptr, GL_DYNAMIC_DRAW);
		}

		glNamedBufferSubData(m_pushConstantUBO, 0, size, data);
		glBindBufferBase(GL_UNIFORM_BUFFER, m_pushConstantPos, m_pushConstantUBO);
	}

	DescriptorSetLayoutHandle OpenGLRHI::createDescriptorSetLayout(
		const DescriptorSetLayoutDesc& desc)
	{
		// Same rule as the Vulkan side: one binding number, one descriptor. OpenGL could tolerate a
		// repeat, since an image and a buffer land in separate namespaces here, but a layout that only
		// works on one backend is worse than one that is refused on both.
		if (descriptorLayoutHasDuplicateBinding(desc)) {
			RhiLog::error(
				"createDescriptorSetLayout: a binding number is declared twice; one binding, one descriptor");
			return {};
		}

		GLDescriptorSetLayout layout{};
		layout.desc = desc;
		layout.live = true;

		DescriptorSetLayoutHandle h;
		h.id = storeResource(m_descriptorSetLayouts, m_freeDescriptorSetLayouts, std::move(layout));
		return h;
	}


	void OpenGLRHI::destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout)
	{
		recycleResource(m_descriptorSetLayouts, m_freeDescriptorSetLayouts, layout.id);
	}

	DescriptorSetHandle OpenGLRHI::allocateDescriptorSet(DescriptorSetLayoutHandle layout)
	{
		const GLDescriptorSetLayout* layoutObj = getLive(m_descriptorSetLayouts, layout.id);
		if (!layoutObj)
			return {};

		GLDescriptorSet set{};
		set.layout = layoutObj->desc;
		set.live = true;
		set.bindings->reserve(set.layout.bindings.size());
		for (const auto& b : set.layout.bindings) {
			GLDescriptorBindingData data;
			data.binding = b.binding;
			data.type = b.type;
			set.bindings->push_back(std::move(data));
		}

		DescriptorSetHandle h;
		h.id = storeResource(m_descriptorSets, m_freeDescriptorSets, std::move(set));
		return h;
	}


	void OpenGLRHI::freeDescriptorSet(DescriptorSetHandle set)
	{
		recycleResource(m_descriptorSets, m_freeDescriptorSets, set.id);
	}

	void OpenGLRHI::updateDescriptorSets(const std::vector<WriteDescriptor>& writes)
	{
		for (const auto& w : writes)
		{
			GLDescriptorSet* set = getLive(m_descriptorSets, w.set.id);
			if (!set)
				continue;

			// The layout fixes the descriptor type at every binding, so the layout decides what is
			// written here, not the caller's WriteDescriptor::type. Matching a slot on binding *and*
			// type used to append a second slot when the two disagreed, which left the original slot
			// authoritative and made the rewrite silently invisible -- while Vulkan handed the same
			// input to the driver and took an access violation.
			// The layout defines the binding's type, and binding numbers are unique within it, so the
			// number alone names the descriptor.
			const DescriptorSetLayoutBinding* declared = nullptr;
			for (const auto& b : set->layout.bindings) {
				if (b.binding == w.binding) {
					declared = &b;
					break;
				}
			}
			if (!declared) {
				RhiLog::error("updateDescriptorSets: set has no binding %u", w.binding);
				continue;
			}
			const DescriptorType type = declared->type;

			if (set->bindings.use_count() > 1) {
				set->bindings = std::make_shared<GLDescriptorBindings>(*set->bindings);
			}
			GLDescriptorBindingData* slot = nullptr;
			for (auto& b : *set->bindings) {
				if (b.binding == w.binding) {
					slot = &b;
					break;
				}
			}
			if (!slot) {
				set->bindings->push_back({ w.binding, type, {}, {} });
				slot = &set->bindings->back();
			}

			switch (type)
			{
			case DescriptorType::UniformBufferDynamic:
			case DescriptorType::StorageBufferDynamic:
				if (std::any_of(w.buffers.begin(), w.buffers.end(), [](const DescriptorBufferInfo& b) { return b.range == 0; })) {
					// Vulkan needs the range, so it is required on both.
					RhiLog::error("updateDescriptorSets: dynamic binding %u needs a range", w.binding);
					break;
				}
				slot->buffers = w.buffers;
				break;

			case DescriptorType::UniformBuffer:
			case DescriptorType::StorageBuffer:
				slot->buffers = w.buffers;
				break;

			case DescriptorType::CombinedImageSampler:
				slot->images = w.images;
				break;
			}
		}
	}


	void OpenGLRHI::cmdBindDescriptorSets(CommandListHandle cmd, PipelineHandle, uint32_t, const DescriptorSetHandle* sets, uint32_t setCount,
		const uint32_t* dynamicOffsets, const uint32_t dynamicOffsetCount)
	{
		GLCommandList* list = recordingList(cmd);
		if (!list || !sets || setCount == 0) {
			return;
		}

		if (setCount > CmdBindDescriptorSets::kMaxSets) {
			RhiLog::error("cmdBindDescriptorSets: %u sets, at most %zu", setCount, CmdBindDescriptorSets::kMaxSets);
			return;
		}
		CmdBindDescriptorSets rec;
		uint32_t nextOffset = 0;
		for (uint32_t i = 0; i < setCount; ++i) {
			const GLDescriptorSet* set = getLive(m_descriptorSets, sets[i].id);
			if (!set) {
				continue;
			}
			const auto setIndex = rec.setCount++;
			rec.sets[setIndex] = set->bindings;

			// As Vulkan takes them: the set's dynamic descriptors in ascending binding number, one offset each.
			std::array<const GLDescriptorBindingData*, CmdBindDescriptorSets::kMaxDynamic> dynamic{};
			size_t dynamicInSet = 0;
			for (const auto& binding : *set->bindings) {
				if (isDynamicDescriptor(binding.type) && dynamicInSet < dynamic.size()) {
					dynamic[dynamicInSet++] = &binding;
				}
			}
			std::sort(dynamic.begin(), dynamic.begin() + dynamicInSet, [](const auto* a, const auto* b) { return a->binding < b->binding; });
			for (size_t d = 0; d < dynamicInSet; ++d) {
				for (uint32_t element = 0; element < dynamic[d]->buffers.size(); ++element) {
					if (dynamicOffsets && nextOffset < dynamicOffsetCount && rec.dynamicCount < rec.dynamic.size()) {
						rec.dynamic[rec.dynamicCount++] = { setIndex, dynamic[d]->binding, element, dynamicOffsets[nextOffset] };
					}
					++nextOffset;
				}
			}
		}
		if (nextOffset != (dynamicOffsets ? dynamicOffsetCount : 0)) {
			RhiLog::error("cmdBindDescriptorSets: %u dynamic offsets for %u dynamic descriptors", dynamicOffsets ? dynamicOffsetCount : 0, nextOffset);
		}
		list->commands.emplace_back(std::move(rec));
	}

	void OpenGLRHI::execBindDescriptorSets(const CmdBindDescriptorSets& bind)
	{
		for (uint8_t setIndex = 0; setIndex < bind.setCount; ++setIndex) {
			if (!bind.sets[setIndex]) {
				continue;
			}
		const auto& bindings = *bind.sets[setIndex];
		const auto dynamicOffset = [&](const GLDescriptorBindingData& b, uint32_t element) -> uint32_t {
			for (uint8_t d = 0; d < bind.dynamicCount; ++d) {
				const auto& entry = bind.dynamic[d];
				if (entry.set == setIndex && entry.binding == b.binding && entry.element == element) {
					return entry.offset;
				}
			}
			return 0;
		};
		// Для OpenGL считаем:
		// texture unit = binding + index in the array, matching how GLSL assigns consecutive units to
		// the elements of a `uniform sampler2D tex[N]` declared at that binding
		// UBO/SSBO = glBindBufferBase(binding + index), same reasoning

		for (const auto& b : bindings)
		{
			switch (b.type)
			{
			case DescriptorType::UniformBuffer:
				for (uint32_t i = 0; i < b.buffers.size(); ++i)
				{
					const auto* glb = getLive(m_buffers, b.buffers[i].buffer.id);
					if (!glb)
						continue;
					glBindBufferBase(GL_UNIFORM_BUFFER, b.binding + i, glb->id);
				}
				break;

			case DescriptorType::StorageBuffer:
				for (uint32_t i = 0; i < b.buffers.size(); ++i)
				{
					const auto* glb = getLive(m_buffers, b.buffers[i].buffer.id);
					if (!glb)
						continue;
					glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b.binding + i, glb->id);
				}
				break;

			case DescriptorType::UniformBufferDynamic:
			case DescriptorType::StorageBufferDynamic:
				for (uint32_t i = 0; i < b.buffers.size(); ++i)
				{
					const auto& info = b.buffers[i];
					const auto* glb = getLive(m_buffers, info.buffer.id);
					if (!glb)
						continue;
					glBindBufferRange(b.type == DescriptorType::UniformBufferDynamic ? GL_UNIFORM_BUFFER : GL_SHADER_STORAGE_BUFFER,
						b.binding + i, glb->id, static_cast<GLintptr>(info.offset + dynamicOffset(b, i)), static_cast<GLsizeiptr>(info.range));
				}
				break;

			case DescriptorType::CombinedImageSampler:
				for (uint32_t i = 0; i < b.images.size(); ++i)
				{
					const auto& img = b.images[i];
					if (const auto* t = getLive(textures, img.texture.id))
					{
						const uint32_t unit = b.binding + i;
						glActiveTexture(GL_TEXTURE0 + unit);
						glBindTexture(ToOpenglTargetType(t->desc.dimension), t->id);

						if (const auto* samp = getLive(samplers, img.sampler.id))
							glBindSampler(unit, samp->id);
					}
				}
				break;
			}
		}
		}
	}

	FramebufferHandle OpenGLRHI::getBackbufferFramebuffer()
	{
		return {};
	}


	PipelineHandle OpenGLRHI::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
	{
		if (desc.shaders.vertex.id == UINT32_MAX ||
			desc.shaders.fragment.id == UINT32_MAX)
		{
			RhiLog::fatal("OpenGLRHI::createGraphicsPipeline: vertex/fragment shader is required");
			return {};
		}

		const GLuint program = linkProgram({ desc.shaders.vertex, desc.shaders.fragment, desc.shaders.geometry,
			desc.shaders.tessControl, desc.shaders.tessEval }, false);
		if (program == 0) {
			return {};
		}

		// Заполняем GLGraphicsPipeline
		GLGraphicsPipeline gp{};
		gp.programId = program;
		gp.desc = desc;

		gp.primitiveGL = ToGLTopology(desc.topology);
		gp.cullModeGL = ToGLCullMode(desc.cullMode);
		gp.frontFaceGL = ToGLFrontFace(desc.frontFace);
		gp.fillModeGL = ToGLFillMode(desc.fillMode);
		if (desc.shaders.tessEval.id != UINT32_MAX) {
			gp.patchControlPoints = (std::max)(1u, desc.patchControlPoints);
			gp.primitiveGL = GL_PATCHES;
		}

		gp.depthTestEnabled = desc.depthStencilState.depthTest;
		gp.depthWriteEnabled = desc.depthStencilState.depthWrite;
		gp.depthFuncGL = ToGLCompareFunc(desc.depthStencilState.depthCompareOp);

		gp.blendEnabled = desc.blend.enabled;
		gp.blendSrcGL = ToGLBlendFactor(desc.blend.srcColor);
		gp.blendDstGL = ToGLBlendFactor(desc.blend.dstColor);
		gp.blendSrcAlphaGL = ToGLBlendFactor(desc.blend.srcAlpha.value_or(desc.blend.srcColor));
		gp.blendDstAlphaGL = ToGLBlendFactor(desc.blend.dstAlpha.value_or(desc.blend.dstColor));


		// ------------ НОВОЕ: создаём VAO и настраиваем layout ------------
		glCreateVertexArrays(1, &gp.vaoId);
		// Bound while it is configured: some drivers (Intel, GL 4.6) apply glVertexArrayAttribIFormat to the
		// currently bound VAO instead of the one named, so an integer attribute such as bone ids stayed a
		// float at offset 0 and every skinned vertex collapsed -- silently, on OpenGL only.
		glBindVertexArray(gp.vaoId);

		const auto& layout = desc.layout;

		// 1) binding'и: stride + divisor
		for (const VertexBindingDesc& b : layout.bindings)
		{
			// stride пока можем взять отсюда, буфер подцепим позже через bindVertexBuffer
			// divisor: instancing
			GLuint divisor = (b.inputRate == VertexInputRate::PerInstance) ? 1u : 0u;
			glVertexArrayBindingDivisor(gp.vaoId, b.binding, divisor);
		}

		// 2) атрибуты: location, binding, формат, offset
		for (const VertexAttributeDesc& a : layout.attributes)
		{
			glEnableVertexArrayAttrib(gp.vaoId, a.location);

			// Привязка атрибута к binding-слоту
			glVertexArrayAttribBinding(gp.vaoId, a.location, a.binding);

			// Формат: float / int / uint – зависит от VertexFormat
			GLenum type = toGLType(a.format);
			GLint  size = toGLSize(a.format);
			GLboolean normalized = a.normalized ? GL_TRUE : GL_FALSE;
			
			if (isInteger(a.format))
			{
				glVertexArrayAttribIFormat(
					gp.vaoId,
					a.location,
					size,
					type,
					a.offset
				);
			}
			else
			{
				glVertexArrayAttribFormat(
					gp.vaoId,
					a.location,
					size,
					type,
					normalized,
					a.offset
				);
			}
		}
		glBindVertexArray(0);

		// -----------------------------------------------------------------

		PipelineHandle handle;
		gp.live = true;
		const auto* vertex = getLive(m_shaders, desc.shaders.vertex.id);
		const auto* fragment = getLive(m_shaders, desc.shaders.fragment.id);
		gp.label = pipelineLabel(vertex ? vertex->desc.name : std::string(), fragment ? fragment->desc.name : std::string());
		handle.id = storeResource(m_pipelines, m_freePipelines, std::move(gp));

		return handle;
	}

	PipelineHandle OpenGLRHI::createComputePipeline(const ComputePipelineDesc& desc)
	{
		if (desc.compute.id == UINT32_MAX) {
			RhiLog::fatal("OpenGLRHI::createComputePipeline: compute shader is required");
			return {};
		}

		const GLShaderModule* mod = getLive(m_shaders, desc.compute.id);
		if (!mod || mod->id == 0) {
			RhiLog::fatal("OpenGLRHI::createComputePipeline: invalid compute shader");
			return {};
		}
		const GLuint program = linkProgram({ desc.compute }, true);
		if (program == 0) {
			return {};
		}

		GLGraphicsPipeline gp{};
		gp.programId = program;
		gp.isCompute = true;
		gp.computeShader = desc.compute;
		gp.label = pipelineLabel(mod->desc.name);
		gp.live = true;

		PipelineHandle handle;
		handle.id = storeResource(m_pipelines, m_freePipelines, std::move(gp));
		return handle;
	}

	GLuint OpenGLRHI::linkProgram(const std::vector<ShaderModuleHandle>& modules, const bool compute)
	{
		const GLuint program = glCreateProgram();
		if (program == 0) {
			RhiLog::fatal("OpenGLRHI: glCreateProgram failed");
			return 0;
		}
		std::vector<GLuint> attached;
		for (const auto handle : modules) {
			const GLShaderModule* mod = handle.id != UINT32_MAX ? getLive(m_shaders, handle.id) : nullptr;
			if (mod && mod->id != 0) {
				glAttachShader(program, mod->id);
				attached.push_back(mod->id);
			}
		}
		glLinkProgram(program);
		GLint linkStatus = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
		if (!linkStatus) {
			GLint logLen = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
			std::string log;
			log.resize(static_cast<size_t>(logLen));
			glGetProgramInfoLog(program, logLen, nullptr, log.data());
			RhiLog::fatal("OpenGLRHI: link failed:\n%s", log.c_str());
			glDeleteProgram(program);
			return 0;
		}
		auto bindUniformBlock = [program](const char* name, GLuint binding) {
			const GLuint index = glGetUniformBlockIndex(program, name);
			if (index != GL_INVALID_INDEX) {
				glUniformBlockBinding(program, index, binding);
			}
		};
		if (!compute) {
			bindUniformBlock("LightSpaceMatrices", 11);
			bindUniformBlock("CurrentCascade", 4);
			bindUniformBlock("PointLightMatrices", 3);
			bindUniformBlock("cascadedShadowBlock", 9);
			bindUniformBlock("pointLightsBlock", 10);
			bindUniformBlock("SharedMatrices", 5);
			bindUniformBlock("DrawData", 1);
		}
		bindUniformBlock("PushConstants", 15);
		for (const GLuint shader : attached) {
			glDetachShader(program, shader);
		}
		return program;
	}

	size_t OpenGLRHI::reloadShaders()
	{
		const RhiHost* h = host();
		if (!h) {
			return 0;
		}
		std::vector<uint32_t> remade;
		for (uint32_t id = 0; id < m_shaders.size(); ++id) {
			auto& shader = m_shaders[id];
			if (!shader.live || shader.desc.name.empty()) {
				continue;
			}
			auto source = h->loadShaderSource(shader.desc.name);
			if (source.empty() || source == shader.desc.source) {
				continue;
			}
			const GLuint fresh = glCreateShader(toGLShaderStage(shader.desc.stage));
			const char* text = source.c_str();
			glShaderSource(fresh, 1, &text, nullptr);
			glCompileShader(fresh);
			GLint success = 0;
			glGetShaderiv(fresh, GL_COMPILE_STATUS, &success);
			if (!success) {
				GLint logLength = 0;
				glGetShaderiv(fresh, GL_INFO_LOG_LENGTH, &logLength);
				std::string log(static_cast<size_t>((std::max)(logLength, 1)), '\0');
				glGetShaderInfoLog(fresh, logLength, nullptr, log.data());
				RhiLog::error("OpenGLRHI: %s does not compile; the old one stays:\n%s", shader.desc.name.c_str(), log.c_str());
				glDeleteShader(fresh);
				continue;
			}
			glDeleteShader(shader.id);
			shader.id = fresh;
			shader.desc.source = std::move(source);
			remade.push_back(id);
		}
		if (remade.empty()) {
			return 0;
		}
		const auto uses = [&](const ShaderModuleHandle handle) {
			return handle.id != UINT32_MAX && std::find(remade.begin(), remade.end(), handle.id) != remade.end();
		};
		for (auto& pipeline : m_pipelines) {
			if (!pipeline.live) {
				continue;
			}
			const auto& shaders = pipeline.desc.shaders;
			const bool affected = pipeline.isCompute ? uses(pipeline.computeShader)
				: uses(shaders.vertex) || uses(shaders.fragment) || uses(shaders.geometry) || uses(shaders.tessControl) || uses(shaders.tessEval);
			if (!affected) {
				continue;
			}
			const GLuint program = pipeline.isCompute ? linkProgram({ pipeline.computeShader }, true)
				: linkProgram({ shaders.vertex, shaders.fragment, shaders.geometry, shaders.tessControl, shaders.tessEval }, false);
			if (program == 0) {
				continue;
			}
			glDeleteProgram(pipeline.programId);
			pipeline.programId = program;
			pipeline.uniformLocations.clear();
		}
		// Whatever was bound is bound again by name at the next draw.
		m_currentPipeline = {};
		glUseProgram(0);
		return remade.size();
	}

	void OpenGLRHI::destroyGraphicsPipeline(PipelineHandle pipeline)
	{
		GLGraphicsPipeline* gp = getLive(m_pipelines, pipeline.id);
		if (!gp) {
			return;
		}

		if (m_currentPipeline.id == pipeline.id) {
			m_currentPipeline = {};
			glUseProgram(0);
			glBindVertexArray(0);
		}

		if (gp->programId != 0) {
			glDeleteProgram(gp->programId);
		}
		if (gp->vaoId != 0) {
			glDeleteVertexArrays(1, &gp->vaoId);
		}
		recycleResource(m_pipelines, m_freePipelines, pipeline.id);
	}

	void GLAPIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id,
		GLenum severity, GLsizei length,
		const GLchar* message, const void* userParam)
	{
		// Notifications are chatty driver bookkeeping ("buffer will use VIDEO memory"), and this
		// logger routes errors to sfe_fatal.log -- only report things worth reading.
		if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
			return;
		}
		RhiLog::error("GL DEBUG: %s", message);
	}

	void InitGLDebug()
	{
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(glDebugCallback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE,
			0, nullptr, GL_TRUE);
	}

	void OpenGLRHI::init() {
		if (m_inited) {
			return;
		}

		const RhiSurface surface = hostSurface();
		if (!surface.window) {
			RhiLog::fatal("Try to initialize render without context, forgot to call createWindow?");
			return;
		}
		glfwMakeContextCurrent(surface.window);
		// Created here, on the thread that owns the window: GLFW creates windows on that thread only.
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		m_workerContext = glfwCreateWindow(1, 1, "rhi worker context", nullptr, surface.window);
		glfwDefaultWindowHints();
		glfwMakeContextCurrent(surface.window);
		if (!m_workerContext) {
			RhiLog::error("OpenGLRHI: failed to create the shared worker context");
		}
		RhiLog::check(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)), "Failed to initialize GLAD");
		// GPU-driven rendering draws through glMultiDrawElementsIndirectCount, which is core only from 4.6.
		RhiLog::check(GLAD_GL_VERSION_4_6, "OpenGLRHI: OpenGL 4.6 required, context is %d.%d", GLVersion.major, GLVersion.minor);
		// Intel's Windows driver reports 4.6 but exports the entry point only under its ARB name.
		if (!glad_glMultiDrawElementsIndirectCount) {
			glad_glMultiDrawElementsIndirectCount = reinterpret_cast<PFNGLMULTIDRAWELEMENTSINDIRECTCOUNTPROC>(
				glfwGetProcAddress("glMultiDrawElementsIndirectCountARB"));
		}
		RhiLog::check(glad_glMultiDrawElementsIndirectCount != nullptr, "OpenGLRHI: glMultiDrawElementsIndirectCount is not exported by the driver");
		if (const auto* renderer = glGetString(GL_RENDERER)) {
			m_deviceName = reinterpret_cast<const char*>(renderer);
		}
		m_inited = true;

		// Off by default: GL_DEBUG_OUTPUT_SYNCHRONOUS costs real frame time. Uncomment to get
		// driver diagnostics. It must stay below GLAD -- the entry points are null until then,
		// which is why calling it any earlier crashed on the first glEnable.
		//InitGLDebug();


		glViewport(0, 0, surface.renderWidth, surface.renderHeight);
		glClearColor(0,0,0,0);
		glEnable(GL_CULL_FACE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glClearDepth(1.f);
		// Vulkan cube sampling is always seamless; without this a linear sample near a face edge filters
		// inside one face on OpenGL.
		glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
		glGenQueries(kGpuTimeQueries, mGpuTimeQueries);
		glGenQueries(kGpuTimeQueries * kMaxGpuMarks, &mGpuMarkQueries[0][0]);
	}

	void OpenGLRHI::waitIdle()
	{
		if (m_inited) {
			glFinish();
		}
	}

	void OpenGLRHI::cleanup()
	{
		if (m_inited) {
			if (m_workerContext) {
				std::lock_guard lock(m_workerContextMutex);
				glfwDestroyWindow(m_workerContext);
				m_workerContext = nullptr;
			}
			if (mGpuTimeQueries[0]) {
				glDeleteQueries(kGpuTimeQueries, mGpuTimeQueries);
				for (auto& query : mGpuTimeQueries) {
					query = 0;
				}
			}
			for (auto& fence : mHiddenFrames) {
				if (fence) {
					glDeleteSync(static_cast<GLsync>(fence));
					fence = nullptr;
				}
			}
			if (mGpuMarkQueries[0][0]) {
				glDeleteQueries(kGpuTimeQueries * kMaxGpuMarks, &mGpuMarkQueries[0][0]);
				std::fill(&mGpuMarkQueries[0][0], &mGpuMarkQueries[0][0] + kGpuTimeQueries * kMaxGpuMarks, 0u);
			}
			// Freed with every other buffer below.
			m_transientBuffer = {};
			m_transientData.clear();
			m_transientUsed = 0;
			m_transientOverflow.clear();
			m_pendingBufferDestroys.clear();
			for (auto& gp : m_pipelines) {
				if (!gp.live) {
					continue;
				}
				if (gp.programId != 0) {
					glDeleteProgram(gp.programId);
				}
				if (gp.vaoId != 0) {
					glDeleteVertexArrays(1, &gp.vaoId);
				}
			}
			for (auto& fb : framebuffers) {
				if (fb.live && fb.id != 0) {
					glDeleteFramebuffers(1, &fb.id);
				}
			}
			if (m_presentReadGlFbo != 0) {
				glDeleteFramebuffers(1, &m_presentReadGlFbo);
				m_presentReadGlFbo = 0;
				m_presentReadTexId = UINT32_MAX;
			}
			for (auto& tex : textures) {
				if (tex.live && tex.id != 0) {
					glDeleteTextures(1, &tex.id);
				}
			}
			for (auto& samp : samplers) {
				if (samp.live && samp.id != 0) {
					glDeleteSamplers(1, &samp.id);
				}
			}
			for (auto& buf : m_buffers) {
				if (buf.live && buf.id != 0) {
					glDeleteBuffers(1, &buf.id);
				}
			}
			for (auto& sh : m_shaders) {
				if (sh.live && sh.id != 0) {
					glDeleteShader(sh.id);
				}
			}
			if (m_pushConstantUBO != 0) {
				glDeleteBuffers(1, &m_pushConstantUBO);
				m_pushConstantUBO = 0;
			}
		}

		textures.clear();
		samplers.clear();
		framebuffers.clear();
		m_buffers.clear();
		m_vertexArrays.clear();
		m_shaders.clear();
		m_pipelines.clear();
		m_renderPasses.clear();
		m_descriptorSets.clear();
		m_descriptorSetLayouts.clear();
		m_commandLists.clear();

		m_freeTextures.clear();
		m_freeSamplers.clear();
		m_freeFramebuffers.clear();
		m_freeBuffers.clear();
		m_freeShaders.clear();
		m_freePipelines.clear();
		m_freeRenderPasses.clear();
		m_freeDescriptorSets.clear();
		m_freeDescriptorSetLayouts.clear();
		m_freeCommandLists.clear();

		m_currentPipeline = {};
		m_prevFramebuffer = 0;
		m_inited = false;
	}

	FramebufferHandle OpenGLRHI::createFramebuffer(const FramebufferDesc& desc)
	{
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		// COLOR attachments
		std::vector<GLenum> drawBuffers;
		drawBuffers.reserve(desc.colorAttachments.size());

		RhiLog::check(desc.colorAttachments.size() <= GL_MAX_COLOR_ATTACHMENTS,"Too many color attachments");

		for (uint32_t i = 0; i < desc.colorAttachments.size(); ++i) {
			const auto& att = desc.colorAttachments[i];
			const GLTexture* tex = getLive(textures, att.texture.id);
			if (!tex) {
				RhiLog::fatal("Failed to create framebuffer object!");
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDeleteFramebuffers(1, &fbo);
				return {};
			}

			GLenum attachment = GL_COLOR_ATTACHMENT0 + i;
			drawBuffers.push_back(attachment);

			switch (tex->desc.dimension) {
				case TextureDimension::Tex2D:
					glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
						tex->desc.sampleCount > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
						tex->id, static_cast<GLint>(att.mipLevel));
					break;
				case TextureDimension::Array:
				case TextureDimension::Tex3D:
					// Arrays used to fall through to nothing here: the attachment was silently
					// skipped, the framebuffer came out incomplete and creation failed. The depth
					// path below always handled them; colour targets get the same treatment.
					if (att.layer == UINT32_MAX) {
						glFramebufferTexture(GL_FRAMEBUFFER, attachment, tex->id, static_cast<GLint>(att.mipLevel));
					}
					else {
						glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, tex->id,
							static_cast<GLint>(att.mipLevel), static_cast<GLint>(att.layer));
					}
					break;
				default:;
			}
		}

		if (!drawBuffers.empty()) {
			glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
		}
		else {
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
		}

		if (desc.depthStencilAttachment.has_value()) {
			const auto& att = *desc.depthStencilAttachment;
			const GLTexture* tex = getLive(textures, att.texture.id);
			if (!tex) {
				RhiLog::fatal("Failed to create framebuffer object!");
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDeleteFramebuffers(1, &fbo);
				return {};
			}

			switch (tex->desc.dimension) {
			case TextureDimension::Tex2D:
				glFramebufferTexture2D(GL_FRAMEBUFFER, ToGLAttachmentFormat(tex->desc.format),
					tex->desc.sampleCount > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
					tex->id, static_cast<GLint>(att.mipLevel));
				break;
			case TextureDimension::Array:
			case TextureDimension::Tex3D:
				if (att.layer == UINT32_MAX) {
					glFramebufferTexture(GL_FRAMEBUFFER,
						ToGLAttachmentFormat(tex->desc.format),
						tex->id,
						static_cast<GLint>(att.mipLevel));
				}
				else {
					glFramebufferTextureLayer(GL_FRAMEBUFFER,
						ToGLAttachmentFormat(tex->desc.format),
						tex->id,
						static_cast<GLint>(att.mipLevel),
						static_cast<GLint>(att.layer));
				}
				break;
			default:;
			}
		}

		if (const GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER); fbStatus != GL_FRAMEBUFFER_COMPLETE) {
			RhiLog::fatal("Failed to create framebuffer object! status=0x%x", fbStatus);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &fbo);
			return {};
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		GLFramebuffer glfb{};
		glfb.id = fbo;
		glfb.desc = desc;
		glfb.live = true;

		FramebufferHandle handle;
		handle.id = storeResource(framebuffers, m_freeFramebuffers, std::move(glfb));
		return handle;
	}

	void OpenGLRHI::destroyFramebuffer(FramebufferHandle framebuffer)
	{
		GLFramebuffer* fb = getLive(framebuffers, framebuffer.id);
		if (!fb) {
			return;
		}
		if (m_prevFramebuffer == fb->id) {
			m_prevFramebuffer = 0;
		}
		if (fb->id != 0) {
			glDeleteFramebuffers(1, &fb->id);
		}
		recycleResource(framebuffers, m_freeFramebuffers, framebuffer.id);
	}

	uint64_t OpenGLRHI::getNativeTextureId(TextureHandle tex) const
	{
		const GLTexture* t = getLive(textures, tex.id);
		return t ? t->id : 0;
	}

	void OpenGLRHI::updateTexture(
		TextureHandle tex,
		uint32_t x, uint32_t y,
		uint32_t w, uint32_t h,
		TextureFormat fmt,
		const void* data,
		uint32_t mip,
		uint32_t layer)
	{
		const GLTexture* t = getLive(textures, tex.id);
		if (!data || !t) {
			return;
		}
		
		GLenum target = ToOpenglTargetType(t->desc.dimension);

		glBindTexture(target, t->id);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		if (t->desc.dimension == TextureDimension::Array) {
			glTexSubImage3D(target, static_cast<GLint>(mip), x, y, static_cast<GLint>(layer), w, h, 1, ToGLFormat(fmt), ToGLType(fmt), data);
			return;
		}
		glTexSubImage2D(
			target,
			static_cast<GLint>(mip),
			x, y,
			w, h,
			ToGLFormat(fmt),
			ToGLType(fmt),
			data);
	}

	void OpenGLRHI::updateCubeFace(TextureHandle tex, uint32_t face, uint32_t mip, const void* data)
	{
		const GLTexture* t = getLive(textures, tex.id);
		if (!data || !t || t->desc.dimension != TextureDimension::Cube || face > 5) {
			return;
		}

		uint32_t w = t->desc.width >> mip;
		uint32_t h = t->desc.height >> mip;
		if (w < 1) {
			w = 1;
		}
		if (h < 1) {
			h = 1;
		}

		glBindTexture(GL_TEXTURE_CUBE_MAP, t->id);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(
			GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLint>(face),
			static_cast<GLint>(mip),
			0, 0,
			static_cast<GLsizei>(w),
			static_cast<GLsizei>(h),
			ToGLFormat(t->desc.format),
			ToGLType(t->desc.format),
			data);
	}


	ShaderModuleHandle OpenGLRHI::createShaderModule(const ShaderModuleDesc& desc)
	{
		GLenum glStage = toGLShaderStage(desc.stage);
		if (glStage == 0) {
			RhiLog::fatal("OpenGLRHI: Unsupported shader stage");
			return {};
		}

		GLuint shader = glCreateShader(glStage);
		if (shader == 0) {
			RhiLog::fatal("OpenGLRHI: glCreateShader failed");
			return {};
		}

		const char* src = desc.source.c_str();
		glShaderSource(shader, 1, &src, nullptr);
		glCompileShader(shader);

		// Проверка ошибок компиляции
		GLint success = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
		if (!success)
		{
			GLint logLength = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);

			std::string log;
			log.resize(logLength);
			glGetShaderInfoLog(shader, logLength, nullptr, log.data());

			RhiLog::fatal("OpenGLRHI: Shader '%s' compilation failed:\n%s",
				desc.name.c_str(),
				log.c_str()
			);

			glDeleteShader(shader);
			return {};
		}

		// Создаем handle
		GLShaderModule mod{};
		mod.id = shader;
		mod.desc = desc;
		mod.live = true;

		ShaderModuleHandle handle;
		handle.id = storeResource(m_shaders, m_freeShaders, std::move(mod));
		return handle;
	}

	void OpenGLRHI::destroyShaderModule(ShaderModuleHandle handle)
	{
		GLShaderModule* mod = getLive(m_shaders, handle.id);
		if (!mod) {
			return;
		}

		if (mod->id != 0) {
			glDeleteShader(mod->id);
		}
		recycleResource(m_shaders, m_freeShaders, handle.id);
	}

	TextureHandle OpenGLRHI::createTexture(const TextureDesc& desc, const void* data)
	{
		GLuint tex;
		
		const bool msaa = desc.sampleCount > 1 && desc.dimension == TextureDimension::Tex2D;
		auto target = msaa ? GL_TEXTURE_2D_MULTISAMPLE : ToOpenglTargetType(desc.dimension);

		glGenTextures(1, &tex);
		glBindTexture(target, tex);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		if (msaa) {
			glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE,
				static_cast<GLsizei>(desc.sampleCount),
				ToGLInternalFormat(desc.format),
				static_cast<GLsizei>(desc.width),
				static_cast<GLsizei>(desc.height),
				GL_TRUE);
		}
		else if (desc.dimension == TextureDimension::Tex2D) {
			glTexImage2D(target,
				0,
				ToGLInternalFormat(desc.format),
				static_cast<GLsizei>(desc.width),
				static_cast<GLsizei>(desc.height),
				0,
				ToGLFormat(desc.format),
				ToGLType(desc.format),
				data);
			if (!data && desc.mipLevels > 1) {
				// Nothing to filter from, so the caller means to write the levels itself: they only need to exist.
				for (uint32_t mip = 1; mip < desc.mipLevels; ++mip) {
					const GLsizei mw = static_cast<GLsizei>((std::max)(1u, desc.width >> mip));
					const GLsizei mh = static_cast<GLsizei>((std::max)(1u, desc.height >> mip));
					glTexImage2D(target, static_cast<GLint>(mip), ToGLInternalFormat(desc.format), mw, mh, 0,
						ToGLFormat(desc.format), ToGLType(desc.format), nullptr);
				}
			}
			if (data && desc.mipLevels > 1) {
				const bool depth = desc.format == TextureFormat::DEPTH_COMPONENT16
					|| desc.format == TextureFormat::DEPTH_COMPONENT24
					|| desc.format == TextureFormat::DEPTH_COMPONENT32
					|| desc.format == TextureFormat::DEPTH_COMPONENT32F
					|| desc.format == TextureFormat::DEPTH24_STENCIL8
					|| desc.format == TextureFormat::DEPTH32F_STENCIL8;
				if (!depth) {
					glGenerateMipmap(target);
				}
			}
		}
		else if (desc.dimension == TextureDimension::Cube) {
			const int levels = desc.mipLevels > 0 ? static_cast<int>(desc.mipLevels) : 1;
			for (int mip = 0; mip < levels; ++mip) {
				GLsizei w = static_cast<GLsizei>(desc.width >> mip);
				GLsizei h = static_cast<GLsizei>(desc.height >> mip);
				if (w < 1) {
					w = 1;
				}
				if (h < 1) {
					h = 1;
				}
				for (int face = 0; face < 6; ++face) {
					glTexImage2D(
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
						mip,
						ToGLInternalFormat(desc.format),
						w, h,
						0,
						ToGLFormat(desc.format),
						ToGLType(desc.format),
						nullptr
					);
				}
			}
		}
		else if (desc.dimension == TextureDimension::Array) {
			const GLsizei layers = static_cast<GLsizei>(desc.depth > 1 ? desc.depth : desc.arrayLayers);
			const GLsizei levels = desc.mipLevels > 0 ? static_cast<GLsizei>(desc.mipLevels) : 1;
			glTexStorage3D(target, levels, static_cast<GLenum>(ToGLInternalFormat(desc.format)),
				static_cast<GLsizei>(desc.width),
				static_cast<GLsizei>(desc.height),
				layers);
			if (data) {
				glTexSubImage3D(target, 0, 0, 0, 0,
					static_cast<GLsizei>(desc.width),
					static_cast<GLsizei>(desc.height),
					layers,
					ToGLFormat(desc.format),
					ToGLType(desc.format),
					data);
			}
		}
		else {
			glTexImage3D(target,
				0,
				ToGLInternalFormat(desc.format),
				static_cast<GLsizei>(desc.width),
				static_cast<GLsizei>(desc.height),
				static_cast<GLsizei>(desc.depth),
				0,
				ToGLFormat(desc.format),
				ToGLType(desc.format),
				data);
		}

		if (!msaa && (desc.format == TextureFormat::DEPTH24_STENCIL8 || desc.format == TextureFormat::DEPTH32F_STENCIL8))
		{
			glTextureParameteri(tex, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
		}

		if (!msaa) {
			GLint maxLevel = desc.mipLevels > 1 ? static_cast<GLint>(desc.mipLevels - 1) : 0;
			const bool colorMap = data != nullptr && desc.dimension == TextureDimension::Tex2D;
			glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
			glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, maxLevel);
			glTexParameteri(target, GL_TEXTURE_MIN_FILTER, maxLevel > 0 ? GL_LINEAR_MIPMAP_LINEAR : (colorMap ? GL_LINEAR : GL_NEAREST));
			glTexParameteri(target, GL_TEXTURE_MAG_FILTER, colorMap ? GL_LINEAR : GL_NEAREST);
			glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		}

		glBindTexture(target, 0);

		GLTexture glt{};
		glt.id = tex;
		glt.desc = desc;
		glt.live = true;
		trackTextureMemory(desc, true);

		TextureHandle handle;
		handle.id = storeResource(textures, m_freeTextures, std::move(glt));
		return handle;
	}

	void OpenGLRHI::destroyTexture(TextureHandle texture)
	{
		GLTexture* t = getLive(textures, texture.id);
		if (!t) {
			return;
		}
		if (t->id != 0) {
			glDeleteTextures(1, &t->id);
		}
		trackTextureMemory(t->desc, false);
		recycleResource(textures, m_freeTextures, texture.id);
		// The slot comes back as the next texture made, the picture of another size among them: present puts that one
		// on its read framebuffer, not the one deleted here and still held by it.
		if (texture.id == m_presentReadTexId) {
			m_presentReadTexId = UINT32_MAX;
		}
	}

	SamplerHandle OpenGLRHI::createSampler(const SamplerDesc& desc)
	{
		GLuint samplerId;
		glGenSamplers(1, &samplerId);
		if (samplerId == 0) {
			return {};
		}

		auto toGlFilter = [](FilterMode f, bool isMin) -> GLint
		{
			switch (f) {
			case FilterMode::Nearest:
				return isMin ? GL_NEAREST : GL_NEAREST;
			case FilterMode::LinearMipmapLinear:
				return isMin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
			case FilterMode::Linear:
			default:
				return isMin ? GL_LINEAR : GL_LINEAR;
			}
		};

		auto toGlAddress = [](AddressMode m) -> GLint
		{
			switch (m) {
			case AddressMode::Repeat:				return GL_REPEAT;
			case AddressMode::ClampToEdge:			return GL_CLAMP_TO_EDGE;
			case AddressMode::MirrorRepeat:			return GL_MIRRORED_REPEAT;
			case AddressMode::ClampToBorder:		return GL_CLAMP_TO_BORDER;
			case AddressMode::MirrorClampToEdge:	return GL_MIRROR_CLAMP_TO_EDGE;
			default:								return GL_CLAMP_TO_EDGE;
			}
		};


	
					/*{BASE_LEVEL, baseLevel},

					{MAX_LEVEL, maxLevel},

					{SWIZZLE_R, static_cast<int>(swizzle.R)},
					{SWIZZLE_G, static_cast<int>(swizzle.G)},
					{SWIZZLE_B, static_cast<int>(swizzle.B)},
					{SWIZZLE_A, static_cast<int>(swizzle.A)},*/


		// filtering
		glSamplerParameteri(samplerId, GL_TEXTURE_MIN_FILTER, toGlFilter(desc.minFilter, true));
		glSamplerParameteri(samplerId, GL_TEXTURE_MAG_FILTER, toGlFilter(desc.magFilter, false));

		// addressing
		glSamplerParameteri(samplerId, GL_TEXTURE_WRAP_S, toGlAddress(desc.addressU));
		glSamplerParameteri(samplerId, GL_TEXTURE_WRAP_T, toGlAddress(desc.addressV));
		glSamplerParameteri(samplerId, GL_TEXTURE_WRAP_R, toGlAddress(desc.addressW));

		if (desc.maxAnisotropy > 1.f) {
			GLfloat cap = 1.f;
			glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &cap);
			float aniso = desc.maxAnisotropy;
			if (aniso > cap) {
				aniso = cap;
			}
			glSamplerParameterf(samplerId, GL_TEXTURE_MAX_ANISOTROPY, aniso);
		}

		// comparison
		if (desc.compareOp == CompareOp::None) {
			glSamplerParameteri(samplerId, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		}
		else {
			glSamplerParameteri(samplerId, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
			glSamplerParameteri(samplerId, GL_TEXTURE_COMPARE_FUNC, ToGLCompareFunc(desc.compareOp));
		}


		// if (desc.enableAnisotropy) {
		//     glSamplerParameterf(handle.id, GL_TEXTURE_MAX_ANISOTROPY_EXT, desc.maxAnisotropy);
		// }
		// glSamplerParameterf(handle.id, GL_TEXTURE_MIN_LOD, desc.minLod);
		// glSamplerParameterf(handle.id, GL_TEXTURE_MAX_LOD, desc.maxLod);
		// glSamplerParameterf(handle.id, GL_TEXTURE_LOD_BIAS, desc.mipLodBias);
		//
		// Если есть ClampToBorder и свой цвет бордера:
		// if (desc.addressU == AddressMode::ClampToBorder ||
		//     desc.addressV == AddressMode::ClampToBorder ||
		//     desc.addressW == AddressMode::ClampToBorder) {
		//     glSamplerParameterfv(handle.id, GL_TEXTURE_BORDER_COLOR, desc.borderColor.data());
		// }

		SamplerHandle handle;
		GLSampler gls{};
		gls.id = samplerId;
		gls.desc = desc;
		gls.live = true;
		handle.id = storeResource(samplers, m_freeSamplers, std::move(gls));
		return handle;
	}

	void OpenGLRHI::destroySampler(SamplerHandle sampler)
	{
		GLSampler* s = getLive(samplers, sampler.id);
		if (!s) {
			return;
		}
		if (s->id != 0) {
			glDeleteSamplers(1, &s->id);
		}
		recycleResource(samplers, m_freeSamplers, sampler.id);
	}
	static GLbitfield ToGLMask(BlitMask mask)
	{
		GLbitfield bits = 0;
		if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(BlitMask::Color))
			bits |= GL_COLOR_BUFFER_BIT;
		if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(BlitMask::Depth))
			bits |= GL_DEPTH_BUFFER_BIT;
		if (static_cast<uint32_t>(mask) & static_cast<uint32_t>(BlitMask::Stencil))
			bits |= GL_STENCIL_BUFFER_BIT;
		return bits;
	}

	static GLenum ToGLFilter(BlitFilter f)
	{
		switch (f) {
		case BlitFilter::Linear:  return GL_LINEAR;
		case BlitFilter::Nearest:
		default:                  return GL_NEAREST;
		}
	}

	void OpenGLRHI::cmdBlit(CommandListHandle cmd, FramebufferHandle src, FramebufferHandle dst, BlitMask mask, BlitFilter filter)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdBlit{ src, dst, mask, filter });
		}
	}

	void OpenGLRHI::cmdSetScissor(CommandListHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdSetScissor{ x, y, width, height });
		}
	}

	void OpenGLRHI::execSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(0, 0, 0, 0);
			return;
		}
		const int fbH = m_currentPassHeight > 0 ? m_currentPassHeight : 1;
		int glX = x;
		int glY = fbH - y - static_cast<int>(height);
		int glW = static_cast<int>(width);
		int glH = static_cast<int>(height);
		if (glX < 0) {
			glW += glX;
			glX = 0;
		}
		if (glY < 0) {
			glH += glY;
			glY = 0;
		}
		if (glW < 0) {
			glW = 0;
		}
		if (glH < 0) {
			glH = 0;
		}
		glEnable(GL_SCISSOR_TEST);
		glScissor(glX, glY, glW, glH);
	}

	void OpenGLRHI::execBlit(FramebufferHandle src, FramebufferHandle dst, BlitMask mask, BlitFilter filter)
	{
		auto resolveFb = [this](FramebufferHandle handle, int& w, int& h) -> GLuint {
			if (!handle) {
				const RhiSurface surface = hostSurface();
				if (surface.window) {
					w = surface.backbufferWidth > 0 ? surface.backbufferWidth : surface.renderWidth;
					h = surface.backbufferHeight > 0 ? surface.backbufferHeight : surface.renderHeight;
				}
				return 0;
			}
			const auto* fb = getLive(framebuffers, handle.id);
			if (!fb) {
				w = 0;
				h = 0;
				return 0;
			}
			w = static_cast<int>(fb->desc.width);
			h = static_cast<int>(fb->desc.height);
			return fb->id;
		};

		int srcW = 0, srcH = 0, dstW = 0, dstH = 0;
		const GLuint srcId = resolveFb(src, srcW, srcH);
		const GLuint dstId = resolveFb(dst, dstW, dstH);
		if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
			return;
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcId);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstId);
		glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, dstW, dstH, ToGLMask(mask), ToGLFilter(filter));
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	static GLbitfield ToGLMemoryBarrier(AccessMask access)
	{
		GLbitfield bits = 0;
		if (access & AccessMask::IndirectCommandRead) {
			bits |= GL_COMMAND_BARRIER_BIT;
		}
		if (access & AccessMask::IndexRead) {
			bits |= GL_ELEMENT_ARRAY_BARRIER_BIT;
		}
		if (access & AccessMask::VertexAttributeRead) {
			bits |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
		}
		if (access & AccessMask::UniformRead) {
			bits |= GL_UNIFORM_BARRIER_BIT;
		}
		if (access & AccessMask::ShaderRead) {
			bits |= GL_TEXTURE_FETCH_BARRIER_BIT
				| GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
				| GL_SHADER_STORAGE_BARRIER_BIT
				| GL_UNIFORM_BARRIER_BIT
				| GL_FRAMEBUFFER_BARRIER_BIT;
		}
		if (access & AccessMask::ShaderWrite) {
			bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT;
		}
		if ((access & AccessMask::ColorAttachmentRead) || (access & AccessMask::ColorAttachmentWrite)
			|| (access & AccessMask::DepthStencilAttachmentRead) || (access & AccessMask::DepthStencilAttachmentWrite)) {
			bits |= GL_FRAMEBUFFER_BARRIER_BIT;
		}
		if ((access & AccessMask::TransferRead) || (access & AccessMask::TransferWrite)) {
			bits |= GL_TEXTURE_UPDATE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT;
		}
		if ((access & AccessMask::HostRead) || (access & AccessMask::HostWrite)) {
			bits |= GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
		}
		return bits;
	}

	void OpenGLRHI::cmdPipelineBarrier(CommandListHandle cmd, const PipelineBarrier& barrier)
	{
		if (GLCommandList* list = recordingList(cmd)) {
			list->commands.emplace_back(CmdPipelineBarrier{ barrier });
		}
	}

	void OpenGLRHI::writeGpuMark(CommandListHandle cmd, const char* label, const bool endsPass)
	{
		if (GLCommandList* list = recordingList(cmd); list && mGpuMarkQueries[0][0]) {
			list->commands.emplace_back(CmdGpuMark{ label, endsPass });
		}
	}

	void OpenGLRHI::execPipelineBarrier(const PipelineBarrier& barrier)
	{
		const GLbitfield bits = ToGLMemoryBarrier(barrier.dstAccess);
		if (bits == 0) {
			return;
		}
		glMemoryBarrier(bits);
	}

	BufferHandle OpenGLRHI::createBuffer(const BufferDesc& desc, const void* initialData)
	{
		GLBuffer glb{};
		glb.desc = desc;
		glb.target = toGLBufferTarget(desc.usage);

		glGenBuffers(1, &glb.id);
		if (glb.id == 0)
		{
			BufferHandle invalid{};
			return invalid;
		}

		glBindBuffer(glb.target, glb.id);
		GLenum usage = toGLAccessFlags(desc.cpuAccess);
		glBufferData(glb.target, static_cast<GLsizeiptr>(desc.size), initialData, usage);
		glBindBuffer(glb.target, 0);

		glb.live = true;
		trackBufferMemory(desc, true);

		BufferHandle handle;
		handle.id = storeResource(m_buffers, m_freeBuffers, std::move(glb));
		return handle;
	}

	void OpenGLRHI::destroyBuffer(BufferHandle handle)
	{
		GLBuffer* glb = getLive(m_buffers, handle.id);
		if (!glb) {
			return;
		}
		if (m_openCommandList) {
			// The open list may already name it, and names are resolved when the list is replayed: the buffer and
			// its id stay until the submit, or a buffer created meanwhile could take the id and be bound instead.
			if (std::find(m_pendingBufferDestroys.begin(), m_pendingBufferDestroys.end(), handle.id) == m_pendingBufferDestroys.end()) {
				m_pendingBufferDestroys.push_back(handle.id);
			}
			return;
		}

		if (glb->id != 0)
		{
			glDeleteBuffers(1, &glb->id);
		}
		trackBufferMemory(glb->desc, false);
		recycleResource(m_buffers, m_freeBuffers, handle.id);
	}

	void OpenGLRHI::updateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
	{
		execUpdateBuffer(handle, offset, size, data);
	}

	void OpenGLRHI::cmdUpdateBuffer(CommandListHandle cmd, BufferHandle handle, size_t offset, size_t size, const void* data)
	{
		if (!recordingList(cmd)) {
			return;
		}
		execUpdateBuffer(handle, offset, size, data);
	}

	void OpenGLRHI::cmdCopyBuffer(CommandListHandle cmd, BufferHandle src, BufferHandle dst,
		size_t srcOffset, size_t dstOffset, size_t size)
	{
		if (!recordingList(cmd) || size == 0) {
			return;
		}
		const GLBuffer* s = getLive(m_buffers, src.id);
		const GLBuffer* d = getLive(m_buffers, dst.id);
		if (!s || !d) {
			return;
		}
		if (srcOffset + size > s->desc.size || dstOffset + size > d->desc.size) {
			RhiLog::error("OpenGLRHI: cmdCopyBuffer OOB src=%zu dst=%zu size=%zu", srcOffset, dstOffset, size);
			return;
		}
		glBindBuffer(GL_COPY_READ_BUFFER, s->id);
		glBindBuffer(GL_COPY_WRITE_BUFFER, d->id);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
			static_cast<GLintptr>(srcOffset), static_cast<GLintptr>(dstOffset), static_cast<GLsizeiptr>(size));
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	}

	void OpenGLRHI::execUpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
	{
		GLBuffer* glb = getLive(m_buffers, handle.id);
		if (!glb || glb->id == 0 || data == nullptr || size == 0) {
			return;
		}

		glNamedBufferSubData(glb->id,
			static_cast<GLintptr>(offset),
			static_cast<GLsizeiptr>(size),
			data);
	}

	TransientAllocation OpenGLRHI::allocTransient(size_t size)
	{
		if (size == 0) {
			return {};
		}
		if (!getLive(m_buffers, m_transientBuffer.id)) {
			GLint uniformAlign = 0;
			GLint storageAlign = 0;
			glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniformAlign);
			glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlign);
			m_transientAlign = (std::max)({ size_t{ 16 }, static_cast<size_t>(uniformAlign), static_cast<size_t>(storageAlign) });
			size_t capacity = size_t{ 1 } << 20;
			while (capacity < m_transientPeak) {
				capacity *= 2;
			}
			BufferDesc desc;
			desc.size = capacity;
			desc.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index;
			desc.cpuAccess = CpuAccess::Write;
			desc.debugName = "Transient";
			m_transientBuffer = createBuffer(desc, nullptr);
			m_transientData.assign(m_transientBuffer ? capacity : 0, 0);
			m_transientUsed = 0;
		}

		const size_t offset = (m_transientUsed + m_transientAlign - 1) & ~(m_transientAlign - 1);
		m_transientPeak = (std::max)(m_transientPeak, offset + size);
		if (offset + size > m_transientData.size()) {
			BufferDesc desc;
			desc.size = size;
			desc.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index;
			desc.cpuAccess = CpuAccess::Write;
			desc.debugName = "Transient overflow";
			auto& overflow = m_transientOverflow.emplace_back();
			overflow.buffer = createBuffer(desc, nullptr);
			overflow.data.assign(size, 0);
			return { overflow.buffer, 0, overflow.data.data() };
		}
		m_transientUsed = offset + size;
		return { m_transientBuffer, static_cast<uint32_t>(offset), m_transientData.data() + offset };
	}

	void OpenGLRHI::uploadTransient()
	{
		if (m_transientUsed > 0) {
			execUpdateBuffer(m_transientBuffer, 0, m_transientUsed, m_transientData.data());
		}
		for (const auto& overflow : m_transientOverflow) {
			execUpdateBuffer(overflow.buffer, 0, overflow.data.size(), overflow.data.data());
		}
	}

	void OpenGLRHI::resetTransient()
	{
		m_transientUsed = 0;
		for (const auto& overflow : m_transientOverflow) {
			destroyBuffer(overflow.buffer);
		}
		m_transientOverflow.clear();
		if (m_transientBuffer && m_transientPeak > m_transientData.size()) {
			// Too small for the list just replayed: the next allocation makes one sized to the peak.
			destroyBuffer(m_transientBuffer);
			m_transientBuffer = {};
		}
	}

	void* OpenGLRHI::mapBuffer(BufferHandle handle, MapMode mode)
	{
		GLBuffer* glb = getLive(m_buffers, handle.id);
		if (!glb || glb->id == 0) {
			return nullptr;
		}

		glBindBuffer(glb->target, glb->id);

		GLbitfield flags = toGLMapFlags(mode, glb->desc.cpuAccess);

		void* ptr = glMapBufferRange(
			glb->target,
			0,
			static_cast<GLsizeiptr>(glb->desc.size),
			flags
		);

		// Можно оставить привязанным или отвязать — в зависимости от стиля
		glBindBuffer(glb->target, 0);

		return ptr;
	}

	void OpenGLRHI::unmapBuffer(BufferHandle handle)
	{
		GLBuffer* glb = getLive(m_buffers, handle.id);
		if (!glb || glb->id == 0) {
			return;
		}

		glBindBuffer(glb->target, glb->id);
		glUnmapBuffer(glb->target);
		glBindBuffer(glb->target, 0);
	}
}

#include "DebugPass.h"

#include "assetsModule/shaderModule/ShaderBase.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/modelModule/Vertex.h"
#include "componentsModule/CascadeShadowComponent.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/GizmoComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "systemsModule/SystemManager.h"

#include <algorithm>


namespace SFE::Render::RenderPasses {
	DebugPass::DebugPass() = default;

	void DebugPass::init(SystemsModule::RenderData&) {
		auto rhi = Engine::instance()->rhi();

		RenderPassDesc rpd;
		rpd.colorAttachmentCount = 1;
		rpd.colorAttachments[0].loadOp = AttachmentLoadOp::Load;
		rpd.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
		rpd.colorAttachments[0].format = TextureFormat::RGBA8;
		debugRenderPass = rhi->createRenderPass(rpd);


		VertexLayoutDesc lineLayout;
		lineLayout.bindings.push_back({ 0, sizeof(Math::Vec3), VertexInputRate::PerVertex });
		lineLayout.attributes.push_back({
			0,
			0,
			VertexFormat::Float3,
			0,
			false
			});


		VertexLayoutDesc triLayout;
		triLayout.bindings.push_back({ 0, sizeof(SFE::Vertex3D), VertexInputRate::PerVertex });

		triLayout.attributes.push_back({
			0,
			0,
			VertexFormat::Float3,
			static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, position)),
			false
			});

		triLayout.attributes.push_back({
			1,
			0,
			VertexFormat::Float3,
			static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, normal)),
			false
			});

		triLayout.attributes.push_back({
			2,
			0,
			VertexFormat::Float4,
			static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, color)),
			false
			});

		GraphicsPipelineDesc debugPipelineDesc{};
		debugPipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/simpleColoredTriangle.vs");
		debugPipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/simpleColoredTriangle.fs");
		debugPipelineDesc.topology = PrimitiveTopology::Triangles;
		debugPipelineDesc.cullMode = CullMode::None;
		debugPipelineDesc.frontFace = FrontFace::CounterClockWise;
		debugPipelineDesc.fillMode = PolygonFillMode::Fill;
		debugPipelineDesc.depthStencilState.depthTest = false;
		debugPipelineDesc.depthStencilState.depthWrite = false;
		debugPipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
		debugPipelineDesc.blend.enabled = true;
		debugPipelineDesc.blend.srcColor = BlendFactor::SrcAlpha;
		debugPipelineDesc.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
		debugPipelineDesc.layout = triLayout;
		debugPipelineDesc.renderPass = debugRenderPass;
		debugPipelineDesc.pushConstantSize = sizeof(DrawDataTriangles);
		debugPipelineDesc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
		trianglesPipeline = rhi->createGraphicsPipeline(debugPipelineDesc);

		GraphicsPipelineDesc linePipelineDesc;
		linePipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/xyzLines.vs");
		linePipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/colored_lines.fs");
		linePipelineDesc.layout = lineLayout;
		linePipelineDesc.topology = PrimitiveTopology::Lines;
		linePipelineDesc.cullMode = CullMode::None;
		linePipelineDesc.frontFace = FrontFace::CounterClockWise;
		linePipelineDesc.fillMode = PolygonFillMode::Fill;
		linePipelineDesc.depthStencilState.depthTest = false;
		linePipelineDesc.depthStencilState.depthWrite = false;
		linePipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
		linePipelineDesc.blend.enabled = false;
		linePipelineDesc.renderPass = debugRenderPass;
		linePipelineDesc.pushConstantSize = sizeof(DrawDataLines);
		linePipelineDesc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
		linesPipeline = rhi->createGraphicsPipeline(linePipelineDesc);
	}

	DebugPass::~DebugPass() {}

	void DebugPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
		FUNCTION_BENCHMARK;

		if (const auto octreeSys = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
			if (octreeSys->drawOctrees) {
				for (auto& [pos, tree] : octreeSys->mOctrees) {
					(void)pos;
					auto lock = tree.readLock();
					tree.drawOctree(octreeSys->drawObjAABB);
				}
			}
		}

		for (auto [entId, gizmoComp] : ECSHandler::registry().view<ComponentsModule::GizmoComponent>()) {
			gizmoComp->gizmo.setEntity(entId);
			gizmoComp->gizmo.update();
		}

		auto* rhi = Engine::instance()->rhi();
		auto fb = renderDataHandle.frame.framebuffer(FrameId::Final);
		auto* dbg = SystemsModule::RenderDebugInfo::instance();
		if (!fb || (Utils::renderTriangles.empty() && Utils::renderVertices.empty())) {
			Utils::renderTriangles.clear();
			Utils::renderVertices.clear();
			return;
		}

		RenderPassBeginInfo rpBeginInfo;
		rpBeginInfo.renderPass = debugRenderPass;
		rpBeginInfo.framebuffer = fb;
		rpBeginInfo.width = Engine::instance()->getWindow()->getScreenData().renderW;
		rpBeginInfo.height = Engine::instance()->getWindow()->getScreenData().renderH;
		rhi->cmdBeginRenderPass(cmd, rpBeginInfo);

		if (!Utils::renderTriangles.empty() && trianglesPipeline) {
			drawDataTriangles.pv = renderDataHandle.current.PV;
			drawDataTriangles.viewPos = Math::Vec4(renderDataHandle.mCameraPos, 1.f);
			const size_t triStride = sizeof(Utils::Triangle);
			rhi->cmdBindPipeline(cmd, trianglesPipeline);
			rhi->cmdPushConstants(cmd, trianglesPipeline, drawDataTriangles);
			for (auto& [data, triangles] : Utils::renderTriangles) {
				(void)data;
				if (triangles.empty()) {
					continue;
				}
				const auto vbo = rhi->allocTransient(triangles.size() * triStride);
				if (!vbo) {
					continue;
				}
				std::memcpy(vbo.data, triangles.data(), triangles.size() * triStride);
				rhi->cmdBindVertexBuffer(cmd, 0, vbo.buffer, vbo.offset);
				rhi->cmdDraw(cmd, static_cast<uint32_t>(triangles.size() * 3));
				++dbg->drawCalls;
			}
			Utils::renderTriangles.clear();
		}

		if (!Utils::renderVertices.empty() && linesPipeline) {
			rhi->cmdBindPipeline(cmd, linesPipeline);
			drawDataLines.pv = renderDataHandle.current.PV;
			const size_t vertStride = sizeof(Math::Vec3);
			for (auto& [data, vertices] : Utils::renderVertices) {
				drawDataLines.color = data.color;
				rhi->cmdPushConstants(cmd, linesPipeline, drawDataLines);
				if (vertices.empty()) {
					continue;
				}
				const auto vbo = rhi->allocTransient(vertices.size() * vertStride);
				if (!vbo) {
					continue;
				}
				std::memcpy(vbo.data, vertices.data(), vertices.size() * vertStride);
				rhi->cmdBindVertexBuffer(cmd, 0, vbo.buffer, vbo.offset);
				rhi->cmdDraw(cmd, static_cast<uint32_t>(vertices.size()));
				++dbg->drawCalls;
			}
			Utils::renderVertices.clear();
		}

		rhi->cmdEndRenderPass(cmd);
	}

	PipelineHandle DebugPass::getTrianglePipeline(bool blend, bool depthTest, bool cull, std::pair<BlendFactor, BlendFactor> blendFactors)
	{
		PipelineDesc desc;
		desc.blend = blend;
		desc.depthTest = depthTest;
		desc.cull = cull;
		desc.blendFactors = blendFactors;

		if (!trianglesPipelinesCache.contains(desc)) {
			GraphicsPipelineDesc debugPipelineDesc{};
			auto* rhi = Engine::instance()->rhi();
			debugPipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/simpleColoredTriangle.vs");
			debugPipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/simpleColoredTriangle.fs");
			debugPipelineDesc.renderPass = debugRenderPass;
			debugPipelineDesc.pushConstantSize = sizeof(DrawDataTriangles);
			debugPipelineDesc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;

			debugPipelineDesc.topology = PrimitiveTopology::Triangles;
			debugPipelineDesc.cullMode = cull ? CullMode::Back : CullMode::None;
			debugPipelineDesc.frontFace = FrontFace::CounterClockWise;
			debugPipelineDesc.fillMode = PolygonFillMode::Fill;
			debugPipelineDesc.depthStencilState.depthTest = depthTest;
			debugPipelineDesc.depthStencilState.depthWrite = depthTest;
			debugPipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
			debugPipelineDesc.blend.enabled = blend;
			if (blend) {
				debugPipelineDesc.blend.srcColor = blendFactors.first;
				debugPipelineDesc.blend.dstColor = blendFactors.second;
			}
			debugPipelineDesc.layout = VertexLayoutDesc{};
			debugPipelineDesc.layout.bindings.push_back({ 0, sizeof(SFE::Vertex3D), VertexInputRate::PerVertex });
			debugPipelineDesc.layout.attributes.push_back({
				0,                            // location 0
				0,
				VertexFormat::Float3,         // vec3 position
				static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, position)),
				false                         // normalized
				});
			debugPipelineDesc.layout.attributes.push_back({
				1,                            // location 1
				0,
				VertexFormat::Float3,         // vec3 normal
				static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, normal)),
				true                          // normalized = true
				});
			debugPipelineDesc.layout.attributes.push_back({
				2,                            // location 2
				0,
				VertexFormat::Float4,         // vec4 color
				static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, color)),
				true                          // normalized = true
				});

			trianglesPipelinesCache[desc] = rhi->createGraphicsPipeline(debugPipelineDesc);
		}

		return trianglesPipelinesCache[desc];
	}
}


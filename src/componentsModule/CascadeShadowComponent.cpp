#include "CascadeShadowComponent.h"

#include <algorithm>
#include <cmath>

#include "CameraComponent.h"
#include "TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "mathModule/MatrixOperations.h"
#include "mathModule/Utils.h"
#include "rhi/GraphicsBackend.h"
#include "systemsModule/systems/CameraSystem.h"
#include "windowsModule/Window.h"


namespace SFE::ComponentsModule {
	namespace {
		// How far the furthest of `corners` is from their centre: the sphere round them, the same however they are turned.
		float sphereRadius(const std::vector<Math::Vec4>& corners) {
			Math::Vec3 centre(0.f);
			for (const auto& corner : corners) {
				centre.x += corner.x;
				centre.y += corner.y;
				centre.z += corner.z;
			}
			const float invN = 1.f / static_cast<float>(corners.size());
			centre.x *= invN;
			centre.y *= invN;
			centre.z *= invN;
			float radius = 0.f;
			for (const auto& corner : corners) {
				const float dx = corner.x - centre.x;
				const float dy = corner.y - centre.y;
				const float dz = corner.z - centre.z;
				radius = (std::max)(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
			}
			return radius;
		}

		SFE::Math::Mat4 fitLightSpace(const std::vector<SFE::Math::Vec4>& corners, const SFE::Math::Mat4& lightView, float radius, float nearMultiplier, float farMultiplier, float mapSize, SFE::Math::Mat4* cullingMatrix, float* depthRange, float* worldPerTexelOut);
	}

	std::vector<Math::Vec4> CascadeShadowComponent::getFrustumCornersWorldSpace(const Math::Mat4& proj, const Math::Mat4& view) {
		return getFrustumCornersWorldSpace(proj * view);
	}

	std::vector<Math::Vec4> CascadeShadowComponent::getFrustumCornersWorldSpace(const Math::Mat4& projView) {
		const auto inv = Math::inverse(projView);

		std::vector<Math::Vec4> frustumCorners;
		for (unsigned int x = 0; x < 2; ++x) {
			for (unsigned int y = 0; y < 2; ++y) {
				for (unsigned int z = 0; z < 2; ++z) {
					const Math::Vec4 pt = inv * Math::Vec4(
						2.0f * static_cast<float>(x) - 1.0f,
						2.0f * static_cast<float>(y) - 1.0f,
						2.0f * static_cast<float>(z) - 1.0f,
						1.0f
					);

					frustumCorners.push_back(pt / pt.w);
				}
			}
		}

		return frustumCorners;
	}

	void CascadeShadowComponent::markDirty() {
		mDirty = true;
	}

	void CascadeShadowComponent::calculateLightSpaceMatrices(const MathModule::PerspectiveProjection& projection, const Math::Mat4& view) {
		updateCascades(projection);
		updateLightSpaceMatrices(view);
	}

	const std::vector<Math::Mat4>& CascadeShadowComponent::getLightSpaceMatrices() const {
		return mLightSpaceMatrices;
	}

	void CascadeShadowComponent::updateCascades(const MathModule::PerspectiveProjection& cameraProjection) {
		if (mCameraProjection == cameraProjection && !mDirty) {
			return;
		}
		mCameraProjection = cameraProjection;

		mDirty = false;

		//cascades.clear();
		if (shadowCascadeLevels.empty()) {
			return;
		}

		shadowCascadeLevels.front() = cameraProjection.getNear();
		// The last level is how far shadows reach, and it is authored. It used to be overwritten with
		// 70% of the camera far plane, which ties shadow quality to draw distance: at far = 5000 the
		// outer cascade stretched to 3500 units, so one texel of a 4096 map covered about a metre and
		// distant shadows turned into steps. Draw distance and shadow distance are separate choices.
		const float camFar = Engine::instance()->getWindow()->getScreenData().far;
		const float authoredFar = shadowCascadeLevels.back();
		const float shadowFar = (std::min)(authoredFar > shadowCascadeLevels.front() ? authoredFar : camFar, camFar);
		shadowCascadeLevels.back() = shadowFar;

		// The levels between are as authored, held inside the two ends. They were spread over the whole distance by
		// a formula that overwrote them: the first reached 6 m, and in a sphere round that much of a first person's view
		// a 1024 map has 1.7 cm texels -- every shadow near the eye blurred.
		for (size_t i = 1; i + 1 < shadowCascadeLevels.size(); ++i) {
			shadowCascadeLevels[i] = (std::clamp)(shadowCascadeLevels[i], shadowCascadeLevels.front(), shadowFar);
		}

		auto fov = cameraProjection.getFOV();
		auto aspect = cameraProjection.getAspect();

		cascades.resize(shadowCascadeLevels.size() - 1);

		const float cascadeNear = shadowCascadeLevels.front();
		for (size_t i = 1; i < shadowCascadeLevels.size(); ++i) {
			cascades[i - 1].viewProjection = {
				fov, aspect, cascadeNear, shadowCascadeLevels[i]
			};
		}

	}

	void CascadeShadowComponent::updateLightSpaceMatrices(const Math::Mat4& cameraView) {
		mLightSpaceMatrices.clear();
		if (cascades.empty()) {
			return;
		}

		auto tc = ECSHandler::registry().pinComponent<const TransformComponent>(getEntityId());
		if (!tc) {
			return;
		}

		const auto s = Math::normalize(tc->getRight());
		const auto u = Math::normalize(tc->getUp());
		const auto f = Math::normalize(tc->getForward());

		for (auto& shadowCascade : cascades) {
			const auto fitPts = getFrustumCornersWorldSpace(
				shadowCascade.viewProjection.getProjectionsMatrix(), cameraView);

			Math::Vec3 center(0.f);
			for (const auto& p : fitPts) {
				center.x += p.x;
				center.y += p.y;
				center.z += p.z;
			}
			const float invN = 1.f / static_cast<float>(fitPts.size());
			center.x *= invN;
			center.y *= invN;
			center.z *= invN;

			const Math::Vec3 eye = center - f;
			Math::Mat4 lightView(1);
			lightView[0][0] = s.x;
			lightView[1][0] = s.y;
			lightView[2][0] = s.z;
			lightView[0][1] = u.x;
			lightView[1][1] = u.y;
			lightView[2][1] = u.z;
			lightView[0][2] = -f.x;
			lightView[1][2] = -f.y;
			lightView[2][2] = -f.z;
			// Moved along the light only, not across it: the texels then lie on one grid fixed in the world, which
			// getLightSpaceMatrix snaps the map to. Moved across with the slice's centre, the grid slid under every
			// shadow as the camera moved or turned, and their edges shimmered as the head only breathed.
			lightView[3][0] = 0.f;
			lightView[3][1] = 0.f;
			lightView[3][2] = Math::dot(f, eye);

			// The sphere round the slice from the cascade's projection alone, in the camera's own space: the same to the
			// last bit every frame, and the texel with it. Measured off the slice's corners in the world, a few hundred
			// metres out, it came out a few tenths of a millimetre different whenever the eye drifted; and the grid,
			// counted from the world's origin some thirty thousand texels away, slid two or three texels under every
			// shadow every frame.
			const float radius = sphereRadius(getFrustumCornersWorldSpace(shadowCascade.viewProjection.getProjectionsMatrix()));
			Math::Mat4 cullingMatrix;
			auto projViewMatrix = fitLightSpace(fitPts, lightView, radius, shadowCascade.zMult.x, shadowCascade.zMult.y, resolution.x, &cullingMatrix, &shadowCascade.depthRange, &shadowCascade.worldPerTexel);
			mLightSpaceMatrices.push_back(projViewMatrix);
			shadowCascade.frustum = SFE::FrustumModule::createFrustum(cullingMatrix);
		}
	}

	SFE::Math::Mat4 CascadeShadowComponent::getLightSpaceMatrix(const std::vector<SFE::Math::Vec4>& corners, const SFE::Math::Mat4& lightView, float nearMultiplier, float farMultiplier, float mapSize, SFE::Math::Mat4* cullingMatrix, float* depthRange, float* worldPerTexelOut) {
		return fitLightSpace(corners, lightView, sphereRadius(corners), nearMultiplier, farMultiplier, mapSize, cullingMatrix, depthRange, worldPerTexelOut);
	}

	namespace {
	// The light's box over `corners`: across the light the square round the circle of `radius` (and a margin) about their
	// centre, on the world's grid of its texels; along it their depth, padded.
	SFE::Math::Mat4 fitLightSpace(const std::vector<SFE::Math::Vec4>& corners, const SFE::Math::Mat4& lightView, float radius, float nearMultiplier, float farMultiplier, float mapSize, SFE::Math::Mat4* cullingMatrix, float* depthRange, float* worldPerTexelOut) {
		Math::Vec4 transform = lightView * corners[0];
		auto minX = transform.x;
		auto maxX = transform.x;

		auto minY = transform.y;
		auto maxY = transform.y;

		auto minZ = transform.z;
		auto maxZ = transform.z;

		for (auto i = 1u; i < corners.size(); i++) {
			transform = lightView * corners[i];

			minX = (std::min)(minX, transform.x);
			maxX = (std::max)(maxX, transform.x);

			minY = (std::min)(minY, transform.y);
			maxY = (std::max)(maxY, transform.y);

			minZ = (std::min)(minZ, transform.z);
			maxZ = (std::max)(maxZ, transform.z);
		}

		// The circle round the slice's centre that holds the sphere round its corners: the same size however the camera
		// turns (the corners stay as far from the centre), so a texel keeps its size. Fitted to the slice's outline
		// across the light, it grew and shrank as the head nodded, and the grid it snaps to with it.
		float cx = 0.f;
		float cy = 0.f;
		for (const auto& corner : corners) {
			transform = lightView * corner;
			cx += transform.x;
			cy += transform.y;
		}
		cx /= static_cast<float>(corners.size());
		cy /= static_cast<float>(corners.size());

		// Widened before the texel is sized: the map spans res texels, so a margin added after sizing them left the
		// square 2r wide but starting at cx - 1.15r -- off centre by 0.15r, and the circle the lighting picks the
		// cascade by cut off ground inside the slice on one side, near the camera, where the next cascade had no
		// shadow for it.
		radius *= 1.15f;
		const float res = mapSize > 1.f ? mapSize : 1024.f;
		float worldPerTexel = (radius * 2.f) / res;
		if (worldPerTexel < 1e-6f) {
			worldPerTexel = 1e-6f;
		}

		minX = cx - radius;
		maxX = cx + radius;
		minY = cy - radius;
		maxY = cy + radius;

		minX = std::floor(minX / worldPerTexel) * worldPerTexel;
		minY = std::floor(minY / worldPerTexel) * worldPerTexel;
		maxX = minX + worldPerTexel * res;
		maxY = minY + worldPerTexel * res;

		if (minZ > maxZ) {
			std::swap(minZ, maxZ);
		}
		const float zRange = (std::max)(maxZ - minZ, 1.f);
		const float zNearPad = zRange * (std::clamp)(nearMultiplier, 0.5f, 3.f);
		// The circle reaches past the slice sideways, and so does the ground under it. Ground inside the circle but
		// further along the light than any corner of the slice sat behind the far plane, and its shadow ended on a
		// straight line wherever that plane met the floor -- visible on the last cascade, which has nothing past it
		// to hide the cut. Across a circle of this radius the ground can fall back by up to its diameter.
		const float zFarPad = (std::max)(zRange * (std::clamp)(farMultiplier, 0.5f, 3.f) * 0.25f, radius * 2.f);
		minZ -= zNearPad;
		maxZ += zFarPad;

		if (depthRange) {
			*depthRange = (std::max)(maxZ - minZ, 1e-4f);
		}
		if (worldPerTexelOut) {
			*worldPerTexelOut = worldPerTexel;
		}

		// OpenGL clip Z [-1, 1]. Vulkan RHI matches it via VK_EXT_depth_clip_control.
		const auto ortho = Math::orthoRH_NO(minX, maxX, minY, maxY, minZ, maxZ);
		if (cullingMatrix) {
			*cullingMatrix = ortho * lightView;
		}
		return ortho * lightView;
	}
	}

	const std::vector<Math::Mat4>& CascadeShadowComponent::getCacheLightSpaceMatrices() {
		return mLightMatricesCache;
	}

	void CascadeShadowComponent::cacheMatrices() {
		mLightMatricesCache.clear();
		mLightMatricesCache = getLightSpaceMatrices();
	}

	void CascadeShadowComponent::clearCacheMatrices() {
		mLightMatricesCache.clear();
	}

	void CascadeShadowComponent::debugDraw(const std::vector<SFE::Math::Mat4>& lightSpaceMatrices, const Math::Mat4& cameraProjection, const Math::Mat4& cameraView) {
		/*if (lightSpaceMatrices.empty()) {
			return;
		}

		if (!mInited) {
			mInited = true;
			Render::GraphicsPipelineDesc pipelineDesc;
			pipelineDesc.shaders.vertex = Engine::instance()->rhi()->createShaderModule({Render::ShaderStage::Vertex, "CascadeShadowsDebug_VS", ShaderModule::ShaderBase::loadShaderCode("shaders/debugCascadeShader.vs")});
			pipelineDesc.shaders.fragment = Engine::instance()->rhi()->createShaderModule({ Render::ShaderStage::Fragment, "CascadeShadowsDebug_FS", ShaderModule::ShaderBase::loadShaderCode("shaders/debugCascadeShader.fs") });
			pipelineDesc.depthStencilState.depthTest = true;
			pipelineDesc.depthStencilState.depthWrite = true;
			pipelineDesc.depthStencilState.depthCompareOp = Render::CompareOp::Less;
			pipelineDesc.cullMode = Render::CullMode::Back;
			pipelineDesc.frontFace = Render::FrontFace::CounterClockWise;
			pipelineDesc.fillMode = Render::PolygonFillMode::Fill;
			pipelineDesc.topology = Render::PrimitiveTopology::Triangles;
			pipelineDesc.blend.enabled = true;
			pipelineDesc.blend.srcColor = Render::BlendFactor::SrcAlpha;
			pipelineDesc.blend.dstColor = Render::BlendFactor::OneMinusSrcAlpha;
			pipelineDesc.layout.bindings = {
				{0, sizeof(Math::Vec4), Render::VertexInputRate::PerVertex}
			};
			pipelineDesc.layout.attributes = {
				{0, 0, Render::VertexFormat::Float3, 0, true}
			};
			debugPipeline = Engine::instance()->rhi()->createGraphicsPipeline(pipelineDesc);

			Render::BufferDesc vertexBufferDesc;
			vertexBufferDesc.size = sizeof(Math::Vec4) * 8;
			vertexBufferDesc.usage = Render::BufferUsage::Vertex;
			vertexBufferDesc.cpuAccess = Render::CpuAccess::Write;
			debugMatricesUBO = Engine::instance()->rhi()->createBuffer(vertexBufferDesc);
			const GLuint indices[] = {
			0, 2, 3,
			0, 3, 1,
			4, 6, 2,
			4, 2, 0,
			5, 7, 6,
			5, 6, 4,
			1, 3, 7,
			1, 7, 5,
			6, 7, 3,
			6, 3, 2,
			1, 5, 4,
			0, 1, 4
			};

			Render::BufferDesc indexBufferDesc;
			indexBufferDesc.size = sizeof(GLuint) * 36;
			indexBufferDesc.usage = Render::BufferUsage::Index;
			indexBufferDesc.cpuAccess = Render::CpuAccess::Write;
			debugMatricesEBO = Engine::instance()->rhi()->createBuffer(indexBufferDesc, indices);
		}

		Engine::instance()->rhi()->bindPipeline(debugPipeline);
		Engine::instance()->rhi()->setUniform(debugPipeline, "projection", cameraProjection);
		Engine::instance()->rhi()->setUniform(debugPipeline, "view", cameraView);
		

		drawCascadeVolumeVisualizers(lightSpaceMatrices);*/
	}

	void CascadeShadowComponent::drawCascadeVolumeVisualizers(const std::vector<SFE::Math::Mat4>& lightMatrices) {
		// todo this code should not exist, move to debug renderer later
		/*static const Math::Vec4 colors[] = {
			{1.0, 0.0, 0.0, 0.5f},
			{0.0, 1.0, 0.0, 0.5f},
			{0.0, 0.0, 1.0, 0.5f},
		};

		for (int i = 0; i < lightMatrices.size(); ++i) {
			const auto corners = CascadeShadowComponent::getFrustumCornersWorldSpace(lightMatrices[i]);
			Engine::instance()->rhi()->updateBuffer(debugMatricesUBO, 0, sizeof(Math::Vec4) * corners.size(), corners.data());
			Engine::instance()->rhi()->setUniform(debugPipeline, "color", colors[i % 3]);
			Engine::instance()->rhi()->bindVertexBuffer(0, debugMatricesUBO);
			Engine::instance()->rhi()->bindIndexBuffer(debugMatricesEBO, Render::IndexType::Uint32);
			Engine::instance()->rhi()->drawIndexed(36, 0, 1, Render::IndexType::Uint32, 0);
		}*/
	}
}

#pragma once

#include <map>
#include <unordered_map>

#include "assetsModule/modelModule/Vertex.h"

#include "mathModule/Forward.h"
#include "mathModule/Quaternion.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	class Utils {
	public:
		// Draws the ground drawn dense over `patch` (TerrainPatch::square) with the pipeline bound, its vertex shader
		// g_buffer_ground.vs's sfeGroundPatchVertex: a row of quads an instance, from its own index buffer.
		static void drawGroundPatch(CommandListHandle cmd, const Math::Vec4& patch);

		struct LineData {
			Math::Vec4 color;
			float thickness = 1.f;
			PrimitiveTopology renderType = PrimitiveTopology::Lines;
		};
		inline static std::vector<std::pair<LineData, std::vector<Math::Vec3>>> renderVertices;

		struct TriangleData {
			friend bool operator==(const TriangleData& lhs, const TriangleData& rhs) {
				return lhs.depthTest == rhs.depthTest
					&& lhs.blend == rhs.blend
					&& lhs.cull == rhs.cull;
			}

			friend bool operator!=(const TriangleData& lhs, const TriangleData& rhs) {
				return !(lhs == rhs);
			}

			bool depthTest = false;
			bool blend = true;
			bool cull = false;

		};

		struct Triangle {
			Triangle() = default;
			Triangle(const Math::Vec3& a, const Math::Vec3& b, const Math::Vec3& c, const Math::Vec4& color) : A(a), B(b), C(c) {
				A.color = color;
				B.color = color;
				C.color = color;
				recalculateNormal();
			}

			void recalculateNormal() {
				const auto AB = B.position - A.position;
				const auto AC = C.position - A.position;

				//Normal of ABC triangle
				const auto Normal = SFE::Math::normalize(SFE::Math::cross(AB, AC));

				A.normal = Normal;
				B.normal = Normal;
				C.normal = Normal;
			}

			Vertex3D A;
			Vertex3D B;
			Vertex3D C;
		};

		inline static std::vector<std::pair<TriangleData, std::vector<Triangle>>> renderTriangles;

		static std::vector<Math::Vec3>& getVerticesArray(const Math::Vec4& color, float thickness, PrimitiveTopology renderType);
		static std::vector<Triangle>& getTrianglesArray(const TriangleData& data);

		static void renderQuad(CommandListHandle cmd);

		// A texture the size the picture is drawn at (ViewportData::renderW, renderH, over `divisor`) and a framebuffer
		// over it for renderPass (none for a copy's target): what a screen pass draws into. What the handles held
		// before is let go first, so RenderPass::resize makes them again with the same call.
		static void screenTarget(TextureHandle& texture, FramebufferHandle& framebuffer, RenderPassHandle renderPass,
			TextureFormat format = TextureFormat::RGBA16F, int divisor = 1);

		// A std140 `mat4 x[count]` block for this frame: the matrices, then the last one repeated (zeros if none),
		// so the bound range always covers the whole declared array.
		static TransientAllocation allocMatrices(const std::vector<Math::Mat4>& matrices, size_t count);
		static void renderQuad(CommandListHandle cmd, float x1, float y1, float x2, float y2);

		static void renderLine(const Math::Vec3& begin, const Math::Vec3& end, const Math::Vec4& color, float thickness = 1.f);

		static void renderCube(const Math::Vec3& LTN, const Math::Vec3& RBF, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color);
		static void renderQuad(Render::CommandListHandle cmd, const Math::Vec3& min, const Math::Vec3& max, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color);

		static void renderCubeMesh(const Math::Vec3& LTN, const Math::Vec3& RBF, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color);

		static void renderCircle(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale, float radius, const Math::Vec4& color, int numSegments = 16, float lineThicness = 3.f, PrimitiveTopology renderType = PrimitiveTopology::LineStrip);
		static void renderCircleFilled(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale, float radius, const Math::Vec4& color, int numSegments, float startAngle = 0.f, float endAngle = 360.f);

		static void renderCone(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale, float radius, float height, const Math::Vec4& color, int numSegments = 64);
	};
}

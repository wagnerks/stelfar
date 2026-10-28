#include "Utils.h"

#include <algorithm>
#include <cmath>

#include "mathModule/Utils.h"

#include "assetsModule/modelModule/Mesh.h"
#include "assetsModule/modelModule/Vertex.h"
#include "core/Engine.h"
#include "mathModule/Quaternion.h"
#include "systemsModule/SystemManager.h"
#include "windowsModule/Window.h"

using namespace SFE::Render;

void Utils::drawGroundPatch(const CommandListHandle cmd, const Math::Vec4& patch) {
	constexpr uint32_t kMostColumns = 2048;
	auto* rhi = Engine::instance()->rhi();
	static const BufferHandle indices = [rhi] {
		// A column's vertices are 2 * column on the row's near side and the next up on its far side; each quad (a, c, b),
		// (b, c, d), wound and split as the terrain's own cells are.
		std::vector<uint32_t> row;
		row.reserve(static_cast<size_t>(kMostColumns) * 6);
		for (uint32_t column = 0; column < kMostColumns; ++column) {
			const uint32_t a = 2 * column;
			const uint32_t c = a + 1;
			const uint32_t b = a + 2;
			const uint32_t d = a + 3;
			for (const uint32_t index : { a, c, b, b, c, d }) {
				row.push_back(index);
			}
		}
		BufferDesc desc;
		desc.size = row.size() * sizeof(uint32_t);
		desc.usage = BufferUsage::Index;
		desc.cpuAccess = CpuAccess::None;
		return rhi->createBuffer(desc, row.data());
	}();
	const auto columns = (std::min)(static_cast<uint32_t>(std::lround(patch.z / (std::max)(patch.w, 1e-3f))), kMostColumns);
	if (columns == 0) {
		return;
	}
	rhi->cmdBindIndexBuffer(cmd, indices, IndexType::Uint32, 0);
	rhi->cmdDrawIndexed(cmd, columns * 6, columns);
}

// A LineStrip batch is never merged with an existing one: appending vertices to a strip joins the
// last vertex of the old run to the first of the new, drawing a segment nobody asked for.
std::vector<SFE::Math::Vec3>& Utils::getVerticesArray(const Math::Vec4& color, float thickness, PrimitiveTopology renderType) {
	auto it = std::find_if(renderVertices.begin(), renderVertices.end(), [color, thickness, renderType](std::pair<LineData, std::vector<Math::Vec3>>& a) {
		return a.first.color == color && std::fabs(a.first.thickness - thickness) <= std::numeric_limits<float>::epsilon() && a.first.renderType == renderType && renderType != PrimitiveTopology::LineStrip;
	});

	if (it != renderVertices.end()) {
		return it->second;
	}

	renderVertices.push_back(std::make_pair(LineData{ color, thickness, renderType }, std::vector<Math::Vec3>()));
	return renderVertices.back().second;
}

std::vector<Utils::Triangle>& Utils::getTrianglesArray(const TriangleData& data) {
	auto it = std::find_if(renderTriangles.begin(), renderTriangles.end(), [&data](std::pair<TriangleData, std::vector<Triangle>>& a) {
		return a.first == data;
	});

	if (it != renderTriangles.end()) {
		return it->second;
	}

	renderTriangles.push_back(std::make_pair(data, std::vector<Triangle>()));
	return renderTriangles.back().second;
}


TransientAllocation Utils::allocMatrices(const std::vector<Math::Mat4>& matrices, size_t count) {
	const auto allocation = Engine::instance()->rhi()->allocTransient(sizeof(Math::Mat4) * count);
	if (!allocation) {
		return allocation;
	}
	auto* out = static_cast<Math::Mat4*>(allocation.data);
	for (size_t i = 0; i < count; ++i) {
		out[i] = matrices.empty() ? Math::Mat4{} : matrices[(std::min)(i, matrices.size() - 1)];
	}
	return allocation;
}

void Utils::renderQuad(CommandListHandle cmd) {
	static BufferHandle quadVBO;

	if (!quadVBO) {
		const auto mesh = Mesh<Vertex2D>{
			{
				{ {-1.f,  1.f}, {0.f, 1.f} },
				{ {-1.f, -1.f}, {0.f, 0.f} },
				{ { 1.f,  1.f}, {1.f, 1.f} },\

				{ { 1.f, -1.f}, {1.f, 0.f} },
				{ { 1.f,  1.f}, {1.f, 1.f} },
				{ {-1.f, -1.f}, {0.f, 0.f} },
			}
		};

		BufferDesc desc;
		desc.usage = BufferUsage::Vertex;
		desc.size = sizeof(Vertex2D) * mesh.vertices.size();
		desc.cpuAccess = CpuAccess::None;
		quadVBO = Engine::instance()->rhi()->createBuffer(desc, mesh.vertices.data());
	}

	Engine::instance()->rhi()->cmdBindVertexBuffer(cmd, 0, quadVBO);
	Engine::instance()->rhi()->cmdDraw(cmd, 6);
}

void Utils::screenTarget(TextureHandle& texture, FramebufferHandle& framebuffer, const RenderPassHandle renderPass,
	const TextureFormat format, const int divisor) {
	auto* rhi = Engine::instance()->rhi();
	if (framebuffer) {
		rhi->destroyFramebuffer(framebuffer);
	}
	if (texture) {
		rhi->destroyTexture(texture);
	}
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	const auto w = static_cast<uint32_t>((std::max)(1, screen.renderW / divisor));
	const auto h = static_cast<uint32_t>((std::max)(1, screen.renderH / divisor));
	TextureDesc desc;
	desc.width = w;
	desc.height = h;
	desc.format = format;
	desc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
	texture = rhi->createTexture(desc);
	FramebufferDesc fb;
	fb.width = w;
	fb.height = h;
	fb.renderPass = renderPass;
	fb.colorAttachments = { { texture } };
	framebuffer = rhi->createFramebuffer(fb);
}

void Utils::renderQuad(CommandListHandle cmd, float x1 = -1.f, float y1 = -1.f, float x2 = 1.f, float y2 = 1.f) {
	Mesh<Vertex3D> mesh{ {
			// positions        // texture Coords
		{{x1, y2, 0.0f}, { 0.0f, 1.0f }},
		{ {x1, y1, 0.0f}, {0.0f, 0.0f} },
		{ {x2, y2, 0.0f}, {1.0f, 1.0f} },
		{ {x2, y1, 0.0f}, {1.0f, 0.0f} },
	} };

	static PipelineHandle pipeline;
	if (!pipeline) {
		GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.topology = PrimitiveTopology::TriangleStrip;
		pipelineDesc.cullMode = CullMode::None;
		pipelineDesc.fillMode = PolygonFillMode::Fill;
		

		pipelineDesc.layout.bindings = {
			{ 0, sizeof(Vertex3D), VertexInputRate::PerVertex }
		};
		pipelineDesc.layout.attributes = {
			{ 0, 0, VertexFormat::Float3, offsetof(Vertex3D, position), false },
			{ 1, 0, VertexFormat::Float2, offsetof(Vertex3D, texCoords), false }
		};

		pipeline = Engine::instance()->rhi()->createGraphicsPipeline(pipelineDesc);
	}

	const auto vbo = Engine::instance()->rhi()->allocTransient(sizeof(Vertex3D) * mesh.vertices.size());
	if (!vbo) {
		return;
	}
	std::memcpy(vbo.data, mesh.vertices.data(), sizeof(Vertex3D) * mesh.vertices.size());
	Engine::instance()->rhi()->cmdBindPipeline(cmd, pipeline);
	Engine::instance()->rhi()->cmdBindVertexBuffer(cmd, 0, vbo.buffer, vbo.offset);
	Engine::instance()->rhi()->cmdDraw(cmd, mesh.vertices.size());
}

void Utils::renderLine(const Math::Vec3& begin, const Math::Vec3& end, const Math::Vec4& color, float thickness) {
	getVerticesArray(color, thickness, PrimitiveTopology::Lines).emplace_back(begin);
	getVerticesArray(color, thickness, PrimitiveTopology::Lines).emplace_back(end);
}

void Utils::renderCube(const Math::Vec3& LTN, const Math::Vec3& RBF, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color) {
	//		LTF*------------*RTF
	//		 / |           /|
	//      /  |          / |
	//     /   |         /  |
	// LTN*-----------*RTN  |
	//    |    |        |   |
	//    |    |        |   |
	//    |    *LBF-----|---*RBF
	//    |   /         |  /
	//    |  /          | /
	//    | /           |/
	// LBN*-------------*RBN   


	//Math::Vec3 LTN = { -far,  far, far };
	Math::Vec3 RTN = { RBF.x, LTN.y, LTN.z };
	Math::Vec3 LBN = { LTN.x, RBF.y, LTN.z };
	Math::Vec3 RBN = { RBF.x, RBF.y, LTN.z };

	Math::Vec3 LTF = { LTN.x, LTN.y, RBF.z };
	Math::Vec3 RTF = { RBF.x, LTN.y, RBF.z };
	Math::Vec3 LBF = { LTN.x, RBF.y, RBF.z };
	//Math::Vec3 RBF = { far, -far, -far };
	

	float vertices[] = {
		//far cube
		//backward
		LTN.x, LTN.y, LTN.z, //start
		RTN.x, RTN.y, RTN.z, //end

		LTN.x, LTN.y, LTN.z, //start
		LBN.x, LBN.y, LBN.z, //end

		LBN.x, LBN.y, LBN.z, //start
		RBN.x, RBN.y, RBN.z, //end

		RBN.x, RBN.y, RBN.z, //start
		RTN.x, RTN.y, RTN.z, //end
		//forward
		LTF.x, LTF.y, LTF.z, //start
		RTF.x, RTF.y, RTF.z, //end

		LTF.x, LTF.y, LTF.z, //start
		LBF.x, LBF.y, LBF.z, //end

		LBF.x, LBF.y, LBF.z, //start
		RBF.x, RBF.y, RBF.z, //end

		RBF.x, RBF.y, RBF.z, //start
		RTF.x, RTF.y, RTF.z, //end
		//right
		RTN.x, RTN.y, RTN.z, //start
		RTF.x, RTF.y, RTF.z, //end

		RBN.x, RBN.y, RBN.z, //start
		RBF.x, RBF.y, RBF.z, //end
		//left
		LTN.x, LTN.y, LTN.z, //start
		LTF.x, LTF.y, LTF.z, //end

		LBN.x, LBN.y, LBN.z, //start
		LBF.x, LBF.y, LBF.z, //end
	};

	// Create a rotation matrix (e.g., rotate 45 degrees around the Y-axis)
	const Math::Mat4 transform = Math::translate(Math::Mat4(1.0f), Math::Vec3(pos)) * Math::Mat4(rotate);
	auto& vertArray = getVerticesArray(color, 1.f, PrimitiveTopology::Lines);

	// Apply the rotation to the cube vertices
	for (int i = 0; i < sizeof(vertices) / sizeof(float); i += 3) {
		Math::Vec4 vertex(vertices[i], vertices[i + 1], vertices[i + 2], 1.0f);

		// Apply the model and rotation transformations
		vertex = transform * vertex;
		vertArray.emplace_back(vertex);
	}
}

void Utils::renderQuad(Render::CommandListHandle cmd, const Math::Vec3& min, const Math::Vec3& max, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color) {
	//   A         max
	//    *---------*
	//    |         |
	//    |         |
	//    |         |
	//min *---------* B

	const Math::Vec3 A = { min.x, max.y, min.z };
	const Math::Vec3 B = { max.x, min.y, max.z };

	Triangle vertices[] = {
		{min, A, max, color},
		{min, max, B, color}
	};

	// Create a rotation matrix (e.g., rotate 45 degrees around the Y-axis)
	const Math::Mat4 transform = Math::translate(Math::Mat4(1.0f), Math::Vec3(pos)) * Math::Mat4(rotate);
	auto& vertArray = getTrianglesArray({});

	// Apply the rotation to the cube vertices
	for (int i = 0; i < sizeof(vertices) / sizeof(Triangle); i++) {
		auto& tr = vertices[i];
		tr.A.color = color;
		tr.B.color = color;
		tr.C.color = color;
		// Apply the model and rotation transformations
		tr.A.position = transform * Math::Vec4(tr.A.position, 1.f);
		tr.B.position = transform * Math::Vec4(tr.B.position, 1.f);
		tr.C.position = transform * Math::Vec4(tr.C.position, 1.f);
		vertArray.emplace_back(tr);
	}
}

void Utils::renderCubeMesh(const Math::Vec3& LTN, const Math::Vec3& RBF, const Math::Mat4& rotate, const Math::Vec3& pos, const Math::Vec4& color) {
	//		LTF*------------*RTF
	//		 / |           /|
	//      /  |          / |
	//     /   |         /  |
	// LTN*-----------*RTN  |
	//    |    |        |   |
	//    |    |        |   |
	//    |    *LBF-----|---*RBF
	//    |   /         |  /
	//    |  /          | /
	//    | /           |/
	// LBN*-------------*RBN   


	//Math::Vec3 LTN = { -far,  far, far };
	Math::Vec3 RTN = { RBF.x, LTN.y, LTN.z };
	Math::Vec3 LBN = { LTN.x, RBF.y, LTN.z };
	Math::Vec3 RBN = { RBF.x, RBF.y, LTN.z };

	Math::Vec3 LTF = { LTN.x, LTN.y, RBF.z };
	Math::Vec3 RTF = { RBF.x, LTN.y, RBF.z };
	Math::Vec3 LBF = { LTN.x, RBF.y, RBF.z };
	//Math::Vec3 RBF = { far, -far, -far };


	Triangle vertices[] = {
		{RTN, LTN,  LBN, color},
		{RBN, RTN, LBN, color},

		{LBN, LTN, LBF, color},
		{LBF, LTN, LTF, color},

		{LTF, LTN,  RTN, color},
		{RTF, LTF,   RTN, color},

		{RBF, RTF, RBN, color},
		{RBN, RTF, RTN, color},

		{LTF, RTF, LBF, color},
		{LBF, RTF, RBF, color},

		{LBN, LBF,  RBN, color},
		{LBF, RBF,  RBN, color},
	};

	// Create a rotation matrix (e.g., rotate 45 degrees around the Y-axis)
	const Math::Mat4 transform = Math::translate(Math::Mat4(1.0f), Math::Vec3(pos)) * Math::Mat4(rotate);
	auto& vertArray = getTrianglesArray({false, true, true});

	// Apply the rotation to the cube vertices
	for (int i = 0; i < sizeof(vertices) / sizeof(Triangle); i++) {
		auto& tr = vertices[i];
		tr.A.color = color;
		tr.B.color = color;
		tr.C.color = color;

		// Apply the model and rotation transformations
		tr.A.position = transform * Math::Vec4(tr.A.position, 1.f);
		tr.B.position = transform * Math::Vec4(tr.B.position, 1.f);
		tr.C.position = transform * Math::Vec4(tr.C.position, 1.f);
		vertArray.emplace_back(tr);
	}
}


void Utils::renderCircle(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale, float radius, const Math::Vec4& color, int numSegments, float lineThicness, PrimitiveTopology renderType) {
	auto& vertArray = getVerticesArray(color, lineThicness, renderType);

	auto transform = Math::translate(Math::Mat4{1.f}, pos)* quat.toMat4() * scale;

	for (int i = 0; i < numSegments; ++i) {
		float theta = 2.0f * Math::pi<float>() * static_cast<float>(i) / static_cast<float>(numSegments);

		float x = radius * std::cos(theta);
		float y = radius * std::sin(theta);
		
		vertArray.push_back(transform * Math::Vec4{ x, y, 0.f, 1.f});
	}
}

void Utils::renderCircleFilled(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale, float radius, const Math::Vec4& color, int numSegments, float startAngle, float delta) {
	auto& vertArray = getTrianglesArray({});

	auto transform = Math::translate(Math::Mat4{1.f}, pos) * quat.toMat4() * scale;
	for (int i = 0; i < numSegments; i++) {
		Triangle tr;
		tr.A.color = color;
		tr.B.color = color;
		tr.C.color = color;

		float theta = Math::radians(startAngle + delta * static_cast<float>(i) / static_cast<float>(numSegments));
		tr.A.position.x = radius * std::cos(theta);
		tr.A.position.y = radius * std::sin(theta);
		tr.A.position = transform * Math::Vec4{ tr.A.position, 1.f};

		theta = Math::radians(startAngle + delta * static_cast<float>(i + 1) / static_cast<float>(numSegments));
		tr.B.position.x = radius * std::cos(theta);
		tr.B.position.y = radius * std::sin(theta);
		tr.B.position = transform * Math::Vec4{ tr.B.position, 1.f};

		tr.C.position = pos;

		vertArray.push_back(tr);
	}
}

void Utils::renderCone(const Math::Vec3& pos, const Math::Quaternion<float>& quat, const Math::Mat4& scale,	float radius, float height, const Math::Vec4& color, int numSegments) {
	auto& vertArray = getTrianglesArray({});

	auto transform = Math::translate(Math::Mat4{1.f}, pos) * quat.toMat4() * scale;

	for (int i = 0; i < numSegments; i++) {
		Triangle tr;
		tr.A.color = color;
		tr.B.color = color;
		tr.C.color = color;

		float theta = Math::radians(360.f * static_cast<float>(i) / static_cast<float>(numSegments));
		tr.A.position.x = radius * std::cos(theta);
		tr.A.position.z = radius * std::sin(theta);
		tr.A.position = transform * Math::Vec4{ tr.A.position, 1.f};
		
		theta = Math::radians(360.f * static_cast<float>(i + 1) / static_cast<float>(numSegments));
		tr.B.position.x = radius * std::cos(theta);
		tr.B.position.z = radius * std::sin(theta);
		tr.B.position = transform * Math::Vec4{ tr.B.position, 1.f};

		tr.C.position = Math::Vec3{0.f};
		tr.C.position = transform * Math::Vec4{ tr.C.position, 1.f};
		tr.recalculateNormal();
		vertArray.push_back(tr);

		Triangle tr2;
		tr2.A = tr.A;
		tr2.B = tr.B;
		tr2.C.position = Math::Vec3{ 0.f };
		tr2.C.position.y += height;
		tr2.C.position = transform * Math::Vec4{ tr2.C.position, 1.f};

		tr2.A.color = color;
		tr2.B.color = color;
		tr2.C.color = color;
		vertArray.push_back(tr2);
	}
}

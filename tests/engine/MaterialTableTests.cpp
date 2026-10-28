#include <algorithm>
#include <array>
#include <cstddef>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "EngineTestFixture.h"
#include "assetsModule/modelModule/Material.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/FileSystem.h"
#include "ecss/Registry.h"
#include "propertiesModule/ModelBinding.h"
#include "propertiesModule/PropertiesSystem.h"
#include "renderModule/GpuMaterial.h"
#include "renderModule/GpuMaterialTable.h"
#include "renderModule/GpuScene.h"
#include "renderModule/MaterialRules.h"
#include "renderModule/MaterialTextures.h"
#include "systemsModule/TasksManager.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::EngineTests {
	using ComponentsModule::DrawClass;
	using ComponentsModule::TransformComponent;
	using Render::GpuMaterial;
	using Render::GpuScene;

	namespace {
		// A GLSL struct as std430 lays it out: each member's name and byte offset, in order, and the struct's size.
		struct Std430 {
			std::vector<std::pair<std::string, size_t>> members;
			size_t size = 0;
		};

		// The members of `name` in `text`, laid out by std430: a float 4 bytes on 4, a vec2 8 on 8, a vec3 12 on 16, a vec4
		// 16 on 16; an array's elements a stride apart of their size rounded up to their alignment; the struct rounded up to
		// its largest alignment. Any other type fails the test that asked.
		Std430 std430Layout(const std::string& text, const std::string& name) {
			Std430 layout;
			const std::regex comment("//[^\n]*");
			const auto plain = std::regex_replace(text, comment, "");
			std::smatch body;
			if (!std::regex_search(plain, body, std::regex("struct\\s+" + name + "\\s*\\{([^}]*)\\}"))) {
				ADD_FAILURE() << "no struct " << name;
				return layout;
			}
			const std::string members = body[1].str();
			const std::regex member("\\s*(\\w+)\\s+(\\w+)\\s*(?:\\[\\s*(\\d+)\\s*\\])?\\s*;");
			size_t offset = 0;
			size_t largest = 4;
			for (auto it = std::sregex_iterator(members.begin(), members.end(), member); it != std::sregex_iterator(); ++it) {
				const auto type = (*it)[1].str();
				size_t size = 0;
				size_t align = 0;
				if (type == "float" || type == "uint" || type == "int") {
					size = 4;
					align = 4;
				}
				else if (type == "vec2") {
					size = 8;
					align = 8;
				}
				else if (type == "vec3") {
					size = 12;
					align = 16;
				}
				else if (type == "vec4") {
					size = 16;
					align = 16;
				}
				else {
					ADD_FAILURE() << name << " holds a " << type << ", which this layout does not know";
					return layout;
				}
				const size_t count = (*it)[3].matched ? std::stoul((*it)[3].str()) : 1;
				if ((*it)[3].matched) {
					size = (size + align - 1) / align * align * count;
				}
				offset = (offset + align - 1) / align * align;
				layout.members.emplace_back((*it)[2].str(), offset);
				offset += size;
				largest = (std::max)(largest, align);
			}
			layout.size = (offset + largest - 1) / largest * largest;
			return layout;
		}

		// An entity of the world drawn with `mesh` (an id past anything the world loads; nothing is drawn on this backend,
		// so a mesh only has to look uploaded) and `material`.
		ecss::EntityId spawn(const uint32_t mesh, const MaterialComponent& material) {
			auto& registry = ECSHandler::registry();
			const auto id = registry.takeEntity();
			registry.addComponent<TransformComponent>(id, id);
			registry.addComponent<MeshComponent>(id)->meshGraph.root().value = { mesh, 0, 0, 3, 3 };
			*registry.addComponent<MaterialComponent>(id) = material;
			SystemsModule::TasksManager::instance()->notify({ id, SystemsModule::MESH_UPDATED });
			return id;
		}

		// Frames until the G-buffer list holds `batches` batches of `mesh`, as many as the spawned entities take to
		// reach the scene through the transform, AABB and octree workers.
		size_t stepUntil(const uint32_t mesh, const size_t batches) {
			const auto& scene = ECSHandler::getSystem<SystemsModule::RenderSystem>()->scene();
			for (int frame = 0; frame < 300 && scene.batchesOfMesh(GpuScene::List::GBuffer, mesh) < batches; ++frame) {
				step();
			}
			return scene.batchesOfMesh(GpuScene::List::GBuffer, mesh);
		}
	}

	// The C++ record is the shader's SfeMaterial byte for byte: every member of the GLSL struct, in its order, at the
	// offset std430 gives it, and the same size. A member added on one side only, or the pad before user[] left out,
	// moves every material read after it.
	TEST(MaterialTable, GpuMaterialMirrorsTheShader) {
		std::string text;
		ASSERT_TRUE(FileSystem::readFile("shaders/sfe_material_table.glsl", text));
		const auto shader = std430Layout(text, "SfeMaterial");
		const std::vector<std::pair<std::string, size_t>> mirror = {
			{ "matRoughness", offsetof(GpuMaterial, roughness) },
			{ "matMetallic", offsetof(GpuMaterial, metallic) },
			{ "mapMask", offsetof(GpuMaterial, mapMask) },
			{ "alphaCutoff", offsetof(GpuMaterial, alphaCutoff) },
			{ "uvSel", offsetof(GpuMaterial, uvSel) },
			{ "specFactor", offsetof(GpuMaterial, specFactor) },
			{ "clearcoat", offsetof(GpuMaterial, clearcoat) },
			{ "clearcoatRough", offsetof(GpuMaterial, clearcoatRough) },
			{ "tint", offsetof(GpuMaterial, tint) },
			{ "opacity", offsetof(GpuMaterial, opacity) },
			{ "ior", offsetof(GpuMaterial, ior) },
			{ "casterOpacity", offsetof(GpuMaterial, casterOpacity) },
			{ "shadowCutoff", offsetof(GpuMaterial, shadowCutoff) },
			{ "albedoSlot", offsetof(GpuMaterial, albedoSlot) },
			{ "triplanarUv", offsetof(GpuMaterial, triplanarUv) },
			{ "triplanarUv2", offsetof(GpuMaterial, triplanarUv2) },
			{ "triplanarRepeat", offsetof(GpuMaterial, triplanarRepeat) },
			{ "triplanarRepeat2", offsetof(GpuMaterial, triplanarRepeat) + 4 },
			{ "triplanarRotated", offsetof(GpuMaterial, triplanarRotated) },
			{ "dissolve", offsetof(GpuMaterial, dissolve) },
			{ "emissiveR", offsetof(GpuMaterial, emissive) },
			{ "emissiveG", offsetof(GpuMaterial, emissive) + 4 },
			{ "emissiveB", offsetof(GpuMaterial, emissive) + 8 },
			{ "user", offsetof(GpuMaterial, user) },
		};
		ASSERT_EQ(shader.members.size(), mirror.size()) << "SfeMaterial and GpuMaterial do not have as many members";
		for (size_t i = 0; i < mirror.size(); ++i) {
			EXPECT_EQ(shader.members[i].first, mirror[i].first) << "member " << i;
			EXPECT_EQ(shader.members[i].second, mirror[i].second) << mirror[i].first;
		}
		EXPECT_EQ(shader.size, sizeof(GpuMaterial));
		EXPECT_EQ(sizeof(GpuMaterial), 160u);
		EXPECT_EQ(offsetof(GpuMaterial, dissolve), 108u);
		EXPECT_EQ(offsetof(GpuMaterial, user), 128u);
	}

	// An entry is what the shaders read of the material it was acquired for: the application's lanes as they are, the
	// dissolve, and the ground's bit 128 of the map mask. A plain material has none of them.
	TEST(MaterialTable, EntryCarriesTheLanesTheDissolveAndTheGround) {
		Render::GpuMaterialTable table;
		MaterialComponent marked;
		marked.user = { Math::Vec4{ 1.f, 2.f, 3.f, 4.f }, Math::Vec4{ 5.f, 6.f, 7.f, -8.f } };
		marked.dissolve = 0.25f;
		marked.ground = true;
		const MaterialComponent plain;
		const auto markedIndex = table.acquire(marked);
		const auto plainIndex = table.acquire(plain);
		ASSERT_NE(markedIndex, plainIndex);

		const auto entry = table.entry(markedIndex);
		const float lanes[8] = { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, -8.f };
		for (size_t i = 0; i < 8; ++i) {
			EXPECT_EQ(entry.user[i], lanes[i]) << "lane " << i;
		}
		EXPECT_EQ(entry.dissolve, 0.25f);
		EXPECT_NE(static_cast<uint32_t>(entry.mapMask + 0.5f) & 128u, 0u);

		const auto none = table.entry(plainIndex);
		for (size_t i = 0; i < 8; ++i) {
			EXPECT_EQ(none.user[i], 0.f) << "lane " << i;
		}
		EXPECT_EQ(none.dissolve, 0.f);
		EXPECT_EQ(static_cast<uint32_t>(none.mapMask + 0.5f) & 128u, 0u);

		table.release(markedIndex);
		table.release(plainIndex);
		EXPECT_EQ(table.liveEntries(), 0u);
	}

	// Two materials share an entry exactly when the shaders could not tell them apart: a lane a 1024th apart, a lane
	// negative otherwise (a conversion that clamped below zero would join them), the ground, the dissolve each make
	// another; lanes closer than a 4096th, the table's step, share one.
	TEST(MaterialTable, LanesGroundAndDissolveTellMaterialsApart) {
		Render::GpuMaterialTable table;
		std::vector<uint32_t> held;
		const auto acquire = [&](const MaterialComponent& material) {
			held.push_back(table.acquire(material));
			return held.back();
		};
		MaterialComponent base;
		base.user[0] = Math::Vec4{ 0.25f, -1.f, 0.f, 0.f };
		const auto first = acquire(base);
		EXPECT_EQ(acquire(base), first) << "equal materials";
		EXPECT_EQ(table.liveEntries(), 1u);

		auto apart = base;
		apart.user[1].w = 1.f / 1024.f;
		EXPECT_NE(acquire(apart), first) << "a lane a 1024th apart";
		auto negative = base;
		negative.user[0].y = -2.f;
		EXPECT_NE(acquire(negative), first) << "a negative lane";
		auto ground = base;
		ground.ground = true;
		EXPECT_NE(acquire(ground), first) << "the ground";
		auto dissolved = base;
		dissolved.dissolve = 0.5f;
		EXPECT_NE(acquire(dissolved), first) << "the dissolve";
		EXPECT_EQ(table.liveEntries(), 5u);

		auto close = base;
		close.user[0].x = 0.25f + 1.f / 8192.f;
		EXPECT_EQ(acquire(close), first) << "lanes closer than a 4096th";

		for (const auto index : held) {
			table.release(index);
		}
		EXPECT_EQ(table.liveEntries(), 0u);
	}

	// The turn a material is drawn in: the ground as itself whatever else it says, else cut out exactly when its G-buffer
	// shader discards -- a cutoff, alpha from the maps, dithered, or the masked shader -- else solid. A masked asset whose
	// shader is swapped for one of the application's, cutoff 0, is solid, as its draw group always was.
	TEST(MaterialRules, DrawClassOfAMaterial) {
		using Render::MaterialRules::drawClass;
		const auto classOf = [](const auto change) {
			MaterialComponent material;
			change(material);
			return drawClass(material);
		};
		EXPECT_EQ(drawClass(MaterialComponent{}), DrawClass::Solid);
		EXPECT_EQ(classOf([](MaterialComponent& m) { m.alphaCutoff = 0.5f; }), DrawClass::Cutout);
		EXPECT_EQ(classOf([](MaterialComponent& m) { m.alphaDither = true; }), DrawClass::Cutout);
		EXPECT_EQ(classOf([](MaterialComponent& m) { m.alphaFromMaps = true; }), DrawClass::Cutout);
		EXPECT_EQ(classOf([](MaterialComponent& m) { m.fragmentShader = ComponentsModule::DefaultGBufferMaskedFS; }), DrawClass::Cutout);
		EXPECT_EQ(classOf([](MaterialComponent& m) {
			m.ground = true;
			m.alphaCutoff = 0.5f;
			m.alphaDither = true;
		}), DrawClass::Ground);

		SFE::Material asset;
		asset.alphaCutout = true;
		auto swapped = ComponentsModule::fromAssetMaterial(asset);
		ASSERT_EQ(swapped.fragmentShader, ComponentsModule::DefaultGBufferMaskedFS);
		EXPECT_EQ(drawClass(swapped), DrawClass::Cutout);
		swapped.fragmentShader = "shaders/own_surface.fs";
		EXPECT_EQ(drawClass(swapped), DrawClass::Solid);
	}

	// Every pass drawing materials lays out their set alike: a combined image sampler for each slot DIFFUSE..USER3 in the
	// fragment stage. The writes for a material without textures are exactly those bindings, each once: a write to a
	// binding the layout lacks is a Vulkan error.
	TEST(MaterialTextures, LayoutHoldsEverySlotAndTheWritesNoMore) {
		const auto layout = Render::materialTextureLayout();
		std::set<uint32_t> slots;
		for (const auto& binding : layout.bindings) {
			EXPECT_EQ(binding.type, Render::DescriptorType::CombinedImageSampler) << "binding " << binding.binding;
			EXPECT_EQ(binding.stageMask, Render::ShaderStage::Fragment) << "binding " << binding.binding;
			EXPECT_EQ(binding.count, 1u) << "binding " << binding.binding;
			slots.insert(binding.binding);
		}
		std::set<uint32_t> expected;
		for (uint32_t slot = SFE::DIFFUSE; slot <= SFE::USER3; ++slot) {
			expected.insert(slot);
		}
		EXPECT_EQ(slots, expected);
		EXPECT_EQ(layout.bindings.size(), expected.size());

		const auto writes = Render::materialTextureWrites(Render::DescriptorSetHandle{}, ComponentsModule::Materials{});
		std::set<uint32_t> written;
		for (const auto& write : writes) {
			written.insert(write.binding);
			EXPECT_EQ(write.images.size(), 1u) << "binding " << write.binding;
		}
		EXPECT_EQ(written, expected);
		EXPECT_EQ(writes.size(), expected.size()) << "a binding written twice";
	}

	// The ground is a draw group of its own: two entities alike in mesh, shaders and textures, one of them the ground,
	// are drawn in two groups, the ground's Ground and the other's Solid; once it is the ground no more, they share one.
	TEST(EngineTest, AGroundMaterialDrawsInAGroupOfItsOwn) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		const auto& scene = renderSystem->scene();

		constexpr uint32_t kMesh = (1u << 21) + 23;
		MaterialComponent material;
		const auto plain = spawn(kMesh, material);
		material.ground = true;
		const auto ground = spawn(kMesh, material);
		const Finally takeOut([plain, ground] {
			ECSHandler::destroyEntity(plain);
			ECSHandler::destroyEntity(ground);
			step();
		});
		ASSERT_EQ(stepUntil(kMesh, 2), 2u) << "the two were not drawn in two groups";

		bool groundGroup = false;
		bool solidGroup = false;
		for (const auto& group : scene.groups(GpuScene::List::GBuffer)) {
			if (group.batchCount == 0 || group.vertexShader != ComponentsModule::DefaultGBufferVS
				|| group.fragmentShader != ComponentsModule::DefaultGBufferSolidFS) {
				continue;
			}
			groundGroup |= group.drawClass == DrawClass::Ground;
			solidGroup |= group.drawClass == DrawClass::Solid;
		}
		EXPECT_TRUE(groundGroup) << "no Ground group of the plain shaders";
		EXPECT_TRUE(solidGroup) << "no Solid group of the plain shaders";

		registry.pinComponent<MaterialComponent>(ground)->ground = false;
		SystemsModule::TasksManager::instance()->notify({ ground, SystemsModule::MATERIAL_UPDATED });
		step(2);
		EXPECT_EQ(scene.batchesOfMesh(GpuScene::List::GBuffer, kMesh), 1u) << "no longer the ground, it is still drawn apart";
		EXPECT_TRUE(scene.validate());
	}

	// What the game relies on in place of reaching into the renderer's scene: a material changed with no event is not
	// taken up, and after MATERIAL_UPDATED the next frame draws it -- the table then holds the changed one, and the one
	// before is let go of.
	TEST(EngineTest, MaterialUpdatedTakesTheChangedMaterialOnTheNextFrame) {
		auto& registry = ECSHandler::registry();
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		ASSERT_NE(renderSystem, nullptr);
		auto& materials = renderSystem->materials();

		constexpr uint32_t kMesh = (1u << 21) + 29;
		MaterialComponent material;
		material.user[0] = Math::Vec4{ 0.375f, 0.5f, 0.625f, 0.75f };
		const auto entity = spawn(kMesh, material);
		const Finally takeOut([entity] {
			ECSHandler::destroyEntity(entity);
			step();
		});
		ASSERT_EQ(stepUntil(kMesh, 1), 1u) << "the entity never reached the scene, so nothing below is tested";
		step(3);

		// Held for a moment beside the scene's references: a new entry when the scene holds none of it.
		const auto isNew = [&](const MaterialComponent& probe) {
			const size_t before = materials.liveEntries();
			const auto index = materials.acquire(probe);
			const bool fresh = materials.liveEntries() == before + 1;
			materials.release(index);
			return fresh;
		};
		const auto before = *registry.pinComponent<const MaterialComponent>(entity);
		EXPECT_FALSE(isNew(before)) << "the scene does not hold the material it was spawned with";

		registry.pinComponent<MaterialComponent>(entity)->user[1].w = 0.875f;
		const auto changed = *registry.pinComponent<const MaterialComponent>(entity);
		step();
		EXPECT_TRUE(isNew(changed)) << "a change with no event was taken up";
		EXPECT_FALSE(isNew(before));

		SystemsModule::TasksManager::instance()->notify({ entity, SystemsModule::MATERIAL_UPDATED });
		step();
		EXPECT_FALSE(isNew(changed)) << "the next frame after MATERIAL_UPDATED does not draw the changed material";
		EXPECT_TRUE(isNew(before)) << "the material before the change is still held";
	}

	// A model bound again keeps what was changed on its meshes' materials since: the application's lanes and the ground
	// among them, which carry a coat, a ghost's wrist, a soak line, a ground. Its name, solidGlass, castsShadow and
	// dissolve go back to the model's, as they always have (docs/engine_split.md, step 9's list).
	TEST(EngineTest, AReBindKeepsTheUserLanesAndTheGround) {
		auto& registry = ECSHandler::registry();
		constexpr const char* kPath = "models/test_anim.fbx";
		const auto entity = PropertiesModule::PropertiesSystem::spawnModel(kPath, {}, Math::Vec3{ 1.f }, {});
		const Finally takeOut([entity] {
			ECSHandler::destroyEntity(entity);
			step();
		});
		step();

		const std::array<Math::Vec4, 2> lanes{ Math::Vec4{ 1.f, 2.f, 3.f, 4.f }, Math::Vec4{ 5.f, 6.f, 7.f, -8.f } };
		std::vector<MaterialComponent> given;
		{
			auto mesh = registry.pinComponent<MeshComponent>(entity);
			ASSERT_TRUE(mesh) << kPath << " was spawned without meshes";
			ASSERT_FALSE(mesh->meshMaterials.empty()) << kPath << " was bound without mesh materials";
			given = mesh->meshGiven;
			ASSERT_EQ(given.size(), mesh->meshMaterials.size());
			for (auto& material : mesh->meshMaterials) {
				material.user = lanes;
				material.ground = true;
				material.name += " renamed";
				material.solidGlass = !material.solidGlass;
				material.castsShadow = !material.castsShadow;
				material.dissolve = 0.5f;
			}
		}
		SystemsModule::TasksManager::instance()->notify({ entity, SystemsModule::MATERIAL_UPDATED });
		step();

		PropertiesModule::ModelBinding::bind(entity);
		step();

		const auto mesh = registry.pinComponent<const MeshComponent>(entity);
		ASSERT_TRUE(mesh);
		ASSERT_EQ(mesh->meshMaterials.size(), given.size());
		for (size_t i = 0; i < given.size(); ++i) {
			const auto& material = mesh->meshMaterials[i];
			EXPECT_TRUE(material.user == lanes) << "mesh " << i << ": the lanes were dropped";
			EXPECT_TRUE(material.ground) << "mesh " << i << ": the ground was dropped";
			EXPECT_EQ(material.name, given[i].name) << "mesh " << i;
			EXPECT_EQ(material.solidGlass, given[i].solidGlass) << "mesh " << i;
			EXPECT_EQ(material.castsShadow, given[i].castsShadow) << "mesh " << i;
			EXPECT_EQ(material.dissolve, given[i].dissolve) << "mesh " << i;
		}
	}
}

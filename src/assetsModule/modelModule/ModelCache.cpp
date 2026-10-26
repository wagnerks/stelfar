#include "ModelCache.h"

#include <assimp/DefaultIOSystem.h>
#include <assimp/Importer.hpp>
#include <meshoptimizer.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <type_traits>
#include <unordered_set>

#include "core/BinaryCache.h"
#include "core/FileSystem.h"

namespace AssetsModule::ModelCache {
	namespace {
		using SFE::BinaryCache::Reader;
		using SFE::BinaryCache::Writer;

		// Bump when what a model file is read into changes: ModelLoader's reading of it, weldMesh, generateTangents,
		// buildMeshLods, or what is written here. A field added to a type written here changes its size, which the
		// file's name takes in too, so that one is caught without.
		constexpr int kVersion = 1;

		std::string fileOf(const std::string& path) {
			const auto key = std::format("model v{} meshopt {} sizes {} {} {} {} {} {} {} | {}", kVersion, MESHOPTIMIZER_VERSION,
				sizeof(SFE::Vertex3D), sizeof(SFE::MeshObject3D), sizeof(SFE::Material), sizeof(SFE::MaterialTexture),
				sizeof(Armature), sizeof(Bone), sizeof(BoneAnimationKeys), path);
			return SFE::BinaryCache::path("model", SFE::BinaryCache::hash(key));
		}

		// DefaultIOSystem, writing down every file it is asked about.
		class WatchingIOSystem : public Assimp::DefaultIOSystem {
		public:
			explicit WatchingIOSystem(std::vector<Source>& sources) : mSources(sources) {}
			bool Exists(const char* file) const override {
				mSources.push_back(source(absolute(file)));
				return DefaultIOSystem::Exists(file);
			}
			Assimp::IOStream* Open(const char* file, const char* mode = "rb") override {
				mSources.push_back(source(absolute(file)));
				return DefaultIOSystem::Open(file, mode);
			}

		private:
			// The importer opens a relative path from the working directory, not from the content roots.
			static std::string absolute(const char* file) {
				std::error_code ec;
				return std::filesystem::absolute(file, ec).string();
			}
			std::vector<Source>& mSources;
		};

		// Each type written and read by one function, so the two cannot disagree on the layout. A always is Writer or
		// Reader; written, nothing is changed.
		template<class A>
		constexpr bool kReading = std::is_same_v<A, Reader>;

		// No real count comes near it: one past it is a file read wrongly, not something to make room for.
		constexpr uint64_t kMaxCount = uint64_t{ 1 } << 24;

		// Whether to go on: always, written; read, till the read goes wrong.
		template<class A>
		bool good(A& a) {
			if constexpr (kReading<A>) {
				return static_cast<bool>(a);
			}
			return true;
		}

		// A count of what follows: written as it is; read, 0 once the read has gone wrong.
		template<class A>
		uint64_t count(A& a, uint64_t n) {
			a.value(n);
			if constexpr (kReading<A>) {
				if (!a || n > kMaxCount) {
					a.fail();
					return 0;
				}
			}
			return n;
		}

		// Declared ahead of the containers' io, which call them: lookup from inside a template does not find them later,
		// the types being in other namespaces.
		template<class A> void io(A& a, Source& s);
		template<class A> void io(A& a, SFE::MaterialTexture& t);
		template<class A> void io(A& a, SFE::Material& m);
		template<class A> void io(A& a, SFE::MeshObject3D& part);
		template<class A> void io(A& a, SFE::Tree<SFE::MeshObject3D>& node);
		template<class A> void io(A& a, Bone& bone);
		template<class A> void io(A& a, Armature& armature);
		template<class A> void io(A& a, BoneAnimationKeys& keys);
		template<class A> void io(A& a, Animation& animation);

		template<class A, class T> requires std::is_trivially_copyable_v<T>
		void io(A& a, T& v) { a.value(v); }
		template<class A>
		void io(A& a, std::string& s) { a.text(s); }
		template<class A, class T> requires std::is_trivially_copyable_v<T>
		void io(A& a, std::vector<T>& v) { a.vector(v); }

		template<class A, class T> requires (!std::is_trivially_copyable_v<T>)
		void io(A& a, std::vector<T>& v) {
			const auto n = count(a, v.size());
			if constexpr (kReading<A>) {
				v.resize(static_cast<size_t>(n));
			}
			for (size_t i = 0; i < v.size() && good(a); ++i) {
				io(a, v[i]);
			}
		}

		template<class A, class Map>
		void ioMap(A& a, Map& map) {
			if constexpr (kReading<A>) {
				map.clear();
				const auto n = count(a, 0);
				for (uint64_t i = 0; i < n && a; ++i) {
					typename Map::key_type key{};
					typename Map::mapped_type value{};
					io(a, key);
					io(a, value);
					map.emplace(std::move(key), std::move(value));
				}
			}
			else {
				// By key: an unordered map's own order is not the one it was filled in, and the file is then the same
				// for the same model.
				std::vector<typename Map::value_type*> entries;
				for (auto& entry : map) {
					entries.push_back(&entry);
				}
				std::sort(entries.begin(), entries.end(), [](const auto* x, const auto* y) { return x->first < y->first; });
				count(a, entries.size());
				for (auto* entry : entries) {
					auto key = entry->first;
					io(a, key);
					io(a, entry->second);
				}
			}
		}

		template<class A>
		void io(A& a, Source& s) {
			io(a, s.path);
			io(a, s.exists);
			io(a, s.size);
			io(a, s.time);
		}

		template<class A>
		void io(A& a, SFE::MaterialTexture& t) {
			// Not the texture: ModelLoader's publish reads it again from the path.
			io(a, t.uniformSlot);
			io(a, t.type);
			io(a, t.uvSet);
			io(a, t.path);
		}

		template<class A>
		void io(A& a, SFE::Material& m) {
			ioMap(a, m.materialTextures);
			io(a, m.name);
			io(a, m.fragmentShader);
			io(a, m.castsShadow);
			io(a, m.transparent);
			io(a, m.alphaCutout);
			io(a, m.alphaFromMaps);
			io(a, m.alphaDither);
			io(a, m.hasDiffuseAlpha);
			io(a, m.gltfPbr);
			io(a, m.opacity);
			io(a, m.ior);
			io(a, m.roughness);
			io(a, m.metallic);
			io(a, m.alphaCutoff);
			io(a, m.specFactor);
			io(a, m.clearcoat);
			io(a, m.clearcoatRough);
			io(a, m.baseColor);
			io(a, m.emissive);
		}

		// The vertices through meshoptimizer's codec: lossless, a few times smaller, and decoded at gigabytes a second.
		template<class A>
		void ioVertices(A& a, std::vector<SFE::Vertex3D>& vertices) {
			uint64_t n = vertices.size();
			std::vector<unsigned char> encoded;
			if constexpr (!kReading<A>) {
				encoded.resize(meshopt_encodeVertexBufferBound(vertices.size(), sizeof(SFE::Vertex3D)));
				encoded.resize(meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), vertices.data(), vertices.size(), sizeof(SFE::Vertex3D)));
			}
			n = count(a, n);
			io(a, encoded);
			if constexpr (kReading<A>) {
				vertices.resize(static_cast<size_t>(n));
				if (a && meshopt_decodeVertexBuffer(vertices.data(), vertices.size(), sizeof(SFE::Vertex3D), encoded.data(), encoded.size()) != 0) {
					a.fail();
				}
			}
		}

		template<class A>
		void io(A& a, SFE::MeshObject3D& part) {
			io(a, part.mesh.indices);
			ioVertices(a, part.mesh.vertices);
			io(a, part.mesh.lodIndices);
			io(a, part.mesh.lodErrors);
			io(a, part.mesh.poseBounds);
			io(a, part.material);
			io(a, part.transform);
			io(a, part.aabb);
			io(a, part.nodeName);
			ioMap(a, part.extras);
		}

		// The children in their order: Tree::addChild puts each one first, so a read appends them instead.
		template<class A>
		void io(A& a, SFE::Tree<SFE::MeshObject3D>& node) {
			io(a, node.value);
			if constexpr (kReading<A>) {
				const auto n = count(a, 0);
				auto last = node.children.before_begin();
				for (uint64_t i = 0; i < n && a; ++i) {
					last = node.children.emplace_after(last);
					last->parent = &node;
					io(a, *last);
				}
			}
			else {
				count(a, static_cast<uint64_t>(std::distance(node.children.begin(), node.children.end())));
				for (auto& child : node.children) {
					io(a, child);
				}
			}
		}

		template<class A>
		void io(A& a, Bone& bone) {
			io(a, bone.name);
			io(a, bone.id);
			io(a, bone.offset);
			io(a, bone.transform);
			io(a, bone.pos);
			io(a, bone.scale);
			io(a, bone.rotation);
			io(a, bone.parentBoneIdx);
			io(a, bone.childrenBones);
		}

		template<class A>
		void io(A& a, Armature& armature) {
			io(a, armature.name);
			io(a, armature.transform);
			io(a, armature.bones);
			io(a, armature.poseBoundsCount);
			io(a, armature.boneRadius);
			io(a, armature.boneBoundsMask);
		}

		template<class A>
		void io(A& a, BoneAnimationKeys& keys) {
			io(a, keys.positions);
			io(a, keys.rotations);
			io(a, keys.scales);
		}

		template<class A>
		void io(A& a, Animation& animation) {
			std::string name;
			float duration = 0.f;
			float ticks = 0.f;
			if constexpr (!kReading<A>) {
				name = animation.getName();
				duration = animation.getDuration();
				ticks = animation.getTicksPerSecond();
			}
			io(a, name);
			io(a, duration);
			io(a, ticks);
			if constexpr (kReading<A>) {
				animation = Animation(std::move(name), duration, ticks);
				const auto n = count(a, 0);
				for (uint64_t i = 0; i < n && a; ++i) {
					std::string bone;
					io(a, bone);
					io(a, animation.keysOf(bone));
				}
			}
			else {
				ioMap(a, const_cast<std::unordered_map<std::string, BoneAnimationKeys>&>(animation.keys()));
			}
		}
	}

	Source source(const std::string& path) {
		Source s{ path };
		std::error_code ec;
		const std::filesystem::directory_entry entry(SFE::FileSystem::resolve(path), ec);
		if (!ec && entry.is_regular_file(ec)) {
			s.exists = true;
			s.size = entry.file_size(ec);
			s.time = static_cast<int64_t>(entry.last_write_time(ec).time_since_epoch().count());
		}
		return s;
	}

	void watch(Assimp::Importer& importer, std::vector<Source>& sources) {
		importer.SetIOHandler(new WatchingIOSystem(sources));
	}

	std::string file(const std::string& path) {
		return fileOf(path);
	}

	bool read(const std::string& path, SFE::Tree<SFE::MeshObject3D>& meshes, Armature& armature, std::vector<Animation>& animations,
		std::vector<Source>* sources) {
		Reader reader(fileOf(path));
		if (!reader) {
			return false;
		}
		// The file the path finds now: another content root may have one of the name since.
		std::string resolved;
		io(reader, resolved);
		if (!reader || resolved != SFE::FileSystem::resolve(path).string()) {
			return false;
		}
		const auto files = count(reader, 0);
		for (uint64_t i = 0; i < files; ++i) {
			Source was;
			io(reader, was);
			const auto now = source(was.path);
			if (!reader || now.exists != was.exists || now.size != was.size || now.time != was.time) {
				return false;
			}
			if (sources) {
				sources->push_back(std::move(was));
			}
		}
		io(reader, meshes);
		io(reader, armature);
		io(reader, animations);
		return static_cast<bool>(reader);
	}

	void write(const std::string& path, const SFE::Tree<SFE::MeshObject3D>& meshes, const Armature& armature,
		const std::vector<Animation>& animations, const std::vector<Source>& sources) {
		// Not one with letters on it (ModelText): they are laid out on a font's picture made at run time, which no file
		// stands for, and that picture is no compressed texture as every other a model names is.
		for (const auto& node : meshes) {
			if (node.value.extras.contains("text")) {
				return;
			}
		}
		// Each file once, as it was first seen.
		std::vector<Source> seen;
		std::unordered_set<std::string> paths;
		for (const auto& s : sources) {
			if (paths.insert(s.path).second) {
				seen.push_back(s);
			}
		}
		Writer writer(fileOf(path));
		auto resolved = SFE::FileSystem::resolve(path).string();
		io(writer, resolved);
		io(writer, seen);
		io(writer, const_cast<SFE::Tree<SFE::MeshObject3D>&>(meshes));
		io(writer, const_cast<Armature&>(armature));
		io(writer, const_cast<std::vector<Animation>&>(animations));
		writer.commit();
	}
}

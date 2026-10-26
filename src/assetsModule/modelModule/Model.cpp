#include "Model.h"
#include "MeshLod.h"

#include "MeshUtils.h"
#include "MeshVaoRegistry.h"

namespace AssetsModule {
	Model::Model(SFE::Tree<SFE::MeshObject3D> model, Armature armature, std::vector<Animation> animations) : mArmature(std::move(armature)), mAnimations(std::move(animations)), mMeshTree(std::move(model)) {
		if (!mArmature.bones.empty()) {
			measurePoseBounds();
		}
		bindMeshes();

		if (!mArmature.bones.empty()) {
			mDefaultBoneMatrices.resize(mArmature.bones.size(), mArmature.transform);

			//std::function<void(Bone*, SFE::Math::Mat4, std::vector<Bone>&)> calculateBoneTransform;
			//calculateBoneTransform = [this, &calculateBoneTransform](Bone* node, SFE::Math::Mat4 parentTransform, std::vector<Bone>& bones) {
			//	parentTransform *= node->transform;
			//	mDefaultBoneMatrices[node->id] = parentTransform;
			//	
			//	for (const auto child : node->childrenBones) {
			//		calculateBoneTransform(&bones[child], parentTransform, bones);
			//	}
			//};
			//
			//calculateBoneTransform(&mArmature.bones[0], {}/*mArmature.transform*/, mArmature.bones);
		}
	}

	std::vector<Model::LOD>* Model::getLODs() {
		if (mLODs.empty()) {
			for (auto& node : mMeshTree) {
				if (node.value.mesh.vertices.empty()) {
					continue;
				}
				if (0/*node.value.mesh.lod*/ >= mLODs.size()) {
					mLODs.resize(/*node.value.mesh.lod +*/ 1);
				}

				mLODs[/*node.value.mesh.lod*/0].meshes.push_back(&node.value);
			}
		}

		return &mLODs;
	}

	void Model::recalculateNormals(bool smooth) {
		for (auto& node : mMeshTree) {
			SFE::MeshUtils::recalculateNormals(&node.value.mesh, smooth);
		}
	}

	void Model::measurePoseBounds() {
		std::vector<const SFE::Math::Mat4*> offsets;
		for (const auto& bone : mArmature.bones) {
			if (bone.id >= offsets.size()) {
				offsets.resize(bone.id + 1, nullptr);
			}
			offsets[bone.id] = &bone.offset;
		}
		mArmature.boneRadius.assign(offsets.size(), 0.f);
		mArmature.boneBoundsMask.assign(offsets.size(), 0u);
		mArmature.poseBoundsCount = 0;
		for (auto& node : mMeshTree) {
			auto& mesh = node.value.mesh;
			mesh.poseBounds = 0;
			const uint32_t slot = (std::min)(mArmature.poseBoundsCount, Armature::kMaxPoseBounds - 1);
			bool skinned = false;
			for (const auto& v : mesh.vertices) {
				for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
					const int id = v.boneIDs[i];
					if (id < 0 || static_cast<size_t>(id) >= offsets.size() || !offsets[id] || v.weights[i] <= 0.f) {
						continue;
					}
					skinned = true;
					const SFE::Math::Vec4 local = *offsets[id] * SFE::Math::Vec4(v.position, 1.f);
					mArmature.boneRadius[id] = (std::max)(mArmature.boneRadius[id], SFE::Math::length(SFE::Math::Vec3(local)));
					mArmature.boneBoundsMask[id] |= 1u << slot;
				}
			}
			if (skinned) {
				mesh.poseBounds = slot + 1;
				mArmature.poseBoundsCount = (std::min)(mArmature.poseBoundsCount + 1, Armature::kMaxPoseBounds);
			}
		}
	}

	void Model::bindMeshes() {
		std::vector<SFE::Mesh<SFE::Vertex3D>*> meshes;
		for (auto& node : mMeshTree) {
			// Before the upload: the registry puts the whole chain in the index buffer in one go. A mesh that comes with
			// its levels already (the terrain, from its cache) keeps them, and the vertices they index.
			if (node.value.mesh.lodIndices.empty()) {
				SFE::weldMesh(node.value.mesh);
				SFE::markOneSided(node.value.mesh);
				SFE::buildMeshLods(node.value.mesh);
			}
			meshes.push_back(&node.value.mesh);
		}
		SFE::MeshVaoRegistry::instance()->initMeshes(meshes);

		mLODs.clear();
		getLODs();
	}

	const std::vector<SFE::Math::Mat4>& Model::getDefaultBoneMatrices() const {
		return mDefaultBoneMatrices;
	}
}

size_t AssetsModule::Model::keepOwnBones(const Armature& played) {
	// A bone may sit a hair off between two exports of the same rig; this is about bones that were truly moved.
	constexpr float kSameSpot = 1e-4f;
	const auto sits = [](const Bone& bone) {
		// Where the bone sits on its parent is the last column of its rest; Bone::pos is not filled by the loader.
		return SFE::Math::Vec3{ bone.transform[3].x, bone.transform[3].y, bone.transform[3].z };
	};
	// A clip is entitled to move a bone off its parent -- the hips carry the whole body through a run -- so one frame
	// proves nothing. A bone belongs to another rig only when every clip holds it still, and holds it somewhere the
	// model does not: that is a length the clips were built on, not a movement they perform.
	std::vector<char> foreign(played.bones.size(), 1);
	std::vector<char> keyed(played.bones.size(), 0);
	for (const auto& clip : mAnimations) {
		for (size_t i = 0; i < played.bones.size(); ++i) {
			const auto& bone = played.bones[i];
			if (bone.parentBoneIdx >= played.bones.size()) {
				foreign[i] = 0;   // the root's place is the clip's to give: it is how the body is carried about
				continue;
			}
			const auto* keys = clip.getBoneAnimationInfo(bone.name);
			if (!keys || keys->positions.empty()) {
				continue;
			}
			keyed[i] = 1;
			const auto& first = keys->positions.front().position;
			bool still = true;
			for (const auto& key : keys->positions) {
				if (SFE::Math::length(key.position - first) > kSameSpot) {
					still = false;
					break;
				}
			}
			if (!still || SFE::Math::length(first - sits(bone)) <= kSameSpot) {
				foreign[i] = 0;
			}
		}
	}
	std::vector<std::string> wrong;
	for (size_t i = 0; i < played.bones.size(); ++i) {
		if (foreign[i] && keyed[i]) {
			wrong.push_back(played.bones[i].name);
		}
	}
	size_t freed = 0;
	for (auto& clip : mAnimations) {
		freed += clip.forgetBones(wrong);
	}
	return freed;
}

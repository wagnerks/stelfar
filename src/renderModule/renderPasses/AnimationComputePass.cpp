#include "AnimationComputePass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "assetsModule/modelModule/Animation.h"
#include "assetsModule/modelModule/Armature.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/ModelComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render;
using namespace SFE::Render::RenderPasses;

namespace {
	static_assert(sizeof(AnimationComputePass::GpuBoneDesc) == 192);
	static_assert(sizeof(AnimationComputePass::GpuVec3Key) == 16);
	static_assert(sizeof(AnimationComputePass::GpuQuatKey) == 32);
	static_assert(sizeof(AnimationComputePass::GpuRecord) == 224);

	constexpr uint32_t kPlaying = 1u;
	constexpr uint32_t kLoop = 2u;
	constexpr uint32_t kBlending = 4u;
	// The shader's time is a float of seconds since the epoch; this keeps its step under 1e-4 s.
	constexpr double kRebaseSeconds = 512.0;

	struct AnimClock {
		float now = 0.f;
		uint32_t count = 0;
	};

	SFE::Render::BufferHandle createStorage(SFE::Render::RenderHardwareInterface* rhi, size_t size, const void* data, const char* name) {
		SFE::Render::BufferDesc desc;
		desc.size = size < 16 ? 16 : size;
		desc.usage = SFE::Render::BufferUsage::Storage;
		desc.cpuAccess = data ? SFE::Render::CpuAccess::Write : SFE::Render::CpuAccess::None;
		desc.debugName = name;
		return rhi->createBuffer(desc, data);
	}

	// The gap between a channel's keys when every gap is the same (to a thousandth of it), else 0.
	template<typename Key>
	float uniformKeyStep(const std::vector<Key>& keys) {
		if (keys.size() < 2) {
			return 0.f;
		}
		const double step = keys[1].timeStamp - keys[0].timeStamp;
		if (step <= 0.0) {
			return 0.f;
		}
		for (size_t i = 2; i < keys.size(); ++i) {
			if (std::abs((keys[i].timeStamp - keys[i - 1].timeStamp) - step) > step * 1e-3) {
				return 0.f;
			}
		}
		return static_cast<float>(step);
	}

	void collectOrder(const std::vector<AssetsModule::Bone>& bones, uint32_t idx, std::vector<int>& order, std::vector<char>& seen) {
		if (idx >= bones.size() || seen[idx]) {
			return;
		}
		seen[idx] = 1;
		order.push_back(static_cast<int>(idx));
		for (const auto child : bones[idx].childrenBones) {
			collectOrder(bones, child, order, seen);
		}
	}
}

void AnimationComputePass::init(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();

	DescriptorSetLayoutDesc layout;
	layout.bindings = {
		{ 20, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 21, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 22, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 23, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 24, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 25, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 26, DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 2,  DescriptorType::StorageBuffer, ShaderStage::Compute },
		{ 9,  DescriptorType::StorageBuffer, ShaderStage::Compute },
	};
	mSetLayout = rhi->createDescriptorSetLayout(layout);
	mSet = rhi->allocateDescriptorSet(mSetLayout);

	ComputePipelineDesc desc;
	desc.compute = rhi->loadShader(ShaderStage::Compute, "shaders/animateBones.cs");
	desc.descriptorSetLayouts = { mSetLayout };
	desc.pushConstantSize = sizeof(AnimClock);
	mPipeline = rhi->createComputePipeline(desc);
}

const AnimationComputePass::ClipInfo* AnimationComputePass::packClip(const AssetsModule::Animation* animation, const AssetsModule::Armature& armature) {
	if (!animation || armature.bones.empty()) {
		return nullptr;
	}

	if (const auto it = mClips.find(animation); it != mClips.end()) {
		return &it->second;
	}

	ClipInfo info;
	info.descOffset = static_cast<uint32_t>(mDescs.size());
	info.boneCount = static_cast<uint32_t>(armature.bones.size());

	std::vector<char> seen(armature.bones.size(), 0);
	for (size_t i = 0; i < armature.bones.size(); ++i) {
		const auto parent = armature.bones[i].parentBoneIdx;
		if (parent == std::numeric_limits<uint32_t>::max() || parent >= armature.bones.size()) {
			collectOrder(armature.bones, static_cast<uint32_t>(i), mOrder, seen);
		}
	}
	for (size_t i = 0; i < armature.bones.size(); ++i) {
		if (!seen[i]) {
			collectOrder(armature.bones, static_cast<uint32_t>(i), mOrder, seen);
		}
	}

	mDescs.resize(mDescs.size() + armature.bones.size());
	for (size_t i = 0; i < armature.bones.size(); ++i) {
		const auto& bone = armature.bones[i];
		auto& desc = mDescs[info.descOffset + i];
		desc.offset = bone.offset;
		desc.restLocal = bone.transform;
		desc.boneId = static_cast<int>(bone.id);
		if (bone.id < armature.boneRadius.size()) {
			desc.radius = armature.boneRadius[bone.id];
			desc.boundsMask = armature.boneBoundsMask[bone.id];
		}
		if (bone.parentBoneIdx == std::numeric_limits<uint32_t>::max() || bone.parentBoneIdx >= armature.bones.size()) {
			desc.parent = -1;
		}
		else {
			desc.parent = static_cast<int>(bone.parentBoneIdx);
		}

		const auto* keys = animation->getBoneAnimationInfo(bone.name);
		if (!keys) {
			continue;
		}

		desc.posStep = uniformKeyStep(keys->positions);
		desc.rotStep = uniformKeyStep(keys->rotations);
		desc.sclStep = uniformKeyStep(keys->scales);

		desc.posOff = static_cast<int>(mPosKeys.size());
		desc.posCnt = static_cast<int>(keys->positions.size());
		for (const auto& k : keys->positions) {
			mPosKeys.push_back({ k.position.x, k.position.y, k.position.z, k.timeStamp });
		}

		desc.rotOff = static_cast<int>(mRotKeys.size());
		desc.rotCnt = static_cast<int>(keys->rotations.size());
		for (const auto& k : keys->rotations) {
			mRotKeys.push_back({ k.orientation.x, k.orientation.y, k.orientation.z, k.orientation.w, k.timeStamp, {} });
		}

		desc.sclOff = static_cast<int>(mSclKeys.size());
		desc.sclCnt = static_cast<int>(keys->scales.size());
		for (const auto& k : keys->scales) {
			mSclKeys.push_back({ k.scale.x, k.scale.y, k.scale.z, k.timeStamp });
		}
	}

	mClipsDirty = true;
	const auto [it, _] = mClips.emplace(animation, info);
	return &it->second;
}

void AnimationComputePass::markRecord(const uint32_t index) {
	mDirtyLo = (std::min)(mDirtyLo, index);
	mDirtyHi = (std::max)(mDirtyHi, index + 1);
}

void AnimationComputePass::writeRecord(const ecss::EntityId entity) {
	auto& registry = ECSHandler::registry();
	const auto animation = registry.pinComponent<ComponentsModule::AnimationComponent>(entity);
	const auto armature = registry.pinComponent<const ComponentsModule::ArmatureComponent>(entity);
	const ClipInfo* clip = animation && armature ? packClip(animation->mCurrentAnimation, armature->armature) : nullptr;
	const auto bones = clip ? ECSHandler::getSystem<SystemsModule::RenderSystem>()->boneRange(entity) : SystemsModule::RenderSystem::BoneRange{};
	// The clip's bones must fit the range: its worlds rows are the range's, and a longer clip would run into the
	// next entity's while both are written in parallel.
	// Posed by hand, the skeleton is written straight into the component and uploaded there (RenderSystem::uploadBones);
	// the clip is only what that pose was built on, so this pass must not write over it.
	if (!clip || clip->boneCount == 0 || clip->boneCount > bones.count || animation->mPosedByHand) {
		removeRecord(entity);
		return;
	}

	auto [it, added] = mRecordIndex.emplace(entity, static_cast<uint32_t>(mRecords.size()));
	if (added) {
		mRecords.emplace_back();
		mRecordEntities.push_back(entity);
	}
	const auto* anim = animation->mCurrentAnimation;
	if (animation->mPlay && std::isnan(animation->mStart)) {
		animation->setTime(animation->mStoppedTime, Engine::instance()->animationClock());
	}
	auto& record = mRecords[it->second];
	record.armatureTransform = armature->armature.transform;
	record.boneOffset = bones.offset;
	record.descOffset = clip->descOffset;
	record.boneCount = clip->boneCount;
	record.flags = (animation->mPlay ? kPlaying : 0u) | (animation->mLoop ? kLoop : 0u) | (bones.bounds << 8);
	record.start = static_cast<float>(animation->mStart - mEpoch);
	record.ticksPerSecond = ComponentsModule::AnimationComponent::ticksPerSecond(*anim);
	record.duration = anim->getDuration();
	record.stoppedTime = animation->mStoppedTime;
	record.fromFlags = 0;
	if (animation->mFromAnimation && animation->mBlendDuration > 0.f) {
		if (const auto* from = packClip(animation->mFromAnimation, armature->armature); from && from->boneCount == clip->boneCount) {
			const auto* fromAnim = animation->mFromAnimation;
			record.fromDescOffset = from->descOffset;
			record.fromFlags = kBlending | (animation->mFromPlay ? kPlaying : 0u) | (animation->mFromLoop ? kLoop : 0u);
			record.fromStart = static_cast<float>(animation->mFromStart - mEpoch);
			record.fromTicksPerSecond = ComponentsModule::AnimationComponent::ticksPerSecond(*fromAnim);
			record.fromDuration = fromAnim->getDuration();
			record.fromStoppedTime = animation->mFromStoppedTime;
			record.blendStart = static_cast<float>(animation->mBlendStart - mEpoch);
			record.blendDuration = animation->mBlendDuration;
		}
	}
	const auto turn = [&](const Math::Quaternion<float>& by, const auto& shared, float* q, int32_t* bone, float* share) {
		q[0] = by.x;
		q[1] = by.y;
		q[2] = by.z;
		q[3] = by.w;
		for (size_t k = 0; k < shared.size(); ++k) {
			const auto& look = shared[k];
			const auto& bones = armature->armature.bones;
			const auto found = !look.name ? bones.end() : std::find_if(bones.begin(), bones.end(), [&](const auto& b) { return b.name == std::string_view(look.name); });
			bone[k] = found == bones.end() ? -1 : static_cast<int32_t>(found - bones.begin());
			share[k] = look.share;
		}
	};
	turn(animation->mLook, animation->mLookBones, record.look, record.lookBone, record.lookShare);
	turn(animation->mTwist, animation->mTwistBones, record.twist, record.twistBone, record.twistShare);
	markRecord(it->second);
	mWorldsRows = (std::max)(mWorldsRows, static_cast<size_t>(bones.offset) + clip->boneCount);
}

void AnimationComputePass::removeRecord(const ecss::EntityId entity) {
	const auto it = mRecordIndex.find(entity);
	if (it == mRecordIndex.end()) {
		return;
	}
	const auto index = it->second;
	mRecordIndex.erase(it);
	const auto last = static_cast<uint32_t>(mRecords.size() - 1);
	if (index != last) {
		mRecords[index] = mRecords[last];
		mRecordEntities[index] = mRecordEntities[last];
		mRecordIndex[mRecordEntities[index]] = index;
		markRecord(index);
	}
	mRecords.pop_back();
	mRecordEntities.pop_back();
}

void AnimationComputePass::upload() {
	auto* rhi = Engine::instance()->rhi();
	// The RHI keeps a destroyed buffer until the frames that may still bind it have finished.
	if (mClipsDirty) {
		mClipsDirty = false;
		const auto replace = [rhi](BufferHandle& buffer, const auto& data, const char* name) {
			rhi->destroyBuffer(buffer);
			buffer = createStorage(rhi, data.size() * sizeof(data[0]), data.data(), name);
		};
		replace(mDescBuffer, mDescs, "AnimBoneDescs");
		replace(mPosBuffer, mPosKeys, "AnimPosKeys");
		replace(mRotBuffer, mRotKeys, "AnimRotKeys");
		replace(mSclBuffer, mSclKeys, "AnimSclKeys");
		replace(mOrderBuffer, mOrder, "AnimBoneOrder");
	}

	if (mRecords.size() > mRecordCapacity || !mRecordBuffer) {
		mRecordCapacity = (std::max)(mRecordCapacity, size_t{ 64 });
		while (mRecordCapacity < mRecords.size()) {
			mRecordCapacity *= 2;
		}
		rhi->destroyBuffer(mRecordBuffer);
		mRecordBuffer = createStorage(rhi, mRecordCapacity * sizeof(GpuRecord), nullptr, "AnimRecords");
		mDirtyLo = 0;
		mDirtyHi = static_cast<uint32_t>(mRecords.size());
	}
	const auto hi = (std::min)(mDirtyHi, static_cast<uint32_t>(mRecords.size()));
	if (mDirtyLo < hi) {
		rhi->updateBuffer(mRecordBuffer, mDirtyLo * sizeof(GpuRecord), (hi - mDirtyLo) * sizeof(GpuRecord), mRecords.data() + mDirtyLo);
	}
	mDirtyLo = UINT32_MAX;
	mDirtyHi = 0;

	if (mWorldsRows > mWorldsCapacity) {
		mWorldsCapacity = (std::max)(mWorldsCapacity, size_t{ 4096 });
		while (mWorldsCapacity < mWorldsRows) {
			mWorldsCapacity *= 2;
		}
		rhi->destroyBuffer(mWorlds);
		mWorlds = createStorage(rhi, mWorldsCapacity * sizeof(Math::Mat4), nullptr, "AnimBoneWorlds");
	}
}

void AnimationComputePass::prepare(SystemsModule::RenderData&) {
	FUNCTION_BENCHMARK;

	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	if (!renderSystem) {
		return;
	}
	auto dirty = renderSystem->takeDirtyAnimations();
	const double clock = Engine::instance()->animationClock();
	// On whole multiples of kRebaseSeconds, a function of the clock alone: the poses round alike whatever the clock was
	// on the frames before (a held clock among them).
	if (const double epoch = std::floor(clock / kRebaseSeconds) * kRebaseSeconds; epoch != mEpoch) {
		mEpoch = epoch;
		dirty.insert(dirty.end(), mRecordEntities.begin(), mRecordEntities.end());
	}
	for (const auto entity : dirty) {
		writeRecord(entity);
	}
	upload();
}

void AnimationComputePass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	if (!mPipeline || !renderDataHandle.bonesSSBO || !renderDataHandle.prevBonesSSBO || mRecords.empty()) {
		return;
	}

	const double clock = Engine::instance()->animationClock();
	auto* rhi = Engine::instance()->rhi();
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->updateDescriptorSets({
		{ mSet, 20, {{ mDescBuffer }} },
		{ mSet, 21, {{ mPosBuffer }} },
		{ mSet, 22, {{ mRotBuffer }} },
		{ mSet, 23, {{ mSclBuffer }} },
		{ mSet, 24, {{ mOrderBuffer }} },
		{ mSet, 25, {{ mRecordBuffer }} },
		{ mSet, 26, {{ mWorlds }} },
		{ mSet, 2,  {{ renderDataHandle.bonesSSBO }} },
		{ mSet, 9,  {{ renderDataHandle.prevBonesSSBO }} },
	});
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &mSet, 1);
	const AnimClock push{ static_cast<float>(clock - mEpoch), static_cast<uint32_t>(mRecords.size()) };
	rhi->cmdPushConstants(cmd, mPipeline, ShaderStage::Compute, &push, sizeof(push));
	rhi->cmdDispatch(cmd, (push.count + 15u) / 16u, 1, 1);

	rhi->cmdPipelineBarrier(cmd, {
		PipelineStage::Compute,
		PipelineStage::VertexShader,
		AccessMask::ShaderWrite,
		AccessMask::ShaderRead,
	});
}

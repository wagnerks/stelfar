#pragma once
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

#include "BoneAnimationKeys.h"


struct aiAnimation;

namespace AssetsModule {
    class Animation {
    public:
        Animation() = default;

        Animation(const aiAnimation* animation);
        // An empty clip, to be keyed bone by bone (keysOf): `duration` in ticks, `ticksPerSecond` of them a second.
        Animation(std::string name, float duration, float ticksPerSecond);
        // A clip `weight` of the way from `a` to `b`, key by key: for two clips made alike (the same keys at the same
        // times), as a pose made for a small thing and one for a big one. A bone keyed differently in them keeps a's.
        static Animation between(const Animation& a, const Animation& b, float weight);
        const AssetsModule::BoneAnimationKeys* getBoneAnimationInfo(const std::string& boneName) const;
        // Forget everything this clip says about these bones, so they stay where the model rests them. For bones the
        // clip was plainly not made for: a hand refitted to new geometry, a library of clips shared between
        // characters. Their turns go with their places -- a turn belongs to the frame the bone rested in, and on a
        // bone that has since moved it is as wrong as the place. Comes back with how many bones were freed.
        size_t forgetBones(const std::vector<std::string>& bones);

        inline float getTicksPerSecond() const { return mTicksPerSecond; }
        inline float getDuration() const { return mDuration; }
        inline const std::string& getName() const { return mName; }
        void rename(std::string name) { mName = std::move(name); }
        // A bone's keys to change -- a clip edited while the engine runs -- made empty when the clip has none for it.
        BoneAnimationKeys& keysOf(const std::string& boneName) { return mBoneAnimationInfos[boneName]; }
        // Every animated node's keys, by its name (ModelCache writes them).
        const std::unordered_map<std::string, BoneAnimationKeys>& keys() const { return mBoneAnimationInfos; }

    private:
        static void readKeys(const aiAnimation* animation, std::unordered_map<std::string, BoneAnimationKeys>& keys);
        std::string mName;
        float mDuration;
        float mTicksPerSecond;
        std::unordered_map<std::string, BoneAnimationKeys> mBoneAnimationInfos;
    };
}

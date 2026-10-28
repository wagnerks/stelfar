#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mathModule/Forward.h"
#include "systemsModule/SystemBase.h"

struct ma_engine;
struct ma_resource_manager;
struct ma_sound;

namespace SFE::SystemsModule {
	// Plays AudioSourceComponents and one-shot sounds through miniaudio, heard from the AudioListenerComponent or,
	// without one, the current camera. Runs on the main thread after the camera has moved.
	//
	// A sound in the world is heard through Steam Audio from the direction it comes from (an HRTF, for headphones),
	// duller the further off it is, with the reverb after it: see Settings.
	//
	// On the None backend, or when there is no audio device, it mixes into nothing: sounds start and report
	// playing, but never advance, so a sound that does not loop never ends there. Steam Audio is not started there.
	class AudioSystem : public ecss::System {
	public:
		AudioSystem();
		~AudioSystem() override;

		void update(float dt) override;

		// How sounds in the world are heard, to compare by ear: numbers of the registry (Params, group "Audio", registered
		// in AudioSystem.cpp), 1 on and 0 off. Read every frame.
		struct Settings {
			// Through the HRTF, from where each sound is. Off: as before, panned by miniaudio, and without the reverb
			// and the air absorption, which come on the same path.
			float hrtf = 1.f;
			// How loud the reverb is under the sounds it follows. A voice started without it (VoiceParams::reverb off)
			// has none.
			float reverb = 0.2f;
			// Far sounds duller: the air takes their highs first.
			float airAbsorption = 1.f;
		};
		Settings settings;

		// Fire and forget, at a point in the world.
		void playOneShot(std::string_view path, const Math::Vec3& pos, float volume = 1.f);
		// Fire and forget, a piece of a recording: `length` seconds from `start`, faded at both ends.
		void playSlice(std::string_view path, float start, float length, const Math::Vec3& pos, float volume, float pitch);
		// The separate hits in a recording of one sound after another -- the steps in a recording of someone walking
		// -- as start and length in seconds, each at most `maxLength` long. Decodes the file; cache what it returns.
		std::vector<std::pair<float, float>> findHits(std::string_view path, float maxLength);
		// The separate calls in a recording of one call after another -- a bird's songs, a fox's barks, a woodpecker's
		// drumming -- each running on until it falls silent for `gap` seconds, as start and length, at most `maxLength`.
		// Decodes the file; cache what it returns.
		std::vector<std::pair<float, float>> findCalls(std::string_view path, float gap, float maxLength);
		bool isPlaying(ecss::EntityId entity) const;

		// Voices: sounds another system drives itself, each through a low-pass filter so what stands between it and
		// the listener can muffle it. `stream` for long loops. 0 when the file cannot be opened.
		struct VoiceParams {
			Math::Vec3 pos{};
			float volume = 1.f;
			float minDistance = 1.f;
			float maxDistance = 100.f;
			float cutoff = 20000.f; // Hz
			float pitch = 1.f;      // and speed: a wheel's or a motor's turning heard faster
			bool spatial = true;
			// Off: none of the reverb on it, for music and the UI.
			bool reverb = true;
		};
		uint32_t startVoice(std::string_view path, bool loop, bool stream);
		// A voice of a piece of a recording, as playSlice plays one, but moved like any voice -- with what makes it, a
		// thing flying by: `length` seconds from `start` at `pitch` (give setVoice the same), then it ends.
		uint32_t startSlice(std::string_view path, float start, float length, float pitch);
		// Decodes a sound ahead of the first time it plays, on miniaudio's own thread: nothing on the calling one.
		void preload(std::string_view path);
		void setVoice(uint32_t voice, const VoiceParams& params);
		// A voice that does not loop has played to its end (a slice, its piece), or is gone.
		bool voiceEnded(uint32_t voice) const;
		void stopVoice(uint32_t voice);
		void setMasterVolume(float volume);
		float masterVolume() const { return mMasterVolume; }

	private:
		struct EngineDeleter { void operator()(ma_engine* engine) const; };
		struct ResourcesDeleter { void operator()(ma_resource_manager* resources) const; };
		struct SoundDeleter { void operator()(ma_sound* sound) const; };
		using Sound = std::unique_ptr<ma_sound, SoundDeleter>;

		// A node of miniaudio's graph that hears one sound through Steam Audio, and Steam Audio's part of the mix with
		// a pool of them made up front. In AudioSystem.cpp.
		struct Ear;
		struct Spatial;
		struct SpatialDeleter { void operator()(Spatial* spatial) const; };

		struct Source {
			Sound sound;
			std::string path;
			bool stream = false;
			uint64_t seenFrame = 0;
			Ear* ear = nullptr;
		};
		struct OneShot {
			Sound sound;
			Ear* ear = nullptr;
		};

		// `residentOnly`: only the file's resident made (decoded on miniaudio's own thread), no sound to play.
		Sound loadSound(std::string_view path, bool stream, bool residentOnly = false);
		void updateListener();
		// Sends a playing sound on through an ear or straight to the device, as the settings and the sound have it,
		// and turns its ear towards it. `out` is the last node of the sound's own chain: the sound, or a voice's filter.
		void route(ma_sound* sound, void* out, Ear*& ear, bool spatial, bool reverb);
		// The sound before its ear: the sound feeds the ear, which the next sound may take once it is free.
		void stop(Sound& sound, Ear*& ear);
		void startOneShot(Sound sound);
		// A loaded sound made a voice, started at no volume until setVoice: its id.
		uint32_t addVoice(Sound sound, bool loop);
		// A sound set to play `length` seconds from `start` at `pitch`, faded in and out.
		void cut(ma_sound* sound, float start, float length, float pitch);

		// What decodes the sounds and keeps their data (the engine's own would keep it as float at the device's rate):
		// declared before the engine, which uses it, so it is destroyed after it.
		std::unique_ptr<ma_resource_manager, ResourcesDeleter> mResources;
		// Declared before every sound so it is destroyed after them: each is uninitialised against it.
		std::unique_ptr<ma_engine, EngineDeleter> mEngine;
		// Destroyed after every sound, which may feed its ears, and before the engine its nodes belong to.
		std::unique_ptr<Spatial, SpatialDeleter> mSpatial;
		// One idle sound of every file played decoded. miniaudio shares a file's decoded data between its sounds but
		// frees it with the last of them, so a bird that sang, stopped and sang again decoded its song again each time:
		// a few milliseconds on the main thread, a stall every few seconds among many sounds.
		std::unordered_map<std::string, Sound> mResident;
		std::unordered_map<ecss::EntityId, Source> mSources;
		std::vector<OneShot> mOneShots;
		struct Voice;
		struct VoiceDeleter { void operator()(Voice* voice) const; };
		std::unordered_map<uint32_t, std::unique_ptr<Voice, VoiceDeleter>> mVoices;
		uint32_t mNextVoice = 1;
		uint64_t mFrame = 0;
		float mMasterVolume = 1.f;
	};
}

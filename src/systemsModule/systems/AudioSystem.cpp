#include "AudioSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <miniaudio.h>
#include <extras/nodes/ma_reverb_node/ma_reverb_node.h>
#include <phonon.h>

#include "CameraSystem.h"
#include "componentsModule/AudioListenerComponent.h"
#include "componentsModule/AudioSourceComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/FileSystem.h"
#include "logsModule/Logger.h"
#include "mathModule/VectorOperations.h"
#include "rhi/GraphicsBackend.h"

namespace SFE::SystemsModule {
	namespace {
		// Steam Audio works in whole blocks of samples. The device is asked for blocks of this size, so miniaudio's graph
		// runs one block at a time, and Steam Audio is given the size the graph then has: about 5 ms at 48 kHz.
		constexpr ma_uint32 kBlock = 256;
		// Sounds heard through the HRTF at once. One more is panned by miniaudio until an ear is free.
		constexpr size_t kEars = 48;
		// The wood's reverb, on verblib's scales of 0 to 1: about a second long, its highs dying first, as among leaves.
		constexpr float kRoomSize = 0.4f;
		constexpr float kDamping = 0.5f;

		void setPosition(ma_sound* sound, const Math::Vec3& pos) {
			ma_sound_set_position(sound, pos.x, pos.y, pos.z);
		}
	}

	void AudioSystem::EngineDeleter::operator()(ma_engine* engine) const {
		ma_engine_uninit(engine);
		delete engine;
	}

	void AudioSystem::ResourcesDeleter::operator()(ma_resource_manager* resources) const {
		ma_resource_manager_uninit(resources);
		delete resources;
	}

	void AudioSystem::SoundDeleter::operator()(ma_sound* sound) const {
		ma_sound_uninit(sound);
		delete sound;
	}

	// One sound heard through Steam Audio: a node between the sound and the device. It takes the sound to one channel,
	// dulls it by the air it crossed (the direct effect, for its air absorption alone: miniaudio already makes it quieter
	// with distance, as it does every sound) and turns it into what each ear hears from where it is (the binaural effect,
	// the HRTF). Its second output is the dulled sound before the HRTF, for the reverb.
	//
	// The main thread aims it and the audio thread processes it: everything the audio thread reads of it is an atomic of
	// its own, and only the audio thread touches its effects once they are made.
	struct AudioSystem::Ear {
		// First: miniaudio takes the ear's address for its node.
		ma_node_base base{};
		IPLHRTF hrtf = nullptr;
		IPLBinauralEffect binaural = nullptr;
		IPLDirectEffect direct = nullptr;
		bool ready = false;
		// Whether a sound has it, the main thread's alone.
		bool taken = false;
		// A block each: the sound in one channel, and what the left and the right ear hear of it.
		std::vector<float> mono;
		std::vector<float> left;
		std::vector<float> right;
		// From the listener towards the sound, in the listener's own axes: x right, y up, -z ahead.
		std::atomic<float> x{ 0.f };
		std::atomic<float> y{ 0.f };
		std::atomic<float> z{ -1.f };
		// The share of each band, low to high, that the air lets through.
		std::atomic<float> air[IPL_NUM_BANDS];
		// 1 into the reverb, 0 kept dry.
		std::atomic<float> send{ 0.f };
		// Set as it is taken: the last sound's tail is cleared out of the effects before the new sound is heard.
		std::atomic<bool> fresh{ false };

		~Ear();
		void release();
		static void process(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut);
	};

	AudioSystem::Ear::~Ear() {
		// Off the graph first: the audio thread is done with it once it is.
		if (ready) {
			ma_node_uninit(&base, nullptr);
		}
		if (binaural) {
			iplBinauralEffectRelease(&binaural);
		}
		if (direct) {
			iplDirectEffectRelease(&direct);
		}
	}

	void AudioSystem::Ear::release() {
		taken = false;
		ma_node_set_state(&base, ma_node_state_stopped);
	}

	void AudioSystem::Ear::process(ma_node* node, const float** in, ma_uint32*, float** out, ma_uint32* frameCountOut) {
		auto* ear = static_cast<Ear*>(node);
		if (ear->fresh.exchange(false)) {
			iplBinauralEffectReset(ear->binaural);
			iplDirectEffectReset(ear->direct);
		}
		IPLDirectEffectParams dulling{};
		dulling.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
		for (int band = 0; band < IPL_NUM_BANDS; ++band) {
			dulling.airAbsorption[band] = ear->air[band].load();
		}
		IPLBinauralEffectParams hearing{};
		hearing.direction = IPLVector3{ ear->x.load(), ear->y.load(), ear->z.load() };
		hearing.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
		hearing.spatialBlend = 1.f;
		hearing.hrtf = ear->hrtf;
		const float toReverb = ear->send.load();

		const auto block = static_cast<ma_uint32>(ear->mono.size());
		float* mono = ear->mono.data();
		float* sides[2] = { ear->left.data(), ear->right.data() };
		IPLAudioBuffer monoBuffer{ 1, static_cast<IPLint32>(block), &mono };
		IPLAudioBuffer stereoBuffer{ 2, static_cast<IPLint32>(block), sides };
		for (ma_uint32 done = 0; done < *frameCountOut; done += block) {
			const ma_uint32 count = (std::min)(block, *frameCountOut - done);
			const float* from = in[0] + done * 2;
			for (ma_uint32 i = 0; i < count; ++i) {
				mono[i] = 0.5f * (from[2 * i] + from[2 * i + 1]);
			}
			// Steam Audio takes whole blocks. The graph runs in blocks of this size, so a short one never comes.
			std::fill(mono + count, mono + block, 0.f);
			iplDirectEffectApply(ear->direct, &dulling, &monoBuffer, &monoBuffer);
			iplBinauralEffectApply(ear->binaural, &hearing, &monoBuffer, &stereoBuffer);
			float* dry = out[0] + done * 2;
			float* wet = out[1] + done * 2;
			for (ma_uint32 i = 0; i < count; ++i) {
				dry[2 * i] = sides[0][i];
				dry[2 * i + 1] = sides[1][i];
				wet[2 * i] = mono[i] * toReverb;
				wet[2 * i + 1] = mono[i] * toReverb;
			}
		}
	}

	// Steam Audio's part of the mix: its context and the HRTF, the ears, made up front so that a sound that starts makes
	// nothing, and the wood's reverb they all send to. Only with a device to play to.
	struct AudioSystem::Spatial {
		IPLContext context = nullptr;
		IPLHRTF hrtf = nullptr;
		IPLAirAbsorptionModel airModel{};
		std::unique_ptr<ma_reverb_node> reverb;
		bool reverbReady = false;
		std::vector<std::unique_ptr<Ear>> ears;

		static std::unique_ptr<Spatial, SpatialDeleter> start(ma_engine* engine);
		~Spatial();
		Ear* take();
	};

	void AudioSystem::SpatialDeleter::operator()(Spatial* spatial) const {
		delete spatial;
	}

	std::unique_ptr<AudioSystem::Spatial, AudioSystem::SpatialDeleter> AudioSystem::Spatial::start(ma_engine* engine) {
		auto* graph = ma_engine_get_node_graph(engine);
		auto* endpoint = ma_engine_get_endpoint(engine);
		const ma_uint32 channels = ma_engine_get_channels(engine);
		const ma_uint32 sampleRate = ma_engine_get_sample_rate(engine);
		const ma_uint32 block = ma_node_graph_get_processing_size_in_frames(graph);
		// The HRTF is for two ears: a device of more channels keeps miniaudio's panning across its speakers.
		if (channels != 2 || block == 0) {
			LogsModule::Logger::LOG_WARNING("AudioSystem: %u channels in blocks of %u frames, so no HRTF: sounds are panned", channels, block);
			return {};
		}
		std::unique_ptr<Spatial, SpatialDeleter> spatial(new Spatial);
		IPLContextSettings contextSettings{};
		contextSettings.version = STEAMAUDIO_VERSION;
		// No further than AVX2: AVX-512 can slow the whole processor down.
		contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2;
		IPLAudioSettings audio{ static_cast<IPLint32>(sampleRate), static_cast<IPLint32>(block) };
		IPLHRTFSettings hrtfSettings{};
		hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
		hrtfSettings.volume = 1.f;
		hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
		if (iplContextCreate(&contextSettings, &spatial->context) != IPL_STATUS_SUCCESS
			|| iplHRTFCreate(spatial->context, &audio, &hrtfSettings, &spatial->hrtf) != IPL_STATUS_SUCCESS) {
			LogsModule::Logger::LOG_WARNING("AudioSystem: Steam Audio did not start, sounds are panned");
			return {};
		}
		spatial->airModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

		// Wet alone, and silent until the settings are first read: its output volume is the wet level, an atomic the
		// main thread may set whenever.
		spatial->reverb = std::make_unique<ma_reverb_node>();
		const ma_reverb_node_config reverbConfig = ma_reverb_node_config_init(2, sampleRate);
		if (ma_reverb_node_init(graph, &reverbConfig, nullptr, spatial->reverb.get()) == MA_SUCCESS) {
			// Before it is on the graph: verblib's parameters are not for changing under the audio thread.
			verblib_set_room_size(&spatial->reverb->reverb, kRoomSize);
			verblib_set_damping(&spatial->reverb->reverb, kDamping);
			ma_node_set_output_bus_volume(spatial->reverb.get(), 0, 0.f);
			ma_node_attach_output_bus(spatial->reverb.get(), 0, endpoint, 0);
			spatial->reverbReady = true;
		}
		else {
			LogsModule::Logger::LOG_WARNING("AudioSystem: no reverb at %u Hz", sampleRate);
		}

		// The sound's two channels in; out, what the two ears hear, and the dry sound for the reverb.
		static const ma_node_vtable vtable = { &Ear::process, nullptr, 1, 2, 0 };
		const ma_uint32 inputChannels[] = { 2 };
		const ma_uint32 outputChannels[] = { 2, 2 };
		ma_node_config nodeConfig = ma_node_config_init();
		nodeConfig.vtable = &vtable;
		nodeConfig.initialState = ma_node_state_stopped;
		nodeConfig.pInputChannels = inputChannels;
		nodeConfig.pOutputChannels = outputChannels;
		IPLBinauralEffectSettings binauralSettings{ spatial->hrtf };
		IPLDirectEffectSettings directSettings{ 1 };
		for (size_t i = 0; i < kEars; ++i) {
			auto ear = std::make_unique<Ear>();
			ear->hrtf = spatial->hrtf;
			ear->mono.assign(block, 0.f);
			ear->left.assign(block, 0.f);
			ear->right.assign(block, 0.f);
			if (iplBinauralEffectCreate(spatial->context, &audio, &binauralSettings, &ear->binaural) != IPL_STATUS_SUCCESS
				|| iplDirectEffectCreate(spatial->context, &audio, &directSettings, &ear->direct) != IPL_STATUS_SUCCESS
				|| ma_node_init(graph, &nodeConfig, nullptr, &ear->base) != MA_SUCCESS) {
				break;
			}
			ear->ready = true;
			ma_node_attach_output_bus(&ear->base, 0, endpoint, 0);
			if (spatial->reverbReady) {
				ma_node_attach_output_bus(&ear->base, 1, spatial->reverb.get(), 0);
			}
			spatial->ears.push_back(std::move(ear));
		}
		if (spatial->ears.empty()) {
			LogsModule::Logger::LOG_WARNING("AudioSystem: no Steam Audio effects, sounds are panned");
			return {};
		}
		return spatial;
	}

	AudioSystem::Spatial::~Spatial() {
		// The ears off the graph before the reverb they feed, and both before the HRTF and the context they were made with.
		ears.clear();
		if (reverbReady) {
			ma_reverb_node_uninit(reverb.get(), nullptr);
		}
		if (hrtf) {
			iplHRTFRelease(&hrtf);
		}
		if (context) {
			iplContextRelease(&context);
		}
	}

	AudioSystem::Ear* AudioSystem::Spatial::take() {
		for (auto& ear : ears) {
			if (!ear->taken) {
				ear->taken = true;
				return ear.get();
			}
		}
		return nullptr;
	}

	AudioSystem::AudioSystem() : mEngine(new ma_engine) {
		// Every sound decoded whole is kept (a bird's song, a recording cut into steps): 16-bit at the file's own rate
		// and channels, half the memory or less of the engine's own float at the device's rate, and turned into that
		// as it plays.
		auto resources = ma_resource_manager_config_init();
		resources.decodedFormat = ma_format_s16;
		resources.decodedChannels = 0;
		resources.decodedSampleRate = 0;
		auto* manager = new ma_resource_manager;
		if (ma_resource_manager_init(&resources, manager) == MA_SUCCESS) {
			mResources.reset(manager);
		}
		else {
			delete manager;
		}
		ma_engine_config config = ma_engine_config_init();
		config.pResourceManager = mResources.get();
		config.periodSizeInFrames = kBlock;
		const bool silent = Render::activeGraphicsBackend() == Render::GraphicsBackend::None;
		if (!silent && ma_engine_init(&config, mEngine.get()) == MA_SUCCESS) {
			mSpatial = Spatial::start(mEngine.get());
			return;
		}
		if (!silent) {
			LogsModule::Logger::LOG_WARNING("AudioSystem: no audio device, sounds are silent");
		}
		// No device: the engine still takes sounds, it is just never read.
		config.noDevice = MA_TRUE;
		config.channels = 2;
		config.sampleRate = 48000;
		if (ma_engine_init(&config, mEngine.get()) != MA_SUCCESS) {
			LogsModule::Logger::LOG_ERROR("AudioSystem: miniaudio engine failed to start");
			delete mEngine.release();
		}
	}

	struct AudioSystem::Voice {
		Sound sound;
		ma_lpf_node filter{};
		bool filterReady = false;
		float cutoff = 0.f;
		ma_uint32 channels = 2;
		ma_uint32 sampleRate = 48000;
		// What it is heard through, if anything, and how setVoice last asked for it to be heard.
		Ear* ear = nullptr;
		bool spatial = true;
		bool reverb = true;

		// The last node of its own chain: what goes on to the device or to an ear.
		ma_node* out() {
			return filterReady ? static_cast<ma_node*>(&filter) : static_cast<ma_node*>(sound.get());
		}
	};

	void AudioSystem::VoiceDeleter::operator()(Voice* voice) const {
		// The sound first: it feeds the filter, and the filter the ear.
		voice->sound.reset();
		if (voice->filterReady) {
			ma_lpf_node_uninit(&voice->filter, nullptr);
		}
		if (voice->ear) {
			voice->ear->release();
		}
		delete voice;
	}

	AudioSystem::~AudioSystem() {
		// Before the engine they are attached to.
		mVoices.clear();
	}

	uint32_t AudioSystem::startVoice(std::string_view path, bool loop, bool stream) {
		auto sound = loadSound(path, stream);
		return sound ? addVoice(std::move(sound), loop) : 0;
	}

	uint32_t AudioSystem::startSlice(std::string_view path, const float start, const float length, const float pitch) {
		auto sound = loadSound(path, false);
		if (!sound) {
			return 0;
		}
		cut(sound.get(), start, length, pitch);
		return addVoice(std::move(sound), false);
	}

	uint32_t AudioSystem::addVoice(Sound sound, const bool loop) {
		std::unique_ptr<Voice, VoiceDeleter> voice(new Voice);
		voice->channels = ma_engine_get_channels(mEngine.get());
		voice->sampleRate = ma_engine_get_sample_rate(mEngine.get());
		voice->cutoff = 20000.f;
		const auto config = ma_lpf_node_config_init(voice->channels, voice->sampleRate, voice->cutoff, 2);
		if (ma_lpf_node_init(ma_engine_get_node_graph(mEngine.get()), &config, nullptr, &voice->filter) == MA_SUCCESS) {
			voice->filterReady = true;
			ma_node_attach_output_bus(&voice->filter, 0, ma_engine_get_endpoint(mEngine.get()), 0);
			ma_node_attach_output_bus(sound.get(), 0, &voice->filter, 0);
		}
		ma_sound_set_looping(sound.get(), loop);
		ma_sound_set_volume(sound.get(), 0.f);
		ma_sound_start(sound.get());
		voice->sound = std::move(sound);
		const uint32_t id = mNextVoice++;
		mVoices.emplace(id, std::move(voice));
		return id;
	}

	void AudioSystem::setVoice(const uint32_t id, const VoiceParams& params) {
		const auto it = mVoices.find(id);
		if (it == mVoices.end()) {
			return;
		}
		auto& voice = *it->second;
		auto* sound = voice.sound.get();
		ma_sound_set_spatialization_enabled(sound, params.spatial);
		setPosition(sound, params.pos);
		ma_sound_set_volume(sound, params.volume);
		ma_sound_set_pitch(sound, params.pitch);
		ma_sound_set_min_distance(sound, params.minDistance);
		ma_sound_set_max_distance(sound, params.maxDistance);
		// Refiltered only for a change that can be heard.
		if (voice.filterReady && std::abs(params.cutoff - voice.cutoff) > voice.cutoff * 0.05f) {
			voice.cutoff = params.cutoff;
			const auto config = ma_lpf_config_init(ma_format_f32, voice.channels, voice.sampleRate, params.cutoff, 2);
			ma_lpf_node_reinit(&config, &voice.filter);
		}
		voice.spatial = params.spatial;
		voice.reverb = params.reverb;
		route(sound, voice.out(), voice.ear, voice.spatial, voice.reverb);
	}

	bool AudioSystem::voiceEnded(const uint32_t id) const {
		const auto it = mVoices.find(id);
		// A slice is stopped at its end, never reaching the recording's.
		return it == mVoices.end() || ma_sound_at_end(it->second->sound.get()) || !ma_sound_is_playing(it->second->sound.get());
	}

	void AudioSystem::stopVoice(const uint32_t id) {
		mVoices.erase(id);
	}

	AudioSystem::Sound AudioSystem::loadSound(std::string_view path, bool stream, bool residentOnly) {
		if (!mEngine) {
			return {};
		}
		// Synchronous, so a missing file is known here. A short effect decodes in a few milliseconds and a stream
		// only opens its file; miniaudio shares decoded data between sounds of the same file.
		const ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		const auto file = FileSystem::resolve(path);
		const auto open = [&](const ma_uint32 how) -> Sound {
			Sound sound(new ma_sound);
#ifdef _WIN32
			const auto result = ma_sound_init_from_file_w(mEngine.get(), file.c_str(), how, nullptr, nullptr, sound.get());
#else
			const auto result = ma_sound_init_from_file(mEngine.get(), file.c_str(), how, nullptr, nullptr, sound.get());
#endif
			if (result != MA_SUCCESS) {
				// Never initialised, so the deleter must not uninit it.
				delete sound.release();
				LogsModule::Logger::LOG_ERROR("AudioSystem: cannot open %s (%s)", file.string().c_str(), ma_result_description(result));
			}
			return sound;
		};
		// Decoded once: the resident holds the data, every sound after it only shares it. One made ahead (preload) is
		// decoded on miniaudio's thread; a sound of it started sooner plays what is decoded so far.
		if (!stream) {
			auto& resident = mResident[file.string()];
			if (!resident) {
				resident = open(residentOnly ? flags | MA_SOUND_FLAG_ASYNC : flags);
			}
			if (!resident) {
				mResident.erase(file.string());
				return {};
			}
		}
		return residentOnly ? Sound{} : open(flags);
	}

	void AudioSystem::preload(const std::string_view path) {
		if (mEngine) {
			loadSound(path, false, true);
		}
	}

	void AudioSystem::route(ma_sound* sound, ma_node* out, Ear*& ear, const bool spatial, const bool reverb) {
		if (!mSpatial || settings.hrtf <= 0.5f || !spatial) {
			if (ear) {
				// Back to the device, panned as before.
				ma_sound_set_directional_attenuation_factor(sound, 1.f);
				ma_node_attach_output_bus(out, 0, ma_engine_get_endpoint(mEngine.get()), 0);
				ear->release();
				ear = nullptr;
			}
			return;
		}
		const bool taking = !ear;
		if (taking) {
			ear = mSpatial->take();
			// Every ear taken: panned by miniaudio until one is free, never dropped.
			if (!ear) {
				return;
			}
		}
		const ma_vec3f pos = ma_sound_get_position(sound);
		const ma_vec3f at = ma_engine_listener_get_position(mEngine.get(), 0);
		const ma_vec3f ahead = ma_engine_listener_get_direction(mEngine.get(), 0);
		const ma_vec3f up = ma_engine_listener_get_world_up(mEngine.get(), 0);
		const IPLVector3 source{ pos.x, pos.y, pos.z };
		const IPLVector3 listener{ at.x, at.y, at.z };
		// Right on the listener a sound has no direction: it is heard from ahead.
		IPLVector3 direction{ 0.f, 0.f, -1.f };
		if (Math::length(Math::Vec3(pos.x - at.x, pos.y - at.y, pos.z - at.z)) > 0.01f) {
			direction = iplCalculateRelativeDirection(mSpatial->context, source, listener, IPLVector3{ ahead.x, ahead.y, ahead.z }, IPLVector3{ up.x, up.y, up.z });
		}
		ear->x = direction.x;
		ear->y = direction.y;
		ear->z = direction.z;
		float air[IPL_NUM_BANDS];
		for (float& band : air) {
			band = 1.f;
		}
		if (settings.airAbsorption > 0.5f) {
			iplAirAbsorptionCalculate(mSpatial->context, source, listener, &mSpatial->airModel, air);
		}
		for (int band = 0; band < IPL_NUM_BANDS; ++band) {
			ear->air[band] = air[band];
		}
		ear->send = reverb ? 1.f : 0.f;
		if (taking) {
			// Aimed before it is heard. miniaudio still makes it quieter with distance, but no longer pans it.
			ear->fresh = true;
			ma_sound_set_directional_attenuation_factor(sound, 0.f);
			ma_node_attach_output_bus(out, 0, &ear->base, 0);
			ma_node_set_state(&ear->base, ma_node_state_started);
		}
	}

	void AudioSystem::stop(Sound& sound, Ear*& ear) {
		sound.reset();
		if (ear) {
			ear->release();
			ear = nullptr;
		}
	}

	void AudioSystem::update(float dt) {
		if (!mEngine) {
			return;
		}
		++mFrame;
		updateListener();
		if (mSpatial && mSpatial->reverbReady) {
			// All the reverb gives is wet: its volume is the wet level.
			ma_node_set_output_bus_volume(mSpatial->reverb.get(), 0, (std::max)(settings.reverb, 0.f));
		}

		for (auto [entity, source, transform] : ECSHandler::registry().view<AudioSourceComponent, const TransformComponent>()) {
			auto it = mSources.find(entity);
			if (it != mSources.end() && (!source->mPlaying || it->second.path != source->mPath || it->second.stream != source->mStream)) {
				stop(it->second.sound, it->second.ear);
				mSources.erase(it);
				it = mSources.end();
			}
			if (!source->mPlaying) {
				continue;
			}

			bool starting = false;
			if (it == mSources.end()) {
				auto sound = loadSound(source->mPath, source->mStream);
				if (!sound) {
					source->mPlaying = false;
					continue;
				}
				it = mSources.emplace(entity, Source{ std::move(sound), source->mPath, source->mStream }).first;
				starting = true;
			}
			else if (ma_sound_at_end(it->second.sound.get())) {
				stop(it->second.sound, it->second.ear);
				mSources.erase(it);
				source->mPlaying = false;
				continue;
			}

			auto* sound = it->second.sound.get();
			it->second.seenFrame = mFrame;
			ma_sound_set_pitch(sound, source->mPitch);
			ma_sound_set_looping(sound, source->mLoop);
			ma_sound_set_spatialization_enabled(sound, source->mSpatial);
			ma_sound_set_min_distance(sound, source->mMinDistance);
			ma_sound_set_max_distance(sound, source->mMaxDistance);
			float volume = source->mVolume;
			if (transform) {
				const Math::Vec3 pos(transform->copyTransform()[3]);
				setPosition(sound, pos);
				// miniaudio's falloff only levels out at the far distance, it never goes quiet: the last fifth of the way
				// fades it to nothing, or a fire is heard faintly across the whole map.
				if (source->mSpatial) {
					const auto listener = ma_engine_listener_get_position(mEngine.get(), 0);
					const float distance = Math::length(pos - Math::Vec3(listener.x, listener.y, listener.z));
					const float fade = (std::max)(0.2f * source->mMaxDistance, 0.01f);
					volume *= std::clamp((source->mMaxDistance - distance) / fade, 0.f, 1.f);
				}
			}
			ma_sound_set_volume(sound, volume);
			route(sound, sound, it->second.ear, source->mSpatial, true);
			// Started once it is set and sent where it is heard, not from the world's origin at full volume.
			if (starting) {
				ma_sound_start(sound);
			}
		}

		// Sources whose component or entity is gone.
		for (auto& [entity, source] : mSources) {
			if (source.seenFrame != mFrame) {
				stop(source.sound, source.ear);
			}
		}
		std::erase_if(mSources, [](const auto& source) { return !source.second.sound; });
		// One-shots that have ended; the others turned to where the listener now is.
		for (auto& shot : mOneShots) {
			if (ma_sound_at_end(shot.sound.get()) || !ma_sound_is_playing(shot.sound.get())) {
				stop(shot.sound, shot.ear);
			}
			else {
				route(shot.sound.get(), shot.sound.get(), shot.ear, true, true);
			}
		}
		std::erase_if(mOneShots, [](const OneShot& shot) { return !shot.sound; });
		for (auto& [id, voice] : mVoices) {
			route(voice->sound.get(), voice->out(), voice->ear, voice->spatial, voice->reverb);
		}
	}

	void AudioSystem::updateListener() {
		auto listener = ecss::INVALID_ID;
		for (auto [entity, tag, transform] : ECSHandler::registry().view<const AudioListenerComponent, const TransformComponent>()) {
			if (transform) {
				listener = entity;
				break;
			}
		}
		if (listener == ecss::INVALID_ID) {
			if (const auto camera = ECSHandler::systemManager().getSystem<CameraSystem>()) {
				listener = camera->getCurrentCamera();
			}
		}

		if (listener == ecss::INVALID_ID) {
			return;
		}
		const auto transform = ECSHandler::registry().pinComponent<const TransformComponent>(listener);
		if (!transform) {
			return;
		}
		const auto m = transform->copyTransform();
		const Math::Vec3 forward = Math::normalize(-Math::Vec3(m[2]));
		const Math::Vec3 up = Math::normalize(Math::Vec3(m[1]));
		ma_engine_listener_set_position(mEngine.get(), 0, m[3][0], m[3][1], m[3][2]);
		ma_engine_listener_set_direction(mEngine.get(), 0, forward.x, forward.y, forward.z);
		ma_engine_listener_set_world_up(mEngine.get(), 0, up.x, up.y, up.z);
	}

	void AudioSystem::startOneShot(Sound sound) {
		OneShot shot{ std::move(sound) };
		// Sent where it is heard before it starts.
		route(shot.sound.get(), shot.sound.get(), shot.ear, true, true);
		ma_sound_start(shot.sound.get());
		mOneShots.push_back(std::move(shot));
	}

	void AudioSystem::playOneShot(std::string_view path, const Math::Vec3& pos, float volume) {
		auto sound = loadSound(path, false);
		if (!sound) {
			return;
		}
		setPosition(sound.get(), pos);
		ma_sound_set_volume(sound.get(), volume);
		startOneShot(std::move(sound));
	}

	void AudioSystem::playSlice(std::string_view path, const float start, const float length, const Math::Vec3& pos, const float volume, const float pitch) {
		auto sound = loadSound(path, false);
		if (!sound) {
			return;
		}
		cut(sound.get(), start, length, pitch);
		setPosition(sound.get(), pos);
		ma_sound_set_volume(sound.get(), volume);
		startOneShot(std::move(sound));
	}

	void AudioSystem::cut(ma_sound* sound, const float start, const float length, const float pitch) {
		ma_uint32 sampleRate = 0;
		ma_sound_get_data_format(sound, nullptr, nullptr, &sampleRate, nullptr, 0);
		ma_sound_seek_to_pcm_frame(sound, static_cast<ma_uint64>(start * static_cast<float>(sampleRate)));
		ma_sound_set_pitch(sound, pitch);
		ma_sound_set_fade_in_milliseconds(sound, 0.f, 1.f, 4);
		const auto now = ma_engine_get_time_in_milliseconds(mEngine.get());
		ma_sound_set_stop_time_with_fade_in_milliseconds(sound, now + static_cast<ma_uint64>(length * 1000.f / (std::max)(pitch, 0.1f)), 40);
	}

	namespace {
		// A recording's loudness in windows of about 12 ms (kLoudnessWindow seconds each), and its usual level (the
		// median window) and its loud one (louder than all but 2%). Empty when it cannot be decoded.
		constexpr ma_uint32 kLoudnessRate = 22050;
		constexpr size_t kLoudnessFrames = 256;
		constexpr float kLoudnessWindow = static_cast<float>(kLoudnessFrames) / kLoudnessRate;
		std::vector<float> loudnessOf(std::string_view path, float& quiet, float& loud) {
			std::vector<float> energy;
			ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, kLoudnessRate);
			ma_uint64 frames = 0;
			void* data = nullptr;
			const auto file = FileSystem::resolve(path).string();
			if (ma_decode_file(file.c_str(), &config, &frames, &data) != MA_SUCCESS || !data) {
				LogsModule::Logger::LOG_ERROR("AudioSystem: cannot decode %s", file.c_str());
				return energy;
			}
			const auto* samples = static_cast<float*>(data);
			energy.resize(static_cast<size_t>(frames) / kLoudnessFrames);
			for (size_t w = 0; w < energy.size(); ++w) {
				float sum = 0.f;
				for (size_t i = 0; i < kLoudnessFrames; ++i) {
					const float s = samples[w * kLoudnessFrames + i];
					sum += s * s;
				}
				energy[w] = std::sqrt(sum / kLoudnessFrames);
			}
			ma_free(data, nullptr);
			if (!energy.empty()) {
				std::vector<float> sorted = energy;
				std::sort(sorted.begin(), sorted.end());
				quiet = sorted[sorted.size() / 2];
				loud = sorted[sorted.size() * 98 / 100];
			}
			return energy;
		}
	}

	std::vector<std::pair<float, float>> AudioSystem::findCalls(std::string_view path, const float gap, const float maxLength) {
		std::vector<std::pair<float, float>> calls;
		float quiet = 0.f;
		float loud = 0.f;
		const auto energy = loudnessOf(path, quiet, loud);
		// Sound is anything a little over the usual level; a call is sound broken by no silence of `gap` or longer, with
		// something in it well over that level, as a hit has (findHits).
		const float sound = quiet + (loud - quiet) * 0.12f;
		const float strong = quiet + (loud - quiet) * 0.35f;
		const int gapWindows = (std::max)(1, static_cast<int>(gap / kLoudnessWindow));
		const int n = static_cast<int>(energy.size());
		for (int w = 0; w < n;) {
			if (energy[w] < sound) {
				++w;
				continue;
			}
			const int begin = w;
			int last = w;
			float peak = energy[w];
			for (int silent = 0; ++w < n;) {
				if (energy[w] >= sound) {
					last = w;
					silent = 0;
					peak = (std::max)(peak, energy[w]);
				}
				else if (++silent >= gapWindows) {
					break;
				}
			}
			// From just before it rose to a short tail after it last sounded.
			const float start = (std::max)(0.f, static_cast<float>(begin - 2) * kLoudnessWindow);
			const float end = static_cast<float>(last + 1) * kLoudnessWindow + (std::min)(gap * 0.5f, 0.15f);
			if (peak >= strong && static_cast<float>(last - begin + 1) * kLoudnessWindow >= 0.06f) {
				calls.emplace_back(start, (std::min)(end - start, maxLength));
			}
		}
		return calls;
	}

	std::vector<std::pair<float, float>> AudioSystem::findHits(std::string_view path, const float maxLength) {
		std::vector<std::pair<float, float>> hits;
		float quiet = 0.f;
		float loud = 0.f;
		const auto energy = loudnessOf(path, quiet, loud);
		if (energy.empty()) {
			return hits;
		}
		const float threshold = quiet + (loud - quiet) * 0.35f;
		const float windowSeconds = kLoudnessWindow;
		// A hit is the loudest window for a fifth of a second either way, well over the recording's usual level; it
		// starts where the loudness first rose towards it.
		constexpr int kReach = 18;
		std::vector<float> starts;
		for (int w = 0; w < static_cast<int>(energy.size()); ++w) {
			if (energy[w] < threshold) {
				continue;
			}
			bool peak = true;
			for (int k = (std::max)(0, w - kReach); k < (std::min)(static_cast<int>(energy.size()), w + kReach + 1) && peak; ++k) {
				peak = energy[k] < energy[w] || (energy[k] == energy[w] && k >= w);
			}
			if (!peak) {
				continue;
			}
			int begin = w;
			while (begin > 0 && w - begin < 10 && energy[begin - 1] > energy[w] * 0.25f) {
				--begin;
			}
			starts.push_back((std::max)(0.f, (static_cast<float>(begin) - 1.f) * windowSeconds));
		}
		for (size_t i = 0; i < starts.size(); ++i) {
			const float next = i + 1 < starts.size() ? starts[i + 1] : starts[i] + maxLength;
			const float length = (std::min)(next - starts[i] - 0.02f, maxLength);
			if (length > 0.1f) {
				hits.emplace_back(starts[i], length);
			}
		}
		return hits;
	}

	bool AudioSystem::isPlaying(ecss::EntityId entity) const {
		const auto it = mSources.find(entity);
		return it != mSources.end() && ma_sound_is_playing(it->second.sound.get());
	}

	void AudioSystem::setMasterVolume(float volume) {
		mMasterVolume = volume;
		if (mEngine) {
			ma_engine_set_volume(mEngine.get(), volume);
		}
	}
}

#include "AudioSystem.h"

#include <algorithm>
#include <cmath>

#include <miniaudio.h>

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
		void setPosition(ma_sound* sound, const Math::Vec3& pos) {
			ma_sound_set_position(sound, pos.x, pos.y, pos.z);
		}
	}

	void AudioSystem::EngineDeleter::operator()(ma_engine* engine) const {
		ma_engine_uninit(engine);
		delete engine;
	}

	void AudioSystem::SoundDeleter::operator()(ma_sound* sound) const {
		ma_sound_uninit(sound);
		delete sound;
	}

	AudioSystem::AudioSystem() : mEngine(new ma_engine) {
		ma_engine_config config = ma_engine_config_init();
		const bool silent = Render::activeGraphicsBackend() == Render::GraphicsBackend::None;
		if (!silent && ma_engine_init(&config, mEngine.get()) == MA_SUCCESS) {
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
	};

	void AudioSystem::VoiceDeleter::operator()(Voice* voice) const {
		// The sound first: it feeds the filter.
		voice->sound.reset();
		if (voice->filterReady) {
			ma_lpf_node_uninit(&voice->filter, nullptr);
		}
		delete voice;
	}

	AudioSystem::~AudioSystem() {
		// Before the engine they are attached to.
		mVoices.clear();
	}

	uint32_t AudioSystem::startVoice(std::string_view path, bool loop, bool stream) {
		auto sound = loadSound(path, stream);
		if (!sound) {
			return 0;
		}
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
		ma_sound_set_min_distance(sound, params.minDistance);
		ma_sound_set_max_distance(sound, params.maxDistance);
		// Refiltered only for a change that can be heard.
		if (voice.filterReady && std::abs(params.cutoff - voice.cutoff) > voice.cutoff * 0.05f) {
			voice.cutoff = params.cutoff;
			const auto config = ma_lpf_config_init(ma_format_f32, voice.channels, voice.sampleRate, params.cutoff, 2);
			ma_lpf_node_reinit(&config, &voice.filter);
		}
	}

	bool AudioSystem::voiceEnded(const uint32_t id) const {
		const auto it = mVoices.find(id);
		return it == mVoices.end() || ma_sound_at_end(it->second->sound.get());
	}

	void AudioSystem::stopVoice(const uint32_t id) {
		mVoices.erase(id);
	}

	AudioSystem::Sound AudioSystem::loadSound(std::string_view path, bool stream) {
		if (!mEngine) {
			return {};
		}
		// Synchronous, so a missing file is known here. A short effect decodes in a few milliseconds and a stream
		// only opens its file; miniaudio shares decoded data between sounds of the same file.
		const ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		const auto file = FileSystem::resolve(path);
		const auto open = [&]() -> Sound {
			Sound sound(new ma_sound);
#ifdef _WIN32
			const auto result = ma_sound_init_from_file_w(mEngine.get(), file.c_str(), flags, nullptr, nullptr, sound.get());
#else
			const auto result = ma_sound_init_from_file(mEngine.get(), file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
			if (result != MA_SUCCESS) {
				// Never initialised, so the deleter must not uninit it.
				delete sound.release();
				LogsModule::Logger::LOG_ERROR("AudioSystem: cannot open %s (%s)", file.string().c_str(), ma_result_description(result));
			}
			return sound;
		};
		// Decoded once: the resident holds the data, every sound after it only shares it.
		if (!stream) {
			auto& resident = mResident[file.string()];
			if (!resident) {
				resident = open();
			}
			if (!resident) {
				mResident.erase(file.string());
				return {};
			}
		}
		return open();
	}

	void AudioSystem::preload(const std::string_view path) {
		if (mEngine) {
			loadSound(path, false);
		}
	}

	void AudioSystem::update(float dt) {
		if (!mEngine) {
			return;
		}
		++mFrame;
		updateListener();

		for (auto [entity, source, transform] : ECSHandler::registry().view<AudioSourceComponent, const TransformComponent>()) {
			auto it = mSources.find(entity);
			if (it != mSources.end() && (!source->mPlaying || it->second.path != source->mPath || it->second.stream != source->mStream)) {
				mSources.erase(it);
				it = mSources.end();
			}
			if (!source->mPlaying) {
				continue;
			}

			if (it == mSources.end()) {
				auto sound = loadSound(source->mPath, source->mStream);
				if (!sound) {
					source->mPlaying = false;
					continue;
				}
				ma_sound_start(sound.get());
				it = mSources.emplace(entity, Source{ std::move(sound), source->mPath, source->mStream }).first;
			}
			else if (ma_sound_at_end(it->second.sound.get())) {
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
		}

		// Sources whose component or entity is gone.
		std::erase_if(mSources, [this](const auto& source) { return source.second.seenFrame != mFrame; });
		std::erase_if(mOneShots, [](const Sound& sound) { return ma_sound_at_end(sound.get()) || !ma_sound_is_playing(sound.get()); });
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

	void AudioSystem::playOneShot(std::string_view path, const Math::Vec3& pos, float volume) {
		auto sound = loadSound(path, false);
		if (!sound) {
			return;
		}
		setPosition(sound.get(), pos);
		ma_sound_set_volume(sound.get(), volume);
		ma_sound_start(sound.get());
		mOneShots.push_back(std::move(sound));
	}

	void AudioSystem::playSlice(std::string_view path, const float start, const float length, const Math::Vec3& pos, const float volume, const float pitch) {
		auto sound = loadSound(path, false);
		if (!sound) {
			return;
		}
		ma_uint32 sampleRate = 0;
		ma_sound_get_data_format(sound.get(), nullptr, nullptr, &sampleRate, nullptr, 0);
		ma_sound_seek_to_pcm_frame(sound.get(), static_cast<ma_uint64>(start * static_cast<float>(sampleRate)));
		setPosition(sound.get(), pos);
		ma_sound_set_volume(sound.get(), volume);
		ma_sound_set_pitch(sound.get(), pitch);
		ma_sound_set_fade_in_milliseconds(sound.get(), 0.f, 1.f, 4);
		const auto now = ma_engine_get_time_in_milliseconds(mEngine.get());
		ma_sound_set_stop_time_with_fade_in_milliseconds(sound.get(), now + static_cast<ma_uint64>(length * 1000.f / (std::max)(pitch, 0.1f)), 40);
		ma_sound_start(sound.get());
		mOneShots.push_back(std::move(sound));
	}

	std::vector<std::pair<float, float>> AudioSystem::findHits(std::string_view path, const float maxLength) {
		std::vector<std::pair<float, float>> hits;
		constexpr ma_uint32 kRate = 22050;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, kRate);
		ma_uint64 frames = 0;
		void* data = nullptr;
		const auto file = FileSystem::resolve(path).string();
		if (ma_decode_file(file.c_str(), &config, &frames, &data) != MA_SUCCESS || !data) {
			LogsModule::Logger::LOG_ERROR("AudioSystem: cannot decode %s", file.c_str());
			return hits;
		}
		// Loudness in windows of about 12 ms.
		constexpr size_t kWindow = 256;
		const auto* samples = static_cast<float*>(data);
		std::vector<float> energy(static_cast<size_t>(frames) / kWindow);
		for (size_t w = 0; w < energy.size(); ++w) {
			float sum = 0.f;
			for (size_t i = 0; i < kWindow; ++i) {
				const float s = samples[w * kWindow + i];
				sum += s * s;
			}
			energy[w] = std::sqrt(sum / kWindow);
		}
		ma_free(data, nullptr);
		if (energy.empty()) {
			return hits;
		}
		std::vector<float> sorted = energy;
		std::sort(sorted.begin(), sorted.end());
		const float quiet = sorted[sorted.size() / 2];
		const float loud = sorted[sorted.size() * 98 / 100];
		const float threshold = quiet + (loud - quiet) * 0.35f;
		const float windowSeconds = static_cast<float>(kWindow) / kRate;
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

#include "rhi/RenderHardwareInterface.h"

#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace SFE::Render {
	ShaderModuleHandle RenderHardwareInterface::loadShader(const ShaderStage stage, const std::string_view path) {
		ShaderModuleDesc desc;
		desc.stage = stage;
		desc.name = std::string(path);
		if (m_host) {
			desc.source = m_host->loadShaderSource(path);
		}
		return createShaderModule(desc);
	}

	void RenderHardwareInterface::cmdGpuMark(const CommandListHandle cmd, const char* label, const bool endsPass) {
		// The pipeline drawn with since the last mark has its span closed first: a pass's own mark then ends its rest.
		if (endsPass && mMarkedShader) {
			writeGpuMark(cmd, mMarkedShader, false);
		}
		mMarkedShader = nullptr;
		mMarkedList = cmd;
		writeGpuMark(cmd, label, endsPass);
	}

	void RenderHardwareInterface::pipelineBound(const CommandListHandle cmd, const char* label) {
		if (!gpuShaderMarks.load(std::memory_order_relaxed) || cmd.id != mMarkedList.id || label == mMarkedShader) {
			return;
		}
		writeGpuMark(cmd, mMarkedShader, false);
		mMarkedShader = label;
	}

	const char* RenderHardwareInterface::pipelineLabel(const std::string_view first, const std::string_view second) {
		const auto bare = [](const std::string_view name) {
			const auto slash = name.find_last_of("/\\");
			return slash == std::string_view::npos ? name : name.substr(slash + 1);
		};
		std::string label(bare(first));
		if (!second.empty()) {
			label += " | ";
			label += bare(second);
		}
		static std::mutex mutex;
		static auto* kept = new std::unordered_set<std::string>; // a node each: the text stays where it is
		std::lock_guard lock(mutex);
		return kept->insert(std::move(label)).first->c_str();
	}

	void RenderHardwareInterface::publishGpuSpans(const GpuMarkLabel* labels, const double* atMs, const uint32_t count) {
		mGpuSpans.clear();
		for (uint32_t i = 1; i < count; ++i) {
			mGpuSpans.push_back({ labels[i].label, labels[i].endsPass, atMs[i] - atMs[i - 1] });
		}
	}

	void RenderHardwareInterface::updateCubeMip(const TextureHandle tex, const uint32_t mip, const void* packed6Faces, const size_t faceBytes) {
		if (!packed6Faces || faceBytes == 0) {
			return;
		}
		const auto* bytes = static_cast<const std::byte*>(packed6Faces);
		for (uint32_t face = 0; face < 6; ++face) {
			updateCubeFace(tex, face, mip, bytes + face * faceBytes);
		}
	}
}

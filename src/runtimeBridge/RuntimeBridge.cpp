#include "RuntimeBridge.h"

#include "core/Engine.h"
#include "core/InputHandler.h"
#include "editorProtocol/TcpSocket.h"
#include "logsModule/logger.h"
#include "propertiesModule/PropertiesSystem.h"
#include "rhi/GraphicsBackend.h"
#include "rhi/RenderHardwareInterface.h"
#include "windowsModule/Window.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include <json/reader.h>
#include <json/writer.h>

#include <sstream>

#include <algorithm>

#include <chrono>
#include <cstring>

namespace SFE::RuntimeBridge {

	namespace {
		bool isFramePacket(const std::vector<uint8_t>& packet)
		{
			if (packet.size() < sizeof(EditorProtocol::PacketHeader)) {
				return false;
			}
			EditorProtocol::PacketHeader header{};
			std::memcpy(&header, packet.data(), sizeof(header));
			return header.magic == EditorProtocol::kMagic
				&& static_cast<EditorProtocol::MessageType>(header.type) == EditorProtocol::MessageType::Frame;
		}

		bool isPacketType(const std::vector<uint8_t>& packet, EditorProtocol::MessageType type)
		{
			if (packet.size() < sizeof(EditorProtocol::PacketHeader)) {
				return false;
			}
			EditorProtocol::PacketHeader header{};
			std::memcpy(&header, packet.data(), sizeof(header));
			return header.magic == EditorProtocol::kMagic
				&& static_cast<EditorProtocol::MessageType>(header.type) == type;
		}
	}

	void RuntimeBridge::start(uint16_t port)
	{
		if (mRunning.load()) {
			return;
		}
		mPort = port;
		mRunning = true;
		mSendRunning = true;
		mNetworkThread = std::thread(&RuntimeBridge::networkThreadMain, this);
		mSendThread = std::thread(&RuntimeBridge::sendThreadMain, this);
	}

	void RuntimeBridge::stop()
	{
		mRunning = false;
		mSendRunning = false;
		mOutgoingCv.notify_all();
		mListenSocket.close();
		mClientSocket.close();
		if (mNetworkThread.joinable()) {
			mNetworkThread.join();
		}
		if (mSendThread.joinable()) {
			mSendThread.join();
		}
		{
			std::lock_guard lock(mOutgoingMutex);
			mOutgoing.clear();
		}
		mHasClient = false;
		mSubscribeFrames = false;
		mEmbeddedHostMode = false;
		mEmbedConfigDirty = true;
		mAppliedRemoteDisplay = false;
	}

	void RuntimeBridge::sendThreadMain()
	{
		while (mSendRunning.load()) {
			std::vector<uint8_t> packet;
			{
				std::unique_lock lock(mOutgoingMutex);
				mOutgoingCv.wait(lock, [this] {
					return !mOutgoing.empty() || !mSendRunning.load();
				});
				if (!mSendRunning.load() && mOutgoing.empty()) {
					break;
				}
				if (mOutgoing.empty()) {
					continue;
				}
				packet = std::move(mOutgoing.front());
				mOutgoing.pop_front();
			}
			if (mHasClient.load()) {
				mClientSocket.sendAll(packet.data(), packet.size());
			}
		}
	}

	void RuntimeBridge::queueFramePacket(std::vector<uint8_t>&& packet)
	{
		std::lock_guard lock(mOutgoingMutex);
		for (auto it = mOutgoing.begin(); it != mOutgoing.end();) {
			if (isFramePacket(*it)) {
				it = mOutgoing.erase(it);
			}
			else {
				++it;
			}
		}
		mOutgoing.push_front(std::move(packet));
		mOutgoingCv.notify_one();
	}

	bool RuntimeBridge::enqueuePacket(std::vector<uint8_t>&& packet, bool /*dropIfBusy*/)
	{
		std::lock_guard lock(mOutgoingMutex);
		while (mOutgoing.size() >= 32) {
			mOutgoing.pop_front();
		}
		mOutgoing.push_back(std::move(packet));
		mOutgoingCv.notify_one();
		return true;
	}

	bool RuntimeBridge::sendPacket(EditorProtocol::MessageType type, const std::string& json, const void* binary, uint32_t binarySize)
	{
		if (!mHasClient.load()) {
			return false;
		}
		std::vector<uint8_t> packet;
		EditorProtocol::writePacket(packet, type, json, binary, binarySize);
		return enqueuePacket(std::move(packet), false);
	}

	void RuntimeBridge::networkThreadMain()
	{
		EditorProtocol::TcpSocket::initNetwork();
		if (!mListenSocket.listen(mPort)) {
			mRunning = false;
			return;
		}

		while (mRunning.load()) {
			if (!mHasClient.load()) {
				if (!mListenSocket.acceptClient(mClientSocket)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(16));
					continue;
				}
				mHasClient = true;
				mRecvBuffer.clear();
				mSubscribeFrames = false;
				mEmbeddedHostMode = false;
			}

			size_t received = 0;
			std::vector<uint8_t> chunk;
			if (!mClientSocket.recvSome(chunk, 65536, received)) {
				if (!mClientSocket.isConnected()) {
					mClientSocket.close();
					mHasClient = false;
					mSubscribeFrames = false;
					mEmbeddedHostMode = false;
					mEmbedConfigDirty = true;
					mAppliedOverlayX = -1;
					mAppliedOverlayY = -1;
					mAppliedOverlayW = -1;
					mAppliedOverlayH = -1;
					mRecvBuffer.clear();
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (received == 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}

			mRecvBuffer.insert(mRecvBuffer.end(), chunk.begin(), chunk.end());

			while (mRecvBuffer.size() >= sizeof(EditorProtocol::PacketHeader)) {
				EditorProtocol::PacketHeader header{};
				std::memcpy(&header, mRecvBuffer.data(), sizeof(header));
				if (header.magic != EditorProtocol::kMagic) {
					mRecvBuffer.clear();
					break;
				}
				const size_t packetSize = sizeof(EditorProtocol::PacketHeader) + header.jsonSize + header.binarySize;
				if (mRecvBuffer.size() < packetSize) {
					break;
				}

				IncomingMessage msg;
				std::string json;
				std::vector<uint8_t> binary;
				if (EditorProtocol::readPacket(mRecvBuffer.data(), packetSize, header, json, binary)) {
					msg.type = static_cast<EditorProtocol::MessageType>(header.type);
					msg.json = std::move(json);
					msg.binary = std::move(binary);
					{
						std::lock_guard lock(mIncomingMutex);
						mIncoming.push_back(std::move(msg));
					}
				}
				mRecvBuffer.erase(mRecvBuffer.begin(), mRecvBuffer.begin() + static_cast<std::ptrdiff_t>(packetSize));
			}
		}

		mClientSocket.close();
		EditorProtocol::TcpSocket::shutdownNetwork();
	}

	void RuntimeBridge::processInput(const std::string& json)
	{
		Json::Value root;
		Json::CharReaderBuilder builder;
		std::string errors;
		std::istringstream stream(json);
		if (!Json::parseFromStream(builder, stream, &root, &errors)) {
			return;
		}

		const std::string kind = root.get("kind", "").asString();
		if (kind == "key") {
			CoreModule::InputHandler::injectKey(root.get("key", 0).asInt(), root.get("action", 0).asInt());
		}
		else if (kind == "mouseBtn") {
			CoreModule::InputHandler::injectMouseBtn(
				root.get("x", 0.0).asDouble(),
				root.get("y", 0.0).asDouble(),
				root.get("btn", 0).asInt(),
				root.get("action", 0).asInt()
			);
		}
		else if (kind == "mouseMove") {
			CoreModule::InputHandler::injectMouseMove(
				root.get("x", 0.0).asDouble(),
				root.get("y", 0.0).asDouble(),
				root.get("dx", 0.0).asDouble(),
				root.get("dy", 0.0).asDouble()
			);
		}
		else if (kind == "scroll") {
			CoreModule::InputHandler::injectScroll(
				root.get("sx", 0.0).asDouble(),
				root.get("sy", 0.0).asDouble()
			);
		}
	}

	void RuntimeBridge::processMessage(const IncomingMessage& msg)
	{
		using EditorProtocol::MessageType;
		switch (msg.type) {
		case MessageType::Ping:
			sendPacket(MessageType::Pong, R"({"ok":true})");
			sendStatus();
			break;
		case MessageType::SubscribeFrames: {
			mSubscribeFrames = true;
			mEmbeddedHostMode = false;
			if (!msg.json.empty()) {
				Json::Value root;
				Json::CharReaderBuilder builder;
				std::string errors;
				std::istringstream stream(msg.json);
				if (Json::parseFromStream(builder, stream, &root, &errors)) {
					if (root.get("display", "").asString() == "embed") {
						mEmbeddedHostMode = true;
						mEmbedHostHwnd = root.get("hostHwnd", Json::UInt64(0)).asUInt64();
						mEmbedX = root.get("screenX", root.get("x", 0)).asInt();
						mEmbedY = root.get("screenY", root.get("y", 0)).asInt();
						mEmbedW = root.get("w", 0).asInt();
						mEmbedH = root.get("h", 0).asInt();
					}
				}
			}
			mEmbedConfigDirty = true;
			if (!isEmbeddedInEditor() && !mEmbeddedHostMode) {
				setPublishEditorDisplay(true);
			}
			Json::Value status;
			status["subscribed"] = true;
			// The None backend has no window to embed and no image to stream.
			status["display"] = Render::activeGraphicsBackend() == Render::GraphicsBackend::None
				? "none"
				: mEmbeddedHostMode ? "embed" : "stream";
			status["backend"] = Render::graphicsBackendName(Render::activeGraphicsBackend());
			Json::StreamWriterBuilder builder;
			builder["indentation"] = "";
			sendPacket(MessageType::Status, Json::writeString(builder, status));
			break;
		}
		case MessageType::UnsubscribeFrames:
			mSubscribeFrames = false;
			mEmbeddedHostMode = false;
			mEmbedConfigDirty = true;
			if (!isEmbeddedInEditor()) {
				setPublishEditorDisplay(false);
			}
			sendPacket(MessageType::Status, R"({"subscribed":false})");
			break;
		case MessageType::GetWorldSnapshot:
			sendWorldSnapshot(msg.json);
			break;
		case MessageType::Input:
			processInput(msg.json);
			break;
		case MessageType::Command: {
			Json::Value request;
			Json::CharReaderBuilder reader;
			std::string errors;
			std::istringstream stream(msg.json);
			Json::Value reply;
			if (!mClientSocket.isLoopback()) {
				reply["ok"] = false;
				reply["result"] = "commands are taken from this machine only";
			}
			else if (!Json::parseFromStream(reader, stream, &request, &errors) || !request.isObject()) {
				reply["ok"] = false;
				reply["result"] = "not a command: " + errors;
			}
			else {
				// Answered once it has run, at the start of the next frame.
				mQueued.push_back({ request.get("command", "").asString(), request.get("args", Json::Value()), true });
				break;
			}
			Json::StreamWriterBuilder writer;
			writer["indentation"] = "";
			sendPacket(MessageType::CommandResult, Json::writeString(writer, reply));
			break;
		}
		default:
			sendPacket(MessageType::Error, R"({"error":"unsupported"})");
			break;
		}
	}

	bool RuntimeBridge::addCommand(const std::string& name, std::string help, Command run)
	{
		// Two parts of the program answering to one name: only the later is reached, which neither may have meant.
		const bool replaced = mCommands.contains(name);
		if (replaced) {
			LogsModule::Logger::LOG_WARNING("bridge command %s registered again: the later one stands", name.c_str());
		}
		mCommands[name] = { std::move(help), std::move(run) };
		return replaced;
	}

	std::string RuntimeBridge::runCommand(const std::string& name, const Json::Value& args)
	{
		const auto found = mCommands.find(name);
		if (found == mCommands.end()) {
			std::string known;
			for (const auto& [each, entry] : mCommands) {
				known += (known.empty() ? "" : ", ") + each;
			}
			return "no command " + name + " (there are: " + known + ")";
		}
		mLastResult = name + ": " + found->second.run(args);
		return mLastResult;
	}

	void RuntimeBridge::queueCommand(const std::string& name, const Json::Value& args)
	{
		mQueued.push_back({ name, args, false });
	}

	void RuntimeBridge::runQueued()
	{
		auto queued = std::move(mQueued);
		mQueued.clear();
		for (const auto& command : queued) {
			const bool known = mCommands.contains(command.name);
			const auto said = runCommand(command.name, command.args);
			if (command.answer) {
				Json::Value reply;
				reply["command"] = command.name;
				reply["ok"] = known;
				reply["result"] = said;
				Json::StreamWriterBuilder writer;
				writer["indentation"] = "";
				sendPacket(EditorProtocol::MessageType::CommandResult, Json::writeString(writer, reply));
			}
		}
	}

	void RuntimeBridge::sendStatus()
	{
		Json::Value root;
		root["fps"] = Engine::instance()->getFPS();
		root["backend"] = Render::graphicsBackendName(Render::activeGraphicsBackend());
		root["port"] = mPort;
		root["hasClient"] = mHasClient.load();
		root["subscribed"] = mSubscribeFrames.load();
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";
		sendPacket(EditorProtocol::MessageType::Status, Json::writeString(builder, root));
	}

	void RuntimeBridge::sendWorldSnapshot(const std::string& requestJson)
	{
		if (!mHasClient.load()) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - mLastSnapshotRequest < std::chrono::milliseconds(250)) {
			return;
		}
		{
			std::lock_guard lock(mOutgoingMutex);
			for (const auto& pending : mOutgoing) {
				if (isPacketType(pending, EditorProtocol::MessageType::WorldSnapshot)) {
					return;
				}
			}
		}
		mLastSnapshotRequest = now;

		Json::Value request;
		if (!requestJson.empty()) {
			Json::CharReaderBuilder builder;
			std::string errors;
			std::istringstream stream(requestJson);
			Json::parseFromStream(builder, stream, &request, &errors);
		}

		const Json::Value snapshot = PropertiesModule::PropertiesSystem::serializeWorldSnapshot(request);
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";
		sendPacket(EditorProtocol::MessageType::WorldSnapshot, Json::writeString(builder, snapshot));
	}

	// The presented texture, read back and scaled to at most kStreamMaxWidth with a bilinear filter.
	// Rows go out bottom-up (texture row 0 first), the order the editor draws them in.
	bool RuntimeBridge::captureStreamFrame(Render::RenderHardwareInterface& rhi, std::vector<uint8_t>& rgba, int& width, int& height)
	{
		static constexpr int kStreamMaxWidth = 1280;
		int srcW = 0;
		int srcH = 0;
		if (!rhi.readbackTexture(rhi.presentSourceTexture(), mFrameReadback, srcW, srcH) || srcW <= 0 || srcH <= 0) {
			return false;
		}
		width = (std::min)(srcW, kStreamMaxWidth);
		height = (std::max)(1, srcH * width / srcW);
		rgba.resize(static_cast<size_t>(width) * height * 4u);

		const float sx = static_cast<float>(srcW) / static_cast<float>(width);
		const float sy = static_cast<float>(srcH) / static_cast<float>(height);
		for (int y = 0; y < height; ++y) {
			const float fy = (std::max)(0.f, (static_cast<float>(y) + 0.5f) * sy - 0.5f);
			const int y0 = (std::min)(static_cast<int>(fy), srcH - 1);
			const int y1 = (std::min)(y0 + 1, srcH - 1);
			const float ty = fy - static_cast<float>(y0);
			// Readback rows are top-down; the stream's row y is the texture's row y.
			const uint8_t* row0 = mFrameReadback.data() + static_cast<size_t>(srcH - 1 - y0) * srcW * 4u;
			const uint8_t* row1 = mFrameReadback.data() + static_cast<size_t>(srcH - 1 - y1) * srcW * 4u;
			uint8_t* out = rgba.data() + static_cast<size_t>(y) * width * 4u;
			for (int x = 0; x < width; ++x) {
				const float fx = (std::max)(0.f, (static_cast<float>(x) + 0.5f) * sx - 0.5f);
				const int x0 = (std::min)(static_cast<int>(fx), srcW - 1);
				const int x1 = (std::min)(x0 + 1, srcW - 1);
				const float tx = fx - static_cast<float>(x0);
				for (int c = 0; c < 4; ++c) {
					const float top = row0[x0 * 4 + c] + (row0[x1 * 4 + c] - row0[x0 * 4 + c]) * tx;
					const float bottom = row1[x0 * 4 + c] + (row1[x1 * 4 + c] - row1[x0 * 4 + c]) * tx;
					out[x * 4 + c] = static_cast<uint8_t>(top + (bottom - top) * ty + 0.5f);
				}
			}
		}
		return true;
	}

	void RuntimeBridge::streamPresentedFrame()
	{
		if (mEmbeddedHostMode || !mSubscribeFrames.load() || !mHasClient.load()) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - mLastFrameCapture < std::chrono::milliseconds(33)) {
			return;
		}

		auto* rhi = Engine::instance()->rhi();
		if (!rhi) {
			return;
		}

		int w = 0;
		int h = 0;
		if (!captureStreamFrame(*rhi, mFrameRgba, w, h)) {
			return;
		}

		EditorProtocol::FramePayload payload{};
		payload.width = static_cast<uint32_t>(w);
		payload.height = static_cast<uint32_t>(h);
		payload.format = 0;

		Json::Value meta;
		meta["width"] = w;
		meta["height"] = h;
		int renderW = w;
		if (auto* window = Engine::instance()->getWindow()) {
			renderW = (std::max)(1, window->getScreenData().renderW);
		}
		meta["scale"] = static_cast<double>(w) / static_cast<double>(renderW);
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";

		std::vector<uint8_t> packet;
		std::vector<uint8_t> binary(sizeof(EditorProtocol::FramePayload) + mFrameRgba.size());
		std::memcpy(binary.data(), &payload, sizeof(payload));
		if (!mFrameRgba.empty()) {
			std::memcpy(binary.data() + sizeof(payload), mFrameRgba.data(), mFrameRgba.size());
		}
		EditorProtocol::writePacket(
			packet,
			EditorProtocol::MessageType::Frame,
			Json::writeString(builder, meta),
			binary.data(),
			static_cast<uint32_t>(binary.size())
		);
		queueFramePacket(std::move(packet));
		mLastFrameCapture = now;
	}

	void RuntimeBridge::applyEmbedHost()
	{
		if (!mEmbedConfigDirty) {
			return;
		}
		mEmbedConfigDirty = false;

		auto* engine = Engine::instance();
		Render::Window* window = engine->getWindow();
		if (!window) {
			return;
		}

#ifdef _WIN32
		if (mEmbeddedHostMode && mHasClient.load() && mEmbedHostHwnd != 0 && mEmbedW > 0 && mEmbedH > 0) {
			HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(mEmbedHostHwnd));
			if (!IsWindow(owner)) {
				mEmbeddedHostMode = false;
				mEmbedConfigDirty = true;
			}
			else if (window->embedInHost(owner, mEmbedX, mEmbedY, mEmbedW, mEmbedH)) {
				engine->setPresentToScreen(true);
				setPublishEditorDisplay(false);
				mAppliedRemoteDisplay = false;
				return;
			}
			mEmbeddedHostMode = false;
		}

		if (window->isEmbeddedInHost()) {
			window->detachFromHost();
		}
#else
		mEmbeddedHostMode = false;
		(void)engine;
#endif
		if (mEmbeddedHostMode) {
			return;
		}
		if (mHasClient.load() && mSubscribeFrames.load()) {
			setPublishEditorDisplay(true);
		}
	}

	void RuntimeBridge::syncDisplayMode()
	{
		if (isEmbeddedInEditor()) {
			return;
		}

		applyEmbedHost();

		if (mEmbeddedHostMode && mHasClient.load()) {
			return;
		}

		const bool remoteStream = mHasClient.load() && mSubscribeFrames.load() && !mEmbeddedHostMode;
		if (remoteStream == mAppliedRemoteDisplay) {
			return;
		}
		mAppliedRemoteDisplay = remoteStream;

		auto* engine = Engine::instance();
		Render::Window* window = engine->getWindow();
		if (remoteStream) {
			engine->setPresentToScreen(false);
			setPublishEditorDisplay(true);
		}
		else if (!mHasClient.load()) {
			setPublishEditorDisplay(false);
			engine->setPresentToScreen(true);
			if (window && !window->isEmbeddedInHost()) {
				window->show();
			}
		}
	}

	void RuntimeBridge::tick()
	{
		std::vector<IncomingMessage> pending;
		{
			std::lock_guard lock(mIncomingMutex);
			pending.swap(mIncoming);
		}
		for (const auto& msg : pending) {
			processMessage(msg);
		}
		syncDisplayMode();
		if (mEmbeddedHostMode && mHasClient.load() && mEmbedW > 0 && mEmbedH > 0) {
			const bool moved = mEmbedX != mAppliedOverlayX
				|| mEmbedY != mAppliedOverlayY
				|| mEmbedW != mAppliedOverlayW
				|| mEmbedH != mAppliedOverlayH;
			const auto now = std::chrono::steady_clock::now();
			const bool due = now - mLastOverlayRefresh >= std::chrono::milliseconds(50);
			if (moved || due) {
				if (auto* window = Engine::instance()->getWindow()) {
					window->refreshEmbeddedOverlay(
						reinterpret_cast<void*>(static_cast<uintptr_t>(mEmbedHostHwnd)),
						mEmbedX,
						mEmbedY,
						mEmbedW,
						mEmbedH
					);
					mAppliedOverlayX = mEmbedX;
					mAppliedOverlayY = mEmbedY;
					mAppliedOverlayW = mEmbedW;
					mAppliedOverlayH = mEmbedH;
					mLastOverlayRefresh = now;
				}
			}
		}
	}

} // namespace SFE::RuntimeBridge

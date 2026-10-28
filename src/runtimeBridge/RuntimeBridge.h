#pragma once



#include "editorProtocol/EditorProtocol.h"

#include "editorProtocol/TcpSocket.h"



#include "containersModule/Singleton.h"



#include <atomic>

#include <chrono>

#include <condition_variable>

#include <cstdint>

#include <deque>

#include <functional>

#include <map>

#include <json/value.h>

#include <mutex>

#include <string>

#include <thread>

#include <vector>



namespace SFE::Render {
	class RenderHardwareInterface;
}

namespace SFE::RuntimeBridge {



	struct IncomingMessage {

		EditorProtocol::MessageType type = EditorProtocol::MessageType::Error;

		std::string json;

		std::vector<uint8_t> binary;

	};



	class RuntimeBridge : public Singleton<RuntimeBridge> {

	public:

		void start(uint16_t port = EditorProtocol::kDefaultPort);

		void stop();

		void tick();



		bool isRunning() const { return mRunning.load(); }

		bool hasClient() const { return mHasClient.load(); }

		void streamPresentedFrame();

		// Run inside an editor that shows the frames itself (the editor sets it): the bridge then leaves the window and the
		// streaming as the editor has them.
		void setEmbeddedInEditor(bool embedded) { mEmbeddedInEditor = embedded; }
		bool isEmbeddedInEditor() const { return mEmbeddedInEditor; }
		// Whether each presented frame goes to the editor (streamPresentedFrame, from RenderSystem): on while an editor
		// that subscribed to the frames streams them.
		void setPublishEditorDisplay(bool publish) { mPublishEditorDisplay = publish; }
		bool publishEditorDisplay() const { return mPublishEditorDisplay; }

		// Things to do in the running game, by name: from a panel's button, or sent from outside (a Command message --
		// a script, the editor) when the bridge is serving. Each takes its arguments as JSON and says what it did. Run on
		// the main thread, between frames. The engine adds its own; a game adds its own the same way.
		using Command = std::function<std::string(const Json::Value& args)>;
		struct CommandEntry {
			std::string help;
			Command run;
		};
		// A name already taken goes to the later command, with a warning naming it; true when one was replaced so.
		bool addCommand(const std::string& name, std::string help, Command run);
		// What it did, or why it could not.
		std::string runCommand(const std::string& name, const Json::Value& args = Json::Value());
		const std::map<std::string, CommandEntry>& commands() const { return mCommands; }
		// Every command, whether from a panel's button or sent from outside, waits for the start of the next frame
		// (runQueued, from the engine, once the frame has begun and before anything is recorded): there what it changes
		// is not in use, and what it uploads goes with the frame's own uploads rather than each on its own. What the last
		// command run said.
		void queueCommand(const std::string& name, const Json::Value& args = Json::Value());
		void runQueued();
		const std::string& lastResult() const { return mLastResult; }

	private:

		void networkThreadMain();

		void sendThreadMain();

		void processMessage(const IncomingMessage& msg);

		bool sendPacket(EditorProtocol::MessageType type, const std::string& json, const void* binary = nullptr, uint32_t binarySize = 0);

		bool enqueuePacket(std::vector<uint8_t>&& packet, bool dropIfBusy);

		void queueFramePacket(std::vector<uint8_t>&& packet);

		void sendStatus();

		void sendWorldSnapshot(const std::string& requestJson = {});

		void processInput(const std::string& json);

		void syncDisplayMode();

		void applyEmbedHost();



		std::map<std::string, CommandEntry> mCommands;
		struct Queued {
			std::string name;
			Json::Value args;
			bool answer = false; // sent from outside: what it did is sent back
		};
		std::vector<Queued> mQueued; // main thread only
		std::string mLastResult;

		std::thread mNetworkThread;

		std::thread mSendThread;

		std::atomic<bool> mRunning{ false };

		std::atomic<bool> mSendRunning{ false };

		std::atomic<bool> mHasClient{ false };

		std::atomic<bool> mSubscribeFrames{ false };



		std::mutex mIncomingMutex;

		std::vector<IncomingMessage> mIncoming;



		std::mutex mOutgoingMutex;

		std::condition_variable mOutgoingCv;

		std::deque<std::vector<uint8_t>> mOutgoing;



		EditorProtocol::TcpSocket mClientSocket;

		EditorProtocol::TcpSocket mListenSocket;

		uint16_t mPort = EditorProtocol::kDefaultPort;



		std::vector<uint8_t> mRecvBuffer;

		std::vector<uint8_t> mFrameRgba;
		std::vector<uint8_t> mFrameReadback;
		bool captureStreamFrame(Render::RenderHardwareInterface& rhi, std::vector<uint8_t>& rgba, int& width, int& height);

		bool mAppliedRemoteDisplay = false;

		bool mEmbeddedHostMode = false;
		bool mEmbeddedInEditor = false;
		bool mPublishEditorDisplay = false;
		bool mEmbedConfigDirty = false;
		uint64_t mEmbedHostHwnd = 0;
		int mEmbedX = 0;
		int mEmbedY = 0;
		int mEmbedW = 0;
		int mEmbedH = 0;

		int mAppliedOverlayX = -1;
		int mAppliedOverlayY = -1;
		int mAppliedOverlayW = -1;
		int mAppliedOverlayH = -1;
		std::chrono::steady_clock::time_point mLastOverlayRefresh{};
		std::chrono::steady_clock::time_point mLastFrameCapture{};
		std::chrono::steady_clock::time_point mLastSnapshotRequest{};

	};

} // namespace SFE::RuntimeBridge

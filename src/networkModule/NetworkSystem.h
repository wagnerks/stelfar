#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "NetBuffer.h"
#include "NetworkSettings.h"
#include "Transport.h"
#include "systemsModule/SystemBase.h"

namespace SFE::Network {
	using PlayerId = uint16_t;
	constexpr PlayerId kHostPlayer = 0;
	constexpr PlayerId kNoPlayer = 0xFFFF;

	// A message's kind: the first two bytes of every message. Below kFirstGameMessage they are the session's own.
	using MessageType = uint16_t;
	constexpr MessageType kFirstGameMessage = 32;

	// Online play: a session of players, one of them the host, and messages between them. The host is the authority
	// over the shared world; each player is a number the host hands out as they join (the host itself is 0). A client
	// talks to the host alone: what the others should hear, the host passes on. With the default Settings it plays
	// offline, as the one player 0, the host of nobody.
	//
	// Main thread. Messages that arrived are handed to their handlers at the start of each frame (this system runs
	// first); what is sent during the frame goes out at the start of the next.
	class NetworkSystem : public ecss::System {
	public:
		using Channel = Transport::Channel;
		using Handler = std::function<void(PlayerId from, NetReader& message)>;
		using PlayerHandler = std::function<void(PlayerId player)>;

		explicit NetworkSystem(const Settings& settings);
		~NetworkSystem() override;

		void update(float dt) override;

		Role role() const { return mSettings.role; }
		// The host, or playing alone: whoever decides what happens in the shared world.
		bool authority() const { return mSettings.role != Role::Client; }
		bool online() const { return mSettings.role != Role::Offline; }
		// Whether this client has joined yet (always, for the host and offline).
		bool joined() const { return mLocalPlayer != kNoPlayer; }
		PlayerId localPlayer() const { return mLocalPlayer; }
		// Everyone in the session, this player too.
		const std::vector<PlayerId>& players() const { return mPlayers; }
		// The host's clock, in Engine::clockSeconds() seconds, as this machine best knows it: what a state sent by the
		// host is stamped with, and what a client plays it back against.
		double hostTime() const;
		// Whether hostTime is the host's clock yet: a client knows it from its first ping back.
		bool clockSynced() const { return mSettings.role != Role::Client || mClockKnown; }
		// Seconds there and back to the host, or to a player for the host.
		float roundTrip(PlayerId player = kHostPlayer) const;
		// Bytes a second sent and received, all players together, over the last second.
		struct Traffic {
			uint32_t sent = 0;
			uint32_t received = 0;
		};
		Traffic traffic() const { return mTraffic; }
		// A worse network than the real one, from now on (SimulatedTransport); Settings::conditions to start with.
		void setConditions(const Conditions& conditions);
		const Conditions& conditions() const { return mSettings.conditions; }

		// The one handler for a kind of message (a game's are kFirstGameMessage and up).
		void on(MessageType type, Handler handler);
		// Called as a player joins, and as one leaves: for a client, once for each player already there when it joins
		// (itself included), and for the host, once for itself as the session starts.
		void onJoined(PlayerHandler handler) { mJoined.push_back(std::move(handler)); }
		void onLeft(PlayerHandler handler) { mLeft.push_back(std::move(handler)); }

		// To the host, from a client. The host sending to itself hands the message to the handler at once.
		void sendToHost(MessageType type, Channel channel, const NetWriter& message);
		// To one player, from the host.
		void sendTo(PlayerId player, MessageType type, Channel channel, const NetWriter& message);
		// To every player but `except` (and never to the host itself), from the host.
		void broadcast(MessageType type, Channel channel, const NetWriter& message, PlayerId except = kNoPlayer);

	private:
		void handle(Transport::Event& event);
		void dispatch(PlayerId from, const uint8_t* data, size_t size);
		void addPlayer(PlayerId player);
		void removePlayer(PlayerId player);
		std::vector<uint8_t> pack(MessageType type, const NetWriter& message) const;

		Settings mSettings;
		std::unique_ptr<Transport> mTransport;
		class SimulatedTransport* mSimulated = nullptr; // mTransport, which wraps the real one
		std::vector<Transport::Event> mEvents;
		std::unordered_map<MessageType, Handler> mHandlers;
		std::vector<PlayerHandler> mJoined;
		std::vector<PlayerHandler> mLeft;
		bool mStarted = false; // the host's own join has been announced

		PlayerId mLocalPlayer = kNoPlayer;
		std::vector<PlayerId> mPlayers;
		// The host's: which transport peer each player is, and back.
		std::unordered_map<Transport::Peer, PlayerId> mPlayerOfPeer;
		std::unordered_map<PlayerId, Transport::Peer> mPeerOfPlayer;

		// Bytes counted this second, and the last whole second's.
		Traffic mCounting;
		Traffic mTraffic;
		double mTrafficSecond = 0.0;

		// A client's: the host's clock less this one's, eased towards each ping's measure.
		double mClockOffset = 0.0;
		bool mClockKnown = false;
		double mNextPing = 0.0;
	};
}

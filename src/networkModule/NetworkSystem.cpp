#include "NetworkSystem.h"

#include <algorithm>

#include "EnetTransport.h"
#include "SimulatedTransport.h"
#include "core/Engine.h"
#include "logsModule/logger.h"

namespace SFE::Network {
	namespace {
		// The session's own messages.
		enum SessionMessage : MessageType {
			Welcome = 1, // host -> joining client: its player number, and everyone already there
			Joined,      // host -> clients: a player joined
			Left,        // host -> clients: a player left
			Ping,        // client -> host: this client's clock
			Pong,        // host -> client: that clock back, and the host's
		};
		constexpr double kPingEvery = 1.0;   // seconds
		constexpr double kClockEase = 0.1;   // the share of each new measure the clock offset takes
	}

	NetworkSystem::NetworkSystem(const Settings& settings) {
		mSettings = settings;
		if (mSettings.role == Role::Offline) {
			mLocalPlayer = kHostPlayer;
			mPlayers = { kHostPlayer };
			return;
		}
		auto simulated = std::make_unique<SimulatedTransport>(std::make_unique<EnetTransport>());
		simulated->setConditions(mSettings.conditions);
		mSimulated = simulated.get();
		mTransport = std::move(simulated);
		if (mSettings.role == Role::Host) {
			if (!mTransport->host(mSettings.port, mSettings.maxPlayers > 1 ? mSettings.maxPlayers - 1 : 1)) {
				LogsModule::Logger::LOG_ERROR("network: hosting failed, playing offline");
				mSettings.role = Role::Offline;
				mTransport.reset();
			}
			else {
				LogsModule::Logger::LOG_INFO("network: hosting on port %u for %u players", static_cast<unsigned>(mSettings.port), mSettings.maxPlayers);
			}
			mLocalPlayer = kHostPlayer;
			mPlayers = { kHostPlayer };
			return;
		}
		if (!mTransport->connect(mSettings.address, mSettings.port)) {
			LogsModule::Logger::LOG_ERROR("network: cannot join %s:%u, playing offline", mSettings.address.c_str(), static_cast<unsigned>(mSettings.port));
			mSettings.role = Role::Offline;
			mTransport.reset();
			mLocalPlayer = kHostPlayer;
			mPlayers = { kHostPlayer };
			return;
		}
		LogsModule::Logger::LOG_INFO("network: joining %s:%u", mSettings.address.c_str(), static_cast<unsigned>(mSettings.port));
	}

	NetworkSystem::~NetworkSystem() = default;

	double NetworkSystem::hostTime() const {
		return Engine::clockSeconds() + (mSettings.role == Role::Client ? mClockOffset : 0.0);
	}

	float NetworkSystem::roundTrip(PlayerId player) const {
		if (!mTransport) {
			return 0.f;
		}
		if (mSettings.role == Role::Client) {
			return mTransport->roundTrip(0);
		}
		const auto it = mPeerOfPlayer.find(player);
		return it == mPeerOfPlayer.end() ? 0.f : mTransport->roundTrip(it->second);
	}

	void NetworkSystem::setConditions(const Conditions& conditions) {
		mSettings.conditions = conditions;
		if (mTransport && mSimulated) {
			mSimulated->setConditions(conditions);
		}
	}

	void NetworkSystem::on(MessageType type, Handler handler) {
		mHandlers[type] = std::move(handler);
	}

	std::vector<uint8_t> NetworkSystem::pack(MessageType type, const NetWriter& message) const {
		std::vector<uint8_t> packet(sizeof(type) + message.size());
		std::memcpy(packet.data(), &type, sizeof(type));
		std::copy(message.bytes().begin(), message.bytes().end(), packet.begin() + sizeof(type));
		return packet;
	}

	void NetworkSystem::sendToHost(MessageType type, Channel channel, const NetWriter& message) {
		if (mSettings.role == Role::Client) {
			if (mTransport) {
				const auto packet = pack(type, message);
				mTransport->send(0, channel, packet.data(), packet.size());
				mCounting.sent += static_cast<uint32_t>(packet.size());
			}
			return;
		}
		// The host, or offline: it is its own host.
		const auto packet = pack(type, message);
		dispatch(kHostPlayer, packet.data(), packet.size());
	}

	void NetworkSystem::sendTo(PlayerId player, MessageType type, Channel channel, const NetWriter& message) {
		if (mSettings.role != Role::Host || !mTransport) {
			return;
		}
		const auto it = mPeerOfPlayer.find(player);
		if (it == mPeerOfPlayer.end()) {
			return;
		}
		const auto packet = pack(type, message);
		mTransport->send(it->second, channel, packet.data(), packet.size());
		mCounting.sent += static_cast<uint32_t>(packet.size());
	}

	void NetworkSystem::broadcast(MessageType type, Channel channel, const NetWriter& message, PlayerId except) {
		if (mSettings.role != Role::Host || !mTransport || mPeerOfPlayer.empty()) {
			return;
		}
		const auto packet = pack(type, message);
		for (const auto& [player, peer] : mPeerOfPlayer) {
			if (player != except) {
				mTransport->send(peer, channel, packet.data(), packet.size());
				mCounting.sent += static_cast<uint32_t>(packet.size());
			}
		}
	}

	void NetworkSystem::update(float) {
		// The host is in its own session from the first frame, when every system that listens is there.
		if (!mStarted && mSettings.role != Role::Client) {
			mStarted = true;
			for (const auto& handler : mJoined) {
				handler(kHostPlayer);
			}
		}
		if (!mTransport) {
			return;
		}
		mEvents.clear();
		mTransport->poll(mEvents);
		for (auto& event : mEvents) {
			mCounting.received += static_cast<uint32_t>(event.data.size());
			handle(event);
		}
		if (const double now = Engine::clockSeconds(); now >= mTrafficSecond + 1.0) {
			mTraffic = mCounting;
			mCounting = {};
			mTrafficSecond = now;
		}
		if (mSettings.role == Role::Client && joined() && Engine::clockSeconds() >= mNextPing) {
			mNextPing = Engine::clockSeconds() + kPingEvery;
			NetWriter ping;
			ping.write(Engine::clockSeconds());
			sendToHost(Ping, Channel::Unreliable, ping);
		}
	}

	void NetworkSystem::handle(Transport::Event& event) {
		using Type = Transport::Event::Type;
		if (mSettings.role == Role::Host) {
			if (event.type == Type::Connected) {
				// The lowest number free: a player who leaves and comes back may get theirs again.
				PlayerId player = 1;
				while (std::find(mPlayers.begin(), mPlayers.end(), player) != mPlayers.end()) {
					++player;
				}
				mPlayerOfPeer[event.peer] = player;
				mPeerOfPlayer[player] = event.peer;
				NetWriter welcome;
				welcome.write(player);
				welcome.write(static_cast<uint16_t>(mPlayers.size()));
				for (const auto other : mPlayers) {
					welcome.write(other);
				}
				sendTo(player, Welcome, Channel::Reliable, welcome);
				NetWriter joined;
				joined.write(player);
				broadcast(Joined, Channel::Reliable, joined, player);
				LogsModule::Logger::LOG_INFO("network: player %u joined", static_cast<unsigned>(player));
				addPlayer(player);
				return;
			}
			const auto it = mPlayerOfPeer.find(event.peer);
			if (it == mPlayerOfPeer.end()) {
				return;
			}
			const PlayerId player = it->second;
			if (event.type == Type::Disconnected) {
				mPlayerOfPeer.erase(it);
				mPeerOfPlayer.erase(player);
				NetWriter left;
				left.write(player);
				broadcast(Left, Channel::Reliable, left);
				LogsModule::Logger::LOG_INFO("network: player %u left", static_cast<unsigned>(player));
				removePlayer(player);
				return;
			}
			dispatch(player, event.data.data(), event.data.size());
			return;
		}

		// A client: everything comes from the host.
		if (event.type == Type::Connected) {
			LogsModule::Logger::LOG_INFO("network: connected, waiting to be let in");
			return;
		}
		if (event.type == Type::Disconnected) {
			LogsModule::Logger::LOG_WARNING("network: the host is gone, playing on alone");
			for (const auto player : std::vector<PlayerId>(mPlayers)) {
				if (player != mLocalPlayer) {
					removePlayer(player);
				}
			}
			mTransport.reset();
			return;
		}
		dispatch(kHostPlayer, event.data.data(), event.data.size());
	}

	void NetworkSystem::dispatch(PlayerId from, const uint8_t* data, size_t size) {
		NetReader reader(data, size);
		const auto type = reader.read<MessageType>();
		if (!reader.ok()) {
			return;
		}
		switch (type) {
		case Welcome:
			if (mSettings.role == Role::Client && !joined()) {
				mLocalPlayer = reader.read<PlayerId>();
				const auto count = reader.read<uint16_t>();
				std::vector<PlayerId> others(count);
				for (auto& other : others) {
					other = reader.read<PlayerId>();
				}
				if (!reader.ok()) {
					mLocalPlayer = kNoPlayer;
					return;
				}
				LogsModule::Logger::LOG_INFO("network: joined as player %u", static_cast<unsigned>(mLocalPlayer));
				for (const auto other : others) {
					addPlayer(other);
				}
				addPlayer(mLocalPlayer);
			}
			return;
		case Joined:
		case Left:
			if (mSettings.role == Role::Client) {
				const auto player = reader.read<PlayerId>();
				if (reader.ok()) {
					type == Joined ? addPlayer(player) : removePlayer(player);
				}
			}
			return;
		case Ping:
			if (mSettings.role == Role::Host) {
				NetWriter pong;
				pong.write(reader.read<double>());
				pong.write(Engine::clockSeconds());
				if (reader.ok()) {
					sendTo(from, Pong, Channel::Unreliable, pong);
				}
			}
			return;
		case Pong:
			if (mSettings.role == Role::Client) {
				const double sent = reader.read<double>();
				const double host = reader.read<double>();
				if (!reader.ok()) {
					return;
				}
				const double now = Engine::clockSeconds();
				const double measured = host + (now - sent) * 0.5 - now;
				mClockOffset = mClockKnown ? mClockOffset + (measured - mClockOffset) * kClockEase : measured;
				mClockKnown = true;
			}
			return;
		default:
			break;
		}
		if (const auto it = mHandlers.find(type); it != mHandlers.end()) {
			it->second(from, reader);
		}
	}

	void NetworkSystem::addPlayer(PlayerId player) {
		if (std::find(mPlayers.begin(), mPlayers.end(), player) != mPlayers.end()) {
			return;
		}
		mPlayers.push_back(player);
		for (const auto& handler : mJoined) {
			handler(player);
		}
	}

	void NetworkSystem::removePlayer(PlayerId player) {
		const auto it = std::find(mPlayers.begin(), mPlayers.end(), player);
		if (it == mPlayers.end()) {
			return;
		}
		mPlayers.erase(it);
		for (const auto& handler : mLeft) {
			handler(player);
		}
	}
}

#pragma once

#include <cstdint>
#include <string>

namespace SFE::Network {
	enum class Role : uint8_t { Offline, Host, Client };

	// A worse network than the real one, to try the game over (SimulatedTransport): the round trip made longer by
	// `lagMs`, give or take `jitterMs` each way, and `lossPercent` of the messages lost.
	struct Conditions {
		float lagMs = 0.f;
		float jitterMs = 0.f;
		float lossPercent = 0.f;
		bool any() const { return lagMs > 0.f || jitterMs > 0.f || lossPercent > 0.f; }
	};

	// How this run takes part in online play: given to NetworkSystem as it is made (Modules::addNetwork), and read by it
	// as it starts. A host listens on `port` for up to `maxPlayers` - 1 others; a client joins the host at
	// `address`:`port`.
	struct Settings {
		Role role = Role::Offline;
		std::string address;
		uint16_t port = 27015;
		uint32_t maxPlayers = 5;
		Conditions conditions;
	};
}

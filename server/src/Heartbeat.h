#ifndef F4MP_HEARTBEAT_H
#define F4MP_HEARTBEAT_H

#include <string>
#include <cstdint>

// Sends POST /heartbeat to the master server. Blocking: call it from its own thread.
// Does nothing when url is empty. WinHTTP on Windows, libcurl on Linux/others.
namespace Heartbeat {
    void Send(const std::string& url, const std::string& name,
              uint16_t port, uint32_t players, uint32_t maxPlayers);
}

#endif

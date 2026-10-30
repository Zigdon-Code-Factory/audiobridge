#pragma once
#include "mic_pipe.h"
#include "../../common/audiobridge_mic.h"

namespace mic {
class Backend {
public:
    virtual ~Backend() = default;
    virtual DWORD open(HANDLE stop) = 0;
    virtual DWORD push(const MicPacket&) = 0;
    virtual void close() = 0;
};

// The pipe connection is the lease. Every exit (bad input, driver failure, EOF,
// service stop, timeout) releases the endpoint before accepting another client.
inline void serveSession(HANDLE pipe, HANDLE stop, Backend& backend) {
    struct Cleanup { Backend& b; ~Cleanup() { b.close(); } } cleanup{backend};
    MicPacket packet;
    if (!transfer(pipe, false, &packet, sizeof(packet), stop, 3000) ||
        !validMicPacket(packet) || packet.frames != 0) return;
    MicReply reply;
    reply.error = backend.open(stop);
    if (!transfer(pipe, true, &reply, sizeof(reply), stop, 2000) || reply.error) return;
    while (transfer(pipe, false, &packet, sizeof(packet), stop, 3000)) {
        if (!validMicPacket(packet)) return;
        reply.error = packet.frames ? backend.push(packet) : ERROR_SUCCESS;
        if (!transfer(pipe, true, &reply, sizeof(reply), stop, 1000) || reply.error) return;
    }
}
}

#ifndef TUNNELSLOT_H
#define TUNNELSLOT_H

class ClientTunnel;

// Bookkeeping of one tunnel of a TunnelPool. A slot is either empty (waiting for a reconnect),
// connecting, or holds a tunnel that finished the handshake and is ready for new streams.
struct TunnelSlot {
    ClientTunnel* tunnel = nullptr;     // owned by the reactor, cleared in onTunnelClosed()
    bool ready = false;                 // handshake done, accepts new streams
    bool reconnectScheduled = false;
    int reconnectDelayMs = 0;
};

#endif // TUNNELSLOT_H

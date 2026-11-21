#ifndef HANDSHAKERESULT_H
#define HANDSHAKERESULT_H

enum class HandshakeResult {
    Incomplete,     // more bytes are needed
    Accepted,       // valid handshake
    Rejected        // malformed or unauthorised
};

#endif // HANDSHAKERESULT_H

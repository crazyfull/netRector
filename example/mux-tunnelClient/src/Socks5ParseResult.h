#ifndef SOCKS5PARSERESULT_H
#define SOCKS5PARSERESULT_H

enum class Socks5ParseResult {
    Incomplete,     // more bytes are needed
    Invalid,        // malformed or unsupported request (see Socks5Request::error)
    Ok              // complete request
};

#endif // SOCKS5PARSERESULT_H

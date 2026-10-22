#ifndef CLSTLSCONTEXT_H
#define CLSTLSCONTEXT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "clsReactorObject.h"

// forward declaration (BoringSSL)
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct evp_pkey_st EVP_PKEY;
class EpollReactor;
class TLSKeyWorkers;
class TCPSocket;

// yek amaliat-e emza (private key) ke roye thread-e worker anjam mishe
struct TLSKeyJob {
    EpollReactor* reactor { nullptr };
    std::shared_ptr<ReactorLiveToken> live;
    uint64_t socketKey { 0 };
    uint16_t sigalg { 0 };
    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
    std::atomic<bool> done { false };
    bool ok { false };
};

// callback haye SSL_PRIVATE_KEY_METHOD (dakheli)
struct TLSKeyMethod {
    static bool startJob(TCPSocket* sock, uint16_t sigalg, const uint8_t* in, size_t len);
    static TLSKeyJob* job(TCPSocket* sock);
    static void clearJob(TCPSocket* sock);
};

// ============================== TLSContext (BoringSSL) =======================
// Tanzimat moshtarak TLS (certificate, verify, ALPN). Yek bar sakhte mishe va
// beyne hame socket ha va hame reactor ha moshtarak estefade mishe (thread-safe).
// Bayad ta zamani ke socket haye vabaste baz hastan zende bemone.
//
//   server:  TLSContext* ctx = TLSContext::createServer("cert.pem", "key.pem");
//            // dar accept callback:  sock->enableTLS(ctx);
//   client:  TLSContext* ctx = TLSContext::createClient(true);
//            sock->enableTLS(ctx);  sock->connectTo("example.com", 443);
class TLSContext {
public:
    // certChainFile: PEM (leaf + intermediate ha), privateKeyFile: PEM
    static TLSContext* createServer(const char* certChainFile, const char* privateKeyFile, std::string* error = nullptr);

    // verifyPeer=true: certificate va hostname server check mishe.
    // caFile=nullptr: az CA haye system estefade mishe.
    static TLSContext* createClient(bool verifyPeer = true, const char* caFile = nullptr, std::string* error = nullptr);

    ~TLSContext();
    TLSContext(const TLSContext&) = delete;
    TLSContext& operator=(const TLSContext&) = delete;

    // masalan {"h2", "http/1.1"}; ghabl az sakhte avalin connection seda zade beshe
    bool setALPN(const std::vector<std::string>& protocols);

    // faghat server: amaliat-e private key handshake (emza-ye RSA/ECDSA, chand sad
    // microsanie) be yek thread pool montaghel mishe ta halghe-ye reactor block nashe.
    // threads = 0: khamosh (default). ghabl az avalin connection seda zade beshe.
    bool enableAsyncPrivateKey(int threads);
    bool asyncPrivateKey() const { return m_workers != nullptr; }

    // ex_data index baraye residan az SSL* be TCPSocket*
    static int socketExIndex();
    bool submitKeyJob(std::shared_ptr<TLSKeyJob> job);

    bool isServer() const { return m_isServer; }
    bool verifyPeer() const { return m_verifyPeer; }
    SSL_CTX* native() const { return m_ctx; }
    const std::string& alpnWire() const { return m_alpnWire; }

    // akharin khataye BoringSSL dar in thread (va khali kardane error queue)
    static std::string lastErrorString();

private:
    TLSContext(SSL_CTX* ctx, bool isServer, bool verifyPeer);

    SSL_CTX* m_ctx;
    bool m_isServer;
    bool m_verifyPeer;
    std::string m_alpnWire;     // format wire: len1 proto1 len2 proto2 ...
    EVP_PKEY* m_privateKey { nullptr };
    TLSKeyWorkers* m_workers { nullptr };
};

#endif // CLSTLSCONTEXT_H

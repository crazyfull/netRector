#include "clsTLSContext.h"
#include "clsTCPSocket.h"
#include "clsEpollReactor.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// ============================== TLSKeyWorkers ================================
// thread pool-e emza. natije ba EpollReactor::post be thread-e reactor barmigarde.
class TLSKeyWorkers {
public:
    TLSKeyWorkers(EVP_PKEY* key, int threads) : m_key(key) {
        for (int i = 0; i < threads; i++)
            m_threads.emplace_back([this]() { run(); });
    }

    ~TLSKeyWorkers() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads)
            t.join();
    }

    void submit(std::shared_ptr<TLSKeyJob> job) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(job));
        }
        m_cv.notify_one();
    }

    static bool sign(EVP_PKEY* key, TLSKeyJob& job) {
        EVP_MD_CTX md;
        EVP_MD_CTX_init(&md);
        EVP_PKEY_CTX* pctx = nullptr;
        const EVP_MD* digest = SSL_get_signature_algorithm_digest(job.sigalg);   // nullptr baraye Ed25519
        bool ok = EVP_DigestSignInit(&md, &pctx, digest, nullptr, key) == 1;
        if (ok && SSL_is_signature_algorithm_rsa_pss(job.sigalg)) {
            ok = EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1
                 && EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, -1) == 1;
        }
        size_t len = 0;
        if (ok)
            ok = EVP_DigestSign(&md, nullptr, &len, job.in.data(), job.in.size()) == 1;
        if (ok) {
            job.out.resize(len);
            ok = EVP_DigestSign(&md, job.out.data(), &len, job.in.data(), job.in.size()) == 1;
            job.out.resize(ok ? len : 0);
        }
        EVP_MD_CTX_cleanup(&md);
        ERR_clear_error();
        return ok;
    }

private:
    EVP_PKEY* m_key;
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::shared_ptr<TLSKeyJob>> m_queue;
    bool m_stop = false;

    void run() {
        for (;;) {
            std::shared_ptr<TLSKeyJob> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
                if (m_stop)
                    return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }

            job->ok = sign(m_key, *job);
            job->done.store(true, std::memory_order_release);

            // edame-ye handshake roye thread-e reactor-e haman socket
            EpollReactor* reactor = job->reactor;
            reactor->post([job]() {
                TCPSocket* sock = job->reactor->lookupSocket(job->socketKey);
                if (sock)
                    sock->tlsKeyJobDone(job.get());
            });
        }
    }
};

// ---- SSL_PRIVATE_KEY_METHOD ----
static enum ssl_private_key_result_t keySign(SSL* ssl, uint8_t* /*out*/, size_t* /*out_len*/, size_t /*max_out*/,
                                             uint16_t sigalg, const uint8_t* in, size_t in_len)
{
    TCPSocket* sock = static_cast<TCPSocket*>(SSL_get_ex_data(ssl, TLSContext::socketExIndex()));
    if (!sock || !sock->tlsStartKeyJob(sigalg, in, in_len))
        return ssl_private_key_failure;
    return ssl_private_key_retry;
}

static enum ssl_private_key_result_t keyDecrypt(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out,
                                                const uint8_t* in, size_t in_len)
{
    // RSA key exchange (TLS 1.2 bedone ECDHE): nadir; hamin ja anjam mishe
    TCPSocket* sock = static_cast<TCPSocket*>(SSL_get_ex_data(ssl, TLSContext::socketExIndex()));
    TLSContext* ctx = sock ? sock->tlsContext() : nullptr;
    EVP_PKEY* key = ctx ? SSL_CTX_get0_privatekey(ctx->native()) : nullptr;
    RSA* rsa = key ? EVP_PKEY_get0_RSA(key) : nullptr;
    if (!rsa || RSA_decrypt(rsa, out_len, out, max_out, in, in_len, RSA_NO_PADDING) != 1)
        return ssl_private_key_failure;
    return ssl_private_key_success;
}

static enum ssl_private_key_result_t keyComplete(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out)
{
    TCPSocket* sock = static_cast<TCPSocket*>(SSL_get_ex_data(ssl, TLSContext::socketExIndex()));
    if (!sock)
        return ssl_private_key_failure;

    TLSKeyJob* job = sock->tlsKeyJob();
    if (!job)
        return ssl_private_key_failure;
    if (!job->done.load(std::memory_order_acquire))
        return ssl_private_key_retry;

    bool ok = job->ok && job->out.size() <= max_out;
    if (ok) {
        memcpy(out, job->out.data(), job->out.size());
        *out_len = job->out.size();
    }
    sock->tlsClearKeyJob();
    return ok ? ssl_private_key_success : ssl_private_key_failure;
}

static const SSL_PRIVATE_KEY_METHOD g_asyncKeyMethod = { keySign, keyDecrypt, keyComplete };

int TLSContext::socketExIndex()
{
    static int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

bool TLSContext::enableAsyncPrivateKey(int threads)
{
    if (!m_isServer || m_workers)
        return false;
    if (threads <= 0)
        return true;

    // BoringSSL ba set shodane key method, key-e khodesh ro raha mikone; yek reference negah midarim
    m_privateKey = SSL_CTX_get0_privatekey(m_ctx);
    if (!m_privateKey)
        return false;
    EVP_PKEY_up_ref(m_privateKey);

    // faghat cipher haye ECDHE: amaliat-e key dar handshake faghat emza hast
    SSL_CTX_set_strict_cipher_list(m_ctx,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305");

    SSL_CTX_set_private_key_method(m_ctx, &g_asyncKeyMethod);
    m_workers = new TLSKeyWorkers(m_privateKey, threads);
    return true;
}

bool TLSContext::submitKeyJob(std::shared_ptr<TLSKeyJob> job)
{
    if (!m_workers)
        return false;
    m_workers->submit(std::move(job));
    return true;
}

static int alpnSelectCallback(SSL* /*ssl*/, const uint8_t** out, uint8_t* outlen, const uint8_t* in, unsigned inlen, void* arg)
{
    TLSContext* self = static_cast<TLSContext*>(arg);
    const std::string& wire = self->alpnWire();
    if (wire.empty())
        return SSL_TLSEXT_ERR_NOACK;

    // tartib olaviat ba server hast
    if (SSL_select_next_proto(const_cast<uint8_t**>(out), outlen,
                              reinterpret_cast<const uint8_t*>(wire.data()), (unsigned)wire.size(),
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    return SSL_TLSEXT_ERR_OK;
}

static SSL_CTX* newBaseContext()
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        return nullptr;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // PARTIAL_WRITE: SSL_write bad az har record barmigarde (hamahang ba safe ersal)
    // ACCEPT_MOVING_WRITE_BUFFER: retry ba pointer motafavet (data az stack be SendQueue copy mishe)
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

TLSContext::TLSContext(SSL_CTX *ctx, bool isServer, bool verifyPeer)
    : m_ctx(ctx), m_isServer(isServer), m_verifyPeer(verifyPeer)
{
}

TLSContext::~TLSContext()
{
    delete m_workers;   // thread ha join mishan
    if (m_privateKey)
        EVP_PKEY_free(m_privateKey);
    if (m_ctx)
        SSL_CTX_free(m_ctx);
}

std::string TLSContext::lastErrorString()
{
    std::string out;
    uint32_t code;
    char buf[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!out.empty())
            out += "; ";
        out += buf;
    }
    return out;
}

TLSContext *TLSContext::createServer(const char *certChainFile, const char *privateKeyFile, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    if (!certChainFile || !privateKeyFile
        || SSL_CTX_use_certificate_chain_file(ctx, certChainFile) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, privateKeyFile, SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(ctx) != 1) {
        if (error) {
            *error = lastErrorString();
            if (error->empty()) *error = "certificate or private key not usable";
        }
        SSL_CTX_free(ctx);
        return nullptr;
    }

    static const uint8_t sessionIdContext[] = "netRector";
    SSL_CTX_set_session_id_context(ctx, sessionIdContext, sizeof(sessionIdContext) - 1);

    TLSContext* self = new TLSContext(ctx, true, false);
    SSL_CTX_set_alpn_select_cb(ctx, alpnSelectCallback, self);
    return self;
}

TLSContext *TLSContext::createClient(bool verifyPeer, const char *caFile, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    if (verifyPeer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        int ok = (caFile && *caFile)
                     ? SSL_CTX_load_verify_locations(ctx, caFile, nullptr)
                     : SSL_CTX_set_default_verify_paths(ctx);
        if (ok != 1) {
            if (error) {
                *error = lastErrorString();
                if (error->empty()) *error = "can not load CA certificates";
            }
            SSL_CTX_free(ctx);
            return nullptr;
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    return new TLSContext(ctx, false, verifyPeer);
}

bool TLSContext::setALPN(const std::vector<std::string> &protocols)
{
    std::string wire;
    for (const std::string& p : protocols) {
        if (p.empty() || p.size() > 255)
            return false;
        wire.push_back(static_cast<char>(p.size()));
        wire += p;
    }

    m_alpnWire = wire;

    if (!m_isServer) {
        // dar in API meghdar 0 yani movafagh
        if (SSL_CTX_set_alpn_protos(m_ctx, reinterpret_cast<const uint8_t*>(m_alpnWire.data()), (unsigned)m_alpnWire.size()) != 0)
            return false;
    }
    return true;
}

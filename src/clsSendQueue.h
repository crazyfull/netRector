#ifndef CLSSENDQUEUE_H
#define CLSSENDQUEUE_H

#include "clsBufferPool.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/types.h>
#include <sys/uio.h>

// ============================== SendQueue ====================================
// Safe ersal be shekle zanjire-ye chunk (intrusive, bedone container-e std).
// Yek SendQueue-e khali hich hafeze-i roye heap nadare (mohem baraye connection
// haye bikar). Har chunk yek block az BufferPool (ya fallback malloc) hast ke
// header-e chunk dar ebtedaye haman block gharar dare.
//
// Noe chunk ha:
//   Pool/Heap : data copy shode (push / pushv)
//   External  : eshare be hafeze-ye app bedone copy (pushExternal); bad az ersal
//               release seda zade mishe
//   File      : baze-i az yek file (pushFile) ke ba sendfile ersal mishe
class SendQueue {
public:
    enum ChunkKind : uint8_t { Pool = 0, Heap = 1, External = 2, File = 3 };
    using ReleaseFn = void(*)(void* data, void* arg);

    struct Chunk {
        Chunk* next;
        size_t len;         // Pool/Heap: bytes neveshte shode | External/File: tool kol
        size_t off;         // bytes ersal shode az ebteda
        size_t cap;         // Pool/Heap: zarfiat data
        ChunkKind kind;
        union {
            struct {
                const char* ptr;
                ReleaseFn release;
                void* arg;
            } ext;
            struct {
                int fd;
                off_t offset;   // offset ebtedaye baze dar file
            } file;
        };

        // pointer be data-ye ersal nashode (faghat Pool/Heap/External)
        const char* unsent() const {
            const char* base = (kind == External) ? ext.ptr : reinterpret_cast<const char*>(this + 1);
            return base + off;
        }
        size_t avail() const { return len - off; }
        char* writable() { return reinterpret_cast<char*>(this + 1); }
    };

    SendQueue() = default;
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    void init(BufferPool* pool) { m_pool = pool; }

    // transactional: ya kole data ezafe mishe ya hich (false: hafeze nist)
    bool push(const void* data, size_t len);
    bool pushv(const struct iovec* iov, int iovcnt);
    bool pushExternal(const void* data, size_t len, size_t alreadySent, ReleaseFn release, void* arg);
    bool pushFile(int fd, off_t offset, size_t len, size_t alreadySent = 0);

    bool empty() const { return m_head == nullptr; }
    Chunk* front() { return m_head; }
    const Chunk* front() const { return m_head; }
    void pop_front();
    void consume(size_t n);     // n byte az ebtedaye saf ersal shod
    void clear();
    size_t size() const { return m_len; }       // majmoe bytes ersal nashode
    size_t count() const { return m_count; }

private:
    BufferPool* m_pool { nullptr };
    Chunk* m_head { nullptr };
    Chunk* m_tail { nullptr };
    size_t m_len { 0 };
    size_t m_count { 0 };

    Chunk* allocChunk(size_t dataCap);
    void freeChunk(Chunk* c);
    void append(Chunk* c);
};

#endif // CLSSENDQUEUE_H

#include "clsSendQueue.h"
#include "constants.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

SendQueue::~SendQueue() {
    clear();
}

SendQueue::Chunk* SendQueue::allocChunk(size_t dataCap)
{
    size_t total = sizeof(Chunk) + dataCap;
    Chunk* c = nullptr;
    ChunkKind kind = Pool;

    if (m_pool)
        c = static_cast<Chunk*>(m_pool->allocate(total));

    if (!c) {
        // pool por shode: hafeze-ye system (connection ghat nemishe)
        c = static_cast<Chunk*>(malloc(total));
        if (!c)
            return nullptr;
        kind = Heap;
    }

    c->next = nullptr;
    c->len = 0;
    c->off = 0;
    c->cap = dataCap;
    c->kind = kind;
    return c;
}

void SendQueue::freeChunk(Chunk* c)
{
    switch (c->kind) {
    case Pool:
        m_pool->deallocate(c);
        return;
    case Heap:
        free(c);
        return;
    case External:
        if (c->ext.release)
            c->ext.release(const_cast<char*>(c->ext.ptr), c->ext.arg);
        break;
    case File:
        break;
    }

    // header-e External/File khodesh az pool/heap gerefte shode (cap == 0 -> pool)
    if (c->cap == 1)
        free(c);
    else
        m_pool->deallocate(c);
}

void SendQueue::append(Chunk* c)
{
    if (m_tail)
        m_tail->next = c;
    else
        m_head = c;
    m_tail = c;
    m_count++;
}

bool SendQueue::push(const void* data, size_t len) {
    struct iovec iov;
    iov.iov_base = const_cast<void*>(data);
    iov.iov_len = len;
    return pushv(&iov, 1);
}

bool SendQueue::pushv(const struct iovec* iov, int iovcnt)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;
    if (total == 0)
        return true;

    // fazaye khali block akhar (faghat agar az noe copy bashe)
    size_t room = 0;
    if (m_tail && (m_tail->kind == Pool || m_tail->kind == Heap))
        room = m_tail->cap - m_tail->len;

    // hame-ye hafeze aval gerefte mishe ta dar soorat shekast chizi taghir nakone
    Chunk* fresh = nullptr;
    if (total > room) {
        size_t need = total - room;
        fresh = allocChunk(std::max(need, SEND_BLOCK_MIN));
        if (!fresh)
            return false;
    }

    size_t remainingRoom = room;
    for (int i = 0; i < iovcnt; i++) {
        const char* src = static_cast<const char*>(iov[i].iov_base);
        size_t n = iov[i].iov_len;

        if (remainingRoom > 0 && n > 0) {
            size_t c = std::min(remainingRoom, n);
            memcpy(m_tail->writable() + m_tail->len, src, c);
            m_tail->len += c;
            remainingRoom -= c;
            src += c;
            n -= c;
        }

        if (n > 0) {
            memcpy(fresh->writable() + fresh->len, src, n);
            fresh->len += n;
        }
    }

    if (fresh)
        append(fresh);

    m_len += total;
    return true;
}

bool SendQueue::pushExternal(const void* data, size_t len, size_t alreadySent, ReleaseFn release, void* arg)
{
    Chunk* c = nullptr;
    if (m_pool)
        c = static_cast<Chunk*>(m_pool->allocate(sizeof(Chunk)));
    size_t capMark = 0;
    if (!c) {
        c = static_cast<Chunk*>(malloc(sizeof(Chunk)));
        if (!c)
            return false;
        capMark = 1;    // header az heap
    }

    c->next = nullptr;
    c->len = len;
    c->off = alreadySent;
    c->cap = capMark;
    c->kind = External;
    c->ext.ptr = static_cast<const char*>(data);
    c->ext.release = release;
    c->ext.arg = arg;
    append(c);
    m_len += len - alreadySent;
    return true;
}

bool SendQueue::pushFile(int fd, off_t offset, size_t len, size_t alreadySent)
{
    Chunk* c = nullptr;
    if (m_pool)
        c = static_cast<Chunk*>(m_pool->allocate(sizeof(Chunk)));
    size_t capMark = 0;
    if (!c) {
        c = static_cast<Chunk*>(malloc(sizeof(Chunk)));
        if (!c)
            return false;
        capMark = 1;
    }

    c->next = nullptr;
    c->len = len;
    c->off = alreadySent;
    c->cap = capMark;
    c->kind = File;
    c->file.fd = fd;
    c->file.offset = offset;
    append(c);
    m_len += len - alreadySent;
    return true;
}

void SendQueue::pop_front() {
    Chunk* c = m_head;
    if (!c)
        return;

    m_len -= c->avail();
    m_head = c->next;
    if (!m_head)
        m_tail = nullptr;
    m_count--;
    freeChunk(c);
}

void SendQueue::consume(size_t n) {
    while (n > 0 && m_head) {
        Chunk* c = m_head;
        size_t avail = c->avail();
        if (n >= avail) {
            n -= avail;
            pop_front();
        } else {
            c->off += n;
            m_len -= n;
            n = 0;
        }
    }
}

void SendQueue::clear() {
    while (m_head)
        pop_front();
}

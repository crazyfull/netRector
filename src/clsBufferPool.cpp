#include "clsBufferPool.h"
#include "tlsf.h"
#include <new>

BufferPool::BufferPool(size_t pool_size) {
    if (pool_size < 4096)
        pool_size = 4096;

    pool_ = malloc(pool_size);
    if (!pool_)
        throw std::bad_alloc();

    m_tlsf = tlsf_create_with_pool(pool_, pool_size);
    if (!m_tlsf) {
        free(pool_);
        pool_ = nullptr;
        throw std::bad_alloc();
    }
}

BufferPool::~BufferPool() {
    if (m_tlsf)
        tlsf_destroy(m_tlsf);
    free(pool_);
}

void *BufferPool::allocate(size_t size) {
    return tlsf_malloc(m_tlsf, size);
}

void *BufferPool::reallocate(void *ptr, size_t size) {
    return tlsf_realloc(m_tlsf, ptr, size);
}

void BufferPool::deallocate(void *ptr) {
    if (ptr)
        tlsf_free(m_tlsf, ptr);
}

#ifndef CLSBUFFERPOOL_H
#define CLSBUFFERPOOL_H

#include <cstdlib>
#include <tlsf.h>
#include <cstddef>

class BufferPool {
public:
    BufferPool(size_t pool_size = 1024 * 1024);
    ~BufferPool();
    void* allocate(size_t size);
    void* reallocate(void* ptr, size_t size);
    void deallocate(void* ptr);

private:
    void* pool_ = nullptr;
    tlsf_t m_tlsf = nullptr;
};

#endif // CLSBUFFERPOOL_H

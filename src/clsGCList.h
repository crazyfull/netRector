#ifndef CLSGCLIST_H
#define CLSGCLIST_H
#include <cstdio>
#include <deque>
#include <cstdint>
#include <time.h>

#ifndef LIKELY
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif


// List hazf ba takhir. Object ha hargez vasate callback delete nemishan;
// reactor dar entehaye har dor az halghe event (noghte amn) flush ro seda mizane.
// is not thread-safe: faghat az thread reactor.
template <typename T>
class GCList {
    struct GCNode {
        T*        ptr;
        uint32_t  createdTime;  // sanie (monotonic)
    };

    std::deque<GCNode> m_qList;

public:
    GCList()                     = default;
    GCList(const GCList&)        = delete;
    GCList& operator=(GCList&&)  = delete;
    GCList(GCList&&)             = delete;
    GCList& operator=(const GCList&) = delete;

    ~GCList() noexcept {
        flush_all();
    }

    bool empty() const noexcept {
        return m_qList.empty();
    }

    size_t size() const noexcept {
        return m_qList.size();
    }

    // add object
    void retire(T* p) noexcept {
        m_qList.push_back(GCNode{p, now_s_coarse()});
    }

    // hazf object haiy ke bishtar az maxAgeSec dar list bodan
    void flush(uint32_t maxAgeSec) noexcept {
        if (m_qList.empty())
            return;

        uint32_t now = now_s_coarse();
        while (!m_qList.empty()) {
            GCNode& node = m_qList.front();
            if ((now - node.createdTime) < maxAgeSec)
                break;  // list be tartib zaman hast

            T* p = node.ptr;
            m_qList.pop_front();
            delete p;
        }
    }

    void flush_all() noexcept {
        while (!m_qList.empty()) {
            T* p = m_qList.front().ptr;
            m_qList.pop_front();
            delete p;
        }
    }

private:
    static uint32_t now_s_coarse() noexcept {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        return static_cast<uint32_t>(ts.tv_sec); // get secound
    }
};



#endif // CLSGCLIST_H

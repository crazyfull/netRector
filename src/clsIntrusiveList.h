#ifndef CLSINTRUSIVELIST_H
#define CLSINTRUSIVELIST_H

#include <functional>
#include <cstddef> // for offsetof


struct IntrusiveLink {
    IntrusiveLink *next {nullptr};
    IntrusiveLink *prev {nullptr};
};

template <typename T, IntrusiveLink T::* LinkMember>
class IntrusiveList {
private:
    IntrusiveLink m_head;
    size_t m_count {0};


    static T* getObjectFromLink(IntrusiveLink* link) {
        const size_t offset = (size_t) &(((T*)nullptr)->*LinkMember);
        return (T*)((char*)link - offset);
    }

public:
    IntrusiveList() {
        m_head.next = &m_head;
        m_head.prev = &m_head;
    }


    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    size_t size() const { return m_count; }

    /**
     * @brief اضافه کردن شیء به انتهای لیست. O(1) و بدون تخصیص حافظه
     */
    void push_back(T* obj) {
        IntrusiveLink* link = &(obj->*LinkMember);

        if (link->next != nullptr)
            return;

        link->prev = m_head.prev;
        link->next = &m_head;
        m_head.prev->next = link;
        m_head.prev = link;
        m_count++;
    }

    void remove(T* obj) {
        IntrusiveLink* link = &(obj->*LinkMember);

        //
        if (link->next == nullptr) return;

        link->prev->next = link->next;
        link->next->prev = link->prev;

        //
        link->next = nullptr;
        link->prev = nullptr;
        m_count--;
    }


    void for_each(std::function<void(T*)> callback) {
        IntrusiveLink* current = m_head.next;
        while (current != &m_head) {
            T* obj = getObjectFromLink(current);
            //
            current = current->next;
            callback(obj);
        }
    }
};

#endif // CLSINTRUSIVELIST_H

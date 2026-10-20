#ifndef PLATFORM_MSGQUEUE_H_
#define PLATFORM_MSGQUEUE_H_

#include <platform/sync.h>
#include <cstdint>
#include <deque>

namespace pf
{

struct Message
{
    uint32_t type;
    uint32_t code;
    uint32_t data1;
    uint32_t data2;
    uint32_t window;
};

// Thread-safe FIFO with blocking wait.
class MessageQueue
{
public:
    static const uint32_t Infinite = 0xffffffffu;

    MessageQueue();

    void push(const Message& message);
    bool wait(Message& out, uint32_t timeoutMs); // false on timeout
    // The first message, without waiting; it stays queued unless remove is set. False when the queue is empty.
    bool peek(Message& out, bool remove);

private:
    Mutex               m_mutex;
    CondVar             m_cond;
    std::deque<Message> m_queue;
};

}

#endif

#ifndef LIB_MUTEX_H_
#define LIB_MUTEX_H_

#include <lib/winapp.h>
#include <platform/sync.h>
#include <memory>

namespace win32
{

// Recursive mutex: guest CRITICAL_SECTION / mutex HANDLE, and the host's own locks. Lightweight unless the kernel
// mutex's hand-off on unlock matters: Kernel passes it to the first waiter (FIFO), KernelPriority to the waiter with
// the highest thread priority (the execution lock, WinApplication::yieldContext).
class Mutex : public GenericResource
{
public:
    enum Kind
    {
        Lightweight,
        Kernel,
        KernelPriority
    };

    explicit Mutex(const char* name, Kind kind = Lightweight);
    ~Mutex();

    void lock();
    void unlock();
    bool tryLock();

    x86::reg32 getResourceIndex() const { return m_resourceIndex; }

private:
    std::unique_ptr<pf::Mutex>      m_kernel;
    std::unique_ptr<pf::LwMutex>    m_light;
};

}

#endif

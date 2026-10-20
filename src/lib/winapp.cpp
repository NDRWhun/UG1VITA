#include <lib/winapp.h>
#include <lib/foreigncode.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/library.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/thread.h>
#include <lib/timer.h>
#include <winapi/types.h>
#include <x86.h>

#include <winapi/com.h>
#include <winapi/ddraw.h>
#include <winapi/kernel32.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace win32
{

static x86::reg32 s_resourceIndex;
static std::atomic<int> s_resourceCount{0};

// Words of the guest thread block (fs) used by the host; guest code only uses fs:[0].
static const x86::reg32 ThreadLockOffset     = teb::HostLock;
static const x86::reg32 ThreadControlOffset  = teb::HostControl;

// How long a thread of a raised level waits awake for the execution lock before it blocks, and how long a thread
// that yielded it waits for such a thread to take it before queueing again.
static const uint64_t PreemptSpinUs = 500;

// Threads waiting for the execution lock per preemption level above normal (m_preemptWaiters is their sum).
static std::atomic<int> s_levelWaiters[scheduler::LevelCount];

// Thread blocks and guest stacks of returned runThread calls, reused: timer callbacks run about 125 times a second,
// and finding 1 MB in the guest memory map takes a long first-fit search.
struct ThreadMemory
{
    MemMap* block;
    MemMap* stack;
};
static std::vector<ThreadMemory> s_threadMemory;
static pf::LwMutex s_threadMemoryLock("thread_memory");

// Holds one thread block and stack for the duration of a runThread call.
class ThreadMemoryLease
{
public:
    ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        if (s_threadMemory.empty())
        {
            m_memory = ThreadMemory{ new MemMap(teb::Size), new MemMap(1024 * 1024) };
        }
        else
        {
            m_memory = s_threadMemory.back();
            s_threadMemory.pop_back();
        }
    }

    ~ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        s_threadMemory.push_back(m_memory);
    }

    const MemMap& block() const { return *m_memory.block; }
    const MemMap& stack() const { return *m_memory.stack; }

private:
    ThreadMemoryLease(const ThreadMemoryLease&) = delete;
    ThreadMemoryLease& operator=(const ThreadMemoryLease&) = delete;
    ThreadMemory m_memory;
};

static void freeThreadMemory()
{
    pf::LwLockGuard guard(s_threadMemoryLock);
    for (const ThreadMemory& memory : s_threadMemory)
    {
        delete memory.stack;
        delete memory.block;
    }
    s_threadMemory.clear();
}

// The method table doubles while methods register; about 7,800 methods end in 16,384 slots.
static const x86::reg32 s_initialMethodSlotBits = 10;

static ForeignCodeResolver s_foreignCodeResolver = nullptr;

void setForeignCodeResolver(ForeignCodeResolver resolver)
{
    s_foreignCodeResolver = resolver;
}

[[noreturn]] static void missingMethod(x86::reg32 address)
{
    pf::logError("dynamic_call: no method registered at guest address 0x%08x", address);
    __builtin_trap();
}

// Empty slots hold address 0, so only a call to guest address 0 reaches this.
static void callAddressZero(WinApplication* application, x86::CPU& cpu)
{
    RC_USE(application);
    RC_USE(cpu);
    missingMethod(0);
}

GenericResource::GenericResource()
    :   m_resourceIndex(0)
{
    s_resourceCount.fetch_add(1);
}

GenericResource::~GenericResource()
{
    RC_ASSERT(m_resourceIndex == 0);
    s_resourceCount.fetch_sub(1);
}

void GenericResource::setResourceIndex(x86::reg32 resourceIndex)
{
    RC_ASSERT(!resourceIndex || !m_resourceIndex);
    m_resourceIndex = resourceIndex;
}

x86::reg32 GenericResource::getResourceIndex() const
{
    return m_resourceIndex;
}


WinApplication::WinApplication(const char* appName, x86::reg32 baseAddress, const std::vector<Section>& sections)
    :   m_appName(nullptr)
    ,   m_appNameW(nullptr)
    ,   m_env(nullptr)
    ,   m_executionContext(new Mutex("ExecutionContext", Mutex::KernelPriority))
    ,   m_preemptWaiters(0)
    ,   m_contextDepth(0)
    ,   m_resourceContext(new Mutex("Resource"))
    ,   m_methodSlots((x86::reg32(1) << s_initialMethodSlotBits) + 1, MethodSlot{0, &callAddressZero})
    ,   m_methodShift(32 - s_initialMethodSlotBits)
    ,   m_cpu{}
{
    MemMap::init(baseAddress, sections);
    m_appName = new MemMap(4096);
    m_appNameW = new MemMap(4096);
    m_env = new MemMap(4096);
    ddraw::s_dDrawRegistry->registerSymbols(this);
    kernel32::registerModules(this);
    scheduler::configureGameThreads();
    Event::init();
    Timer::init();
    com::InitialiseComMemory(this);
    strcpy(&getMemory<char>(m_appName->getBlockStart()), appName);
    getMemory<char>(m_env->getBlockStart()) = 0;
    WCHAR* wchar = &getMemory<WCHAR>(m_appNameW->getBlockStart());
    for (const char* appNameW = appName; *appNameW; ++wchar, ++appNameW)
    {
        *wchar = WCHAR(*appNameW);
    }
    *wchar = 0;
}

WinApplication::~WinApplication()
{
    // Timer callbacks and guest threads stop first so no guest code runs while resources are freed.
    Timer::fini();
    Thread::terminateAll();
    freeThreadMemory();
    delete m_appNameW;
    delete m_appName;
    // Highest handles first: later objects depend on earlier ones (sound buffers on their device).
    m_resourceContext->lock();
    while (!m_resources.empty())
    {
        std::map<x86::reg32, GenericResource*>::iterator it = --m_resources.end();
        GenericResource* resource = it->second;
        m_resources.erase(it);
        resource->setResourceIndex(0);
        delete resource;
    }
    m_resourceContext->unlock();
    Event::fini();
    MemMap::fini();
    delete m_resourceContext;
    delete m_executionContext;
    RC_ASSERT(s_resourceCount.load() == 0);
}

void WinApplication::setEnvironment(std::initializer_list<const char*> variables)
{
    char* block = &getMemory<char>(m_env->getBlockStart());
    const char* const end = block + m_env->getBlockSize() - 1;
    for (const char* variable : variables)
    {
        const size_t length = strlen(variable) + 1;
        if (block + length >= end)
            break;
        memcpy(block, variable, length);
        block += length;
    }
    *block = 0;
}

x86::reg32 WinApplication::allocateResourceFixed(GenericResource *resource, x86::reg32 fixedHandle)
{
    RC_ASSERT((fixedHandle < 0x20000) || (fixedHandle > 0x20000 + 0x30000));
    m_resourceContext->lock();
    resource->setResourceIndex(fixedHandle);
    bool inserted = m_resources.insert(std::make_pair(fixedHandle, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return fixedHandle;
}

x86::reg32 WinApplication::allocateResource(GenericResource *resource)
{
    m_resourceContext->lock();
    x86::reg32 resourceIndex = ++s_resourceIndex + 0x20000;
    resource->setResourceIndex(resourceIndex);
    bool inserted = m_resources.insert(std::make_pair(resourceIndex, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return resourceIndex;
}

GenericResource* WinApplication::getResource(x86::reg32 resource)
{
    GenericResource* result = nullptr;
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it != m_resources.end())
    {
        result = it->second;
    }
    else
    {
        result = nullptr;
    }
    m_resourceContext->unlock();
    return result;
}

// Deleted outside the resource lock: a destructor may wait for a thread that needs it.
bool WinApplication::freeResource(x86::reg32 resource)
{
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it == m_resources.end())
    {
        m_resourceContext->unlock();
        return false;
    }
    GenericResource* object = it->second;
    m_resources.erase(it);
    m_resourceContext->unlock();
    object->setResourceIndex(0);
    delete object;
    return true;
}

void WinApplication::registerMethod(x86::reg32 pointer, Method method)
{
    RC_ASSERT(pointer != 0);
    RC_ASSERT(m_methodNames.find(pointer) == m_methodNames.end());
    m_methodNames[pointer] = std::move(method.name);
    if (m_methodNames.size() * 2 > methodSlotCount())
        growMethodTable();
    insertMethod(pointer, method.method);
}

MethodPtr WinApplication::replaceMethod(x86::reg32 pointer, MethodPtr method)
{
    const MethodPtr previous = findMethod(pointer);
    insertMethod(pointer, method);
    return previous;
}

const char* WinApplication::getMethodName(x86::reg32 address) const
{
    std::unordered_map<x86::reg32, std::string>::const_iterator it = m_methodNames.find(address);
    return it != m_methodNames.end() ? it->second.c_str() : nullptr;
}

MethodPtr WinApplication::findMethod(x86::reg32 address) const
{
    const x86::reg32 mask = methodSlotCount() - 1;
    for (x86::reg32 i = methodSlot(address); ; i = (i + 1) & mask)
    {
        const MethodSlot& slot = m_methodSlots[i];
        if (slot.address == address)
            return slot.method;
        if (slot.address == 0)
        {
            // Not cached: foreign code comes and goes with the data that holds it.
            if (const MethodPtr foreign = s_foreignCodeResolver ? s_foreignCodeResolver(address) : nullptr)
                return foreign;
            missingMethod(address);
        }
    }
}

// Stores at the first slot holding the address or empty.
void WinApplication::insertMethod(x86::reg32 address, MethodPtr method)
{
    const x86::reg32 mask = methodSlotCount() - 1;
    x86::reg32 i = methodSlot(address);
    while (m_methodSlots[i].address != 0 && m_methodSlots[i].address != address)
        i = (i + 1) & mask;
    m_methodSlots[i] = MethodSlot{address, method};
}

void WinApplication::growMethodTable()
{
    std::vector<MethodSlot> slots(methodSlotCount() * 2 + 1, MethodSlot{0, &callAddressZero});
    slots.swap(m_methodSlots);
    --m_methodShift;
    for (std::vector<MethodSlot>::const_iterator it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->address)
            insertMethod(it->address, it->method);
    }
}

int WinApplication::runThread(x86::CPU& cpu, x86::reg32 entryPoint, std::initializer_list<x86::reg32> arguments,
                              bool threadLock, bool preempts)
{
    if (!m_cpu.terminate)
    {
        const ThreadMemoryLease memory;
        // A new thread block is zeroed, as on Windows.
        const x86::reg32 block = memory.block().getBlockStart();
        const x86::reg32 stackTop = memory.stack().getBlockStart() + memory.stack().getBlockSize();
        memset(&getMemory<x86::reg8>(block), 0, teb::Size);
        cpu.init(block, entryPoint);
        getMemory<x86::reg32>(block + teb::ExceptionList) = 0xffffffff;
        getMemory<x86::reg32>(block + teb::StackBase) = stackTop;
        getMemory<x86::reg32>(block + teb::StackLimit) = memory.stack().getBlockStart();
        getMemory<x86::reg32>(block + teb::Self) = block;
        getMemory<x86::reg32>(block + teb::ProcessId) = teb::GuestProcessId;
        getMemory<x86::reg32>(block + teb::ThreadId) = Thread::currentThreadId();
        getMemory<Mutex*>(block + ThreadLockOffset) = threadLock ? m_executionContext : nullptr;
        if (preempts)
            scheduler::raiseCurrentLevel(scheduler::LevelAboveNormal);
        getMemory<ThreadControl*>(block + ThreadControlOffset) = scheduler::currentControl();
        LockContext ctx(*this, cpu);
        // A return address slot with the arguments above it, below 4 spare bytes.
        cpu.esp = stackTop - 8 - 4 * x86::reg32(arguments.size());
        x86::reg32 slot = cpu.esp + 4;
        for (x86::reg32 argument : arguments)
        {
            getMemory<x86::reg32>(slot) = argument;
            slot += 4;
        }
        getMemory<x86::reg32>(cpu.esp) = 0;
        dynamic_call(entryPoint, cpu);
        return int(cpu.eax);
    }
    else
    {
        return 0;
    }
}

// Takes the lock, counting a waiter above the normal level so the owner yields to it (allowPreemption).
static void acquire(Mutex* executionContext, scheduler::Level level, std::atomic<int>& preemptWaiters)
{
    if (level == scheduler::LevelNormal)
    {
        executionContext->lock();
        return;
    }
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    s_levelWaiters[level].fetch_add(1, std::memory_order_relaxed);
    preemptWaiters.fetch_add(1, std::memory_order_relaxed);
    // The owner gives the lock up at its next preemption point, usually within microseconds: a waiter that stays
    // awake takes it without a wake-up.
    const uint64_t spinEnd = pf::ticksUs() + PreemptSpinUs;
    bool owned = executionContext->tryLock();
    while (!owned && pf::ticksUs() < spinEnd)
        owned = executionContext->tryLock();
    if (!owned)
        executionContext->lock();
    preemptWaiters.fetch_sub(1, std::memory_order_relaxed);
    s_levelWaiters[level].fetch_sub(1, std::memory_order_relaxed);
    if (timed)
        pf::perf::preemptWait(pf::perf::now() - start);
}

// perf_log accounting of the execution lock. The holder fields change only with the lock held, the game fields only
// on the game thread.
static uint32_t s_heldSince = 0;
static bool     s_gameReleased = false;
static uint32_t s_gameReleasedAt = 0;
static int      s_gameBlock = pf::perf::BlockOther;

// A thread suspended by SuspendThread stops here, without the lock, each time it would take it.
void WinApplication::lockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    RC_ASSERT(executionContext);
    if (!executionContext)
        return;
    ThreadControl* control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
    const scheduler::Level level = scheduler::level(control);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    acquire(executionContext, level, m_preemptWaiters);
    ++m_contextDepth;
    while (scheduler::anySuspended() && m_contextDepth == 1 && scheduler::mustPark(control) && !cpu.terminate)
    {
        --m_contextDepth;
        executionContext->unlock();
        scheduler::park(control, cpu);
        acquire(executionContext, level, m_preemptWaiters);
        ++m_contextDepth;
    }
    if (timed && m_contextDepth == 1)
    {
        const uint32_t acquired = pf::perf::now();
        s_heldSince = acquired;
        if (s_gameReleased && scheduler::holder(control) == pf::perf::HolderGame)
        {
            s_gameReleased = false;
            pf::perf::gameBlocked(pf::perf::Block(s_gameBlock), acquired - s_gameReleasedAt, acquired - start);
        }
    }
}

void WinApplication::unlockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    RC_ASSERT(executionContext);
    if (executionContext)
    {
        if (pf::perf::enabled() && m_contextDepth == 1)
        {
            const uint32_t released = pf::perf::now();
            const ThreadControl* control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
            const pf::perf::Holder holder = scheduler::holder(control);
            pf::perf::held(holder, released - s_heldSince);
            if (holder == pf::perf::HolderGame)
            {
                s_gameReleased = true;
                s_gameReleasedAt = released;
                s_gameBlock = pf::perf::g_nextBlock;
            }
        }
        pf::perf::g_nextBlock = pf::perf::BlockOther;
        --m_contextDepth;
        executionContext->unlock();
    }
}

// Releases the execution lock and queues for it again when a thread of a higher level waits. The lock orders its
// waiters by host priority, which follows the level, so the waiting thread of the highest priority runs next, until
// it blocks or yields itself. Threads without the lock and a lock held more than once do not yield.
void WinApplication::yieldContext(const x86::CPU& cpu)
{
    if (getMemory<Mutex*>(cpu.efs + ThreadLockOffset) != m_executionContext || m_contextDepth != 1)
        return;
    const int level = scheduler::level(getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset));
    auto higherWaits = [level]()
    {
        for (int waiting = level + 1; waiting < scheduler::LevelCount; ++waiting)
        {
            if (s_levelWaiters[waiting].load(std::memory_order_relaxed) != 0)
                return true;
        }
        return false;
    };
    if (!higherWaits())
        return;
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    pf::perf::blocking(pf::perf::BlockPreempt);
    unlockContext(cpu);
    // A waiter that spins for the lock takes it now; this thread must not take it back first.
    const uint64_t spinEnd = pf::ticksUs() + PreemptSpinUs;
    while (higherWaits() && pf::ticksUs() < spinEnd)
    {
    }
    lockContext(cpu);
    if (timed)
        pf::perf::add(pf::perf::Preempted, pf::perf::now() - start);
}

void WinApplication::terminate()
{
    m_cpu.terminate = true;
}

}

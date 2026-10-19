#include <lib/thread.h>
#include <lib/event.h>
#include <lib/scheduler.h>
#include <lib/window.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <algorithm>
#include <map>
#include <memory>

namespace win32
{

static const uint32_t s_hostStackSize = 1024 * 1024;

// A parked thread re-checks its terminate flag this often.
static const uint32_t s_parkCheckMs = 100;

static std::atomic<uint32_t> s_threadCount{1};

struct ThreadControl
{
    explicit ThreadControl(x86::reg32 guestId)
        :   guestId(guestId)
        ,   level(scheduler::LevelNormal)
        ,   priority(0)
        ,   holder(pf::perf::HolderMain)
        ,   suspendCount(0)
        ,   startPending(false)
        ,   start(nullptr)
        ,   host(nullptr)
        ,   resumed("thread_resume", 0)
    {
    }

    x86::reg32          guestId;
    std::atomic<int>    level;          // scheduler::Level
    std::atomic<int>    priority;       // Windows thread priority
    pf::perf::Holder    holder;         // kind of thread, for perf_log
    std::atomic<int>    suspendCount;   // changed under s_tableMutex
    bool                startPending;   // created suspended and not started yet
    pf::Semaphore*      start;          // start semaphore of a guest thread
    pf::Thread*         host;           // host thread of a guest thread, null for other host threads
    pf::Semaphore       resumed;        // posted when the suspend count drops to 0
};

// Host thread id -> guest thread id and control. Unregistered threads report the main thread (1).
struct ThreadIdEntry
{
    int             hostId;
    x86::reg32      guestId;
    ThreadControl*  control;
};
static const int    s_maxThreadIds = 64;
static ThreadIdEntry s_threadIds[s_maxThreadIds];
static int          s_threadIdCount = 0;
// Guards the thread id table, the controls and the suspend counts.
static pf::LwMutex  s_tableMutex("thread_table");
// Controls of guest threads (CreateThread), by guest thread id; host threads' controls live as long as the process.
static std::vector<std::unique_ptr<ThreadControl>> s_guestControls;
static std::vector<std::unique_ptr<ThreadControl>> s_hostControls;
static std::atomic<int> s_suspended{0};    // controls with a suspend count above 0

struct RoleEntry
{
    x86::reg32      entryPoint;
    pf::cores::Role role;
};
// Written before guest code runs, read-only afterwards.
static std::vector<RoleEntry> s_roles;

// Under s_tableMutex.
static ThreadControl* findGuestControl(x86::reg32 guestId)
{
    for (const std::unique_ptr<ThreadControl>& control : s_guestControls)
    {
        if (control->guestId == guestId)
            return control.get();
    }
    return nullptr;
}

// Under s_tableMutex.
static ThreadIdEntry* findEntry(int hostId)
{
    for (int i = 0; i < s_threadIdCount; ++i)
    {
        if (s_threadIds[i].hostId == hostId)
            return &s_threadIds[i];
    }
    return nullptr;
}

// Under s_tableMutex.
static void registerEntry(x86::reg32 guestId, ThreadControl* control)
{
    if (s_threadIdCount < s_maxThreadIds)
        s_threadIds[s_threadIdCount++] = ThreadIdEntry{ pf::Thread::currentId(), guestId, control };
    else
        pf::logError("thread id table full");
}

static void unregisterCurrentThread()
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    if (entry)
        *entry = s_threadIds[--s_threadIdCount];
}

std::vector<Thread::Shared*> Thread::s_live;
static pf::LwMutex s_liveMutex("threads");

Thread::Shared::Shared(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags)
    :   m_refCount(1)
    ,   m_app(app)
    ,   m_entryPoint(entryPoint)
    ,   m_parameter(parameter)
    ,   m_flags(flags)
    ,   m_threadId(1 + s_threadCount.fetch_add(1))
    ,   m_priority(0)
    ,   m_stop(false)
    ,   m_finished(false)
    ,   m_semaphore(nullptr)
    ,   m_thread(nullptr)
    ,   m_cpu{}
{
}

Thread::Shared::~Shared()
{
    delete m_thread;
    delete m_semaphore;
}

// Handle of the calling thread (DuplicateHandle of GetCurrentThread): no host thread of its own.
Thread::Thread()
    :   m_shared(new Shared(nullptr, 0, 0, 0))
{
    m_shared->m_threadId = currentThreadId();
}

Thread::Thread(const Thread& other)
    :   m_shared(other.m_shared)
{
    m_shared->m_refCount.fetch_add(1);
}

Thread::Thread(WinApplication *app, x86::reg32 method, x86::reg32 parameter, x86::reg32 flags)
    :   m_shared(new Shared(app, method, parameter, flags))
{
    reapFinished();
    const bool suspended = (flags & 0x4) != 0;  // CREATE_SUSPENDED
    const pf::cores::Role role = scheduler::threadRole(method);
    m_shared->m_semaphore = new pf::Semaphore("thread_start",suspended ? 0 : 1);
    m_shared->m_thread = new pf::Thread("guest",&run, m_shared, pf::Thread::PriorityNormal, s_hostStackSize,
                                        pf::cores::mask(role));
    {
        pf::LwLockGuard guard(s_tableMutex);
        s_guestControls.emplace_back(new ThreadControl(m_shared->m_threadId));
        ThreadControl* control = s_guestControls.back().get();
        control->holder = role == pf::cores::Game ? pf::perf::HolderGame
                        : role == pf::cores::Io ? pf::perf::HolderIo : pf::perf::HolderHelper;
        control->start = m_shared->m_semaphore;
        control->host = m_shared->m_thread;
        if (suspended)
        {
            control->startPending = true;
            control->suspendCount.store(1);
            s_suspended.fetch_add(1);
        }
    }
    pf::logInfo("thread 0x%x: entry 0x%08x, %s core %d%s", m_shared->m_threadId, method, pf::cores::name(role),
                pf::cores::core(role), suspended ? ", suspended" : "");
    m_shared->m_refCount.fetch_add(1);
    {
        pf::LwLockGuard guard(s_liveMutex);
        s_live.push_back(m_shared);
    }
    m_shared->m_thread->start();
}

Thread::~Thread()
{
    release(m_shared);
    reapFinished();
}

void Thread::release(Shared* shared)
{
    if (shared->m_refCount.fetch_sub(1) != 1)
        return;
    if (shared->m_app)
    {
        pf::LwLockGuard guard(s_tableMutex);
        for (auto it = s_guestControls.begin(); it != s_guestControls.end(); ++it)
        {
            if ((*it)->guestId == shared->m_threadId)
            {
                if ((*it)->suspendCount.load() > 0)
                    s_suspended.fetch_sub(1);
                s_guestControls.erase(it);
                break;
            }
        }
    }
    delete shared;
}

// Joins a host thread taken off the live list, stopping it first if it may still run, and drops the list's
// reference.
void Thread::join(Shared* shared)
{
    if (!shared->m_finished.load())
    {
        shared->m_stop.store(true);
        shared->m_cpu.terminate = true;
        // A thread that was never resumed starts and returns at once.
        shared->m_semaphore->post();
        Event::broadcastTerminate();
        pf::logDebug("Waiting for thread 0x%x", shared->m_threadId);
        Window::wake();
    }
    shared->m_thread->join();
    {
        pf::LwLockGuard guard(s_tableMutex);
        ThreadControl* control = findGuestControl(shared->m_threadId);
        if (control)
            control->host = nullptr;
    }
    delete shared->m_thread;
    shared->m_thread = nullptr;
    release(shared);
}

// Joins the threads that have returned, so their host threads do not wait for shutdown.
void Thread::reapFinished()
{
    std::vector<Shared*> finished;
    {
        pf::LwLockGuard guard(s_liveMutex);
        for (std::vector<Shared*>::iterator it = s_live.begin(); it != s_live.end();)
        {
            if ((*it)->m_finished.load())
            {
                finished.push_back(*it);
                it = s_live.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (Shared* shared : finished)
        join(shared);
}

void Thread::terminateAll()
{
    for (;;)
    {
        std::vector<Shared*> threads;
        {
            pf::LwLockGuard guard(s_liveMutex);
            threads.swap(s_live);
        }
        if (threads.empty())
            return;
        for (Shared* shared : threads)
            join(shared);
    }
}

void Thread::resume()
{
    if (m_shared->m_semaphore)
        m_shared->m_semaphore->post();
}

// Takes effect at once: the host priority, and the preemption level the execution lock checks.
void Thread::setPriority(int priority)
{
    m_shared->m_priority.store(priority, std::memory_order_relaxed);
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(m_shared->m_threadId);
    if (!control)
        return;
    control->priority.store(priority);
    control->level.store(scheduler::levelOf(priority));
    if (control->host)
        control->host->setPriority(scheduler::hostPriorityOf(priority));
}

x86::reg32 Thread::registerHostThread()
{
    const x86::reg32 id = 1 + s_threadCount.fetch_add(1);
    pf::LwLockGuard guard(s_tableMutex);
    s_hostControls.emplace_back(new ThreadControl(id));
    registerEntry(id, s_hostControls.back().get());
    return id;
}

x86::reg32 Thread::currentThreadId()
{
    pf::LwLockGuard guard(s_tableMutex);
    const ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    return entry ? entry->guestId : 1;
}

// Whoever takes the state off the live list joins it; a thread joined elsewhere is left alone.
void Thread::terminate()
{
    bool owned = false;
    {
        pf::LwLockGuard guard(s_liveMutex);
        std::vector<Shared*>::iterator it = std::find(s_live.begin(), s_live.end(), m_shared);
        if (it != s_live.end())
        {
            s_live.erase(it);
            owned = true;
        }
    }
    if (owned)
        join(m_shared);
}

x86::reg32 Thread::threadId() const
{
    return m_shared->m_threadId;
}

void Thread::sleep(x86::reg32 milliseconds)
{
    pf::sleepMs(milliseconds);
}

int Thread::run(void* data)
{
    Shared* shared = static_cast<Shared*>(data);
    {
        pf::LwLockGuard guard(s_tableMutex);
        ThreadControl* control = findGuestControl(shared->m_threadId);
        registerEntry(shared->m_threadId, control);
        if (pf::perf::enabled() && control && control->holder != pf::perf::HolderHelper)
            pf::perf::trackThread(control->holder == pf::perf::HolderGame ? "game" : "io");
    }
    shared->m_semaphore->wait();
    int result = 0;
    if (!shared->m_stop.load())
        result = shared->m_app->runThread(shared->m_cpu, shared->m_entryPoint, { shared->m_parameter });
    scheduler::setExitCode(shared->m_threadId, x86::reg32(result));
    unregisterCurrentThread();
    shared->m_finished.store(true);
    return result;
}

namespace scheduler
{

Level levelOf(int windowsPriority)
{
    if (windowsPriority >= 15)
        return LevelTimeCritical;
    if (windowsPriority >= 2)
        return LevelHighest;
    if (windowsPriority == 1)
        return LevelAboveNormal;
    return LevelNormal;
}

// THREAD_PRIORITY_* to host priorities around pf::Thread::PriorityNormal (lower is more urgent). The audio output
// and GXM render threads (PriorityHigh) stay above every guest thread.
int hostPriorityOf(int windowsPriority)
{
    if (windowsPriority >= 15)
        return pf::Thread::PriorityNormal - 24;
    if (windowsPriority <= -15)
        return pf::Thread::PriorityNormal + 28;
    const int clamped = std::max(-2, std::min(2, windowsPriority));
    return pf::Thread::PriorityNormal - 8 * clamped;
}

void setThreadRole(x86::reg32 entryPoint, pf::cores::Role role)
{
    s_roles.push_back(RoleEntry{ entryPoint, role });
}

pf::cores::Role threadRole(x86::reg32 entryPoint)
{
    for (const RoleEntry& entry : s_roles)
    {
        if (entry.entryPoint == entryPoint)
            return entry.role;
    }
    return pf::cores::Helper;
}

ThreadControl* currentControl()
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    if (entry && entry->control)
        return entry->control;
    s_hostControls.emplace_back(new ThreadControl(entry ? entry->guestId : 1));
    if (entry)
        entry->control = s_hostControls.back().get();
    else
        registerEntry(1, s_hostControls.back().get());
    return s_hostControls.back().get();
}

Level level(const ThreadControl* control)
{
    return control ? Level(control->level.load(std::memory_order_relaxed)) : LevelNormal;
}

pf::perf::Holder holder(const ThreadControl* control)
{
    if (!control)
        return pf::perf::HolderMain;
    // Host threads at the time-critical level run timer callbacks.
    if (!control->host && control->level.load(std::memory_order_relaxed) == LevelTimeCritical)
        return pf::perf::HolderTimer;
    return control->holder;
}

void raiseCurrentLevel(Level level)
{
    ThreadControl* control = currentControl();
    int current = control->level.load();
    while (current < level && !control->level.compare_exchange_weak(current, level))
    {
    }
}

void setCurrentPriority(int windowsPriority)
{
    ThreadControl* control = currentControl();
    control->priority.store(windowsPriority);
    control->level.store(levelOf(windowsPriority));
    pf::Thread::setCurrentPriority(hostPriorityOf(windowsPriority));
}

int currentPriority()
{
    return currentControl()->priority.load();
}

int priority(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    const ThreadControl* control = findGuestControl(threadId);
    return control ? control->priority.load() : 0;
}

int suspend(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(threadId);
    if (!control)
        return -1;
    const int previous = control->suspendCount.load();
    if (previous == 0)
        s_suspended.fetch_add(1);
    control->suspendCount.store(previous + 1);
    return previous;
}

int resume(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(threadId);
    if (!control)
        return -1;
    const int previous = control->suspendCount.load();
    if (previous == 0)
        return 0;
    control->suspendCount.store(previous - 1);
    if (previous == 1)
    {
        s_suspended.fetch_sub(1);
        if (control->startPending)
        {
            control->startPending = false;
            control->start->post();
        }
        else
        {
            control->resumed.post();
        }
    }
    return previous;
}

bool anySuspended()
{
    return s_suspended.load(std::memory_order_relaxed) != 0;
}

bool mustPark(const ThreadControl* control)
{
    return control && control->suspendCount.load() > 0;
}

// Guest thread id -> exit code, under s_tableMutex. Ids are never reused.
static std::map<x86::reg32, x86::reg32> s_exitCodes;

void setExitCode(x86::reg32 threadId, x86::reg32 code)
{
    pf::LwLockGuard guard(s_tableMutex);
    s_exitCodes.insert(std::make_pair(threadId, code));
}

bool exitCode(x86::reg32 threadId, x86::reg32* code)
{
    pf::LwLockGuard guard(s_tableMutex);
    const auto it = s_exitCodes.find(threadId);
    if (it == s_exitCodes.end())
        return false;
    *code = it->second;
    return true;
}

void park(ThreadControl* control, const x86::CPU& cpu)
{
    pf::logDebug("thread 0x%x suspended", control->guestId);
    // A wake-up left from an earlier resume is harmless: the count is checked again.
    while (!cpu.terminate && control->suspendCount.load() > 0)
        control->resumed.waitFor(s_parkCheckMs);
    pf::logDebug("thread 0x%x resumed", control->guestId);
}

}

}

#include <platform/log.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pf
{

// s_logPath is read and cleared under s_logMutex, which stays until the process exits so that no logger can use a
// deleted mutex. The path is copied: callers may pass a temporary.
static char        s_logPathBuffer[256];
static const char* s_logPath = nullptr;
static SceUID      s_logMutex = -1;
static bool        s_console = false;

// Text waiting for the writer thread, under s_logMutex. The writer appends all of it with one open/write/close when
// woken by s_logSema; text that does not fit is written by the caller after what is pending.
static const unsigned PendingSize = 64 * 1024;
static char     s_pending[PendingSize];
static unsigned s_pendingLength = 0;
static SceUID   s_logSema = -1;
static SceUID   s_logThread = -1;
static bool     s_writerStop = false;

static void appendFile(const char* text, unsigned length)
{
    if (!s_logPath || !length)
        return;
    int file = open(s_logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file >= 0)
    {
        write(file, text, length);
        close(file);
    }
}

// Under s_logMutex.
static void flushPending()
{
    appendFile(s_pending, s_pendingLength);
    s_pendingLength = 0;
}

// Under s_logMutex: the text goes to the writer thread, or to the file at once when no thread runs or it does not fit.
static void queueText(const char* text, unsigned length)
{
    if (s_logThread < 0 || length > PendingSize)
    {
        flushPending();
        appendFile(text, length);
        return;
    }
    if (s_pendingLength + length > PendingSize)
        flushPending();
    memcpy(s_pending + s_pendingLength, text, length);
    s_pendingLength += length;
    sceKernelSignalSema(s_logSema, 1);
}

static int writerEntry(SceSize, void*)
{
    static char batch[PendingSize];
    for (;;)
    {
        sceKernelWaitSema(s_logSema, 1, nullptr);
        sceKernelLockMutex(s_logMutex, 1, nullptr);
        const bool stop = s_writerStop;
        const unsigned length = s_pendingLength;
        memcpy(batch, s_pending, length);
        s_pendingLength = 0;
        sceKernelUnlockMutex(s_logMutex, 1);
        appendFile(batch, length);
        if (stop)
            return 0;
    }
}

static void writeLine(const char* level, const char* fmt, va_list args, bool now = false)
{
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof(buffer), "[%s] ", level);
    int len = vsnprintf(buffer + prefix, sizeof(buffer) - prefix - 1, fmt, args);
    if (len < 0)
        return;
    len += prefix;
    if (len > int(sizeof(buffer)) - 2)
        len = int(sizeof(buffer)) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;

    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    if (s_console)
        sceClibPrintf("%s", buffer);
    if (now)
    {
        flushPending();
        appendFile(buffer, unsigned(len));
    }
    else
    {
        queueText(buffer, unsigned(len));
    }
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logInit(const char* logFilePath)
{
    s_logMutex = sceKernelCreateMutex("log",0, 0, nullptr);
    if (logFilePath)
    {
        snprintf(s_logPathBuffer, sizeof(s_logPathBuffer), "%s", logFilePath);
        s_logPath = s_logPathBuffer;
    }
    if (s_logPath)
    {
        int file = open(s_logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (file >= 0)
            close(file);
    }
    s_logSema = sceKernelCreateSema("log", 0, 0, 0x7fffffff, nullptr);
    const SceUID thread = s_logSema >= 0 ? sceKernelCreateThread("log", &writerEntry, 160, 16 * 1024, 0,
                                                                 0x00040000, nullptr)
                                         : -1;
    if (thread >= 0 && sceKernelStartThread(thread, 0, nullptr) >= 0)
        s_logThread = thread;
}

void logSetConsole(bool enabled)
{
    s_console = enabled;
}

void logShutdown()
{
    if (s_logThread >= 0)
    {
        sceKernelLockMutex(s_logMutex, 1, nullptr);
        s_writerStop = true;
        sceKernelSignalSema(s_logSema, 1);
        sceKernelUnlockMutex(s_logMutex, 1);
        sceKernelWaitThreadEnd(s_logThread, nullptr, nullptr);
        sceKernelDeleteThread(s_logThread);
        s_logThread = -1;
    }
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    flushPending();
    s_logPath = nullptr;
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logDebug(const char* fmt, ...)
{
#ifdef NFSU_LOG_DEBUG
    va_list args;
    va_start(args, fmt);
    writeLine("debug", fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logBlock(const char* text, unsigned length)
{
    if (!length)
        return;
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    if (s_console)
        sceClibPrintf("%.*s", int(length), text);
    queueText(text, length);
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}

void fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("fatal", fmt, args, true);
    va_end(args);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}

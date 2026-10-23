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

// Each line is appended with its own open/close so nothing is lost if the process dies; lines are infrequent (no
// per-frame logging), so the cost does not matter.
static void writeLine(const char* level, const char* fmt, va_list args)
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
    if (s_logPath)
    {
        int file = open(s_logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (file >= 0)
        {
            write(file, buffer, len);
            close(file);
        }
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
}

void logSetConsole(bool enabled)
{
    s_console = enabled;
}

void logShutdown()
{
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
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
    writeLine("fatal", fmt, args);
    va_end(args);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}

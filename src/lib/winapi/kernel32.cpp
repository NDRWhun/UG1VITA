#include <winapi/kernel32.h>
#include <x86.h>
#include <lib/memmap.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/file.h>
#include <lib/heap.h>
#include <lib/library.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/timer.h>
#include <lib/window.h>
#include <lib/thread.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/time.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace win32 { namespace kernel32
{

struct OSVERSIONINFOA
{
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR  szCSDVersion[128];
};

struct OSVERSIONINFOEXA
{
    OSVERSIONINFOA versionInfoA;
    WORD  wServicePackMajor;
    WORD  wServicePackMinor;
    WORD  wSuiteMask;
    BYTE  wProductType;
    BYTE  wReserved;
};

// Pseudo handles of the calling process and thread; they need no closing.
static const HANDLE CurrentProcessHandle = 0xffffffff;
static const HANDLE CurrentThreadHandle = 0xfffffffe;

static const DWORD WAIT_FAILED = 0xffffffff;
static const DWORD MEM_COMMIT = 0x1000;
static const DWORD MEM_RELEASE = 0x8000;
static const DWORD HEAP_ZERO_MEMORY = 0x8;
static const DWORD GMEM_ZEROINIT = 0x40;

// Path of the executable and the drive type of each drive letter (set by the port before guest code runs).
static std::string s_modulePath = "C:\\GAME.EXE";

void setModulePath(const char* path)
{
    s_modulePath = path;
}

// Field of the calling thread's block (TEB).
static x86::reg32& threadField(WinApplication* app, const x86::CPU& cpu, x86::reg32 offset)
{
    return *reinterpret_cast<x86::reg32*>(&app->getMemory<x86::reg8>(cpu.efs + offset));
}

// Copies text with its terminator if it fits in size characters; false (nothing copied) otherwise.
static bool copyString(char* buffer, DWORD size, const char* text)
{
    const size_t length = strlen(text);
    if (!buffer || length >= size)
        return false;
    memcpy(buffer, text, length + 1);
    return true;
}

// Win32 convention for returned paths: the length without the terminator when the text fits, else the size
// needed with it (nothing copied).
static DWORD returnPath(char* buffer, DWORD size, const std::string& path)
{
    if (!copyString(buffer, size, path.c_str()))
        return DWORD(path.size() + 1);
    return DWORD(path.size());
}

BOOL CloseHandle(WinApplication* app, x86::CPU& cpu,
                 HANDLE hObject)
{
    RC_USE(cpu);
    if (!hObject || hObject == CurrentProcessHandle || hObject == CurrentThreadHandle)
        return 1;
    return app->freeResource(hObject) ? 1 : 0;
}

LONG CompareFileTime(WinApplication* app, x86::CPU& cpu,
                     const FILETIME *lpFileTime1, const FILETIME *lpFileTime2)
{
    RC_USE(app);
    RC_USE(cpu);
    if (lpFileTime1->dwHighDateTime < lpFileTime2->dwHighDateTime)
    {
        return -1;
    }
    else if (lpFileTime1->dwHighDateTime == lpFileTime2->dwHighDateTime)
    {
        if (lpFileTime1->dwLowDateTime < lpFileTime2->dwLowDateTime)
        {
            return -1;
        }
        else if (lpFileTime1->dwLowDateTime == lpFileTime2->dwLowDateTime)
        {
            return 0;
        }
        else
        {
            return 1;
        }
    }
    else
    {
        return 1;
    }
}

BOOL CreateDirectoryA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpSecurityAttributes);
    const std::string host = File::hostPath(lpPathName);
    if (mkdir(host.c_str(), 0777) != 0)
    {
        pf::logError("CreateDirectoryA %s failed (%s)", lpPathName, host.c_str());
        return 0;
    }
    return 1;
}

HANDLE CreateEventA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
    RC_USE(cpu);
    RC_USE(lpEventAttributes);
    RC_USE(lpName);
    win32::Event* event = new win32::Event(bManualReset ? win32::Event::ManualReset : win32::Event::AutoReset,
                                           bInitialState != 0);
    const HANDLE handle = app->allocateResource(event);
    event->registerHandle(handle);
    return handle;
}

// Names only identify the object to other processes; each call makes a new semaphore.
HANDLE CreateSemaphoreA(WinApplication* app, x86::CPU& cpu,
                        LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                        LPCSTR lpName)
{
    RC_USE(cpu);
    RC_USE(lpSemaphoreAttributes);
    RC_USE(lpName);
    if (lMaximumCount <= 0 || lInitialCount < 0 || lInitialCount > lMaximumCount)
        return 0;
    win32::Event* semaphore = new win32::Event(win32::Event::Semaphore, false, x86::reg32(lInitialCount),
                                               x86::reg32(lMaximumCount));
    const HANDLE handle = app->allocateResource(semaphore);
    semaphore->registerHandle(handle);
    return handle;
}
HANDLE CreateFileA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    RC_USE(cpu);
    RC_USE(dwShareMode);
    RC_USE(lpSecurityAttributes);
    RC_USE(hTemplateFile);
    win32::File* file = new win32::File(lpFileName, dwDesiredAccess, dwFlagsAndAttributes, dwCreationDisposition);
    if (*file)
    {
        return app->allocateResource(file);
    }
    else
    {
        delete file;
        return HANDLE(-1);
    }
}

HANDLE CreateFileMappingA(WinApplication* app, x86::CPU& cpu,
                          HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                          DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
                          LPCSTR lpName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpFileMappingAttributes);
    RC_USE(dwMaximumSizeHigh);
    RC_USE(dwMaximumSizeLow);
    RC_USE(lpName);
    RC_ASSERT(flProtect & 0x2);
    const File* source = dynamic_cast<File*>(app->getResource(hFile));
    if (!source)
        return 0;
    File* file = new File(*source);
    if (!*file)
    {
        delete file;
        return 0;
    }
    return app->allocateResource(file);
}

// A mutex handle is a waitable object owned by the guest thread that acquired it (Event of kind Mutex).
HANDLE CreateMutexA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName)
{
    RC_USE(lpMutexAttributes);
    RC_USE(lpName);
    win32::Event* mutex = new win32::Event(win32::Event::Mutex, false);
    const HANDLE handle = app->allocateResource(mutex);
    mutex->registerHandle(handle);
    // New, so this never blocks.
    if (bInitialOwner)
        win32::Event::wait(cpu, &handle, 1, false, 0, threadField(app, cpu, teb::ThreadId));
    return handle;
}
HANDLE CreateThread(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, x86::reg32 lpParameter, DWORD dwCreationFlags,
                    LPDWORD lpThreadId)
{
    RC_USE(cpu);
    RC_USE(lpThreadAttributes);
    RC_ASSERT(dwStackSize <= 1024*1024);
    // Starts at once unless CREATE_SUSPENDED (then ResumeThread starts it).
    win32::Thread* thread = new win32::Thread(app, lpStartAddress, lpParameter, dwCreationFlags);
    if (lpThreadId) *lpThreadId = thread->threadId();
    return app->allocateResource(thread);
}

void DebugBreak(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    x86::guestTrap(cpu.ip, "DebugBreak");
}
void DeleteCriticalSection(WinApplication* app, x86::CPU& cpu,
                           LPCRITICAL_SECTION lpCriticalSection)
{
    RC_USE(app);
    RC_USE(cpu);
    app->freeResource((*lpCriticalSection)->getResourceIndex());
}

BOOL DeleteFileA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    return File::remove(lpFileName);
}

BOOL DuplicateHandle(WinApplication* app, x86::CPU& cpu,
                     HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                     LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions)
{
    RC_USE(cpu);
    RC_USE(hSourceProcessHandle);
    RC_USE(hTargetProcessHandle);
    RC_USE(dwDesiredAccess);
    RC_USE(bInheritHandle);
    RC_USE(dwOptions);
    if (!lpTargetHandle)
        return 0;
    // Only thread handles are duplicated; a handle of the current thread carries no state.
    if (hSourceHandle == CurrentThreadHandle)
    {
        *lpTargetHandle = app->allocateResource(new win32::Thread());
        return 1;
    }
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hSourceHandle));
    if (!thread)
    {
        pf::logError("DuplicateHandle: 0x%x is not a thread handle", hSourceHandle);
        return 0;
    }
    *lpTargetHandle = app->allocateResource(thread->duplicate());
    return 1;
}

void EnterCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection)
{
    // Blocks only with the execution lock released: the owner may need that lock to leave the section.
    if (!(*lpCriticalSection)->tryLock())
    {
        pf::perf::blocking(pf::perf::BlockCritSec);
        app->unlockContext(cpu);
        (*lpCriticalSection)->lock();
        app->lockContext(cpu);
    }
}

BOOL EscapeCommFunction(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, DWORD dwFunc)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(dwFunc);
    RC_ASSERT(false);
    return 0;
}

void ExitProcess(WinApplication* app, x86::CPU& cpu,
                 UINT uExitCode)
{
    RC_USE(app);
    RC_USE(uExitCode);
    win32::Window::postMessage(0, 0x0012, 0, 0);
    cpu.terminate = true;
}

void ExitThread(WinApplication* app, x86::CPU& cpu,
                DWORD dwExitCode)
{
    RC_USE(app);
    RC_USE(dwExitCode);
    cpu.terminate = true;
}

BOOL FindClose(WinApplication* app, x86::CPU& cpu,
               HANDLE hFindFile)
{
    RC_USE(app);
    RC_USE(cpu);
    return app->freeResource(hFindFile) ? 1 : 0;
}

HANDLE FindFirstFileA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    RC_USE(cpu);
    win32::FileEnumerator* enumerator = new win32::FileEnumerator(lpFileName);
    if (enumerator->findNext(lpFindFileData))
    {
        return app->allocateResource(enumerator);
    }
    else
    {
        delete enumerator;
        return -1;
    }
}

BOOL FindNextFileA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    RC_USE(cpu);
    FileEnumerator* enumerator = dynamic_cast<FileEnumerator*>(app->getResource(hFindFile));
    return enumerator && enumerator->findNext(lpFindFileData);
}

// Writes go straight to the file system.
BOOL FlushFileBuffers(WinApplication* app, x86::CPU& cpu,
                      HANDLE hFile)
{
    RC_USE(cpu);
    return dynamic_cast<win32::File*>(app->getResource(hFile)) ? 1 : 0;
}
BOOL FreeEnvironmentStringsA(WinApplication* app, x86::CPU& cpu,
                             LPSTR lpszEnvironmentBlock)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpszEnvironmentBlock
    );
    return 1;
}

BOOL FreeLibrary(WinApplication* app, x86::CPU& cpu,
                 HMODULE hLibModule)
{
    RC_USE(cpu);
    return app->freeResource(hLibModule) ? 1 : 0;
}

// Windows-1252.
UINT GetACP(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1252;
}
BOOL GetCPInfo(WinApplication* app, x86::CPU& cpu,
               UINT CodePage, LPCPINFO lpCPInfo)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(CodePage);
    lpCPInfo->MaxCharSize = 1;
    lpCPInfo->DefaultChar[0] = '?';
    lpCPInfo->DefaultChar[1] = 0;
    lpCPInfo->LeadByte[0] = 0;
    lpCPInfo->LeadByte[1] = 0;
    lpCPInfo->LeadByte[2] = 0;
    lpCPInfo->LeadByte[3] = 0;
    lpCPInfo->LeadByte[4] = 0;
    lpCPInfo->LeadByte[5] = 0;
    lpCPInfo->LeadByte[6] = 0;
    lpCPInfo->LeadByte[7] = 0;
    lpCPInfo->LeadByte[8] = 0;
    lpCPInfo->LeadByte[9] = 0;
    lpCPInfo->LeadByte[10] = 0;
    lpCPInfo->LeadByte[11] = 0;
    return 1;
}

BOOL GetCommModemStatus(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, LPDWORD lpModemStat)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpModemStat);
    RC_ASSERT(false);
    return 0;
}

BOOL GetCommProperties(WinApplication* app, x86::CPU& cpu,
                       HANDLE hFile, LPCOMMPROP lpCommProp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpCommProp);
    RC_ASSERT(false);
    return 0;
}

BOOL GetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpDCB);
    RC_ASSERT(false);
    return 0;
}

x86::reg32 GetCommandLineA(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(cpu);
    return app->getAppName();
}

x86::reg32 GetCommandLineW(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(cpu);
    return app->getAppNameW();
}
BOOL GetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, LPDWORD lpMode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hConsoleHandle);
    *lpMode = 0;
    return 1;
}

DWORD GetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                           DWORD nBufferLength, LPSTR lpBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    return returnPath(lpBuffer, nBufferLength, File::getCurrentDirectory());
}

HANDLE GetCurrentProcess(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return CurrentProcessHandle;
}

DWORD GetCurrentProcessId(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return teb::GuestProcessId;
}
DWORD GetCurrentThreadId(WinApplication* app, x86::CPU& cpu)
{
    return threadField(app, cpu, teb::ThreadId);
}
HANDLE GetCurrentThread(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return CurrentThreadHandle;
}

BOOL GetDiskFreeSpaceA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
                       LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpRootPathName);
    if (lpSectorsPerCluster) *lpSectorsPerCluster = 1;
    if (lpBytesPerSector) *lpBytesPerSector = 512;
    if (lpNumberOfFreeClusters) *lpNumberOfFreeClusters = 512*1024;
    if (lpTotalNumberOfClusters) *lpTotalNumberOfClusters = 1024*1024;
    return 1;
}

// Every drive letter is the data folder: a fixed disk. Without a root it is the current drive.
UINT GetDriveTypeA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpRootPathName)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpRootPathName)
        return 3;   // DRIVE_FIXED
    const char letter = char(toupper(x86::reg8(lpRootPathName[0])));
    return letter >= 'A' && letter <= 'Z' && lpRootPathName[1] == ':' ? 3 : 1;   // DRIVE_NO_ROOT_DIR
}
x86::reg32 GetEnvironmentStrings(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return app->getEnv();
}

DWORD GetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    struct stat status;
    if (stat(File::hostPath(lpFileName).c_str(), &status) != 0)
        return 0xffffffff;
    // FILE_ATTRIBUTE_DIRECTORY or FILE_ATTRIBUTE_NORMAL.
    return S_ISDIR(status.st_mode) ? 0x10 : 0x80;
}

// File times are not kept: every time reads as zero.
BOOL GetFileTime(WinApplication* app, x86::CPU& cpu,
                 HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
{
    RC_USE(cpu);
    if (!dynamic_cast<win32::File*>(app->getResource(hFile)))
        return 0;
    for (LPFILETIME time : { lpCreationTime, lpLastAccessTime, lpLastWriteTime })
    {
        if (time)
        {
            time->dwLowDateTime = 0;
            time->dwHighDateTime = 0;
        }
    }
    return 1;
}
DWORD GetFileSize(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDWORD lpFileSizeHigh)
{
    RC_USE(cpu);
    win32::File* file = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!file)
        return 0xffffffff;
    if (lpFileSizeHigh)
        *lpFileSizeHigh = 0;
    return file->size();
}

DWORD GetFileType(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile)
{
    RC_USE(cpu);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    return f ? 1 : 0;
}

DWORD GetFullPathNameA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, x86::reg32* lpFilePart)
{
    if (!lpFileName)
        return 0;
    const x86::reg32 lpBufferAddress = app->getMemory<x86::reg32>(cpu.esp + 12);
    // Absolute names pass through; relative names are rooted in the guest current directory.
    std::string path;
    if (strlen(lpFileName) >= 2 && lpFileName[1] == ':')
    {
        path = lpFileName;
    }
    else
    {
        path = File::getCurrentDirectory();
        if (path.back() != '\\')
            path += '\\';
        path += lpFileName;
    }
    const DWORD result = returnPath(lpBuffer, nBufferLength, path);
    if (result < nBufferLength && lpFilePart)
    {
        const size_t lastSeparator = path.rfind('\\');
        *lpFilePart = lpBufferAddress + x86::reg32(lastSeparator == std::string::npos ? 0 : lastSeparator + 1);
    }
    return result;
}

DWORD GetLastError(WinApplication* app, x86::CPU& cpu)
{
    return threadField(app, cpu, teb::LastError);
}
void GetLocalTime(WinApplication* app, x86::CPU& cpu,
                  LPSYSTEMTIME lpSystemTime)
{
    RC_USE(app);
    RC_USE(cpu);
    win32::getSystemTime(lpSystemTime);
}

DWORD GetLogicalDriveStringsA(WinApplication* app, x86::CPU& cpu,
                              DWORD nBufferLength, LPSTR lpBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (nBufferLength)
    {
        lpBuffer[0] = 'C';
        lpBuffer[1] = 'D';
        lpBuffer[2] = 0;
    }
    return 2;
}

DWORD GetLogicalDrives(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0x2 | 0x4;
}

DWORD GetModuleFileNameA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
    RC_USE(app);
    RC_USE(cpu);
    // Only the executable (handle 0, or 1 from GetModuleHandleA) has a file name.
    if (hModule > 1 || !lpFilename || nSize == 0)
        return 0;
    // A buffer too small gets nSize - 1 characters and the terminator, and the result is nSize.
    const DWORD length = DWORD(std::min<size_t>(s_modulePath.size(), nSize - 1));
    memcpy(lpFilename, s_modulePath.c_str(), length);
    lpFilename[length] = 0;
    return length < s_modulePath.size() ? nSize : length;
}
DWORD GetModuleFileNameW(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hModule);
    RC_USE(lpFilename);
    RC_USE(nSize);
    RC_ASSERT(false);
    return 0;
}

HMODULE GetModuleHandleA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpModuleName)
{
    RC_USE(app);
    RC_USE(cpu);
    // Only the executable has a module handle; libraries are loaded with LoadLibraryA.
    return lpModuleName ? 0 : 1;
}

UINT GetOEMCP(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 850;
}

BOOL GetOverlappedResult(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFile, LPOVERLAPPED lpOverlapped,
                         LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpOverlapped);
    RC_USE(lpNumberOfBytesTransferred);
    RC_USE(bWait);
    RC_ASSERT(false);
    return 0;
}

FARPROC GetProcAddress(WinApplication* app, x86::CPU& cpu,
                       HMODULE hModule, LPCSTR lpProcName)
{
    RC_USE(cpu);
    win32::LibraryHandle* libraryHandle = dynamic_cast<win32::LibraryHandle*>(app->getResource(hModule));
    RC_ASSERT(libraryHandle);
    const x86::reg32 address = libraryHandle->getLibrary()->find(lpProcName);
    if (!address)
        pf::logInfo("GetProcAddress: no %s", lpProcName);
    return address;
}

HANDLE GetStdHandle(WinApplication* app, x86::CPU& cpu,
                    DWORD nStdHandle)
{
    RC_USE(cpu);
    // STD_INPUT_HANDLE (-10), STD_OUTPUT_HANDLE (-11), STD_ERROR_HANDLE (-12): one handle each, made again only
    // after the game closes it.
    static const char* const names[3] = { "stdin", "stdout", "stderr" };
    static HANDLE handles[3] = { 0, 0, 0 };
    const x86::reg32 index = x86::reg32(-10) - nStdHandle;
    if (index >= 3)
        return HANDLE(-1);
    if (!handles[index] || !dynamic_cast<win32::File*>(app->getResource(handles[index])))
        handles[index] = app->allocateResource(new win32::File(int(index), names[index]));
    return handles[index];
}

// A single Pentium (PROCESSOR_INTEL_PENTIUM); the game derives its clock measurement from the type.
void GetSystemInfo(WinApplication* app, x86::CPU& cpu,
                   LPSYSTEM_INFO lpSystemInfo)
{
    RC_USE(app);
    RC_USE(cpu);
    lpSystemInfo->wProcessorArchitecture = 0;
    lpSystemInfo->wReserved = 0;
    lpSystemInfo->dwPageSize = 4096;
    lpSystemInfo->lpMinimumApplicationAddress = 0x10000;
    lpSystemInfo->lpMaximumApplicationAddress = 0x7ffeffff;
    lpSystemInfo->dwActiveProcessorMask = 1;
    lpSystemInfo->dwNumberOfProcessors = 1;
    lpSystemInfo->dwProcessorType = 586;
    lpSystemInfo->dwAllocationGranularity = 4096;
    lpSystemInfo->wProcessorLevel = 5;
    lpSystemInfo->wProcessorRevision = 0x0503;
}
// THREAD_PRIORITY_ERROR_RETURN (MAXLONG) for a handle that is not a thread.
int GetThreadPriority(WinApplication* app, x86::CPU& cpu,
                      HANDLE hThread)
{
    RC_USE(cpu);
    if (hThread == CurrentThreadHandle)
        return scheduler::currentPriority();
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hThread));
    return thread ? scheduler::priority(thread->threadId()) : 0x7fffffff;
}

DWORD GetTickCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    DWORD result = timeGetTickCount();
    return result;
}

// UTC without daylight saving: TIME_ZONE_ID_UNKNOWN.
DWORD GetTimeZoneInformation(WinApplication* app, x86::CPU& cpu,
                             LPTIME_ZONE_INFORMATION lpTimeZoneInformation)
{
    RC_USE(app);
    RC_USE(cpu);
    if (lpTimeZoneInformation)
        memset(lpTimeZoneInformation, 0, sizeof(*lpTimeZoneInformation));
    return 0;
}
DWORD GetVersion(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0x80000304;
}

BOOL GetVersionExA(WinApplication* app, x86::CPU& cpu,
                   LPOSVERSIONINFOA lpVersionInformation)
{
    RC_USE(app);
    RC_USE(cpu);
    lpVersionInformation->dwMajorVersion = 5;
    lpVersionInformation->dwMinorVersion = 1;
    lpVersionInformation->dwBuildNumber = 2600;
    lpVersionInformation->dwPlatformId = 2;
    strcpy(lpVersionInformation->szCSDVersion, "Service Pack 3");
    if (lpVersionInformation->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
    {
        OSVERSIONINFOEXA* info = reinterpret_cast<OSVERSIONINFOEXA*>(lpVersionInformation);
        info->wProductType = 1;
        info->wServicePackMajor = 3;
        info->wServicePackMinor = 0;
    }
    return 1;
}

BOOL GetVolumeInformationA(WinApplication* app, x86::CPU& cpu,
                           LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
                           LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
                           LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer,
                           DWORD nFileSystemNameSize)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpRootPathName);
    if (lpVolumeNameBuffer && !copyString(lpVolumeNameBuffer, nVolumeNameSize, "DISK"))
        return 0;
    if (lpFileSystemNameBuffer && !copyString(lpFileSystemNameBuffer, nFileSystemNameSize, "FAT"))
        return 0;
    if (lpVolumeSerialNumber)
        *lpVolumeSerialNumber = 0x12345678;
    if (lpMaximumComponentLength)
        *lpMaximumComponentLength = 255;
    if (lpFileSystemFlags)
        *lpFileSystemFlags = 1|2|4;
    return 1;
}

UINT GetWindowsDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPSTR lpBuffer, UINT uSize)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpBuffer);
    RC_USE(uSize);
    RC_ASSERT(false);
    return 0;
}

void GlobalMemoryStatus(WinApplication* app, x86::CPU& cpu,
                        LPMEMORYSTATUS lpBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    lpBuffer->dwLength = sizeof(MEMORYSTATUS);
    lpBuffer->dwMemoryLoad = 0;
    lpBuffer->dwTotalPhys = 268435456;
    lpBuffer->dwAvailPhys = 268435456;
    lpBuffer->dwTotalPageFile = 268435456;
    lpBuffer->dwAvailPageFile = 268435456;
    lpBuffer->dwTotalVirtual = 0x7fffffff;
    lpBuffer->dwAvailVirtual = 0x7fffffff;
}

void InitializeCriticalSection(WinApplication* app, x86::CPU& cpu,
                               LPCRITICAL_SECTION lpCriticalSection)
{
    RC_USE(app);
    RC_USE(cpu);
    *lpCriticalSection = new win32::Mutex("");
    app->allocateResource(*lpCriticalSection);
}

void LeaveCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection)
{
    RC_USE(app);
    RC_USE(cpu);
    // Keeps the execution lock: a thread that was waiting for the section takes the lock at its owner's next
    // release or preemption point.
    (*lpCriticalSection)->unlock();
}

HMODULE LoadLibraryA(WinApplication* app, x86::CPU& cpu,
                     LPCSTR lpLibFileName)
{
    RC_USE(cpu);
    win32::LibraryHandle* lib = new win32::LibraryHandle(lpLibFileName);
    if (*lib)
        return app->allocateResource(lib);
    pf::logInfo("LoadLibraryA: %s is not available", lpLibFileName);
    delete lib;
    return 0;
}

x86::reg32 MapViewOfFile(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                         DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
{
    RC_USE(dwDesiredAccess);
    File* file = dynamic_cast<File*>(app->getResource(hFileMappingObject));
    if (!file || dwFileOffsetHigh != 0)
        return 0;
    // Size 0 maps from the offset to the end of the file.
    const x86::reg32 fileSize = file->size();
    if (dwNumberOfBytesToMap == 0)
        dwNumberOfBytesToMap = fileSize > dwFileOffsetLow ? fileSize - dwFileOffsetLow : 0;
    if (dwNumberOfBytesToMap == 0)
        return 0;
    pf::perf::blocking(pf::perf::BlockFile);
    app->unlockContext(cpu);
    MemMap* result = new MemMap(dwNumberOfBytesToMap);
    x86::reg8* data = &app->getMemory<x86::reg8>(result->getBlockStart());
    x86::reg32 bytesRead = 0;
    const bool loaded = file->seek(0, x86::sreg32(dwFileOffsetLow)) == dwFileOffsetLow
                     && file->read(data, dwNumberOfBytesToMap, &bytesRead);
    if (loaded)
    {
        // Past the end of the file the view reads as zeros.
        memset(data + bytesRead, 0, result->getBlockSize() - bytesRead);
    }
    app->lockContext(cpu);
    if (!loaded)
    {
        delete result;
        return 0;
    }
    return result->getBlockStart();
}

BOOL MoveFileA(WinApplication* app, x86::CPU& cpu,
               LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpExistingFileName || !lpNewFileName)
        return 0;
    const std::string from = File::hostPath(lpExistingFileName);
    const std::string to = File::hostPath(lpNewFileName);
    struct stat status;
    // MoveFile fails when the target exists.
    if (stat(to.c_str(), &status) == 0 || rename(from.c_str(), to.c_str()) != 0)
        return 0;
    return 1;
}
int MultiByteToWideChar(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr,
                        int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(CodePage);
    RC_USE(dwFlags);
    // Bytes map to the same code points. cbMultiByte -1 converts up to and with the terminator; cchWideChar 0 asks
    // for the length; a buffer too small fails.
    if (!lpMultiByteStr || cbMultiByte == 0 || cchWideChar < 0)
        return 0;
    const int length = cbMultiByte < 0 ? int(strlen(lpMultiByteStr)) + 1 : cbMultiByte;
    if (cchWideChar == 0)
        return length;
    if (!lpWideCharStr || length > cchWideChar)
        return 0;
    for (int i = 0; i < length; ++i)
        lpWideCharStr[i] = WCHAR(x86::reg8(lpMultiByteStr[i]));
    return length;
}

void OutputDebugStringA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpOutputString)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::logInfo("%s", lpOutputString);
}

BOOL PeekConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer,
                       DWORD nLength, LPDWORD lpNumberOfEventsRead)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hConsoleInput);
    RC_USE(lpBuffer);
    RC_USE(nLength);
    RC_USE(lpNumberOfEventsRead);
    *lpNumberOfEventsRead = 0;
    return 1;
}

BOOL PulseEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    RC_USE(cpu);
    Event* event = dynamic_cast<Event*>(app->getResource(hEvent));
    return event ? event->pulse() : 0;
}

BOOL PurgeComm(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 0;
}

// There is no console: no input records are read.
BOOL ReadConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer, DWORD nLength,
                       LPDWORD lpNumberOfEventsRead)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hConsoleInput);
    RC_USE(lpBuffer);
    RC_USE(nLength);
    if (lpNumberOfEventsRead)
        *lpNumberOfEventsRead = 0;
    return 0;
}
BOOL ReadFile(WinApplication* app, x86::CPU& cpu,
              HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    RC_USE(cpu);
    RC_USE(lpOverlapped);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!f)
    {
        return 0;
    }
    else
    {
        pf::perf::blocking(pf::perf::BlockFile);
        app->unlockContext(cpu);
        BOOL result = f->read(lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead);
        app->lockContext(cpu);
        return result;
    }
}

BOOL RemoveDirectoryA(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpPathName);
    RC_ASSERT(false);
    return 0;
}

BOOL ResetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    RC_USE(cpu);
    Event* event = dynamic_cast<Event*>(app->getResource(hEvent));
    if (!event)
        return 0;
    event->reset();
    return 1;
}

// Returns the previous suspend count; a count that drops to 0 starts a suspended thread or lets it go on.
DWORD ResumeThread(WinApplication* app, x86::CPU& cpu,
                   HANDLE hThread)
{
    const x86::reg32 id = hThread == CurrentThreadHandle ? threadField(app, cpu, teb::ThreadId) : 0;
    win32::Thread* thread = id ? nullptr : dynamic_cast<win32::Thread*>(app->getResource(hThread));
    if (!id && !thread)
        return 0xffffffff;
    return DWORD(scheduler::resume(id ? id : thread->threadId()));
}

// Calls the handlers of the SEH chain above TargetFrame with EXCEPTION_UNWINDING, removing each, as Windows does;
// returns to the caller (TargetIp is the address after the call in the CRT's use).
void RtlUnwind(WinApplication* app, x86::CPU& cpu,
               PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    RC_USE(TargetIp);
    RC_USE(ExceptionRecord);
    const x86::reg32 target = TargetFrame ? x86::reg32(static_cast<x86::reg8*>(TargetFrame) - g_guestBase) : 0xffffffff;
    // An exception record of its own, for the handlers to read.
    MemMap record(80);
    const x86::reg32 recordAddress = record.getBlockStart();
    memset(&app->getMemory<x86::reg8>(recordAddress), 0, 80);
    app->getMemory<x86::reg32>(recordAddress + 0) = 0xc0000027;     // STATUS_UNWIND
    app->getMemory<x86::reg32>(recordAddress + 4) = 2;              // EXCEPTION_UNWINDING
    x86::reg32& head = threadField(app, cpu, teb::ExceptionList);
    const x86::CPU saved = cpu;
    while (head != 0xffffffff && head != target)
    {
        const x86::reg32 frame = head;
        const x86::reg32 handler = app->getMemory<x86::reg32>(frame + 4);
        // handler(record, frame, context, dispatcher context), cdecl.
        cpu.esp -= 20;
        app->getMemory<x86::reg32>(cpu.esp + 4) = recordAddress;
        app->getMemory<x86::reg32>(cpu.esp + 8) = frame;
        app->getMemory<x86::reg32>(cpu.esp + 12) = 0;
        app->getMemory<x86::reg32>(cpu.esp + 16) = 0;
        cpu.ip = handler;
        app->dynamic_call(handler, cpu);
        head = app->getMemory<x86::reg32>(frame);
        cpu.esp = saved.esp;
        cpu.ebx = saved.ebx;
        cpu.esi = saved.esi;
        cpu.edi = saved.edi;
        cpu.ebp = saved.ebp;
    }
    cpu.eax = ReturnValue ? x86::reg32(static_cast<x86::reg8*>(ReturnValue) - g_guestBase) : 0;
}
BOOL SetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpDCB);
    RC_ASSERT(false);
    return 0;
}

BOOL SetCommTimeouts(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LPCOMMTIMEOUTS lpCommTimeouts)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hFile);
    RC_USE(lpCommTimeouts);
    RC_ASSERT(false);
    return 0;
}

BOOL SetConsoleCtrlHandler(WinApplication* app, x86::CPU& cpu,
                           PHANDLER_ROUTINE HandlerRoutine, BOOL Add)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(HandlerRoutine);
    RC_USE(Add);
    RC_ASSERT(false);
    return 0;
}

BOOL SetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, DWORD  dwMode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hConsoleHandle);
    RC_USE(dwMode);
    return 1;
}

BOOL SetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpPathName)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpPathName || !*lpPathName)
        return 0;
    return File::setCurrentDirectory(lpPathName) ? 1 : 0;
}

BOOL SetEndOfFile(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile)
{
    RC_USE(cpu);
    File* file = dynamic_cast<File*>(app->getResource(hFile));
    return file ? file->truncate() : 0;
}

BOOL SetEnvironmentVariableA(WinApplication* app, x86::CPU& cpu,
                             LPCSTR lpName, LPCSTR lpValue)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpName);
    RC_USE(lpValue);
    return 0;
}

BOOL SetEnvironmentVariableW(WinApplication* app, x86::CPU& cpu,
                             LPCWSTR lpName, LPCWSTR lpValue)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpName);
    RC_USE(lpValue);
    return 0;
}
UINT SetErrorMode(WinApplication* app, x86::CPU& cpu, UINT uMode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uMode);
    return 0;
}

BOOL SetEvent(WinApplication* app, x86::CPU& cpu,
              HANDLE hEvent)
{
    RC_USE(app);
    RC_USE(cpu);
    if (win32::Event::setHandle(hEvent))
        return 1;
    // Windows fails the call for a closed handle; a thread may signal one its owner just closed.
    pf::logError("SetEvent: no event with handle 0x%x", hEvent);
    return 0;
}

BOOL SetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpFileName, DWORD  dwFileAttributes)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpFileName);
    RC_USE(dwFileAttributes);
    return 0;
}

DWORD SetFilePointer(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    RC_USE(cpu);
    win32::File* file = dynamic_cast<win32::File*>(app->getResource(hFile));
    RC_ASSERT(lpDistanceToMoveHigh == 0);
    return file ? file->seek(dwMoveMethod, lDistanceToMove) : 0xffffffff;
}

void SetLastError(WinApplication* app, x86::CPU& cpu,
                  DWORD dwErrCode)
{
    threadField(app, cpu, teb::LastError) = dwErrCode;
}
BOOL SetStdHandle(WinApplication* app, x86::CPU& cpu,
                  DWORD nStdHandle, HANDLE hHandle)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(nStdHandle);
    RC_USE(hHandle);
    return 1;
}
BOOL SetThreadPriority(WinApplication* app, x86::CPU& cpu,
                       HANDLE hThread, int nPriority)
{
    RC_USE(cpu);
    if (hThread == CurrentThreadHandle)
    {
        scheduler::setCurrentPriority(nPriority);
        return 1;
    }
    win32::Thread* thread = dynamic_cast<win32::Thread*>(app->getResource(hThread));
    if (!thread)
        return 0;
    thread->setPriority(nPriority);
    return 1;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                                                         LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpTopLevelExceptionFilter);
    return 0;
}

// Without the execution lock. Long and infinite sleeps wake up in slices to see a terminating thread.
DWORD SleepEx(WinApplication* app, x86::CPU& cpu,
              DWORD dwMilliseconds, BOOL bAlertable)
{
    RC_USE(bAlertable);
    static const DWORD Slice = 100;
    static const DWORD Infinite = 0xffffffff;
    pf::perf::blocking(pf::perf::BlockSleep);
    app->unlockContext(cpu);
    if (dwMilliseconds <= Slice)
    {
        win32::Thread::sleep(dwMilliseconds);
    }
    else
    {
        const uint64_t end = pf::ticksUs() + uint64_t(dwMilliseconds) * 1000;
        for (uint64_t now = pf::ticksUs(); !cpu.terminate && (dwMilliseconds == Infinite || now < end);
             now = pf::ticksUs())
        {
            const uint64_t left = dwMilliseconds == Infinite ? Slice * 1000 : end - now;
            pf::sleepUs(uint32_t(std::min<uint64_t>(left, Slice * 1000)));
        }
    }
    app->lockContext(cpu);
    return 0;
}

BOOL TerminateThread(WinApplication* app, x86::CPU& cpu,
                     HANDLE hThread, DWORD dwExitCode)
{
    RC_USE(cpu);
    RC_USE(dwExitCode);
    // The pseudo handle of the calling thread is not a thread object: the call fails, the thread goes on.
    Thread* thread = dynamic_cast<Thread*>(app->getResource(hThread));
    if (!thread)
        return 0;
    app->unlockContext(cpu);
    thread->terminate();
    app->lockContext(cpu);
    return 1;
}

static std::atomic<uint64_t> s_tlsUsed{0};

// TLS_OUT_OF_INDEXES when all slots are taken.
DWORD TlsAlloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    uint64_t used = s_tlsUsed.load();
    for (;;)
    {
        if (used == ~uint64_t(0))
            return 0xffffffff;
        const DWORD index = DWORD(__builtin_ctzll(~used));
        if (s_tlsUsed.compare_exchange_weak(used, used | (uint64_t(1) << index)))
            return index;
    }
}
BOOL TlsFree(WinApplication* app, x86::CPU& cpu,
             DWORD dwTlsIndex)
{
    RC_USE(app);
    RC_USE(cpu);
    if (dwTlsIndex >= teb::TlsSlotCount)
        return 0;
    s_tlsUsed.fetch_and(~(uint64_t(1) << dwTlsIndex));
    return 1;
}
x86::reg32 TlsGetValue(WinApplication* app, x86::CPU& cpu,
                       DWORD dwTlsIndex)
{
    if (dwTlsIndex >= teb::TlsSlotCount)
        return 0;
    threadField(app, cpu, teb::LastError) = 0;
    return threadField(app, cpu, teb::TlsSlots + 4 * dwTlsIndex);
}
BOOL TlsSetValue(WinApplication* app, x86::CPU& cpu,
                 DWORD dwTlsIndex, LPVOID lpTlsValue)
{
    if (dwTlsIndex >= teb::TlsSlotCount)
        return 0;
    threadField(app, cpu, teb::TlsSlots + 4 * dwTlsIndex) =
        lpTlsValue ? x86::reg32(static_cast<x86::reg8*>(lpTlsValue) - g_guestBase) : 0;
    return 1;
}
// Only reached on a guest exception, which the port cannot recover from.
LONG UnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                              struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    RC_USE(app);
    RC_USE(ExceptionInfo);
    x86::guestTrap(cpu.ip, "unhandled guest exception");
}
BOOL UnmapViewOfFile(WinApplication* app, x86::CPU& cpu,
    Packed<const void> lpBaseAddress)
{
    RC_USE(app);
    RC_USE(cpu);
    MemMap* memBlock = lpBaseAddress ? MemMap::findBlock(lpBaseAddress) : nullptr;
    if (!memBlock)
        return 0;
    delete memBlock;
    return 1;
}

// Every block is reserved and committed at once: committing pages of a block returns them as they are.
x86::reg32 VirtualAlloc(WinApplication* app, x86::CPU& cpu,
                        Packed<void> lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    RC_USE(cpu);
    RC_USE(flProtect);
    if (dwSize == 0)
        return 0;
    if (lpAddress)
    {
        const MemMap* block = MemMap::findBlock(lpAddress);
        if (!(flAllocationType & MEM_COMMIT) || !block
            || x86::reg64(lpAddress) + dwSize > x86::reg64(block->getBlockStart()) + block->getBlockSize())
            return 0;
        return lpAddress & ~4095u;
    }
    MemMap* map = new MemMap(dwSize);
    memset(&app->getMemory<x86::reg8>(map->getBlockStart()), 0, dwSize);
    return map->getBlockStart();
}

BOOL VirtualFree(WinApplication* app, x86::CPU& cpu,
                 Packed<void> lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwSize);
    MemMap* memBlock = lpAddress ? MemMap::findBlock(lpAddress) : nullptr;
    if (!memBlock)
        return 0;
    // MEM_DECOMMIT keeps the block: its pages stay committed.
    if (dwFreeType == MEM_RELEASE)
        delete memBlock;
    return 1;
}

BOOL VirtualProtect(WinApplication* app, x86::CPU& cpu,
                    Packed<void> lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpAddress);
    RC_USE(dwSize);
    RC_USE(flNewProtect);
    RC_USE(lpflOldProtect);
    return 1;
}

SIZE_T VirtualQuery(WinApplication* app, x86::CPU& cpu,
                    Packed<const void> lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
{
    RC_USE(app);
    RC_USE(cpu);
    MemMap* memBlock = MemMap::findBlock(lpAddress);
    if (!memBlock || !lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION))
        return 0;
    lpBuffer->BaseAddress = memBlock->getBlockStart();
    lpBuffer->AllocationBase = memBlock->getBlockStart();
    lpBuffer->AllocationProtect = 0x04; //READWRITE
    lpBuffer->RegionSize = memBlock->getBlockSize();
    lpBuffer->State = 0x1000; //COMMIT
    lpBuffer->Protect = 0x04; //READWRITE
    lpBuffer->Type = 0x20000; //PRIVATE
    return sizeof(*lpBuffer);
}

// Events, semaphores and mutexes; any other handle fails the wait (WAIT_FAILED).
DWORD WaitForMultipleObjectsEx(WinApplication* app, x86::CPU& cpu,
                               DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll,
                               DWORD dwMilliseconds, BOOL bAlertable)
{
    RC_USE(bAlertable);
    static const DWORD WAIT_TIMEOUT = 0x102;
    const x86::reg32 thread = threadField(app, cpu, teb::ThreadId);
    // A wait that is satisfied at once keeps the execution lock: no hand-off to another thread.
    DWORD result = DWORD(win32::Event::wait(cpu, lpHandles, nCount, bWaitAll != 0, 0, thread));
    if (result != WAIT_TIMEOUT || dwMilliseconds == 0)
        return result;
    pf::perf::blocking(pf::perf::BlockWait);
    app->unlockContext(cpu);
    result = DWORD(win32::Event::wait(cpu, lpHandles, nCount, bWaitAll != 0, x86::sreg32(dwMilliseconds), thread));
    app->lockContext(cpu);
    return result;
}

DWORD WaitForMultipleObjects(WinApplication* app, x86::CPU& cpu,
                             DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    return WaitForMultipleObjectsEx(app, cpu, nCount, lpHandles, bWaitAll, dwMilliseconds, 0);
}
DWORD WaitForSingleObject(WinApplication* app, x86::CPU& cpu,
                          HANDLE hHandle, DWORD dwMilliseconds)
{
    return WaitForMultipleObjectsEx(app, cpu, 1, &hHandle, 0, dwMilliseconds, 0);
}
// Code points below 256 map to the same byte, others to the default character. cchWideChar -1 converts up to and
// with the terminator; cbMultiByte 0 asks for the length; a buffer too small fails.
int WideCharToMultiByte(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(CodePage);
    RC_USE(dwFlags);
    if (!lpWideCharStr || cchWideChar == 0 || cbMultiByte < 0)
        return 0;
    int length = cchWideChar;
    if (length < 0)
    {
        length = 0;
        while (lpWideCharStr[length])
            ++length;
        ++length;
    }
    if (lpUsedDefaultChar)
        *lpUsedDefaultChar = 0;
    if (cbMultiByte == 0)
        return length;
    if (!lpMultiByteStr || length > cbMultiByte)
        return 0;
    for (int i = 0; i < length; ++i)
    {
        const WCHAR c = lpWideCharStr[i];
        if (c < 0x100)
        {
            lpMultiByteStr[i] = char(c);
        }
        else
        {
            lpMultiByteStr[i] = lpDefaultChar ? *lpDefaultChar : '?';
            if (lpUsedDefaultChar)
                *lpUsedDefaultChar = 1;
        }
    }
    return length;
}
BOOL WriteConsoleA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hConsoleOutput, const void* lpBuffer, DWORD nNumberOfCharsToWrite,
                   LPDWORD lpNumberOfCharsWritten, LPVOID  lpReserved)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hConsoleOutput);
    RC_USE(lpBuffer);
    RC_USE(nNumberOfCharsToWrite);
    RC_USE(lpNumberOfCharsWritten);
    RC_USE(lpReserved);
    RC_ASSERT(false);
    return 0;
}

BOOL WriteFile(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    RC_USE(cpu);
    RC_ASSERT(lpOverlapped == nullptr);
    win32::File* f = dynamic_cast<win32::File*>(app->getResource(hFile));
    if (!f)
    {
        return 0;
    }
    else
    {
        pf::perf::blocking(pf::perf::BlockFile);
        app->unlockContext(cpu);
        x86::reg32 result = f->write(lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten);
        app->lockContext(cpu);
        return result;
    }
}

// ---- Private profiles (ini files) ----------------------------------------------------------------------------

namespace
{

typedef std::vector<std::pair<std::string, std::string>> ProfileSection;
typedef std::vector<std::pair<std::string, ProfileSection>> Profile;

std::string trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// The sections of an ini file in file order with their key=value lines, keys and values trimmed and values
// without surrounding quotes; a section named twice is one section. Empty when the file does not exist.
Profile readProfile(const char* fileName)
{
    Profile profile;
    if (!fileName)
        return profile;
    File file(fileName, GENERIC_READ, 0, OPEN_EXISTING);
    if (!file)
        return profile;
    std::string text(file.size(), '\0');
    x86::reg32 length = 0;
    if (!text.empty())
        file.read(&text[0], x86::reg32(text.size()), &length);
    text.resize(length);
    // A UTF-8 byte order mark is not part of the first line.
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    size_t section = std::string::npos;
    size_t position = 0;
    while (position < text.size())
    {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = trim(text.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            section = std::string::npos;
            if (close == std::string::npos)
                continue;
            const std::string name = trim(line.substr(1, close - 1));
            for (size_t i = 0; i < profile.size() && section == std::string::npos; ++i)
            {
                if (strcasecmp(profile[i].first.c_str(), name.c_str()) == 0)
                    section = i;
            }
            if (section == std::string::npos)
            {
                section = profile.size();
                profile.emplace_back(name, ProfileSection());
            }
            continue;
        }
        const size_t equals = line.find('=');
        if (section == std::string::npos || equals == std::string::npos)
            continue;
        std::string value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        profile[section].second.emplace_back(trim(line.substr(0, equals)), value);
    }
    return profile;
}

const ProfileSection* findProfileSection(const Profile& profile, const char* section)
{
    for (const auto& entry : profile)
    {
        if (strcasecmp(entry.first.c_str(), section) == 0)
            return &entry.second;
    }
    return nullptr;
}

bool readProfileValue(const char* fileName, const char* section, const char* key, std::string* value)
{
    const Profile profile = readProfile(fileName);
    const ProfileSection* entries = findProfileSection(profile, section);
    if (!entries)
        return false;
    for (const auto& entry : *entries)
    {
        if (strcasecmp(entry.first.c_str(), key) == 0)
        {
            *value = entry.second;
            return true;
        }
    }
    return false;
}

// Stores the strings, each terminated, then a final terminator. A list that does not fit is cut to size - 2
// characters, terminated twice, and size - 2 is returned, as Windows does.
DWORD copyProfileList(const std::vector<std::string>& items, LPSTR buffer, DWORD size)
{
    if (!buffer || size < 2)
        return 0;
    DWORD length = 0;
    for (const std::string& item : items)
    {
        if (length + item.size() + 2 > size)
        {
            memcpy(buffer + length, item.c_str(), size - 2 - length);
            buffer[size - 2] = 0;
            buffer[size - 1] = 0;
            return size - 2;
        }
        memcpy(buffer + length, item.c_str(), item.size() + 1);
        length += DWORD(item.size() + 1);
    }
    buffer[length] = 0;
    if (length == 0)
        buffer[1] = 0;
    return length;
}

}

UINT GetPrivateProfileIntA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault, LPCSTR lpFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    std::string value;
    if (!lpAppName || !lpKeyName || !readProfileValue(lpFileName, lpAppName, lpKeyName, &value))
        return UINT(nDefault);
    return UINT(strtol(value.c_str(), nullptr, 10));
}

// "key=value" per entry.
DWORD GetPrivateProfileSectionA(WinApplication* app, x86::CPU& cpu,
                                LPCSTR lpAppName, LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    std::vector<std::string> lines;
    const Profile profile = readProfile(lpFileName);
    const ProfileSection* entries = lpAppName ? findProfileSection(profile, lpAppName) : nullptr;
    if (entries)
    {
        for (const auto& entry : *entries)
            lines.push_back(entry.first + "=" + entry.second);
    }
    return copyProfileList(lines, lpReturnedString, nSize);
}

// A null section name lists the section names, a null key name the keys of the section.
DWORD GetPrivateProfileStringA(WinApplication* app, x86::CPU& cpu,
                               LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
                               DWORD nSize, LPCSTR lpFileName)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpReturnedString || nSize == 0)
        return 0;
    if (!lpAppName || !lpKeyName)
    {
        std::vector<std::string> names;
        const Profile profile = readProfile(lpFileName);
        if (!lpAppName)
        {
            for (const auto& section : profile)
                names.push_back(section.first);
        }
        else if (const ProfileSection* entries = findProfileSection(profile, lpAppName))
        {
            for (const auto& entry : *entries)
                names.push_back(entry.first);
        }
        return copyProfileList(names, lpReturnedString, nSize);
    }
    std::string value;
    if (!readProfileValue(lpFileName, lpAppName, lpKeyName, &value))
        value = trim(lpDefault ? lpDefault : "");
    const DWORD length = std::min(DWORD(value.size()), nSize - 1);
    memcpy(lpReturnedString, value.c_str(), length);
    lpReturnedString[length] = 0;
    return length;
}

// ---- Heaps (HeapCreate, the process heap) and global memory: all on the guest heap ---------------------------

namespace
{

class Heap : public GenericResource
{
};

bool isHeap(WinApplication* app, HANDLE heap)
{
    return dynamic_cast<Heap*>(app->getResource(heap)) != nullptr;
}

}

HANDLE GetProcessHeap(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(cpu);
    static HANDLE s_handle = 0;
    if (!s_handle)
        s_handle = app->allocateResource(new Heap());
    return s_handle;
}

HANDLE HeapCreate(WinApplication* app, x86::CPU& cpu,
                  DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize)
{
    RC_USE(cpu);
    RC_USE(flOptions);
    RC_USE(dwInitialSize);
    RC_USE(dwMaximumSize);
    return app->allocateResource(new Heap());
}

// The blocks of the heap stay allocated (the CRT destroys its heap only when start-up fails).
BOOL HeapDestroy(WinApplication* app, x86::CPU& cpu,
                 HANDLE hHeap)
{
    RC_USE(cpu);
    return isHeap(app, hHeap) && app->freeResource(hHeap) ? 1 : 0;
}

x86::reg32 HeapAlloc(WinApplication* app, x86::CPU& cpu,
                     HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    RC_USE(cpu);
    if (!isHeap(app, hHeap))
        return 0;
    return heap::allocate(dwBytes, (dwFlags & HEAP_ZERO_MEMORY) != 0);
}

BOOL HeapFree(WinApplication* app, x86::CPU& cpu,
              HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hHeap);
    RC_USE(dwFlags);
    return !lpMem || heap::release(lpMem) ? 1 : 0;
}

x86::reg32 HeapReAlloc(WinApplication* app, x86::CPU& cpu,
                       HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem, SIZE_T dwBytes)
{
    RC_USE(cpu);
    RC_USE(hHeap);
    const SIZE_T oldSize = heap::size(lpMem);
    if (oldSize == 0xffffffff)
        return 0;
    const x86::reg32 address = heap::allocate(dwBytes, false);
    memcpy(&app->getMemory<x86::reg8>(address), &app->getMemory<x86::reg8>(lpMem), std::min(oldSize, dwBytes));
    if ((dwFlags & HEAP_ZERO_MEMORY) && dwBytes > oldSize)
        memset(&app->getMemory<x86::reg8>(address + oldSize), 0, dwBytes - oldSize);
    heap::release(lpMem);
    return address;
}

SIZE_T HeapSize(WinApplication* app, x86::CPU& cpu,
                HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hHeap);
    RC_USE(dwFlags);
    return heap::size(lpMem);
}

// The CRT debug heap validates its own structures; the blocks themselves are always valid here.
BOOL HeapValidate(WinApplication* app, x86::CPU& cpu,
                  HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem)
{
    RC_USE(cpu);
    RC_USE(dwFlags);
    return isHeap(app, hHeap) && (!lpMem || heap::size(lpMem) != 0xffffffff) ? 1 : 0;
}

// Global memory is never moved: a handle is the block's address, also for GMEM_MOVEABLE.
x86::reg32 GlobalAlloc(WinApplication* app, x86::CPU& cpu,
                       UINT uFlags, SIZE_T dwBytes)
{
    RC_USE(app);
    RC_USE(cpu);
    return heap::allocate(dwBytes, (uFlags & GMEM_ZEROINIT) != 0);
}

x86::reg32 GlobalFree(WinApplication* app, x86::CPU& cpu,
                      x86::reg32 hMem)
{
    RC_USE(app);
    RC_USE(cpu);
    return !hMem || heap::release(hMem) ? 0 : hMem;
}

x86::reg32 GlobalLock(WinApplication* app, x86::CPU& cpu,
                      x86::reg32 hMem)
{
    RC_USE(app);
    RC_USE(cpu);
    return hMem;
}

BOOL GlobalUnlock(WinApplication* app, x86::CPU& cpu,
                  x86::reg32 hMem)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hMem);
    return 0;
}

// ---- Resources of the executable ------------------------------------------------------------------------------

namespace
{

x86::reg32 s_imageBase = 0;
x86::reg32 s_resourceSection = 0;

// OffsetToData of the entry of a resource directory (PE IMAGE_RESOURCE_DIRECTORY) with an integer id, or with a
// name when id is a string address (compared without case, as Windows does); 0 when there is none.
x86::reg32 findResourceEntry(WinApplication* app, x86::reg32 directory, x86::reg32 id)
{
    const x86::reg32 count = x86::reg32(app->getMemory<x86::reg16>(directory + 12)) +
                             app->getMemory<x86::reg16>(directory + 14);
    const bool byName = id >= 0x10000;
    const char* name = byName ? &app->getMemory<char>(id) : nullptr;
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const x86::reg32 entry = directory + 16 + 8 * i;
        const x86::reg32 nameWord = app->getMemory<x86::reg32>(entry);
        bool match;
        if (nameWord & 0x80000000)
        {
            // Counted UTF-16 name.
            const x86::reg32 text = s_resourceSection + (nameWord & 0x7fffffff);
            const x86::reg32 length = app->getMemory<x86::reg16>(text);
            match = byName && strlen(name) == length;
            for (x86::reg32 c = 0; match && c < length; ++c)
            {
                const x86::reg32 stored = app->getMemory<x86::reg16>(text + 2 + 2 * c);
                match = x86::reg32(toupper(x86::reg8(name[c]))) == (stored < 0x80 ? x86::reg32(toupper(int(stored))) : stored);
            }
        }
        else
        {
            match = !byName && nameWord == id;
        }
        if (match)
            return app->getMemory<x86::reg32>(entry + 4);
    }
    return 0;
}

}

void setResourceSection(x86::reg32 imageBase, x86::reg32 address)
{
    s_imageBase = imageBase;
    s_resourceSection = address;
}

// The handle is the guest address of the resource's data entry (RVA, size, code page, reserved).
x86::reg32 FindResourceA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, x86::reg32 lpName, x86::reg32 lpType)
{
    RC_USE(cpu);
    RC_USE(hModule);
    if (!s_resourceSection)
        return 0;
    // Type, name and language levels; the first language is taken.
    const x86::reg32 type = findResourceEntry(app, s_resourceSection, lpType);
    if (!(type & 0x80000000))
        return 0;
    const x86::reg32 name = findResourceEntry(app, s_resourceSection + (type & 0x7fffffff), lpName);
    if (!(name & 0x80000000))
        return 0;
    const x86::reg32 languages = s_resourceSection + (name & 0x7fffffff);
    if (app->getMemory<x86::reg16>(languages + 12) + app->getMemory<x86::reg16>(languages + 14) == 0)
        return 0;
    const x86::reg32 data = app->getMemory<x86::reg32>(languages + 16 + 4);
    return data & 0x80000000 ? 0 : s_resourceSection + data;
}

x86::reg32 LoadResource(WinApplication* app, x86::CPU& cpu,
                        HMODULE hModule, x86::reg32 hResInfo)
{
    RC_USE(cpu);
    RC_USE(hModule);
    return hResInfo ? s_imageBase + app->getMemory<x86::reg32>(hResInfo) : 0;
}

x86::reg32 LockResource(WinApplication* app, x86::CPU& cpu,
                        x86::reg32 hResData)
{
    RC_USE(app);
    RC_USE(cpu);
    return hResData;
}

DWORD SizeofResource(WinApplication* app, x86::CPU& cpu,
                     HMODULE hModule, x86::reg32 hResInfo)
{
    RC_USE(cpu);
    RC_USE(hModule);
    return hResInfo ? x86::reg32(app->getMemory<x86::reg32>(hResInfo + 4)) : 0;
}

static const HFILE HFILE_ERROR = HFILE(-1);

HFILE _lclose(WinApplication* app, x86::CPU& cpu, HFILE hFile)
{
    RC_USE(cpu);
    return app->freeResource(hFile) ? hFile : HFILE_ERROR;
}

// OF_READ (0), OF_WRITE (1) or OF_READWRITE (2); share flags are ignored.
HFILE _lopen(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName, int iReadWrite)
{
    RC_USE(cpu);
    static const DWORD access[3] = { GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE };
    const x86::reg32 mode = x86::reg32(iReadWrite) & 3;
    if (mode > 2)
        return HFILE_ERROR;
    win32::File* file = new win32::File(lpPathName, access[mode], 0, OPEN_EXISTING);
    if (!*file)
    {
        delete file;
        return HFILE_ERROR;
    }
    return app->allocateResource(file);
}


// ---- Synchronisation, threads, process ---------------------------------------------------------------------------

BOOL ReleaseMutex(WinApplication* app, x86::CPU& cpu,
                  HANDLE hMutex)
{
    return win32::Event::releaseMutex(hMutex, threadField(app, cpu, teb::ThreadId)) ? 1 : 0;
}

BOOL ReleaseSemaphore(WinApplication* app, x86::CPU& cpu,
                      HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount)
{
    RC_USE(app);
    RC_USE(cpu);
    x86::reg32 previous = 0;
    if (lReleaseCount <= 0 || !win32::Event::releaseSemaphore(hSemaphore, x86::reg32(lReleaseCount), &previous))
        return 0;
    if (lpPreviousCount)
        *lpPreviousCount = LONG(previous);
    return 1;
}

LONG InterlockedIncrement(WinApplication* app, x86::CPU& cpu,
                          LPLONG lpAddend)
{
    RC_USE(app);
    RC_USE(cpu);
    return __atomic_add_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

LONG InterlockedDecrement(WinApplication* app, x86::CPU& cpu,
                          LPLONG lpAddend)
{
    RC_USE(app);
    RC_USE(cpu);
    return __atomic_sub_fetch(lpAddend, 1, __ATOMIC_SEQ_CST);
}

void Sleep(WinApplication* app, x86::CPU& cpu,
           DWORD dwMilliseconds)
{
    SleepEx(app, cpu, dwMilliseconds, 0);
}

// Returns the previous suspend count. The caller holds the execution lock, so the target is not running guest code;
// it stops before it next takes the lock. A thread suspending itself stops here.
DWORD SuspendThread(WinApplication* app, x86::CPU& cpu,
                    HANDLE hThread)
{
    const x86::reg32 self = threadField(app, cpu, teb::ThreadId);
    win32::Thread* thread = hThread == CurrentThreadHandle ? nullptr
                          : dynamic_cast<win32::Thread*>(app->getResource(hThread));
    if (hThread != CurrentThreadHandle && !thread)
        return 0xffffffff;
    const x86::reg32 id = thread ? thread->threadId() : self;
    const int previous = scheduler::suspend(id);
    if (previous >= 0 && id == self)
    {
        app->unlockContext(cpu);
        app->lockContext(cpu);
    }
    return DWORD(previous);
}

void TerminateProcess(WinApplication* app, x86::CPU& cpu,
                      HANDLE hProcess, UINT uExitCode)
{
    RC_USE(hProcess);
    ExitProcess(app, cpu, uExitCode);
}

// The process has no exception dispatcher: a raised exception ends it.
void RaiseException(WinApplication* app, x86::CPU& cpu,
                    DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                    const x86::reg32* lpArguments)
{
    RC_USE(app);
    RC_USE(dwExceptionFlags);
    RC_USE(nNumberOfArguments);
    RC_USE(lpArguments);
    pf::logError("RaiseException 0x%08x", dwExceptionCode);
    x86::guestTrap(cpu.ip, "RaiseException");
}

// The counter is in microseconds (QueryPerformanceFrequency would report 1 MHz).
BOOL QueryPerformanceCounter(WinApplication* app, x86::CPU& cpu,
                             x86::reg64* lpPerformanceCount)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpPerformanceCount)
        return 0;
    const x86::reg64 now = pf::ticksUs();
    memcpy(lpPerformanceCount, &now, sizeof(now));
    return 1;
}

struct STARTUPINFOA
{
    DWORD cb;
    DWORD lpReserved;
    DWORD lpDesktop;
    DWORD lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD  wShowWindow;
    WORD  cbReserved2;
    DWORD lpReserved2;
    DWORD hStdInput;
    DWORD hStdOutput;
    DWORD hStdError;
};

void GetStartupInfoA(WinApplication* app, x86::CPU& cpu,
                     STARTUPINFOA* lpStartupInfo)
{
    RC_USE(app);
    RC_USE(cpu);
    memset(lpStartupInfo, 0, sizeof(*lpStartupInfo));
    lpStartupInfo->cb = sizeof(*lpStartupInfo);
}

UINT SetHandleCount(WinApplication* app, x86::CPU& cpu,
                    UINT uNumber)
{
    RC_USE(app);
    RC_USE(cpu);
    return uNumber;
}

// ---- Files ----------------------------------------------------------------------------------------------------

// Finds a file by name in the current directory (lpPath null) or the given one. Libraries the port provides are
// found in the current directory, as an installed driver would be.
DWORD SearchPathA(WinApplication* app, x86::CPU& cpu,
                  LPCSTR lpPath, LPCSTR lpFileName, LPCSTR lpExtension, DWORD nBufferLength, LPSTR lpBuffer,
                  x86::reg32* lpFilePart)
{
    if (!lpFileName)
        return 0;
    std::string name = lpFileName;
    if (lpExtension && name.find('.') == std::string::npos)
        name += lpExtension;
    std::string directory = lpPath ? std::string(lpPath) : File::getCurrentDirectory();
    if (!directory.empty() && directory.back() != '\\')
        directory += '\\';
    const std::string path = directory + name;
    struct stat status;
    if (!Library::findLibrary(name) && stat(File::hostPath(path.c_str()).c_str(), &status) != 0)
        return 0;
    const x86::reg32 bufferAddress = app->getMemory<x86::reg32>(cpu.esp + 20);
    const DWORD result = returnPath(lpBuffer, nBufferLength, path);
    if (result < nBufferLength && lpFilePart)
        *lpFilePart = bufferAddress + x86::reg32(directory.size());
    return result;
}

// MS-DOS date and time of a FILETIME (100 ns units since 1601).
BOOL FileTimeToDosDateTime(WinApplication* app, x86::CPU& cpu,
                           const FILETIME* lpFileTime, LPWORD lpFatDate, LPWORD lpFatTime)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpFileTime || !lpFatDate || !lpFatTime)
        return 0;
    const x86::reg64 ticks = (x86::reg64(lpFileTime->dwHighDateTime) << 32) | lpFileTime->dwLowDateTime;
    const int64_t unixSeconds = int64_t(ticks / 10000000) - 11644473600LL;
    if (unixSeconds < 315532800)    // before 1980
        return 0;
    const time_t t = time_t(unixSeconds);
    struct tm parts;
    gmtime_r(&t, &parts);
    *lpFatDate = WORD(((parts.tm_year - 80) << 9) | ((parts.tm_mon + 1) << 5) | parts.tm_mday);
    *lpFatTime = WORD((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
    return 1;
}

// Guest memory is always mapped: pointers are bad only when null or outside the guest address space.
BOOL IsBadReadPtr(WinApplication* app, x86::CPU& cpu,
                  x86::reg32 lp, x86::reg32 ucb)
{
    RC_USE(app);
    RC_USE(cpu);
    return ucb != 0 && (lp == 0 || !MemMap::isGuestRange(lp, ucb)) ? 1 : 0;
}

BOOL IsBadWritePtr(WinApplication* app, x86::CPU& cpu,
                   x86::reg32 lp, x86::reg32 ucb)
{
    return IsBadReadPtr(app, cpu, lp, ucb);
}

// ---- Environment and locale (the "C" locale with code page 1252) ------------------------------------------------

x86::reg32 GetEnvironmentStringsW(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

BOOL FreeEnvironmentStringsW(WinApplication* app, x86::CPU& cpu,
                             LPWSTR lpszEnvironmentBlock)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpszEnvironmentBlock);
    return 1;
}

static const DWORD LCMAP_LOWERCASE = 0x100;
static const DWORD LCMAP_UPPERCASE = 0x200;

// Case mapping of ASCII; other flags copy. cchSrc -1 includes the terminator; cchDest 0 asks for the length.
int LCMapStringA(WinApplication* app, x86::CPU& cpu,
                 DWORD Locale, DWORD dwMapFlags, LPCSTR lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    if (!lpSrcStr || cchSrc == 0 || cchDest < 0)
        return 0;
    const int length = cchSrc < 0 ? int(strlen(lpSrcStr)) + 1 : cchSrc;
    if (cchDest == 0)
        return length;
    if (!lpDestStr || length > cchDest)
        return 0;
    for (int i = 0; i < length; ++i)
    {
        const int c = x86::reg8(lpSrcStr[i]);
        lpDestStr[i] = char(c < 0x80 ? (dwMapFlags & LCMAP_UPPERCASE ? toupper(c)
                                        : dwMapFlags & LCMAP_LOWERCASE ? tolower(c) : c) : c);
    }
    return length;
}

int LCMapStringW(WinApplication* app, x86::CPU& cpu,
                 DWORD Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    if (!lpSrcStr || cchSrc == 0 || cchDest < 0)
        return 0;
    int length = cchSrc;
    if (length < 0)
    {
        length = 0;
        while (lpSrcStr[length])
            ++length;
        ++length;
    }
    if (cchDest == 0)
        return length;
    if (!lpDestStr || length > cchDest)
        return 0;
    for (int i = 0; i < length; ++i)
    {
        const WCHAR c = lpSrcStr[i];
        lpDestStr[i] = c < 0x80 ? WCHAR(dwMapFlags & LCMAP_UPPERCASE ? toupper(c)
                                        : dwMapFlags & LCMAP_LOWERCASE ? tolower(c) : c) : c;
    }
    return length;
}

// CT_CTYPE1 classes of a character below 256 (C1_UPPER 1, LOWER 2, DIGIT 4, SPACE 8, PUNCT 0x10, CNTRL 0x20,
// BLANK 0x40, XDIGIT 0x80, ALPHA 0x100).
static WORD characterType(int c)
{
    WORD type = 0;
    if (c < 0x80)
    {
        if (isupper(c)) type |= 0x001 | 0x100;
        if (islower(c)) type |= 0x002 | 0x100;
        if (isdigit(c)) type |= 0x004;
        if (isspace(c)) type |= 0x008;
        if (ispunct(c)) type |= 0x010;
        if (iscntrl(c)) type |= 0x020;
        if (c == ' ' || c == '\t') type |= 0x040;
        if (isxdigit(c)) type |= 0x080;
    }
    else if (c >= 0xc0 && c != 0xd7 && c != 0xf7)
    {
        type |= (c < 0xe0 ? 0x001 : 0x002) | 0x100;
    }
    else
    {
        type |= 0x010;
    }
    return type;
}

static const DWORD CT_CTYPE1 = 1;

BOOL GetStringTypeA(WinApplication* app, x86::CPU& cpu,
                    DWORD Locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    if (!lpSrcStr || !lpCharType)
        return 0;
    const int length = cchSrc < 0 ? int(strlen(lpSrcStr)) + 1 : cchSrc;
    for (int i = 0; i < length; ++i)
        lpCharType[i] = dwInfoType == CT_CTYPE1 ? characterType(x86::reg8(lpSrcStr[i])) : 0;
    return 1;
}

BOOL GetStringTypeW(WinApplication* app, x86::CPU& cpu,
                    DWORD dwInfoType, LPCWSTR lpSrcStr, int cchSrc, LPWORD lpCharType)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpSrcStr || !lpCharType)
        return 0;
    int length = cchSrc;
    if (length < 0)
    {
        length = 0;
        while (lpSrcStr[length])
            ++length;
        ++length;
    }
    for (int i = 0; i < length; ++i)
        lpCharType[i] = dwInfoType == CT_CTYPE1 && lpSrcStr[i] < 0x100 ? characterType(lpSrcStr[i]) : 0;
    return 1;
}

static const int CSTR_LESS_THAN = 1;
static const int CSTR_EQUAL = 2;
static const int CSTR_GREATER_THAN = 3;
static const DWORD NORM_IGNORECASE = 1;

template< typename C >
static int compareStrings(DWORD flags, const C* a, int lengthA, const C* b, int lengthB)
{
    if (lengthA < 0)
        for (lengthA = 0; a[lengthA]; ++lengthA) {}
    if (lengthB < 0)
        for (lengthB = 0; b[lengthB]; ++lengthB) {}
    for (int i = 0; i < lengthA && i < lengthB; ++i)
    {
        int ca = int(a[i] & 0xffff), cb = int(b[i] & 0xffff);
        if ((flags & NORM_IGNORECASE) && ca < 0x80 && cb < 0x80)
        {
            ca = tolower(ca);
            cb = tolower(cb);
        }
        if (ca != cb)
            return ca < cb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return lengthA == lengthB ? CSTR_EQUAL : lengthA < lengthB ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

int CompareStringA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwCmpFlags, LPCSTR lpString1, int cchCount1, LPCSTR lpString2, int cchCount2)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    if (!lpString1 || !lpString2)
        return 0;
    return compareStrings(dwCmpFlags, reinterpret_cast<const x86::reg8*>(lpString1), cchCount1,
                          reinterpret_cast<const x86::reg8*>(lpString2), cchCount2);
}

int CompareStringW(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwCmpFlags, LPCWSTR lpString1, int cchCount1, LPCWSTR lpString2, int cchCount2)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    if (!lpString1 || !lpString2)
        return 0;
    return compareStrings(dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2);
}

// Locale data of English (United States) with code page 1252; unknown items are not found (0).
static const char* localeInfo(DWORD type)
{
    switch (type & 0xffff)
    {
    case 0x0001: return "0409";     // LOCALE_ILANGUAGE
    case 0x0003: return "ENU";      // LOCALE_SABBREVLANGNAME
    case 0x0007: return "USA";      // LOCALE_SABBREVCTRYNAME
    case 0x000b: return "437";      // LOCALE_IDEFAULTCODEPAGE
    case 0x000e: return ".";        // LOCALE_SDECIMAL
    case 0x000f: return ",";        // LOCALE_STHOUSAND
    case 0x0010: return "3;0";      // LOCALE_SGROUPING
    case 0x1004: return "1252";     // LOCALE_IDEFAULTANSICODEPAGE
    default:     return nullptr;
    }
}

int GetLocaleInfoA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD LCType, LPSTR lpLCData, int cchData)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    const char* value = localeInfo(LCType);
    if (!value)
        return 0;
    const int length = int(strlen(value)) + 1;
    if (cchData == 0)
        return length;
    if (!lpLCData || cchData < length)
        return 0;
    memcpy(lpLCData, value, length);
    return length;
}

int GetLocaleInfoW(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD LCType, LPWSTR lpLCData, int cchData)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    const char* value = localeInfo(LCType);
    if (!value)
        return 0;
    const int length = int(strlen(value)) + 1;
    if (cchData == 0)
        return length;
    if (!lpLCData || cchData < length)
        return 0;
    for (int i = 0; i < length; ++i)
        lpLCData[i] = WCHAR(value[i]);
    return length;
}

// Formats with the US defaults (M/d/yyyy, h:mm:ss tt) when no format is given; a format string is used as is.
static int formatResult(const std::string& text, LPSTR buffer, int size)
{
    const int length = int(text.size()) + 1;
    if (size == 0)
        return length;
    if (!buffer || size < length)
        return 0;
    memcpy(buffer, text.c_str(), length);
    return length;
}

static SYSTEMTIME timeOrNow(const SYSTEMTIME* time)
{
    SYSTEMTIME now;
    if (time)
        return *time;
    getSystemTime(&now);
    return now;
}

int GetDateFormatA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwFlags, const SYSTEMTIME* lpDate, LPCSTR lpFormat, LPSTR lpDateStr,
                   int cchDate)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    RC_USE(dwFlags);
    const SYSTEMTIME t = timeOrNow(lpDate);
    char text[64];
    if (lpFormat)
        snprintf(text, sizeof(text), "%s", lpFormat);
    else
        snprintf(text, sizeof(text), "%u/%u/%u", t.wMonth, t.wDay, t.wYear);
    return formatResult(text, lpDateStr, cchDate);
}

int GetTimeFormatA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwFlags, const SYSTEMTIME* lpTime, LPCSTR lpFormat, LPSTR lpTimeStr,
                   int cchTime)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(Locale);
    RC_USE(dwFlags);
    const SYSTEMTIME t = timeOrNow(lpTime);
    char text[64];
    if (lpFormat)
        snprintf(text, sizeof(text), "%s", lpFormat);
    else
        snprintf(text, sizeof(text), "%u:%02u:%02u %s", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute, t.wSecond,
                 t.wHour < 12 ? "AM" : "PM");
    return formatResult(text, lpTimeStr, cchTime);
}

}}

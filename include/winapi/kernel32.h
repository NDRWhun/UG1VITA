#ifndef kernel32_H_
#define kernel32_H_

#include <x86.h>
#include <lib/winapp.h>
#include <winapi/types.h>

namespace win32 { namespace kernel32
{

struct EXCEPTION_RECORD;
typedef EXCEPTION_RECORD* PEXCEPTION_RECORD;
struct OSVERSIONINFOA;
typedef OSVERSIONINFOA* LPOSVERSIONINFOA;
typedef int HFILE;

BOOL CloseHandle(WinApplication* app, x86::CPU& cpu,
                 HANDLE hObject);
LONG CompareFileTime(WinApplication* app, x86::CPU& cpu,
                     const FILETIME *lpFileTime1, const FILETIME *lpFileTime2);
BOOL CreateDirectoryA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
HANDLE CreateEventA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
HANDLE CreateFileA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE CreateFileMappingA(WinApplication* app, x86::CPU& cpu,
                          HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                          DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
                          LPCSTR lpName);
HANDLE CreateMutexA(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE CreateThread(WinApplication* app, x86::CPU& cpu,
                    LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, x86::reg32 lpParameter, DWORD dwCreationFlags,
                    LPDWORD lpThreadId);
void DebugBreak(WinApplication* app, x86::CPU& cpu);
void DeleteCriticalSection(WinApplication* app, x86::CPU& cpu,
                           LPCRITICAL_SECTION lpCriticalSection);
BOOL DeleteFileA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpFileName);
BOOL DuplicateHandle(WinApplication* app, x86::CPU& cpu,
                     HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                     LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);
void EnterCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection);
BOOL EscapeCommFunction(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, DWORD dwFunc);
void ExitProcess(WinApplication* app, x86::CPU& cpu,
                 UINT uExitCode);
void ExitThread(WinApplication* app, x86::CPU& cpu,
                DWORD dwExitCode);
BOOL FindClose(WinApplication* app, x86::CPU& cpu,
               HANDLE hFindFile);
HANDLE FindFirstFileA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL FindNextFileA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
x86::reg32 FindResourceA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, x86::reg32 lpName, x86::reg32 lpType);
BOOL FlushFileBuffers(WinApplication* app, x86::CPU& cpu,
                      HANDLE hFile);
BOOL FreeEnvironmentStringsA(WinApplication* app, x86::CPU& cpu,
                             LPSTR lpszEnvironmentBlock);
BOOL FreeLibrary(WinApplication* app, x86::CPU& cpu,
                 HMODULE hLibModule);
UINT GetACP(WinApplication* app, x86::CPU& cpu);
BOOL GetCPInfo(WinApplication* app, x86::CPU& cpu,
               UINT CodePage, LPCPINFO lpCPInfo);
BOOL GetCommModemStatus(WinApplication* app, x86::CPU& cpu,
                        HANDLE hFile, LPDWORD lpModemStat);
BOOL GetCommProperties(WinApplication* app, x86::CPU& cpu,
                       HANDLE hFile, LPCOMMPROP lpCommProp);
BOOL GetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB);
x86::reg32 GetCommandLineA(WinApplication* app, x86::CPU& cpu);
x86::reg32 GetCommandLineW(WinApplication* app, x86::CPU& cpu);
BOOL GetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, LPDWORD lpMode);
DWORD GetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                           DWORD nBufferLength, LPSTR lpBuffer);
HANDLE GetCurrentProcess(WinApplication* app, x86::CPU& cpu);
DWORD GetCurrentProcessId(WinApplication* app, x86::CPU& cpu);
DWORD GetCurrentThreadId(WinApplication* app, x86::CPU& cpu);
HANDLE GetCurrentThread(WinApplication* app, x86::CPU& cpu);
BOOL GetDiskFreeSpaceA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
                       LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters);
UINT GetDriveTypeA(WinApplication* app, x86::CPU& cpu,
                   LPCSTR lpRootPathName);
x86::reg32 GetEnvironmentStrings(WinApplication* app, x86::CPU& cpu);
DWORD GetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpFileName);
DWORD GetFileSize(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL GetFileTime(WinApplication* app, x86::CPU& cpu,
                 HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
DWORD GetFileType(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile);
DWORD GetFullPathNameA(WinApplication* app, x86::CPU& cpu,
                       LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, x86::reg32* lpFilePart);
DWORD GetLastError(WinApplication* app, x86::CPU& cpu);
void GetLocalTime(WinApplication* app, x86::CPU& cpu,
                  LPSYSTEMTIME lpSystemTime);
DWORD GetLogicalDriveStringsA(WinApplication* app, x86::CPU& cpu,
                              DWORD nBufferLength, LPSTR lpBuffer);
DWORD GetLogicalDrives(WinApplication* app, x86::CPU& cpu);
DWORD GetModuleFileNameA(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPSTR lpFilename, DWORD nSize);
DWORD GetModuleFileNameW(WinApplication* app, x86::CPU& cpu,
                         HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
HMODULE GetModuleHandleA(WinApplication* app, x86::CPU& cpu,
                         LPCSTR lpModuleName);
UINT GetOEMCP(WinApplication* app, x86::CPU& cpu);
BOOL GetOverlappedResult(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFile, LPOVERLAPPED lpOverlapped,
                         LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
UINT GetPrivateProfileIntA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault, LPCSTR lpFileName);
DWORD GetPrivateProfileSectionA(WinApplication* app, x86::CPU& cpu,
                                LPCSTR lpAppName, LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName);
DWORD GetPrivateProfileStringA(WinApplication* app, x86::CPU& cpu,
                               LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
                               DWORD nSize, LPCSTR lpFileName);
FARPROC GetProcAddress(WinApplication* app, x86::CPU& cpu,
                       HMODULE hModule, LPCSTR lpProcName);
HANDLE GetProcessHeap(WinApplication* app, x86::CPU& cpu);
HANDLE GetStdHandle(WinApplication* app, x86::CPU& cpu,
                    DWORD nStdHandle);
void GetSystemInfo(WinApplication* app, x86::CPU& cpu,
                   LPSYSTEM_INFO lpSystemInfo);
int GetThreadPriority(WinApplication* app, x86::CPU& cpu,
                      HANDLE hThread);
DWORD GetTickCount(WinApplication* app, x86::CPU& cpu);
DWORD GetTimeZoneInformation(WinApplication* app, x86::CPU& cpu,
                             LPTIME_ZONE_INFORMATION lpTimeZoneInformation);
DWORD GetVersion(WinApplication* app, x86::CPU& cpu);
BOOL GetVersionExA(WinApplication* app, x86::CPU& cpu,
                   LPOSVERSIONINFOA lpVersionInformation);
BOOL GetVolumeInformationA(WinApplication* app, x86::CPU& cpu,
                           LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
                           LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
                           LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer,
                           DWORD nFileSystemNameSize);
UINT GetWindowsDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPSTR lpBuffer, UINT uSize);
x86::reg32 HeapAlloc(WinApplication* app, x86::CPU& cpu,
                     HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
BOOL HeapFree(WinApplication* app, x86::CPU& cpu,
              HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem);
x86::reg32 HeapReAlloc(WinApplication* app, x86::CPU& cpu,
                       HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem, SIZE_T dwBytes);
SIZE_T HeapSize(WinApplication* app, x86::CPU& cpu,
                HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem);
void GlobalMemoryStatus(WinApplication* app, x86::CPU& cpu,
                        LPMEMORYSTATUS lpBuffer);
void InitializeCriticalSection(WinApplication* app, x86::CPU& cpu,
                               LPCRITICAL_SECTION lpCriticalSection);
void LeaveCriticalSection(WinApplication* app, x86::CPU& cpu,
                          LPCRITICAL_SECTION lpCriticalSection);
HMODULE LoadLibraryA(WinApplication* app, x86::CPU& cpu,
                     LPCSTR lpLibFileName);
x86::reg32 LoadResource(WinApplication* app, x86::CPU& cpu,
                        HMODULE hModule, x86::reg32 hResInfo);
x86::reg32 LockResource(WinApplication* app, x86::CPU& cpu,
                        x86::reg32 hResData);
x86::reg32 MapViewOfFile(WinApplication* app, x86::CPU& cpu,
                         HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                         DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
BOOL MoveFileA(WinApplication* app, x86::CPU& cpu,
               LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
int MultiByteToWideChar(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr,
                        int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
void OutputDebugStringA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpOutputString);
BOOL PeekConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer,
                       DWORD nLength, LPDWORD lpNumberOfEventsRead);
BOOL PulseEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent);
BOOL PurgeComm(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, DWORD dwFlags);
BOOL ReadConsoleInputA(WinApplication* app, x86::CPU& cpu,
                       HANDLE hConsoleInput, PINPUT_RECORD lpBuffer, DWORD nLength,
                       LPDWORD lpNumberOfEventsRead);
BOOL ReadFile(WinApplication* app, x86::CPU& cpu,
              HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL RemoveDirectoryA(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName);
BOOL ResetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent);
DWORD ResumeThread(WinApplication* app, x86::CPU& cpu,
                   HANDLE hThread);
void RtlUnwind(WinApplication* app, x86::CPU& cpu,
               PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
BOOL SetCommState(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile, LPDCB lpDCB);
BOOL SetCommTimeouts(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LPCOMMTIMEOUTS lpCommTimeouts);
BOOL SetConsoleCtrlHandler(WinApplication* app, x86::CPU& cpu,
                           PHANDLER_ROUTINE HandlerRoutine, BOOL Add);
BOOL SetConsoleMode(WinApplication* app, x86::CPU& cpu,
                    HANDLE hConsoleHandle, DWORD  dwMode);
BOOL SetCurrentDirectoryA(WinApplication* app, x86::CPU& cpu,
                          LPCSTR lpPathName);
BOOL SetEndOfFile(WinApplication* app, x86::CPU& cpu,
                  HANDLE hFile);
BOOL SetEnvironmentVariableA(WinApplication* app, x86::CPU& cpu,
                             LPCSTR lpName, LPCSTR lpValue);
BOOL SetEnvironmentVariableW(WinApplication* app, x86::CPU& cpu,
                             LPCWSTR lpName, LPCWSTR lpValue);
BOOL SetEvent(WinApplication* app, x86::CPU& cpu,
              HANDLE hEvent);
UINT SetErrorMode(WinApplication* app, x86::CPU& cpu, UINT uMode);
BOOL SetFileAttributesA(WinApplication* app, x86::CPU& cpu,
                        LPCSTR lpFileName, DWORD  dwFileAttributes);
DWORD SetFilePointer(WinApplication* app, x86::CPU& cpu,
                     HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
void SetLastError(WinApplication* app, x86::CPU& cpu,
                  DWORD dwErrCode);
BOOL SetStdHandle(WinApplication* app, x86::CPU& cpu,
                  DWORD nStdHandle, HANDLE hHandle);
BOOL SetThreadPriority(WinApplication* app, x86::CPU& cpu,
                       HANDLE hThread, int nPriority);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                                                         LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
DWORD SizeofResource(WinApplication* app, x86::CPU& cpu,
                     HMODULE hModule, x86::reg32 hResInfo);
DWORD SleepEx(WinApplication* app, x86::CPU& cpu,
              DWORD dwMilliseconds, BOOL bAlertable);
BOOL TerminateThread(WinApplication* app, x86::CPU& cpu,
                     HANDLE hThread, DWORD dwExitCode);
DWORD TlsAlloc(WinApplication* app, x86::CPU& cpu);
BOOL TlsFree(WinApplication* app, x86::CPU& cpu,
             DWORD dwTlsIndex);
x86::reg32 TlsGetValue(WinApplication* app, x86::CPU& cpu,
                       DWORD dwTlsIndex);
BOOL TlsSetValue(WinApplication* app, x86::CPU& cpu,
                 DWORD dwTlsIndex, LPVOID lpTlsValue);
LONG UnhandledExceptionFilter(WinApplication* app, x86::CPU& cpu,
                              struct _EXCEPTION_POINTERS *ExceptionInfo);
BOOL UnmapViewOfFile(WinApplication* app, x86::CPU& cpu,
                     Packed<const void> lpBaseAddress);
x86::reg32 VirtualAlloc(WinApplication* app, x86::CPU& cpu,
                        Packed<void> lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL VirtualFree(WinApplication* app, x86::CPU& cpu,
                 Packed<void> lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL VirtualProtect(WinApplication* app, x86::CPU& cpu,
                    Packed<void> lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
SIZE_T VirtualQuery(WinApplication* app, x86::CPU& cpu,
                    Packed<const void> lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
DWORD WaitForMultipleObjectsEx(WinApplication* app, x86::CPU& cpu,
                               DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll,
                               DWORD dwMilliseconds, BOOL bAlertable);
DWORD WaitForSingleObject(WinApplication* app, x86::CPU& cpu,
                          HANDLE hHandle, DWORD dwMilliseconds);
int WideCharToMultiByte(WinApplication* app, x86::CPU& cpu,
                        UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);
BOOL WriteConsoleA(WinApplication* app, x86::CPU& cpu,
                   HANDLE hConsoleOutput, const void* lpBuffer, DWORD nNumberOfCharsToWrite,
                   LPDWORD lpNumberOfCharsWritten, LPVOID  lpReserved);
BOOL WriteFile(WinApplication* app, x86::CPU& cpu,
               HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
HFILE _lclose(WinApplication* app, x86::CPU& cpu, HFILE hFile);
HFILE _lopen(WinApplication* app, x86::CPU& cpu, LPCSTR lpPathName, int iReadWrite);

HANDLE CreateSemaphoreA(WinApplication* app, x86::CPU& cpu,
                        LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                        LPCSTR lpName);
BOOL ReleaseMutex(WinApplication* app, x86::CPU& cpu,
                  HANDLE hMutex);
BOOL ReleaseSemaphore(WinApplication* app, x86::CPU& cpu,
                      HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount);
DWORD WaitForMultipleObjects(WinApplication* app, x86::CPU& cpu,
                             DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
LONG InterlockedIncrement(WinApplication* app, x86::CPU& cpu,
                          LPLONG lpAddend);
LONG InterlockedDecrement(WinApplication* app, x86::CPU& cpu,
                          LPLONG lpAddend);
void Sleep(WinApplication* app, x86::CPU& cpu,
           DWORD dwMilliseconds);
DWORD SuspendThread(WinApplication* app, x86::CPU& cpu,
                    HANDLE hThread);
void TerminateProcess(WinApplication* app, x86::CPU& cpu,
                      HANDLE hProcess, UINT uExitCode);
void RaiseException(WinApplication* app, x86::CPU& cpu,
                    DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                    const x86::reg32* lpArguments);
BOOL QueryPerformanceCounter(WinApplication* app, x86::CPU& cpu,
                             x86::reg64* lpPerformanceCount);
struct STARTUPINFOA;
void GetStartupInfoA(WinApplication* app, x86::CPU& cpu,
                     STARTUPINFOA* lpStartupInfo);
UINT SetHandleCount(WinApplication* app, x86::CPU& cpu,
                    UINT uNumber);
HANDLE HeapCreate(WinApplication* app, x86::CPU& cpu,
                  DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
BOOL HeapDestroy(WinApplication* app, x86::CPU& cpu,
                 HANDLE hHeap);
BOOL HeapValidate(WinApplication* app, x86::CPU& cpu,
                  HANDLE hHeap, DWORD dwFlags, x86::reg32 lpMem);
x86::reg32 GlobalAlloc(WinApplication* app, x86::CPU& cpu,
                       UINT uFlags, SIZE_T dwBytes);
x86::reg32 GlobalFree(WinApplication* app, x86::CPU& cpu,
                      x86::reg32 hMem);
x86::reg32 GlobalLock(WinApplication* app, x86::CPU& cpu,
                      x86::reg32 hMem);
BOOL GlobalUnlock(WinApplication* app, x86::CPU& cpu,
                  x86::reg32 hMem);
DWORD SearchPathA(WinApplication* app, x86::CPU& cpu,
                  LPCSTR lpPath, LPCSTR lpFileName, LPCSTR lpExtension, DWORD nBufferLength, LPSTR lpBuffer,
                  x86::reg32* lpFilePart);
BOOL FileTimeToDosDateTime(WinApplication* app, x86::CPU& cpu,
                           const FILETIME* lpFileTime, LPWORD lpFatDate, LPWORD lpFatTime);
BOOL IsBadReadPtr(WinApplication* app, x86::CPU& cpu,
                  x86::reg32 lp, x86::reg32 ucb);
BOOL IsBadWritePtr(WinApplication* app, x86::CPU& cpu,
                   x86::reg32 lp, x86::reg32 ucb);
x86::reg32 GetEnvironmentStringsW(WinApplication* app, x86::CPU& cpu);
BOOL FreeEnvironmentStringsW(WinApplication* app, x86::CPU& cpu,
                             LPWSTR lpszEnvironmentBlock);
int LCMapStringA(WinApplication* app, x86::CPU& cpu,
                 DWORD Locale, DWORD dwMapFlags, LPCSTR lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest);
int LCMapStringW(WinApplication* app, x86::CPU& cpu,
                 DWORD Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest);
BOOL GetStringTypeA(WinApplication* app, x86::CPU& cpu,
                    DWORD Locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType);
BOOL GetStringTypeW(WinApplication* app, x86::CPU& cpu,
                    DWORD dwInfoType, LPCWSTR lpSrcStr, int cchSrc, LPWORD lpCharType);
int CompareStringA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwCmpFlags, LPCSTR lpString1, int cchCount1, LPCSTR lpString2, int cchCount2);
int CompareStringW(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwCmpFlags, LPCWSTR lpString1, int cchCount1, LPCWSTR lpString2, int cchCount2);
int GetLocaleInfoA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD LCType, LPSTR lpLCData, int cchData);
int GetLocaleInfoW(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD LCType, LPWSTR lpLCData, int cchData);
int GetDateFormatA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwFlags, const SYSTEMTIME* lpDate, LPCSTR lpFormat, LPSTR lpDateStr,
                   int cchDate);
int GetTimeFormatA(WinApplication* app, x86::CPU& cpu,
                   DWORD Locale, DWORD dwFlags, const SYSTEMTIME* lpTime, LPCSTR lpFormat, LPSTR lpTimeStr,
                   int cchTime);

// Resource section of the executable (FindResourceA): its guest address and the image base its RVAs refer to.
void setResourceSection(x86::reg32 imageBase, x86::reg32 address);
// Guest path of the executable (GetModuleFileNameA).
void setModulePath(const char* path);

}}

#endif

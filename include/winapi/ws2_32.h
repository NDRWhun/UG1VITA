#ifndef ws2_32_H_
#define ws2_32_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

// Winsock 2. The port is offline: WSAStartup reports the network subsystem as not ready, every socket call fails
// with WSANOTINITIALISED, name lookups find nothing. Byte order helpers work.
namespace win32 { namespace ws2_32
{

typedef x86::reg32 SOCKET;

int WSAStartup(WinApplication* app, x86::CPU& cpu, WORD wVersionRequested, x86::reg32 lpWSAData);
int WSACleanup(WinApplication* app, x86::CPU& cpu);
int WSAGetLastError(WinApplication* app, x86::CPU& cpu);
int WSAIoctl(WinApplication* app, x86::CPU& cpu, SOCKET s, DWORD dwIoControlCode, x86::reg32 lpvInBuffer,
             DWORD cbInBuffer, x86::reg32 lpvOutBuffer, DWORD cbOutBuffer, x86::reg32 lpcbBytesReturned,
             x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine);
SOCKET socket(WinApplication* app, x86::CPU& cpu, int af, int type, int protocol);
int closesocket(WinApplication* app, x86::CPU& cpu, SOCKET s);
int shutdown(WinApplication* app, x86::CPU& cpu, SOCKET s, int how);
int bind(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen);
int connect(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen);
int listen(WinApplication* app, x86::CPU& cpu, SOCKET s, int backlog);
SOCKET accept(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 addr, x86::reg32 addrlen);
int ioctlsocket(WinApplication* app, x86::CPU& cpu, SOCKET s, LONG cmd, x86::reg32 argp);
int setsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               int optlen);
int getsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               x86::reg32 optlen);
int send(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags);
int sendto(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 to,
           int tolen);
int recv(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags);
int recvfrom(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 from,
             x86::reg32 fromlen);
int select(WinApplication* app, x86::CPU& cpu, int nfds, x86::reg32 readfds, x86::reg32 writefds,
           x86::reg32 exceptfds, x86::reg32 timeout);
int getpeername(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen);
int getsockname(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen);
x86::reg32 gethostbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name);
ULONG htonl(WinApplication* app, x86::CPU& cpu, ULONG hostlong);
ULONG ntohl(WinApplication* app, x86::CPU& cpu, ULONG netlong);
WORD htons(WinApplication* app, x86::CPU& cpu, WORD hostshort);
WORD ntohs(WinApplication* app, x86::CPU& cpu, WORD netshort);

}}

#endif

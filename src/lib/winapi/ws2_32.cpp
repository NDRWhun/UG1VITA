#include <winapi/ws2_32.h>
#include <winapi/support.h>
#include <lib/teb.h>
#include <lib/winapp.h>

namespace win32 { namespace ws2_32
{

static const int SOCKET_ERROR = -1;
static const SOCKET INVALID_SOCKET = 0xffffffff;
static const int WSASYSNOTREADY = 10091;
static const int WSANOTINITIALISED = 10093;

// A socket call without a started Winsock: SOCKET_ERROR with WSANOTINITIALISED, as Windows answers.
static int notInitialised(WinApplication* app, x86::CPU& cpu, const char* api)
{
    offline(app, cpu, api);
    setLastError(app, cpu, WSANOTINITIALISED);
    return SOCKET_ERROR;
}

static ULONG swap32(ULONG value)
{
    return __builtin_bswap32(value);
}

static WORD swap16(WORD value)
{
    return WORD((value >> 8) | (value << 8));
}

// The network subsystem is never ready, so Winsock is never started.
int WSAStartup(WinApplication* app, x86::CPU& cpu, WORD wVersionRequested, x86::reg32 lpWSAData)
{
    RC_USE(wVersionRequested);
    RC_USE(lpWSAData);
    offline(app, cpu, "WSAStartup");
    return WSASYSNOTREADY;
}

int WSACleanup(WinApplication* app, x86::CPU& cpu)
{
    return notInitialised(app, cpu, "WSACleanup");
}

// The calling thread's last error, as GetLastError.
int WSAGetLastError(WinApplication* app, x86::CPU& cpu)
{
    return int(app->getMemory<x86::reg32>(cpu.efs + teb::LastError));
}

int WSAIoctl(WinApplication* app, x86::CPU& cpu, SOCKET s, DWORD dwIoControlCode, x86::reg32 lpvInBuffer,
             DWORD cbInBuffer, x86::reg32 lpvOutBuffer, DWORD cbOutBuffer, x86::reg32 lpcbBytesReturned,
             x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine)
{
    RC_USE(s);
    RC_USE(dwIoControlCode);
    RC_USE(lpvInBuffer);
    RC_USE(cbInBuffer);
    RC_USE(lpvOutBuffer);
    RC_USE(cbOutBuffer);
    RC_USE(lpcbBytesReturned);
    RC_USE(lpOverlapped);
    RC_USE(lpCompletionRoutine);
    return notInitialised(app, cpu, "WSAIoctl");
}

SOCKET socket(WinApplication* app, x86::CPU& cpu, int af, int type, int protocol)
{
    RC_USE(af);
    RC_USE(type);
    RC_USE(protocol);
    notInitialised(app, cpu, "socket");
    return INVALID_SOCKET;
}

int closesocket(WinApplication* app, x86::CPU& cpu, SOCKET s)
{
    RC_USE(s);
    return notInitialised(app, cpu, "closesocket");
}

int shutdown(WinApplication* app, x86::CPU& cpu, SOCKET s, int how)
{
    RC_USE(s);
    RC_USE(how);
    return notInitialised(app, cpu, "shutdown");
}

int bind(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "bind");
}

int connect(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "connect");
}

int listen(WinApplication* app, x86::CPU& cpu, SOCKET s, int backlog)
{
    RC_USE(s);
    RC_USE(backlog);
    return notInitialised(app, cpu, "listen");
}

SOCKET accept(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 addr, x86::reg32 addrlen)
{
    RC_USE(s);
    RC_USE(addr);
    RC_USE(addrlen);
    notInitialised(app, cpu, "accept");
    return INVALID_SOCKET;
}

int ioctlsocket(WinApplication* app, x86::CPU& cpu, SOCKET s, LONG cmd, x86::reg32 argp)
{
    RC_USE(s);
    RC_USE(cmd);
    RC_USE(argp);
    return notInitialised(app, cpu, "ioctlsocket");
}

int setsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               int optlen)
{
    RC_USE(s);
    RC_USE(level);
    RC_USE(optname);
    RC_USE(optval);
    RC_USE(optlen);
    return notInitialised(app, cpu, "setsockopt");
}

int getsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               x86::reg32 optlen)
{
    RC_USE(s);
    RC_USE(level);
    RC_USE(optname);
    RC_USE(optval);
    RC_USE(optlen);
    return notInitialised(app, cpu, "getsockopt");
}

int send(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    return notInitialised(app, cpu, "send");
}

int sendto(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 to,
           int tolen)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    RC_USE(to);
    RC_USE(tolen);
    return notInitialised(app, cpu, "sendto");
}

int recv(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    return notInitialised(app, cpu, "recv");
}

int recvfrom(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 from,
             x86::reg32 fromlen)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    RC_USE(from);
    RC_USE(fromlen);
    return notInitialised(app, cpu, "recvfrom");
}

int select(WinApplication* app, x86::CPU& cpu, int nfds, x86::reg32 readfds, x86::reg32 writefds,
           x86::reg32 exceptfds, x86::reg32 timeout)
{
    RC_USE(nfds);
    RC_USE(readfds);
    RC_USE(writefds);
    RC_USE(exceptfds);
    RC_USE(timeout);
    return notInitialised(app, cpu, "select");
}

int getpeername(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "getpeername");
}

int getsockname(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "getsockname");
}

// No host entry: NULL with WSANOTINITIALISED.
x86::reg32 gethostbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name)
{
    RC_USE(name);
    notInitialised(app, cpu, "gethostbyname");
    return 0;
}

ULONG htonl(WinApplication* app, x86::CPU& cpu, ULONG hostlong)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap32(hostlong);
}

ULONG ntohl(WinApplication* app, x86::CPU& cpu, ULONG netlong)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap32(netlong);
}

WORD htons(WinApplication* app, x86::CPU& cpu, WORD hostshort)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap16(hostshort);
}

WORD ntohs(WinApplication* app, x86::CPU& cpu, WORD netshort)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap16(netshort);
}

}}

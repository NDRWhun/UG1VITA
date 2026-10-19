#include <winapi/netapi32.h>
#include <winapi/support.h>
#include <lib/winapp.h>
#include <cstddef>

namespace win32 { namespace netapi32
{

// The network control block (nb30.h), 64 bytes.
struct NCB
{
    BYTE        ncb_command;
    BYTE        ncb_retcode;
    BYTE        ncb_lsn;
    BYTE        ncb_num;
    x86::reg32  ncb_buffer;
    WORD        ncb_length;
    BYTE        ncb_callname[16];
    BYTE        ncb_name[16];
    BYTE        ncb_rto;
    BYTE        ncb_sto;
    x86::reg32  ncb_post;
    BYTE        ncb_lana_num;
    BYTE        ncb_cmd_cplt;
    BYTE        ncb_reserve[10];
    x86::reg32  ncb_event;
} __attribute__((packed));
static_assert(sizeof(NCB) == 64, "NCB layout");
static_assert(offsetof(NCB, ncb_lana_num) == 48, "NCB layout");

static const BYTE NCBENUM = 0x37;
static const BYTE ASYNCH = 0x80;
static const BYTE NRC_GOODRET = 0x00;
static const BYTE NRC_BRIDGE = 0x23;    // invalid adapter number

BYTE Netbios(WinApplication* app, x86::CPU& cpu, NCB* pncb)
{
    offline(app, cpu, "Netbios");
    if (!pncb)
        return NRC_BRIDGE;
    BYTE result = NRC_BRIDGE;
    // LANA_ENUM: a count byte, then the adapter numbers.
    if ((pncb->ncb_command & ~ASYNCH) == NCBENUM && pncb->ncb_buffer && pncb->ncb_length >= 1)
    {
        app->getMemory<BYTE>(pncb->ncb_buffer) = 0;
        result = NRC_GOODRET;
    }
    pncb->ncb_retcode = result;
    pncb->ncb_cmd_cplt = result;
    return result;
}

}}

// Motion Blur setting with an enhanced value (display.h).
//
// Customize Display Settings screen (ebx or edx below): 32 entries, two per setting row (its right and left arrow
// buttons), with the row object's name hash at +0x1B0 and the value at +0x230 of each entry (the row's value in the
// even one); [screen + 0xC] is the package name. 0x416A10 changes the value of a row when its arrow is pressed,
// 0x4169B0 shows each row's value (the on_option_NN or off_option_NN string of row NN - 1).

#include "display.h"

#include <lib/guest.h>
#include <lib/memmap.h>
#include <native.h>
#include <recomp.h>
#include <winapi/d3d9_native.h>

#include <cstdio>
#include <cstring>

namespace nfsu
{

void sub_4169b0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace display
{

namespace
{

using x86::reg32;

const reg32 MotionBlur = 0x00700E98;
const reg32 Enhanced = 2;
const reg32 MotionBlurRow = 0xAE5C3F41;     // name hash of motion_blur_option
const reg32 Entries = 0x20, EntryRow = 0x1b0, EntryValue = 0x230, PackageName = 0xc;
// The game's language: English 0, French 1, German 2, Italian 3, Spanish 4, Dutch 5, Swedish 6, Korean 7, Chinese 8.
const reg32 Language = 0x0070430C;
// The language table: entries (label hash, string) sorted by hash at [0x734434], their count at [0x73442C].
const reg32 StringTable = 0x00734434, StringCount = 0x0073442C;
const reg32 OnLabel = 0x0000063C;            // ON: "On"
// ecx = package name, eax = object name hash, cdecl text: sets the text of a string object of the package.
const reg32 SetStringText = 0x004F6720;

// "Enhanced" in the languages 0..6 (Latin-1, as the language tables); English for the others.
const char* const EnhancedTexts[] = {
    "Enhanced", "Am\xe9lior\xe9", "Verbessert", "Migliorato", "Mejorado", "Verbeterd", "F\xf6rb\xe4ttrad",
};
const int LanguageCount = int(sizeof(EnhancedTexts) / sizeof(EnhancedTexts[0]));

win32::MemMap* s_textMemory = nullptr;
reg32 s_texts[LanguageCount] = {};
bool s_enhancedShown = false;               // the on string of the Motion Blur row reads Enhanced

// FE object name hash (0x4FD230): h = h * 33 + c from 0xFFFFFFFF, letters upper case.
reg32 nameHash(const char* name)
{
    reg32 h = 0xffffffff;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(name); *c; ++c)
        h = h * 33 + (*c >= 'a' && *c <= 'z' ? *c - 0x20 : *c);
    return h;
}

// The entry of a row (its first), or Entries.
reg32 findEntry(reg32 screen, reg32 row)
{
    reg32 i = 0;
    while (i < Entries && rc::ld32(screen + EntryRow + 4 * i) != row)
        ++i;
    return i;
}

reg32 enhancedText()
{
    if (!s_textMemory)
    {
        s_textMemory = new win32::MemMap(4096, win32::MemMap::High);
        reg32 at = s_textMemory->getBlockStart();
        for (int i = 0; i < LanguageCount; ++i)
        {
            const size_t length = strlen(EnhancedTexts[i]) + 1;
            memcpy(win32::g_guestBase + at, EnhancedTexts[i], length);
            s_texts[i] = at;
            at += reg32(length);
        }
    }
    const reg32 language = rc::ld32(Language);
    return s_texts[language < reg32(LanguageCount) ? language : 0];
}

// The string of a label in the loaded language table, or 0.
reg32 languageString(reg32 label)
{
    const reg32 table = rc::ld32(StringTable), count = rc::ld32(StringCount);
    if (!table || !count || count > 100000 || !win32::MemMap::isGuestRange(table, count * 8))
        return 0;
    reg32 lo = 0, hi = count;
    while (lo < hi)
    {
        const reg32 mid = (lo + hi) / 2;
        if (rc::ld32(table + mid * 8) < label)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && rc::ld32(table + lo * 8) == label ? rc::ld32(table + lo * 8 + 4) : 0;
}

// The on string of the Motion Blur row reads Enhanced while the row's value is 2; at another value after that, the
// language's On again.
void showMotionBlur(win32::WinApplication* app, x86::CPU& cpu, reg32 screen)
{
    const reg32 entry = findEntry(screen, MotionBlurRow);
    if (entry >= Entries)
        return;
    const bool enhanced = rc::ld32(screen + EntryValue + 4 * entry) == Enhanced;
    if (!enhanced && !s_enhancedShown)
        return;
    const reg32 text = enhanced ? enhancedText() : languageString(OnLabel);
    if (!text)
        return;
    char name[24];
    snprintf(name, sizeof(name), "on_option_%02u", unsigned(entry / 2 + 1));
    const reg32 eax = cpu.eax, ebx = cpu.ebx, ecx = cpu.ecx, edx = cpu.edx, esi = cpu.esi, edi = cpu.edi,
                ebp = cpu.ebp, esp = cpu.esp;
    cpu.esp -= 4;
    rc::st32(cpu.esp, text);
    cpu.ecx = rc::ld32(screen + PackageName);
    cpu.eax = nameHash(name);
    ::native::callGuest(app, cpu, SetStringText);
    if (cpu.terminate)
        return;
    cpu.eax = eax, cpu.ebx = ebx, cpu.ecx = ecx, cpu.edx = edx, cpu.esi = esi, cpu.edi = edi, cpu.ebp = ebp;
    cpu.esp = esp;
    s_enhancedShown = enhanced;
}

}

void frame()
{
    win32::d3d9::bridge::setMotionBlurEnhanced(rc::ld32(MotionBlur) == Enhanced);
}

}

namespace native
{

using x86::reg32;

// Value change of a row (screen in edx, the row's name hash in esi, a stack argument 1 for the left arrow, 0 for the
// right, popped by the return): the guest flips the row's first entry between 0 and 1 (eax = the entry, or 0x20; ecx = its +0x1B0 slot). The
// Motion Blur row steps through 0, 1, 2 instead, backwards for the left arrow.
void sub_416a10(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 screen = cpu.edx, row = cpu.esi;
    const bool left = rc::ld32(cpu.esp + 4) != 0;
    const reg32 entry = display::findEntry(screen, row);
    cpu.eax = entry;
    cpu.ecx = screen + display::EntryRow + 4 * entry;
    if (entry < display::Entries)
    {
        const reg32 at = screen + display::EntryValue + 4 * entry;
        reg32 value = rc::ld32(at);
        if (row == display::MotionBlurRow)
        {
            value = value > display::Enhanced ? 1 : value;
            value = left ? (value + 2) % 3 : (value + 1) % 3;
        }
        else
        {
            value ^= 1;
        }
        rc::st32(at, value);
    }
    cpu.esp += 8;
}

// Shows each row's value (screen in ebx), then the Motion Blur row's Enhanced (showMotionBlur).
void sub_4169b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 screen = cpu.ebx;
    sub_4169b0_guest(app, cpu);
    if (cpu.terminate)
        return;
    display::showMotionBlur(app, cpu, screen);
}

}

}

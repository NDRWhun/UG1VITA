// Motion Blur setting with an enhanced value and the Anti-Aliasing setting (display.h).
//
// Customize Display Settings screen (ebx or edx below): 32 entries, two per setting row (its right and left arrow
// buttons), with the row object's name hash at +0x1B0 and the value at +0x230 of each entry (the row's value in the
// even one); [screen + 0xC] is the package name. 0x416A10 changes the value of a row when its arrow is pressed,
// 0x4169B0 shows each row's value (the on_option_NN or off_option_NN string of row NN - 1).
//
// Display Settings screen (PC_graphics.fng; ebx or ecx below): the Resolution row's value at [screen + 0x7C] (an index
// 0..4 into 640x480, 800x600, 1024x768, 1280x960, 1280x1024), shown by 0x4151C0 as the string Res_01..Res_05 of that
// index with the others hidden, changed by 0x415120 (eax = the message: left 0x9120409E, right 0xB5971BF1, others
// show it again); the screen writes it to RES [0x700E70] when it is left. [screen + 0xC] is the package name.

#include "display.h"

#include <lib/guest.h>
#include <lib/memmap.h>
#include <native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <winapi/d3d9_native.h>

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <utility>
#include <vector>

namespace nfsu
{

void sub_4169b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_410a00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_415120_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4151c0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace display
{

namespace
{

using x86::reg32;

const reg32 MotionBlur = 0x00700E98;
const reg32 Enhanced = 2;
const reg32 MotionBlurRow = 0xAE5C3F41;     // name hash of motion_blur_option
const reg32 Entries = 0x20, EntryRow = 0x1b0, EntryValue = 0x230, PackageName = 0xc;
// Anti-Aliasing: 2 is 2x, 4 is 4x, any other value off. It is the BPP setting, saved in the profile with the other
// display settings and read by no game code.
const reg32 AntiAliasing = 0x00700E6C;
const reg32 ResolutionIndex = 0x7c;
const reg32 LeftMessage = 0x9120409E, RightMessage = 0xB5971BF1;
// The game's language: English 0, French 1, German 2, Italian 3, Spanish 4, Dutch 5, Swedish 6, Korean 7, Chinese 8.
const reg32 Language = 0x0070430C;
// The language table: entries (label hash, string) sorted by hash at [0x734434], their count at [0x73442C].
const reg32 StringTable = 0x00734434, StringCount = 0x0073442C;
const reg32 OnLabel = 0x0000063C;            // ON: "On"
const reg32 OffLabel = 0x0000CCFA;           // OFF: "Off"
// ecx = package name, eax = object name hash, cdecl text: sets the text of a string object of the package.
const reg32 SetStringText = 0x004F6720;

// Texts in the languages 0..6 (Latin-1, as the language tables); English for the others.
const int LanguageCount = 7;
const char* const EnhancedTexts[LanguageCount] = {
    "Enhanced", "Am\xe9lior\xe9", "Verbessert", "Migliorato", "Mejorado", "Verbeterd", "F\xf6rb\xe4ttrad",
};
const char* const AntiAliasingTexts[LanguageCount] = {
    "Anti-Aliasing", "Anticr\xe9nelage", "Kantengl\xe4ttung", "Antialiasing", "Antialiasing", "Anti-aliasing",
    "Kantutj\xe4mning",
};

win32::MemMap* s_textMemory = nullptr;
reg32 s_textEnd = 0;
std::vector<std::pair<const char*, reg32>> s_texts;     // host text -> its copy in guest memory
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

// A copy of a text in guest memory (made once per text).
reg32 guestText(const char* text)
{
    for (const auto& t : s_texts)
    {
        if (t.first == text)
            return t.second;
    }
    if (!s_textMemory)
    {
        s_textMemory = new win32::MemMap(4096, win32::MemMap::High);
        s_textEnd = s_textMemory->getBlockStart();
    }
    const size_t length = strlen(text) + 1;
    memcpy(win32::g_guestBase + s_textEnd, text, length);
    s_texts.emplace_back(text, s_textEnd);
    s_textEnd += reg32(length);
    return s_texts.back().second;
}

// The text of the game's language.
reg32 languageText(const char* const* texts)
{
    const reg32 language = rc::ld32(Language);
    return guestText(texts[language < reg32(LanguageCount) ? language : 0]);
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

// Sets the text of a string object of the screen's package; the registers are kept.
bool setText(win32::WinApplication* app, x86::CPU& cpu, reg32 screen, const char* name, reg32 text)
{
    const reg32 eax = cpu.eax, ebx = cpu.ebx, ecx = cpu.ecx, edx = cpu.edx, esi = cpu.esi, edi = cpu.edi,
                ebp = cpu.ebp, esp = cpu.esp;
    cpu.esp -= 4;
    rc::st32(cpu.esp, text);
    cpu.ecx = rc::ld32(screen + PackageName);
    cpu.eax = nameHash(name);
    ::native::callGuest(app, cpu, SetStringText);
    if (cpu.terminate)
        return false;
    cpu.eax = eax, cpu.ebx = ebx, cpu.ecx = ecx, cpu.edx = edx, cpu.esi = esi, cpu.edi = edi, cpu.ebp = ebp;
    cpu.esp = esp;
    return true;
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
    const reg32 text = enhanced ? languageText(EnhancedTexts) : languageString(OnLabel);
    if (!text)
        return;
    char name[24];
    snprintf(name, sizeof(name), "on_option_%02u", unsigned(entry / 2 + 1));
    if (setText(app, cpu, screen, name, text))
        s_enhancedShown = enhanced;
}

// Samples of an Anti-Aliasing value: 1 (off), 2 or 4.
reg32 samplesOf(reg32 value)
{
    return value == 4 ? 4 : value == 2 ? 2 : 1;
}

// The cfg key msaa (off, 2x, 4x), when the file has it: its sample count, which the renderer takes in place of the
// Anti-Aliasing setting; 0 without the key.
reg32 msaaOverride()
{
    static const char* const Names[] = { "off", "2x", "4x" };
    static const reg32 Samples[] = { 1, 2, 4 };
    const char* text = pf::Config::getString("msaa", nullptr);
    if (!text)
        return 0;
    for (int i = 0; i < 3; ++i)
    {
        if (strcasecmp(text, Names[i]) == 0)
        {
            pf::logInfo("display: msaa=%s (cfg) is used in place of the Anti-Aliasing setting", Names[i]);
            return Samples[i];
        }
    }
    pf::logError("config msaa=%s is not one of off/2x/4x, the Anti-Aliasing setting is used", text);
    return 0;
}

// Display Settings: with widescreen=on the back buffer size does not follow RES (native.cpp sub_4086b0), and the
// Resolution row is the Anti-Aliasing row.
bool antiAliasingRow()
{
    static const bool on = pf::Config::getBool("widescreen", true);
    return on;
}

// The Anti-Aliasing row: the Resolution title reads Anti-Aliasing, Res_01 the setting's Off, 2x or 4x.
void showAntiAliasing(win32::WinApplication* app, x86::CPU& cpu, reg32 screen)
{
    const reg32 samples = samplesOf(rc::ld32(AntiAliasing));
    reg32 value;
    if (samples == 1)
    {
        value = languageString(OffLabel);
        if (!value)
            value = guestText("Off");
    }
    else
    {
        value = guestText(samples == 4 ? "4x" : "2x");
    }
    if (setText(app, cpu, screen, "ResolutionOption_text", languageText(AntiAliasingTexts)))
        setText(app, cpu, screen, "Res_01", value);
}

}

void frame()
{
    static const reg32 cfgSamples = msaaOverride();
    win32::d3d9::bridge::setMotionBlurEnhanced(rc::ld32(MotionBlur) == Enhanced);
    win32::d3d9::bridge::setMsaaSamples(cfgSamples ? cfgSamples : samplesOf(rc::ld32(AntiAliasing)));
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

// Default display settings (at device start-up and from Restore Defaults): the guest's settings for the graphics
// card, then Motion Blur and Anti-Aliasing off. A new profile takes the settings in effect; a profile's saved
// MOTIONBLUR and BPP replace them when it is loaded.
void sub_410a00(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_410a00_guest(app, cpu);
    if (cpu.terminate)
        return;
    rc::st32(display::MotionBlur, 0);
    rc::st32(display::AntiAliasing, 0);
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

// Shows the Resolution row's value (screen in ebx). As the Anti-Aliasing row the guest shows Res_01 (the index reads
// 0 for the call and is kept), then showAntiAliasing sets the texts.
void sub_4151c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!display::antiAliasingRow())
    {
        sub_4151c0_guest(app, cpu);
        return;
    }
    const reg32 screen = cpu.ebx;
    const reg32 index = rc::ld32(screen + display::ResolutionIndex);
    rc::st32(screen + display::ResolutionIndex, 0);
    sub_4151c0_guest(app, cpu);
    rc::st32(screen + display::ResolutionIndex, index);
    if (cpu.terminate)
        return;
    display::showAntiAliasing(app, cpu, screen);
}

// Value change of the Resolution row (message in eax, screen in ecx; ebx and esi kept). As the Anti-Aliasing row the
// right arrow steps Off, 2x, 4x in the setting, the left arrow backwards, RES and the row's index stay; then the row
// is shown.
void sub_415120(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!display::antiAliasingRow())
    {
        sub_415120_guest(app, cpu);
        return;
    }
    const reg32 message = cpu.eax, screen = cpu.ecx, ebx = cpu.ebx;
    if (message == display::LeftMessage || message == display::RightMessage)
    {
        static const reg32 Values[] = { 0, 2, 4 };
        const reg32 samples = display::samplesOf(rc::ld32(display::AntiAliasing));
        const reg32 index = samples == 4 ? 2 : samples == 2 ? 1 : 0;
        rc::st32(display::AntiAliasing, Values[message == display::LeftMessage ? (index + 2) % 3 : (index + 1) % 3]);
    }
    cpu.ebx = screen;
    cpu.esp -= 4;
    sub_4151c0(app, cpu);
    if (cpu.terminate)
        return;
    cpu.ebx = ebx;
    cpu.esp += 4;
}

}

}

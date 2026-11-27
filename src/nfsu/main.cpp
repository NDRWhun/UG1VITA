#include "bankcode.h"
#include "bench.h"
#include "embedded.h"
#include "phases.h"
#include "prompts.h"
#include "region.h"

#include <lib/file.h>
#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#ifdef NFSU_PGO_GENERATE
#include <platform/pgo.h>
#endif
#include <platform/sha256.h>
#include <fpu.h>
#include <mmx.h>
#include <nfsu.h>
#include <nfsu_layout.h>
#include <winapi/d3d9_native.h>
#include <winapi/kernel32.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Process parameters picked up by the VitaSDK runtime.
extern "C" {
int          _newlib_heap_size_user      = 64 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize  = 4 * 1024 * 1024;
}

// The data folder is the game folder and the guest's C:\ drive: Speed.exe and the game's data folders lie in it
// beside nfsug1vita.cfg, nfsug1vita.log and registry.txt.
static std::string s_dataDirectory = "ux0:data/NFSUG1VITA";
static const char* const s_exeName = "Speed.exe";

static std::string dataPath(const char* name)
{
    return s_dataDirectory + "/" + name;
}

// Guest view of the executable and its folder.
static const char* s_guestGameFolder = "C:\\";
static const char* s_guestExePath = "C:\\Speed.exe";
static const char* s_commandLine = "\"C:\\Speed.exe\"";

// Reads the executable. A file other than the one the code was generated from is used too when its data matches
// (imageSections); the log names it.
static bool loadExecutable(std::vector<uint8_t>& image)
{
    const std::string exePath = dataPath(s_exeName);
    const char* const exeFile = exePath.c_str();
    FILE* file = fopen(exeFile, "rb");
    if (!file)
    {
        pf::logError("%s not found: copy the game folder's contents to %s/", exeFile, s_dataDirectory.c_str());
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    image.resize(size > 0 ? size_t(size) : 0);
    const size_t read = image.empty() ? 0 : fread(image.data(), 1, image.size(), file);
    fclose(file);
    if (read != image.size() || image.empty())
    {
        pf::logError("%s: reading %u bytes failed", exeFile, unsigned(image.size()));
        return false;
    }
    const std::string hash = pf::sha256Hex(image.data(), image.size());
    if (hash != nfsu::layout::exeSha256)
        pf::logInfo("%s: %u bytes, SHA-256 %s; the code was generated from %u bytes, SHA-256 %s", exeFile,
                    unsigned(image.size()), hash.c_str(), nfsu::layout::exeSize, nfsu::layout::exeSha256);
    return true;
}

static uint32_t imageWord(const std::vector<uint8_t>& image, size_t offset, size_t bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < bytes && offset + i < image.size(); ++i)
        value |= uint32_t(image[offset + i]) << (8 * i);
    return value;
}

// The headers and the raw data of the sections at their guest addresses, read from the executable's own section
// table. Only the sections of the generated layout are taken, each clipped to its size there; each also gets an
// entry without data that covers its whole size (the arena is zeroed, so the rest of a section reads as zero).
// Sections with a SHA-256 in the layout (all but code) must match it, or the executable must be one of another
// region whose .rdata and .data are then converted into region. Without report a mismatch is not logged (the
// caller tries embeddedFallback next).
static bool imageSections(const std::vector<uint8_t>& image, nfsu::region::Converted& region,
                          std::vector<win32::Section>& sections, bool report = true)
{
    const size_t pe = imageWord(image, 0x3c, 4);
    if (imageWord(image, 0, 2) != 0x5a4d || imageWord(image, pe, 4) != 0x4550)
    {
        if (report)
            pf::logError("%s is not a Windows executable", s_exeName);
        return false;
    }
    const uint32_t imageBase = imageWord(image, pe + 24 + 28, 4);
    if (imageBase != nfsu::layout::imageBase)
    {
        if (report)
            pf::logError("%s: image base 0x%08x, expected 0x%08x", s_exeName, imageBase, nfsu::layout::imageBase);
        return false;
    }
    const size_t count = imageWord(image, pe + 6, 2);
    const size_t table = pe + 24 + imageWord(image, pe + 20, 2);
    constexpr size_t SectionCount = sizeof(nfsu::layout::sections) / sizeof(nfsu::layout::sections[0]);
    size_t rawOffsets[SectionCount], rawSizes[SectionCount];
    std::string hashes[SectionCount];
    const char* targetRdata = "";
    const char* targetData = "";
    const char* rdata = "";
    const char* data = "";
    size_t rdataIndex = 0, dataIndex = 0;
    bool same = true;
    for (size_t i = 0; i < SectionCount; ++i)
    {
        const nfsu::layout::ImageSection& s = nfsu::layout::sections[i];
        size_t entry = table;
        while (entry < table + count * 40 && imageWord(image, entry + 12, 4) != s.address - imageBase)
            entry += 40;
        if (entry >= table + count * 40)
        {
            if (report)
                pf::logError("%s has no section at 0x%08x (%s): not version 1.4", s_exeName, s.address, s.name);
            return false;
        }
        rawOffsets[i] = imageWord(image, entry + 20, 4);
        rawSizes[i] = imageWord(image, entry + 16, 4);
        if (!*s.sha256)
            continue;
        if (rawSizes[i] >= s.rawSize && rawOffsets[i] <= image.size() && image.size() - rawOffsets[i] >= s.rawSize)
            hashes[i] = pf::sha256Hex(image.data() + rawOffsets[i], s.rawSize);
        same = same && hashes[i] == s.sha256;
        if (!strcmp(s.name, ".rdata"))
        {
            targetRdata = s.sha256;
            rdata = hashes[i].c_str();
            rdataIndex = i;
        }
        else if (!strcmp(s.name, ".data"))
        {
            targetData = s.sha256;
            data = hashes[i].c_str();
            dataIndex = i;
        }
    }
    // A region embeddedFallback converted already.
    const bool given = !region.rdata.empty() && !region.data.empty();
    const bool converted = given || (!same && *rdata && *data
        && nfsu::region::convert(targetRdata, targetData, rdata, data, image.data() + rawOffsets[rdataIndex],
                                 image.data() + rawOffsets[dataIndex], region));
    if (!same && !converted)
    {
        if (report)
            pf::logError("%s: a version 1.4 build that is not supported (supported: the US build and the regions in "
                         "region.cpp)", s_exeName);
        return false;
    }
    if (converted && !given)
        pf::logInfo("%s: %s version 1.4, its data converted", s_exeName, region.name);
    sections.push_back(win32::Section{ image.data(), nfsu::layout::imageBase,
                                       x86::reg32(std::min<size_t>(nfsu::layout::headerSize, image.size())) });
    for (size_t i = 0; i < SectionCount; ++i)
    {
        const nfsu::layout::ImageSection& s = nfsu::layout::sections[i];
        const x86::reg32 size = s.virtualSize > s.rawSize ? s.virtualSize : s.rawSize;
        const std::vector<uint8_t>* replaced = !converted ? nullptr
            : i == rdataIndex ? &region.rdata : i == dataIndex ? &region.data : nullptr;
        if (replaced)
            sections.push_back(win32::Section{ replaced->data(), s.address,
                                               x86::reg32(std::min<size_t>(replaced->size(), size)) });
        else if (rawOffsets[i] < image.size() && rawSizes[i])
        {
            const size_t bytes = std::min<size_t>({ rawSizes[i], size, image.size() - rawOffsets[i] });
            sections.push_back(win32::Section{ image.data() + rawOffsets[i], s.address, x86::reg32(bytes) });
        }
        sections.push_back(win32::Section{ nullptr, s.address, size });
    }
    for (const nfsu::region::Variable& v : region.variables)
        sections.push_back(win32::Section{ reinterpret_cast<const x86::reg8*>(&v.value), v.address, 4 });
    return true;
}

// The raw data of the executable's section at guest address, from its section table; nullptr without that section.
static const uint8_t* sectionData(const std::vector<uint8_t>& image, x86::reg32 address, size_t& rawSize)
{
    const size_t pe = imageWord(image, 0x3c, 4);
    if (imageWord(image, 0, 2) != 0x5a4d || imageWord(image, pe, 4) != 0x4550
        || imageWord(image, pe + 24 + 28, 4) != nfsu::layout::imageBase)
        return nullptr;
    const size_t count = imageWord(image, pe + 6, 2);
    const size_t table = pe + 24 + imageWord(image, pe + 20, 2);
    for (size_t entry = table; entry < table + count * 40; entry += 40)
    {
        if (imageWord(image, entry + 12, 4) != address - nfsu::layout::imageBase)
            continue;
        const size_t offset = imageWord(image, entry + 20, 4);
        rawSize = imageWord(image, entry + 16, 4);
        if (offset > image.size())
            return nullptr;
        rawSize = std::min(rawSize, image.size() - offset);
        return image.data() + offset;
    }
    return nullptr;
}

// A version 1.4 executable whose data imageSections cannot use: the copy-protected one of an unmodified install,
// US or European. The protection encrypts its code and .data, so neither is read. It runs from the embedded copy of
// the executable the code was generated from (NFSU_EMBED_EXE, embedded.h), which replaces image. Its .rdata stays
// readable apart from the imports and identifies the build (region::convertProtected): the US build uses the
// embedded sections as they are; a European one keeps its own .rdata, converted into region, and the embedded
// .data, which is what the European .data converts to. An executable of another version matches about 12% and is
// refused.
static bool embeddedFallback(std::vector<uint8_t>& image, nfsu::region::Converted& region)
{
    uint32_t embeddedSize = 0;
    const uint8_t* const embedded = nfsu::image::exe(embeddedSize);
    const nfsu::layout::ImageSection* rdata = nullptr;
    const nfsu::layout::ImageSection* data = nullptr;
    for (const nfsu::layout::ImageSection& s : nfsu::layout::sections)
    {
        if (!strcmp(s.name, ".rdata"))
            rdata = &s;
        else if (!strcmp(s.name, ".data"))
            data = &s;
    }
    if (!embedded || !rdata || !data || size_t(rdata->fileOffset) + rdata->rawSize > embeddedSize
        || size_t(data->fileOffset) + data->rawSize > embeddedSize)
    {
        pf::logError("%s: its data cannot be read, and this build has no embedded executable to run it with "
                     "(NFSU_EMBED_EXE)", s_exeName);
        return false;
    }
    size_t rawSize = 0;
    const uint8_t* const own = sectionData(image, rdata->address, rawSize);
    if (!own)
    {
        pf::logError("%s has no section at 0x%08x (.rdata): not version 1.4", s_exeName, rdata->address);
        return false;
    }
    double share = 0;
    const char* error = nullptr;
    const char* const build = nfsu::region::convertProtected(own, rawSize, embedded + rdata->fileOffset,
                                                             embedded + data->fileOffset, region, share, error);
    if (!build)
    {
        pf::logError("%s: .rdata %.1f%% the same as version 1.4's: %s", s_exeName, 100.0 * share, error);
        return false;
    }
    pf::logInfo("%s: %s version 1.4 (.rdata %.1f%% the same) whose code and data cannot be read (a copy-protected "
                "executable); the embedded executable is used in its place%s", s_exeName, build, 100.0 * share,
                region.rdata.empty() ? "" : ", with this executable's .rdata");
    image.assign(embedded, embedded + embeddedSize);
    return true;
}

// Bindings of the Vita pad, presented as the EMS PS2 adapter (src/lib/controls.cpp). Per button index, then per hat
// direction (down, up, left, right) of each of the 4 hats the game reads. Race: R accelerates, L brakes, Cross is
// nitrous, Square the handbrake, Triangle the camera, Circle looks back, d-pad up/down and the rear touch halves
// shift. Menus: Cross accepts (20), Circle goes back (23), Start is the game's Start (25, also "button 2"), Square
// "button 3" (26), L / R the triggers (27 / 28), d-pad and hat navigate; Triangle presses the menu bar's extra
// item (src/nfsu/prompts.cpp, button_prompts=vita), on the vinyl layer screen it opens the vinyl colours (21).
// Hat 1 (the right stick) orbits the front end's car camera in menus (29-32: orbit up, down, left, right; stick left
// sends orbit right, which turns the view left) at a speed that follows the stick (setOrbitSpeed); in races stick up
// shifts up (7) and stick down shifts down (8).
static const x86::reg32 s_raceActions[16] = { 10, 9, 6, 5, 8, 7, 2, 1, 14, 12, 0, 0, 7, 4, 8, 3 };
static const x86::reg32 s_menuActions[16] = { 0, 23, 20, 26, 0, 0, 27, 28, 0, 25, 0, 0, 16, 19, 17, 18 };
static const x86::reg32 s_raceHat[4] = { 0, 0, 3, 4 };
static const x86::reg32 s_raceRightHat[4] = { 8, 7, 0, 0 };
static const x86::reg32 s_menuHat[4] = { 17, 16, 18, 19 };
static const x86::reg32 s_menuRightHat[4] = { 30, 29, 32, 31 };

static x86::reg32& guestDword(x86::reg32 address)
{
    return *reinterpret_cast<x86::reg32*>(win32::g_guestBase + address);
}

// Exhaustion report: the game's memory pools (table [0x007301D4][8]; pool +0 name, +0x1C allocations, +0x24 bytes
// owned, +0x28 bytes used, +0x2C peak used).
static void reportPools()
{
    for (x86::reg32 i = 0; i < 8; ++i)
    {
        const x86::reg32 pool = guestDword(0x007301D4 + 4 * i);
        if (!pool || !win32::MemMap::isGuestRange(pool, 0x58))
            continue;
        const x86::reg32 nameAddress = guestDword(pool);
        char name[32] = "?";
        if (nameAddress && win32::MemMap::isGuestRange(nameAddress, sizeof(name)))
        {
            memcpy(name, win32::g_guestBase + nameAddress, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
        pf::logInfo("memory: pool %u \"%s\": %u KB owned, %u KB used, peak %u KB, %u allocations", unsigned(i), name,
                    guestDword(pool + 0x24) / 1024, guestDword(pool + 0x28) / 1024, guestDword(pool + 0x2C) / 1024,
                    guestDword(pool + 0x1C));
    }
}

// The game's defaults for the EMS product (entry 0x00704258: GUID, 16 race, 16 menu actions) become the port's.
static void setPadBindings()
{
    // Input scanner of action 21 (entry 0x007039C0, event at +8): FENG_BUTTON1 (24), which the screens receive as
    // PAD_BUTTON1, instead of FENG_SELECT_ALTERNATE (30).
    guestDword(0x007039C8) = 0x18;
    for (x86::reg32 i = 0; i < 16; ++i)
    {
        guestDword(0x00704268 + 4 * i) = s_raceActions[i];
        guestDword(0x007042A8 + 4 * i) = s_menuActions[i];
    }
}

// Env cube faces drawn per frame (0x40AD60, patched in generate.py PATCHES to read the copy written here): frame
// counter [0x7364DC] modulo the divisor of the reflection level [0x700E74] picks one of that level's six rows of six
// face flags. The game's levels: 1 one face a frame, 2 faces 0 and 4 every frame and one of the others, 3 all six.
// car_reflection_interval=n (2..6) spreads level 3 over n frames, face f drawn when the counter modulo n is f modulo
// n. Without the key: 1 (the game's) at frame_rate=30, 2 at frame_rate=60, so each face is drawn 30 times a second at
// either rate.
static void setEnvmapSchedule()
{
    const x86::reg32 GameDivisors = 0x00702AAC, GameRows = 0x00702AC0, Divisors = 0x006D3800, Rows = 0x006D3810;
    for (x86::reg32 i = 0; i < 4; ++i)
        guestDword(Divisors + 4 * i) = guestDword(GameDivisors + 4 * i);
    for (x86::reg32 i = 0; i < 18 * 6; ++i)
        guestDword(Rows + 4 * i) = guestDword(GameRows + 4 * i);
    int interval = pf::Config::getInt("frame_rate", 30) >= 60 ? 2 : 1;
    pf::Config::tryGetInt("car_reflection_interval", &interval);
    const int rate = std::min(std::max(interval, 1), 6);
    if (rate == 1)
        return;
    guestDword(Divisors + 4 * 3) = x86::reg32(rate);
    for (x86::reg32 row = 0; row < 6; ++row)
        for (x86::reg32 face = 0; face < 6; ++face)
            guestDword(Rows + 4 * ((12 + row) * 6 + face)) = row < x86::reg32(rate) && face % rate == row ? 1 : 0;
    pf::logInfo("envmap: the highest reflection level draws the six cube faces over %d frames (car_reflection_interval)",
                rate);
}

// Text fields. Every edit of the game's line editor sets its active flag [0x007363A8],
// buffer [0x0073639C] and length limit [0x007363AC]; the edit ends when the flag is cleared. The front end's
// keyboard screen (the profile name) edits 0x00777B74 and flags itself in [0x00735BA4]; it ignores Back when its
// cancel message [0x00777B64] is -1, and Escape would then leave the profile screen waiting. Accepted characters:
// letters and digits, "_@.-" unless [0x0070108E], the list of 0x004115F0 when [0x007363B3] is 1.
static void pollTextField()
{
    static bool s_editing = false;
    if (!guestDword(0x007363A8))
    {
        if (s_editing)
            win32::Gamepad::endTextEdit();
        s_editing = false;
        return;
    }
    const x86::reg32 buffer = guestDword(0x0073639C);
    const x86::reg32 maxLength = guestDword(0x007363AC);
    char text[256];
    size_t length = 0;
    for (; buffer && length < maxLength && length + 1 < sizeof(text) && win32::g_guestBase[buffer + length]; ++length)
        text[length] = char(win32::g_guestBase[buffer + length]);
    text[length] = 0;
    const bool keyboard = buffer == 0x00777B74 && guestDword(0x00735BA4) != 0;
    const char* extra = win32::g_guestBase[0x007363B3] == 1 ? " .,_@-`=[]\\;'/<>?:\"{}|+()*&%#!~"
                      : win32::g_guestBase[0x0070108E] == 0 ? "_@.-" : "";
    win32::Gamepad::editText(keyboard ? "Profile name" : "Text", text, maxLength, extra,
                             !keyboard || guestDword(0x00777B64) != 0xFFFFFFFF);
    s_editing = true;
}

// Device slots: 6 at 0x0078DAA8, 0x270 bytes each. +0x130 the channel, +0x134 the controller type (0x13 the
// keyboard), +0x18 the race actions of the 16 buttons and +0x58 of the 4 hats' 4 directions, +0x98 and +0xD8 the
// menu actions of the same, +0x23C the action the controls screen is assigning (0: none).
// The race tables are the player's: the controls screen assigns them (0x00406E10), the profile's <name>.cfg in the
// save folder keeps them (written by 0x0041C7A0, read by 0x0041CDA0 at a profile load and when the pad is attached;
// the defaults of 0x004060A0 when the file has none).
// The menu tables are not saved: 0x004064D0 puts the generic menu actions on buttons 0-4 after each load.
const x86::reg32 PadSlots = 0x0078DAA8, PadSlotSize = 0x270, PadSlotCount = 6;
const x86::reg32 RaceButtons = 0x18, RaceHats = 0x58, MenuButtons = 0x98, MenuHats = 0xD8, AssignedAction = 0x23C;

static bool padSlot(x86::reg32 slot)
{
    return guestDword(slot + 0x130) == 0 && guestDword(slot + 0x134) != 0x13;
}

// The pad's slot, 0 when there is none.
static x86::reg32 findPadSlot()
{
    for (x86::reg32 slot = PadSlots; slot < PadSlots + PadSlotCount * PadSlotSize; slot += PadSlotSize)
    {
        if (padSlot(slot))
            return slot;
    }
    return 0;
}

// The current screen's package name is at 0x0072CDD0; on MU_GarageVinylLayerV2.fng Triangle sends action 21.
static bool vinylLayerScreen()
{
    static const char name[] = "mu_garagevinyllayerv2.fng";
    for (x86::reg32 i = 0; i < sizeof(name); ++i)
    {
        char c = char(win32::g_guestBase[0x0072CDD0 + i]);
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != name[i])
            return false;
    }
    return true;
}

// The game builds each device's action mask from both tables of its slot (0x004071B8), whatever map is active. The
// keyboard instead gets one map at a time: 0x004057C0 picks the race map ([0x007041C4] = 0) only while a race is
// driven, the menu map otherwise. The pad follows that map: race actions while driving, menu actions in menus; in
// the front end the race actions stay too. Inputs held across a change stay hidden until released, as the
// keyboard's map change releases its keys.
struct PadMap
{
    bool race;
    bool menu;
    bool vinylColours;
};

static PadMap padMap()
{
    const bool driving = win32::g_guestBase[0x007041C4] == 0;
    const bool frontEnd = guestDword(0x0077A920) == 3;   // GAMEFLOW_STATE_IN_FRONTEND
    PadMap map;
    map.race = driving || frontEnd;
    map.menu = !driving;
    map.vinylColours = map.menu && vinylLayerScreen();
    return map;
}

// The slot's tables outside the read: the player's race actions, except that the right stick (hat 1) always has the
// port's shift actions and a race table without any action gets the port's defaults; the port's menu actions.
static void setPadTables(x86::reg32 slot)
{
    bool bound = false;
    for (x86::reg32 i = 0; i < 32; ++i)
        bound = bound || guestDword(slot + RaceButtons + 4 * i) != 0;
    for (x86::reg32 i = 0; i < 16; ++i)
    {
        if (!bound)
        {
            guestDword(slot + RaceButtons + 4 * i) = s_raceActions[i];
            guestDword(slot + RaceHats + 4 * i) = s_raceHat[i % 4];
        }
        if (i / 4 == 1)
            guestDword(slot + RaceHats + 4 * i) = s_raceRightHat[i % 4];
        guestDword(slot + MenuButtons + 4 * i) = s_menuActions[i];
        guestDword(slot + MenuHats + 4 * i) = i / 4 == 1 ? s_menuRightHat[i % 4] : s_menuHat[i % 4];
    }
    if (!bound)
        pf::logInfo("input: the pad has no race actions, the default layout is set");
}

namespace nfsu
{

void sub_4070a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_41c7a0(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

// The profile's settings as 0x0041C7A0 writes them (nothing without a profile, path at 0x00736230): <name>.cfg gets
// the display settings 0x00700E6C..0x00700EBF (BPP .. SOUNDENABLE), PERFORMANCE 0x00736334 and the pad's race
// actions; <name>.opt (0x0041E200) gets the profile 0x007588C4 at +0x10..+0x20F (HUD, audio volumes, music mode,
// camera, transmission, handling, vibration, quick race options), +0x294..+0x363 (EA Trax playlist) and +0xEEEC
// (autosave). +0x6C and +0x70, the playlist's stream masks, change while the music plays and are left out.
struct SettingsPart
{
    x86::reg32 address;
    x86::reg32 size;
    const char* name;
};

static const SettingsPart s_settingsParts[] = {
    {0x00700E6C, 0x54, "display"},
    {0x00736334, 0x04, "display"},
    {0x007588D4, 0x5C, "options"},
    {0x00758938, 0x19C, "options"},
    {0x00758B58, 0xD0, "options"},
    {0x007677B0, 0x04, "options"},
    {0, 32 * 4, "controls"},        // the pad slot's race tables (RaceButtons, RaceHats)
};

struct Settings
{
    x86::reg8 bytes[0x54 + 0x04 + 0x5C + 0x19C + 0xD0 + 0x04 + 32 * 4];
};

static void readSettings(Settings& settings, x86::reg32 slot)
{
    x86::reg8* at = settings.bytes;
    for (const SettingsPart& part : s_settingsParts)
    {
        memcpy(at, win32::g_guestBase + (part.address ? part.address : slot + RaceButtons), part.size);
        at += part.size;
    }
}

// The names of the parts that differ, comma separated.
static void changedParts(const Settings& a, const Settings& b, char* text, size_t size)
{
    text[0] = 0;
    size_t offset = 0;
    for (const SettingsPart& part : s_settingsParts)
    {
        if (memcmp(a.bytes + offset, b.bytes + offset, part.size) != 0 && !strstr(text, part.name))
            snprintf(text + strlen(text), size - strlen(text), "%s%s", text[0] ? ", " : "", part.name);
        offset += part.size;
    }
}

// The game writes the profile's files at profile changes, when it exits, and when the player leaves the options
// only if the career data changed (0x004C9570); settings changed alone are written at the exit, which the Vita does
// not reach (the process ends). So the settings are read when a profile is loaded or changed; afterwards, each time
// the current screen (package name at 0x0072CDD0) has changed while no race is driven and no action is being
// assigned, they are compared with the ones last read or written, and when they differ the current profile's files
// are written. Nothing with benchmark=on.
static bool saveSettings(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 slot)
{
    static Settings s_saved;
    static char s_profile[260];
    static char s_screen[64];
    if (bench::enabled())
        return true;
    const char* profile = reinterpret_cast<const char*>(win32::g_guestBase + 0x00736230);
    if (!profile[0])
    {
        s_profile[0] = 0;
        return true;
    }
    const bool driving = win32::g_guestBase[0x007041C4] == 0;
    if (driving || guestDword(slot + AssignedAction) != 0)
        return true;
    const char* screen = reinterpret_cast<const char*>(win32::g_guestBase + 0x0072CDD0);
    const bool screenChanged = strncmp(screen, s_screen, sizeof(s_screen) - 1) != 0;
    const bool profileChanged = strncmp(profile, s_profile, sizeof(s_profile) - 1) != 0;
    if (!screenChanged && !profileChanged)
        return true;
    strncpy(s_screen, screen, sizeof(s_screen) - 1);
    Settings current;
    readSettings(current, slot);
    if (profileChanged)
    {
        strncpy(s_profile, profile, sizeof(s_profile) - 1);
        s_saved = current;
        return true;
    }
    if (memcmp(current.bytes, s_saved.bytes, sizeof(current.bytes)) == 0)
        return true;
    char parts[64];
    changedParts(current, s_saved, parts, sizeof(parts));
    s_saved = current;
    // At the read's entry eax, ecx and edx hold nothing the read uses; the writer keeps the other registers.
    cpu.esp -= 4;
    sub_41c7a0(app, cpu);
    if (cpu.terminate)
        return false;
    pf::logInfo("settings: %s changed, profile saved (%s and its .opt)", parts, profile);
    return true;
}

// Device read (slot in esi): the action mask from the device's state and the slot's tables. For the pad the tables
// of the map that is not active are empty during the read, in menus the right stick has no race action, and on the vinyl layer screen Triangle (button 0) sends
// action 21; afterwards the slot holds its own tables again.
void sub_4070a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 slot = cpu.esi;
    if (!padSlot(slot))
    {
        sub_4070a0_guest(app, cpu);
        return;
    }
    setPadTables(slot);
    if (!saveSettings(app, cpu, slot))
        return;
    const PadMap map = padMap();
    x86::reg32 race[32];
    memcpy(race, win32::g_guestBase + slot + RaceButtons, sizeof(race));
    if (!map.race)
        memset(win32::g_guestBase + slot + RaceButtons, 0, sizeof(race));
    if (!map.menu)
        memset(win32::g_guestBase + slot + MenuButtons, 0, 32 * 4);
    else
        memset(win32::g_guestBase + slot + RaceHats + 4 * 4, 0, 4 * 4);   // the right stick shifts only while driving
    if (map.vinylColours)
        guestDword(slot + MenuButtons) = 21;
    sub_4070a0_guest(app, cpu);
    memcpy(win32::g_guestBase + slot + RaceButtons, race, sizeof(race));
    setPadTables(slot);
}

}}

// Front end car camera (0x004AE910, joystick handler of the garage screens): events 0x3C / 0x3D set the vertical
// orbit speed to +value / 255 and -value / 255, events 0x3A / 0x3B the horizontal one to -value / 255 and
// +value / 255. Their input scanners (configs at 0x00703B60, 0x20 bytes each: events 0x3C, 0x3D, 0x3A, 0x3B on
// actions 29-32; 0x005744B0) send the config's byte at +0x14 as the value while the action is held (the game's
// 128) and 0 when it is released; a changed byte sends the event again while held.
// In menus hat 1 follows each axis of the right stick on its own past a band along the axis (no drift across it),
// and the value of a held orbit action follows that axis: 1 at the band, 255 (speed 1) at full deflection, which is
// twice the game's value. A direction reversed between two polls reports the axis centred for one poll, so the
// released direction's 0 cannot come after the new direction's value. Outside menus the values are the game's.
const x86::reg32 OrbitValues = 0x00703B74, OrbitConfigSize = 0x20;
static const float s_orbitAxisBand = 0.15f;

static x86::reg8 orbitValue(float position)
{
    const float amount = (std::fabs(position) - s_orbitAxisBand) / (1.0f - s_orbitAxisBand);
    return x86::reg8(std::max(1L, std::lround(std::min(amount, 1.0f) * 255.0f)));
}

static void setOrbitSpeed(bool menu, float x, float y, int* rightPov)
{
    static int s_dx = 0, s_dy = 0;
    x86::reg8 values[4] = { 128, 128, 128, 128 };   // up, down, right, left (actions 29-32)
    int dx = 0, dy = 0;
    if (menu)
    {
        dx = x > s_orbitAxisBand ? 1 : x < -s_orbitAxisBand ? -1 : 0;
        dy = y > s_orbitAxisBand ? 1 : y < -s_orbitAxisBand ? -1 : 0;
        if (dx && dx == -s_dx)
            dx = 0;
        if (dy && dy == -s_dy)
            dy = 0;
        static const int s_angles[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
        *rightPov = s_angles[dy + 1][dx + 1];
        if (dy)
            values[dy < 0 ? 0 : 1] = orbitValue(y);
        if (dx)
            values[dx > 0 ? 2 : 3] = orbitValue(x);
    }
    s_dx = dx;
    s_dy = dy;
    for (x86::reg32 i = 0; i < 4; ++i)
        win32::g_guestBase[OrbitValues + OrbitConfigSize * i] = values[i];
}

static void padPollHook(x86::reg32* buttons, int* pov, int* rightPov, float rightX, float rightY)
{
    pollTextField();
    nfsu::bench::poll();
    nfsu::prompts::poll();
    const PadMap map = padMap();
    const bool race = map.race, menu = map.menu, vinylColours = map.vinylColours;
    // While the controls screen assigns an action the right stick is not reported, so it gets no race action.
    const x86::reg32 slot = findPadSlot();
    const bool assigning = slot && guestDword(slot + AssignedAction) != 0;

    setOrbitSpeed(menu, rightX, rightY, rightPov);
    if (assigning)
        *rightPov = -1;
    const x86::reg32 hatBit = x86::reg32(1) << 31, rightHatBit = x86::reg32(1) << 30;
    const x86::reg32 held = *buttons | (*pov >= 0 ? hatBit : 0) | (*rightPov >= 0 ? rightHatBit : 0);
    static int s_layout = -1;
    static x86::reg32 s_hidden = 0;
    const int layout = (race ? 1 : 0) | (menu ? 2 : 0);
    if (layout != s_layout)
    {
        s_hidden = s_layout < 0 ? 0 : held;
        s_layout = layout;
    }
    s_hidden &= held;
    *buttons &= ~s_hidden;
    if (s_hidden & hatBit)
        *pov = -1;
    if (s_hidden & rightHatBit)
        *rightPov = -1;
    *buttons |= nfsu::bench::heldButtons();

    // Menus: Triangle (button 0) gives the menu bar the key of the screen's extra item, unless the controls screen
    // is assigning an action.
    static x86::reg32 s_previous = 0;
    const x86::reg32 pressed = *buttons & ~s_previous;
    s_previous = *buttons;
    if (menu && !vinylColours && !assigning && (pressed & 1u))
        nfsu::prompts::menuTriangle();
}

int main(int, char**)
{
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir(s_dataDirectory.c_str(), 0777);
    pf::logInit(dataPath("nfsug1vita.log").c_str());
    pf::logInfo("NFSUG1VITA %s starting", NFSU_VERSION);
    pf::logInfo("clocks: arm %d MHz, bus %d MHz, gpu %d MHz, xbar %d MHz", scePowerGetArmClockFrequency(),
                scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load(dataPath("nfsug1vita.cfg").c_str());
    pf::logSetConsole(pf::Config::getBool("console_log", false));
    pf::cores::init();
    // benchmark=on logs its summary without the timing log; benchmark_timing_log=on adds the timing log to a
    // benchmark.
    pf::perf::init(pf::Config::getBool("timing_log", false)
                   || (pf::Config::getBool("benchmark", false) && pf::Config::getBool("benchmark_timing_log", false)),
                   pf::Config::getBool("hitch_log", false));
#ifdef NFSU_PGO_GENERATE
    pf::pgo::start(dataPath(pf::pgo::FileName).c_str());
#endif
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    std::vector<uint8_t> image;
    nfsu::region::Converted region;
    std::vector<win32::Section> sections;
    // With an embedded executable, a file whose data does not match is tried with it (embeddedFallback), which then
    // logs the reason a file is refused.
    uint32_t embeddedSize = 0;
    const bool embedded = nfsu::image::exe(embeddedSize) != nullptr;
    if (!loadExecutable(image))
        pf::fatal("the game executable is missing (see the log)");
    if (!imageSections(image, region, sections, !embedded))
    {
        region = nfsu::region::Converted();
        sections.clear();
        if (!embedded || !embeddedFallback(image, region) || !imageSections(image, region, sections))
            pf::fatal("the game executable is not version 1.4 (see the log)");
    }
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");
    if (pf::Config::getBool("gxm_program_list", true))
        pf::gxm::Device::get().usePrograms(dataPath("gxm_programs.bin").c_str());

    win32::File::setDataDirectory((s_dataDirectory + "/").c_str());
    win32::File::setCdDirectory((s_dataDirectory + "/").c_str());
    win32::registry::load(dataPath("registry.txt").c_str());
    win32::kernel32::setModulePath(s_guestExePath);
    win32::kernel32::setResourceSection(nfsu::layout::imageBase, nfsu::layout::resourceSection);
    nfsu::bankcode::install();
    {
        nfsu::Application app(s_commandLine, sections);
        std::vector<win32::Section>().swap(sections);
        std::vector<uint8_t>().swap(image);
        region = nfsu::region::Converted();
        setPadBindings();
        win32::MemMap::addReport(reportPools);
        setEnvmapSchedule();
        nfsu::prompts::configure();
        nfsu::bench::configure(app);
        nfsu::phases::configure();
        win32::Gamepad::setPollHook(padPollHook);
        if (!win32::File::setCurrentDirectory(s_guestGameFolder))
            pf::fatal("%s (the game folder) is missing", s_guestGameFolder);
        app.execute();
        // The deferred device calls finish before the application goes.
        win32::d3d9::bridge::drain();
    }
    pf::logInfo("NFSUG1VITA exiting");
#ifdef NFSU_PGO_GENERATE
    pf::pgo::stop();
    pf::pgo::write();
#endif
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
    sceKernelExitProcess(0);
    return 0;
}

#include "bankcode.h"
#include "bench.h"
#include "phases.h"
#include "prompts.h"

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

// Reads the executable and checks it is the one the code was generated from.
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
    if (read != image.size() || image.size() != nfsu::layout::exeSize)
    {
        pf::logError("%s: %u bytes, expected %u", exeFile, unsigned(image.size()), nfsu::layout::exeSize);
        return false;
    }
    const std::string hash = pf::sha256Hex(image.data(), image.size());
    if (hash != nfsu::layout::exeSha256)
    {
        pf::logError("%s: SHA-256 %s, expected %s", exeFile, hash.c_str(), nfsu::layout::exeSha256);
        return false;
    }
    return true;
}

// The headers and the raw data of every section at their guest addresses. Each section also gets an entry without
// data that covers its whole size (the arena is zeroed, so the rest of a section reads as zero).
static std::vector<win32::Section> imageSections(const std::vector<uint8_t>& image)
{
    std::vector<win32::Section> sections;
    sections.push_back(win32::Section{ image.data(), nfsu::layout::imageBase, nfsu::layout::headerSize });
    for (const nfsu::layout::ImageSection& s : nfsu::layout::sections)
    {
        if (s.rawSize)
            sections.push_back(win32::Section{ image.data() + s.fileOffset, s.address, s.rawSize });
        sections.push_back(win32::Section{ nullptr, s.address, s.virtualSize > s.rawSize ? s.virtualSize : s.rawSize });
    }
    return sections;
}

// Bindings of the Vita pad, presented as the EMS PS2 adapter (src/lib/controls.cpp). Per button index, then per hat
// direction (down, up, left, right) of each of the 4 hats the game reads. Race: R accelerates, L brakes, Cross is
// nitrous, Square the handbrake, Triangle the camera, Circle looks back, d-pad up/down and the rear touch halves
// shift. Menus: Cross accepts (20), Circle goes back (23), Start is the game's Start (25, also "button 2"), Square
// "button 3" (26), L / R the triggers (27 / 28), d-pad and hat navigate; Triangle presses the menu bar's extra
// item (src/nfsu/prompts.cpp, button_prompts=vita).
static const x86::reg32 s_raceActions[16] = { 10, 9, 6, 5, 8, 7, 2, 1, 14, 12, 0, 0, 7, 4, 8, 3 };
static const x86::reg32 s_menuActions[16] = { 0, 23, 20, 26, 0, 0, 27, 28, 0, 25, 0, 0, 16, 19, 17, 18 };
static const x86::reg32 s_raceHat[4] = { 0, 0, 3, 4 };
static const x86::reg32 s_menuHat[4] = { 17, 16, 18, 19 };

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
    for (x86::reg32 i = 0; i < 16; ++i)
    {
        guestDword(0x00704268 + 4 * i) = s_raceActions[i];
        guestDword(0x007042A8 + 4 * i) = s_menuActions[i];
    }
}

// Env cube faces drawn per frame (0x40AD60, patched in generate.py PATCHES to read the copy written here): frame
// counter [0x7364DC] modulo the divisor of the reflection level [0x700E74] picks one of that level's six rows of six
// face flags. The game's levels: 1 one face a frame, 2 faces 0 and 4 every frame and one of the others, 3 all six.
// car_reflection_interval=n (2..6, default 1: the game's) spreads level 3 over n frames, face f drawn when the counter modulo n
// is f modulo n.
static void setEnvmapSchedule()
{
    const x86::reg32 GameDivisors = 0x00702AAC, GameRows = 0x00702AC0, Divisors = 0x006D3800, Rows = 0x006D3810;
    for (x86::reg32 i = 0; i < 4; ++i)
        guestDword(Divisors + 4 * i) = guestDword(GameDivisors + 4 * i);
    for (x86::reg32 i = 0; i < 18 * 6; ++i)
        guestDword(Rows + 4 * i) = guestDword(GameRows + 4 * i);
    const int rate = std::min(std::max(pf::Config::getInt("car_reflection_interval", 1), 1), 6);
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

// The game builds each device's action mask from both tables of its slot (0x004071B8), whatever map is active, and
// loads the slot tables from the profile (0x0041CDA0; 0x004064D0 then puts the generic menu actions on buttons
// 0-4). The keyboard instead gets one map at a time: 0x004057C0 picks the race map ([0x007041C4] = 0) only while
// a race is driven, the menu map otherwise. At each pad poll the pad's slot is set to the port's layout for that
// map: race actions while driving, menu actions in menus; in the front end the race table stays (it has no
// consumers there and the controls screen shows it). Saved pad bindings are overridden. Inputs held across a
// change stay hidden until released, as the keyboard's map change releases its keys.
static void padPollHook(x86::reg32* buttons, int* pov)
{
    pollTextField();
    nfsu::bench::poll();
    nfsu::prompts::poll();
    const x86::reg32 slots = 0x0078DAA8, slotSize = 0x270, slotCount = 6;
    const bool driving = win32::g_guestBase[0x007041C4] == 0;
    const bool frontEnd = guestDword(0x0077A920) == 3;   // GAMEFLOW_STATE_IN_FRONTEND
    const bool race = driving || frontEnd;
    const bool menu = !driving;
    for (x86::reg32 slot = slots; slot < slots + slotCount * slotSize; slot += slotSize)
    {
        // Channel 0 (the pad), a controller type (0x13 is the keyboard).
        if (guestDword(slot + 0x130) != 0 || guestDword(slot + 0x134) == 0x13)
            continue;
        for (x86::reg32 i = 0; i < 16; ++i)
        {
            guestDword(slot + 0x18 + 4 * i) = race ? s_raceActions[i] : 0;
            guestDword(slot + 0x98 + 4 * i) = menu ? s_menuActions[i] : 0;
        }
        for (x86::reg32 i = 0; i < 16; ++i)
        {
            guestDword(slot + 0x58 + 4 * i) = race ? s_raceHat[i % 4] : 0;
            guestDword(slot + 0xD8 + 4 * i) = menu ? s_menuHat[i % 4] : 0;
        }
    }

    const x86::reg32 hatBit = x86::reg32(1) << 31;
    const x86::reg32 held = *buttons | (*pov >= 0 ? hatBit : 0);
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
    *buttons |= nfsu::bench::heldButtons();

    // Menus: Triangle (button 0) gives the menu bar the key of the screen's extra item.
    static x86::reg32 s_previous = 0;
    const x86::reg32 pressed = *buttons & ~s_previous;
    s_previous = *buttons;
    if (menu && (pressed & 1u))
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
    if (!loadExecutable(image))
        pf::fatal("the game executable is missing or not the supported version (see the log)");
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
        nfsu::Application app(s_commandLine, imageSections(image));
        std::vector<uint8_t>().swap(image);
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

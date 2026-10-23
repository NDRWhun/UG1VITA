#include "bankcode.h"

#include <lib/file.h>
#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sha256.h>
#include <fpu.h>
#include <mmx.h>
#include <nfsu.h>
#include <nfsu_layout.h>
#include <winapi/kernel32.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <sys/stat.h>
#include <cstdio>
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
// "button 3" (26), L / R the triggers (27 / 28), d-pad and hat navigate.
static const x86::reg32 s_raceActions[16] = { 10, 9, 6, 5, 8, 7, 2, 1, 14, 12, 0, 0, 7, 4, 8, 3 };
static const x86::reg32 s_menuActions[16] = { 0, 23, 20, 26, 0, 0, 27, 28, 0, 25, 0, 0, 16, 19, 17, 18 };
static const x86::reg32 s_raceHat[4] = { 0, 0, 3, 4 };
static const x86::reg32 s_menuHat[4] = { 17, 16, 18, 19 };

static x86::reg32& guestDword(x86::reg32 address)
{
    return *reinterpret_cast<x86::reg32*>(win32::g_guestBase + address);
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

// Text fields (docs/re/input.md section 6). Every edit of the game's line editor sets its active flag [0x007363A8],
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
}

int main(int, char**)
{
    const int clock500 = scePowerSetArmClockFrequency(500);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir(s_dataDirectory.c_str(), 0777);
    pf::logInit(dataPath("nfsug1vita.log").c_str());
    pf::logInfo("NFSUG1VITA starting");
    pf::logInfo("arm clock: request 500 MHz returned 0x%08X, running at %d MHz", unsigned(clock500),
                scePowerGetArmClockFrequency());
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load(dataPath("nfsug1vita.cfg").c_str());
    pf::logSetConsole(pf::Config::getBool("tty_log", false));
    pf::cores::init();
    pf::perf::init(pf::Config::getBool("perf_log", false));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    std::vector<uint8_t> image;
    if (!loadExecutable(image))
        pf::fatal("the game executable is missing or not the supported version (see the log)");
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");

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
        win32::Gamepad::setPollHook(padPollHook);
        if (!win32::File::setCurrentDirectory(s_guestGameFolder))
            pf::fatal("%s (the game folder) is missing", s_guestGameFolder);
        app.execute();
    }
    pf::logInfo("NFSUG1VITA exiting");
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
    sceKernelExitProcess(0);
    return 0;
}

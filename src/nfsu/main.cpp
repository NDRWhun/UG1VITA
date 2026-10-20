#include "bankcode.h"

#include <lib/file.h>
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

// Default bindings of the EMS PS2 adapter, the device the Vita pad is presented as (src/lib/controls.cpp): race and menu
// action per button index. Menus: Cross accepts, Circle goes back. Race: R accelerates, L brakes, Cross is nitrous,
// Square the handbrake, Triangle the camera, Circle looks back, d-pad up/down and the rear touch halves shift.
static void setPadBindings(nfsu::Application& app)
{
    static const x86::reg32 raceActions[16] = { 10, 9, 6, 5, 8, 7, 2, 1, 14, 12, 0, 0, 7, 4, 8, 3 };
    static const x86::reg32 menuActions[16] = { 25, 23, 20, 24, 0, 0, 26, 0, 0, 20, 0, 0, 16, 19, 17, 18 };
    const x86::reg32 race = 0x00704268;     // EMS entry 0x00704258: product GUID, 16 race, 16 menu actions
    const x86::reg32 menu = 0x007042A8;
    for (x86::reg32 i = 0; i < 16; ++i)
    {
        app.getMemory<x86::reg32>(race + 4 * i) = raceActions[i];
        app.getMemory<x86::reg32>(menu + 4 * i) = menuActions[i];
    }
}

int main(int, char**)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir(s_dataDirectory.c_str(), 0777);
    pf::logInit(dataPath("nfsug1vita.log").c_str());
    pf::logInfo("NFSUG1VITA starting");
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load(dataPath("nfsug1vita.cfg").c_str());
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
        setPadBindings(app);
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

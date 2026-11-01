#include "bench.h"

#include <lib/controls.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nfsu { namespace bench {

namespace
{

// Skip-front-end race settings read by 0x004B6390 (started from main when [SkipFe] is set).
const x86::reg32 SkipFe = 0x0073465C;
const x86::reg32 SkipFeAiCars = 0x00734660;
const x86::reg32 SkipFeLaps = 0x006F1D4C;
const x86::reg32 SkipFeRaceMode = 0x006F1D7C;       // 1 circuit, sprint and drag; 3 lap knockout
const x86::reg32 SkipFePointToPoint = 0x00734680;
const x86::reg32 SkipFeDrag = 0x00734678;
const x86::reg32 SkipFeTransmission = 0x00734684;   // player 1's gears: 0 automatic, 1 manual
const x86::reg32 ConfigTrack = 0x0070095C;          // track number, -1: the first track table entry
// Race parameters: the two track map mode bytes, both 6 in a front-end drag race (0x004B55B0). The skip-front-end
// race start leaves them as they are.
const x86::reg32 TrackMapModes = 0x0078A3F6;
const x86::reg8 DragTrackMapMode = 6;
// 0x00420B40 (world step) gives player 0's car this control mode each step when nonzero; 3 is the AI controller.
const x86::reg32 OverridePlayerControl = 0x00736208;
const x86::reg32 AiControl = 3;
const x86::reg32 RandomSeed = 0x006F227C;
// Track table: entries of 0xFC bytes, the track number a 16-bit value at 0x6C, the point-to-point byte at 0x69;
// loader of chunk 0x34201.
const x86::reg32 TrackTable = 0x00736168;
const x86::reg32 TrackCount = 0x0073616C;
const x86::reg32 TrackEntrySize = 0xFC;
const x86::reg32 TrackNumber = 0x6C;
const x86::reg32 TrackPointToPoint = 0x69;
const x86::reg32 TrackTableLoader = 0x00442040;
// Car type table (chunk 0x34600, before the track table in GLOBALB): 35 entries of 0xC90 bytes, the name at 0; a player car has a nonzero byte at 0x40
// and 0 at 0xC54 (the test of 0x004AC460).
const x86::reg32 CarTypeTable = 0x00734588;
const x86::reg32 CarTypeCount = 35;
const x86::reg32 CarTypeEntrySize = 0xC90;
const x86::reg32 CarTypeModel = 0x40;
const x86::reg32 CarTypeUsage = 0xC54;
const size_t CarTypeNameSize = 32;
// Player 1's car type in the default profiles, read by 0x004AB2B0 (start-up, after the global chunks load).
const x86::reg32 DefaultPlayerCarType = 0x006F1DA4;
// Race state: coordinator state 4 is the race in progress, race stage 3 the countdown, 4 racing.
const x86::reg32 RaceCoordinator = 0x007361F0;
const x86::reg32 CurrentRace = 0x0073619C;
const x86::reg32 PlayerCount = 0x007361C4;
const x86::reg32 Player0 = 0x007361BC;
const x86::reg32 PlayerCar = 0x04;
const x86::reg32 PlayerFinishTime = 0x58;
const x86::reg32 CarRide = 0x10;                    // ride info: the car type first
const x86::reg32 Quit = 0x00736124;

const uint32_t MaxFrames = 20 * 60 * 60;

enum Mode { Circuit, Sprint, Drag, Knockout, ModeCount };
const char* const ModeNames[ModeCount] = { "circuit", "sprint", "drag", "knockout" };
// Default track and the track numbers' hundreds (10xx circuits, 11xx sprints, 12xx drags) per mode.
const int ModeTracks[ModeCount] = { 1001, 1102, 1201, 1001 };
const int ModeTrackSeries[ModeCount] = { 10, 11, 12, 10 };

enum Driver { AiDriver, ThrottleDriver, DriverCount };
const char* const DriverNames[DriverCount] = { "ai", "throttle" };

// Names accepted for a car type besides its table name, and the name logged for it.
struct CarAlias
{
    const char* alias;
    const char* table;
};
const CarAlias CarAliases[] = { { "peugeot206", "peugot" }, { "peugeot", "peugot" }, { "206", "peugot" } };

enum Phase { Idle, Recording, Stopping, Done };

bool s_enabled = false;
int s_mode = Circuit;
int s_driver = AiDriver;
int s_track = ModeTracks[Circuit];
int s_laps = 2;
int s_opponents = 3;
int s_seconds = 0;
bool s_exit = false;
std::string s_car;
win32::MethodPtr s_trackTableLoader = nullptr;

// Written by the render thread while Recording, read after Done.
std::atomic<int> s_phase{Idle};
std::vector<uint32_t> s_frames;
uint32_t s_frameCount = 0;
uint32_t s_lastPresent = 0;
const char* s_endReason = "";

x86::reg32 dword(x86::reg32 address)
{
    return *reinterpret_cast<const x86::reg32*>(win32::g_guestBase + address);
}

void setDword(x86::reg32 address, x86::reg32 value)
{
    *reinterpret_cast<x86::reg32*>(win32::g_guestBase + address) = value;
}

// The mixing of 0x004470D0 (random seed at start-up) applied to a fixed start value.
x86::reg32 fixedSeed()
{
    const x86::reg32 start = 0x12345678u ^ 0x1D872B41u;
    const x86::reg32 mixed = start ^ (start >> 5);
    return mixed ^ (mixed << 27) ^ start;
}

// Guest address of the track's table entry, 0 if it is not in the table.
x86::reg32 trackEntry(int track)
{
    const x86::reg32 table = dword(TrackTable);
    const x86::reg32 count = dword(TrackCount);
    for (x86::reg32 i = 0; table && i < count; ++i)
    {
        const x86::reg32 entry = table + i * TrackEntrySize;
        const x86::reg32 number = entry + TrackNumber;
        if (int(int16_t(win32::g_guestBase[number] | (win32::g_guestBase[number + 1] << 8))) == track)
            return entry;
    }
    return 0;
}

// Lowercase letters and digits of text.
std::string carKey(const char* text)
{
    std::string key;
    for (; *text; ++text)
    {
        const char c = *text;
        if (c >= 'A' && c <= 'Z')
            key += char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            key += c;
    }
    return key;
}

x86::reg32 carEntry(int type)
{
    const x86::reg32 table = dword(CarTypeTable);
    return table && type >= 0 && x86::reg32(type) < CarTypeCount ? table + x86::reg32(type) * CarTypeEntrySize : 0;
}

bool playerCar(int type)
{
    const x86::reg32 entry = carEntry(type);
    return entry && win32::g_guestBase[entry + CarTypeModel] && dword(entry + CarTypeUsage) == 0;
}

// Lowercase letters and digits of the car type's table name; empty if there is no such type.
std::string tableKey(int type)
{
    const x86::reg32 entry = carEntry(type);
    char name[CarTypeNameSize + 1] = {};
    if (entry)
        memcpy(name, win32::g_guestBase + entry, CarTypeNameSize);
    return carKey(name);
}

// The car type's name as the settings take it: the alias for the table's name, else the table's name in lowercase.
std::string carName(int type)
{
    const std::string key = tableKey(type);
    if (key.empty())
        return "?";
    for (const CarAlias& alias : CarAliases)
    {
        if (key == alias.table)
            return alias.alias;
    }
    return key;
}

// Player car type of a name, alias or type number; -1 if none.
int findCar(const char* text)
{
    std::string key = carKey(text);
    for (const CarAlias& alias : CarAliases)
    {
        if (key == alias.alias)
            key = alias.table;
    }
    if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos)
    {
        const int type = atoi(key.c_str());
        return playerCar(type) ? type : -1;
    }
    for (int type = 0; type < int(CarTypeCount); ++type)
    {
        if (playerCar(type) && tableKey(type) == key)
            return type;
    }
    return -1;
}

// Sets player 1's default car type from benchmark_car.
void applyCar()
{
    if (!dword(CarTypeTable))
    {
        pf::logError("bench: the car type table is not loaded, benchmark_car not applied");
        return;
    }
    if (!s_car.empty())
    {
        const int type = findCar(s_car.c_str());
        if (type >= 0)
            setDword(DefaultPlayerCarType, x86::reg32(type));
        else
        {
            std::string names;
            for (int i = 0; i < int(CarTypeCount); ++i)
            {
                if (playerCar(i))
                    names += (names.empty() ? "" : " ") + carName(i);
            }
            pf::logError("bench: benchmark_car=%s is not a player car (%s), using %s", s_car.c_str(), names.c_str(),
                         carName(int(int32_t(dword(DefaultPlayerCarType)))).c_str());
        }
    }
    const int type = int(int32_t(dword(DefaultPlayerCarType)));
    pf::logInfo("bench: player car %s (car type %d)", carName(type).c_str(), type);
}

// Runs the loader, then once the table is in: sets the track, its point-to-point flag, the random seed, the car
// (0x004AB2B0 reads it after the global chunks), and a drag race's track map modes.
void trackTableLoaderHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_trackTableLoader(app, cpu);
    static bool s_applied = false;
    if (s_applied || !dword(TrackTable))
        return;
    s_applied = true;
    x86::reg32 entry = trackEntry(s_track);
    if (!entry || s_track / 100 != ModeTrackSeries[s_mode])
    {
        pf::logError("bench: track %d is not a %s track in the track table, using %d", s_track, ModeNames[s_mode],
                     ModeTracks[s_mode]);
        s_track = ModeTracks[s_mode];
        entry = trackEntry(s_track);
    }
    setDword(ConfigTrack, x86::reg32(s_track));
    setDword(SkipFePointToPoint, entry ? win32::g_guestBase[entry + TrackPointToPoint] : 0);
    setDword(RandomSeed, fixedSeed());
    applyCar();
    if (s_mode == Drag)
    {
        win32::g_guestBase[TrackMapModes] = DragTrackMapMode;
        win32::g_guestBase[TrackMapModes + 1] = DragTrackMapMode;
    }
}

bool racing()
{
    const x86::reg32 coordinator = dword(RaceCoordinator);
    const x86::reg32 race = dword(CurrentRace);
    return coordinator && race && dword(coordinator) == 4 && dword(race) == 4;
}

x86::reg32 player0()
{
    return dword(PlayerCount) >= 1 ? dword(Player0) : 0;
}

// Render thread.
void framePresented(uint32_t presentUs)
{
    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == Recording && s_lastPresent && s_frameCount < s_frames.size())
        s_frames[s_frameCount++] = presentUs - s_lastPresent;
    s_lastPresent = presentUs;
    if (phase != Stopping)
        return;

    const uint32_t frames = s_frameCount;
    uint64_t total = 0;
    uint32_t max = 0, over33 = 0, over42 = 0, over50 = 0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint32_t t = s_frames[i];
        total += t;
        max = std::max(max, t);
        over33 += t > 33400;
        over42 += t > 41700;
        over50 += t > 50000;
    }
    uint32_t p99 = 0;
    if (frames)
    {
        std::vector<uint32_t> sorted(s_frames.begin(), s_frames.begin() + frames);
        const size_t rank = std::min<size_t>(frames - 1, (size_t(frames) * 99 + 99) / 100 - 1);
        std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.end());
        p99 = sorted[rank];
    }
    const double seconds = double(total) / 1000000.0;
    pf::logInfo("bench: summary (%s): %u frames in %.1f s, %.2f fps | frame avg %.2f ms p99 %.2f max %.2f"
                " | over 33.4 ms %u (%.1f%%), over 41.7 ms (a missed vblank at 30 fps) %u (%.1f%%),"
                " over 50 ms %u (%.1f%%)",
                s_endReason, unsigned(frames), seconds, seconds > 0.0 ? double(frames) / seconds : 0.0,
                frames ? double(total) / 1000.0 / frames : 0.0, double(p99) / 1000.0, double(max) / 1000.0,
                unsigned(over33), frames ? 100.0 * over33 / frames : 0.0,
                unsigned(over42), frames ? 100.0 * over42 / frames : 0.0,
                unsigned(over50), frames ? 100.0 * over50 / frames : 0.0);
    if (s_frameCount == s_frames.size())
        pf::logInfo("bench: frame buffer full, later frames not counted");
    s_phase.store(Done, std::memory_order_release);
}

}

bool enabled()
{
    return s_enabled;
}

void configure(win32::WinApplication& app)
{
    s_enabled = pf::Config::getBool("benchmark", false);
    if (!s_enabled)
        return;
    s_mode = pf::Config::getChoice("benchmark_mode", ModeNames, ModeCount, Circuit);
    s_track = pf::Config::getInt("benchmark_track", ModeTracks[s_mode]);
    s_opponents = std::min(std::max(pf::Config::getInt("benchmark_opponents", 3), s_mode == Knockout ? 1 : 0), 3);
    int laps = 2;
    const bool lapsSet = pf::Config::tryGetInt("benchmark_laps", &laps);
    if (s_mode == Circuit)
        s_laps = std::min(std::max(laps, 1), 10);
    else
    {
        // A knockout race drops one car per lap; sprints and drags are one run.
        s_laps = s_mode == Knockout ? s_opponents : 1;
        if (lapsSet)
            pf::logInfo("bench: benchmark_laps is not used in a %s race", ModeNames[s_mode]);
    }
    s_driver = pf::Config::getChoice("benchmark_driver", DriverNames, DriverCount, AiDriver);
    if (s_driver == ThrottleDriver && s_mode != Drag)
    {
        pf::logError("bench: benchmark_driver=throttle needs benchmark_mode=drag, the AI drives");
        s_driver = AiDriver;
    }
    s_car = pf::Config::getString("benchmark_car", "");
    s_seconds = std::max(pf::Config::getInt("benchmark_seconds", 0), 0);
    s_exit = pf::Config::getBool("benchmark_exit", false);
    pf::logInfo("bench: on, %s race, track %d, %d laps, %d opponents, %s, seed 0x%08X, measure %s, %s",
                ModeNames[s_mode], s_track, s_laps, s_opponents,
                s_driver == AiDriver ? "AI drives the player's car"
                                     : "the race's drag controls with throttle held and automatic gears",
                unsigned(fixedSeed()), s_seconds ? "up to the finish or the time limit" : "up to the finish",
                s_exit ? "quit after the summary" : "keep running after the summary");
    if (s_seconds)
        pf::logInfo("bench: time limit %d s", s_seconds);

    setDword(SkipFe, 1);
    setDword(SkipFeAiCars, x86::reg32(s_opponents));
    setDword(SkipFeLaps, x86::reg32(s_laps));
    setDword(SkipFeRaceMode, s_mode == Knockout ? 3 : 1);
    setDword(SkipFeDrag, s_mode == Drag ? 1 : 0);
    setDword(SkipFeTransmission, 0);
    if (s_driver == AiDriver)
        setDword(OverridePlayerControl, AiControl);
    s_trackTableLoader = app.replaceMethod(TrackTableLoader, trackTableLoaderHook);

    const uint32_t capacity = s_seconds ? std::min<uint32_t>(MaxFrames, uint32_t(s_seconds) * 60 + 600) : MaxFrames;
    s_frames.assign(capacity, 0);
    pf::perf::setFrameObserver(framePresented);
}

void poll()
{
    if (!s_enabled)
        return;
    static uint32_t s_startUs = 0;
    static bool s_quit = false;
    static x86::reg32 s_lastState = ~0u, s_lastStage = ~0u;
    const x86::reg32 coordinator = dword(RaceCoordinator), race = dword(CurrentRace);
    const x86::reg32 state = coordinator ? dword(coordinator) : ~0u, stage = race ? dword(race) : ~0u;
    if (state != s_lastState || stage != s_lastStage)
    {
        pf::logInfo("bench: coordinator state %d, race stage %d", int(int32_t(state)), int(int32_t(stage)));
        s_lastState = state;
        s_lastStage = stage;
    }
    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == Idle)
    {
        if (!racing())
            return;
        s_startUs = pf::perf::now();
        const x86::reg32 player = player0();
        const x86::reg32 car = player ? dword(player + PlayerCar) : 0;
        const x86::reg32 ride = car ? dword(car + CarRide) : 0;
        const int type = ride ? int(int32_t(dword(ride))) : -1;
        pf::logInfo("bench: race started (%s, track %d), player car %s (car type %d), measuring", ModeNames[s_mode],
                    int(int32_t(dword(ConfigTrack))), carName(type).c_str(), type);
        s_phase.store(Recording, std::memory_order_release);
        return;
    }
    if (phase == Recording)
    {
        const x86::reg32 player = player0();
        const char* reason = nullptr;
        if (player && dword(player + PlayerFinishTime))
            reason = "player finished";
        else if (!racing())
            reason = "race left the racing state";
        else if (s_seconds && pf::perf::now() - s_startUs >= uint32_t(s_seconds) * 1000000u)
            reason = "time limit";
        if (!reason)
            return;
        s_endReason = reason;
        pf::logInfo("bench: measurement ended: %s", reason);
        s_phase.store(Stopping, std::memory_order_release);
        return;
    }
    if (phase == Done && s_exit && !s_quit)
    {
        s_quit = true;
        pf::logInfo("bench: quitting");
        setDword(Quit, 1);
    }
}

x86::reg32 heldButtons()
{
    if (!s_enabled || s_driver != ThrottleDriver || !racing())
        return 0;
    return win32::controls::buttonMask(win32::controls::R);
}

}}

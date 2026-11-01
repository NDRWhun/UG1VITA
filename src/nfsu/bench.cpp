#include "bench.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <atomic>
#include <vector>

namespace nfsu { namespace bench {

namespace
{

// Skip-front-end race settings read by 0x004B6390 (started from main when [SkipFe] is set).
const x86::reg32 SkipFe = 0x0073465C;
const x86::reg32 SkipFeAiCars = 0x00734660;
const x86::reg32 SkipFeLaps = 0x006F1D4C;
const x86::reg32 ConfigTrack = 0x0070095C;          // track number, -1: the first track table entry
// 0x00420B40 (world step) gives player 0's car this control mode each step when nonzero; 3 is the AI controller.
const x86::reg32 OverridePlayerControl = 0x00736208;
const x86::reg32 AiControl = 3;
const x86::reg32 RandomSeed = 0x006F227C;
// Track table: entries of 0xFC bytes, the track number a 16-bit value at 0x6C; loader of chunk 0x34201.
const x86::reg32 TrackTable = 0x00736168;
const x86::reg32 TrackCount = 0x0073616C;
const x86::reg32 TrackEntrySize = 0xFC;
const x86::reg32 TrackNumber = 0x6C;
const x86::reg32 TrackTableLoader = 0x00442040;
// Race state: coordinator state 4 is the race in progress, race stage 3 the countdown, 4 racing.
const x86::reg32 RaceCoordinator = 0x007361F0;
const x86::reg32 CurrentRace = 0x0073619C;
const x86::reg32 PlayerCount = 0x007361C4;
const x86::reg32 Player0 = 0x007361BC;
const x86::reg32 PlayerCar = 0x04;
const x86::reg32 PlayerFinishTime = 0x58;
const x86::reg32 CarRide = 0x10;                    // ride info: the car type first
const x86::reg32 Quit = 0x00736124;

const int DefaultTrack = 1001;
const uint32_t MaxFrames = 20 * 60 * 60;

enum Phase { Idle, Recording, Stopping, Done };

bool s_enabled = false;
int s_track = DefaultTrack;
int s_laps = 2;
int s_opponents = 3;
int s_seconds = 0;
bool s_exit = false;
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

bool trackExists(int track)
{
    const x86::reg32 table = dword(TrackTable);
    const x86::reg32 count = dword(TrackCount);
    for (x86::reg32 i = 0; table && i < count; ++i)
    {
        const x86::reg32 entry = table + i * TrackEntrySize + TrackNumber;
        if (int(int16_t(win32::g_guestBase[entry] | (win32::g_guestBase[entry + 1] << 8))) == track)
            return true;
    }
    return false;
}

// Runs the loader, then sets the track and the random seed once the table is in.
void trackTableLoaderHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_trackTableLoader(app, cpu);
    static bool s_applied = false;
    if (s_applied || !dword(TrackTable))
        return;
    s_applied = true;
    if (!trackExists(s_track))
    {
        pf::logError("bench: track %d is not in the track table, using %d", s_track, DefaultTrack);
        s_track = DefaultTrack;
    }
    setDword(ConfigTrack, x86::reg32(s_track));
    setDword(RandomSeed, fixedSeed());
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
    s_track = pf::Config::getInt("benchmark_track", DefaultTrack);
    s_laps = std::min(std::max(pf::Config::getInt("benchmark_laps", 2), 1), 10);
    s_opponents = std::min(std::max(pf::Config::getInt("benchmark_opponents", 3), 0), 3);
    s_seconds = std::max(pf::Config::getInt("benchmark_seconds", 0), 0);
    s_exit = pf::Config::getBool("benchmark_exit", false);
    pf::logInfo("bench: on, track %d, %d laps, %d opponents, AI drives the player's car, seed 0x%08X, "
                "measure %s, %s", s_track, s_laps, s_opponents, unsigned(fixedSeed()),
                s_seconds ? "up to the finish or the time limit" : "up to the finish",
                s_exit ? "quit after the summary" : "keep running after the summary");
    if (s_seconds)
        pf::logInfo("bench: time limit %d s", s_seconds);

    setDword(SkipFe, 1);
    setDword(SkipFeAiCars, x86::reg32(s_opponents));
    setDword(SkipFeLaps, x86::reg32(s_laps));
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
        pf::logInfo("bench: race started (track %d), player car type %d, measuring", int(int32_t(dword(ConfigTrack))),
                    ride ? int(int32_t(dword(ride))) : -1);
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

}}

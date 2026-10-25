#ifndef NFSU_PHASES_H_
#define NFSU_PHASES_H_

#include <lib/winapp.h>

// Phase log (timing_phases=on): game thread time of the main loop's top-level phases and of the render's sub-phases,
// per frame, without the time the thread had the execution lock released (known with the timing log on), plus
// event counts of all threads (clock reads, critical sections, lock releases, preemptions). One log line pair every
// ReportFrames frames. Off by default; a phase costs two clock reads per call when on.

namespace nfsu { namespace phases
{

enum Id
{
    Step,           // 0x420B40 World::DoTimestep
    Move,           //   0x420310 World::DoTimestepMove (every mover)
    PhysicsMove,    //     0x4774A0 PhysicsMover::Move
    MellowMove,     //     0x474A90 MellowMover::Move
    Collide,        //   0x420380 World::DoTimestepCollisions
    Predict,        //   0x420890 World::DoPredictiveCollision
    AiCar,          //   0x44B1B0 AICarController::Think
    AiDriver,       //   0x462430 AIDriver::Think
    ParticleUpdate, //   0x403A60 particle systems: update and quad buffers (also called by the frame render)
    Cameras,        // 0x48E8B0 UpdateCameraMovers
    Sound,          // 0x51D240 EAXSound::Update
    Streamer,       // 0x445050 TrackStreamer::Service
    Loading,        // 0x4483C0 ServiceResourceLoading
    Render,         // 0x40A580 frame render and present
    Views,          //   0x409580 world views
    Cull,           //     0x446620 scenery culler
    Stuff,          //     0x446730 StuffScenery (light fills and model submission)
    Flush,          //     0x40ED50 draw queue flush
    Flares,         //     0x544F30 world light flares
    Cars,           //     0x571050 RenderCars
    ParticleDraw,   //     0x404110 particle group draw
    FrontEnd,       //   0x4F5730 FE package rendering (HUD and menus)
    Count
};

extern bool g_on;

void enterSlow(Id id);
void leaveSlow(Id id);

// Times one call of a phase; nested calls of the same phase count once.
class Scope
{
public:
    explicit Scope(Id id) : m_id(id)
    {
        if (__builtin_expect(g_on, 0))
            enterSlow(id);
    }
    ~Scope()
    {
        if (__builtin_expect(g_on, 0))
            leaveSlow(m_id);
    }

private:
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id m_id;
};

// Reads timing_phases; before guest code runs.
void configure();

// Game thread, after each frame render: counts the frame and logs every ReportFrames frames.
void frame();

}}

#endif

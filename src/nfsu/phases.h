#ifndef NFSU_PHASES_H_
#define NFSU_PHASES_H_

#include <lib/winapp.h>

// Phase log (timing_phases=on): game thread time of the main loop's top-level phases and of the render's sub-phases,
// per frame, without the time the thread had the execution lock released (known with the timing or hitch log on),
// that released time (by reason with the hitch log on), plus event counts of all threads (clock reads, critical
// sections, lock releases, preemptions). One log line pair every ReportFrames frames. Off by default; a phase costs
// two clock reads per call when on. The hitch log (hitch_log=on) turns the phase timing on without the log lines and
// takes each frame's sums.

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
    Witnesses,      //     0x469580 DynamicCollisionBody::MaybeCreateWitnesses
    ContactTest,    //     0x468060 CollisionBodyWitness::TestForContact
    ContactResolve, //     0x46B5E0 Contact::Resolve
    Gjk,            //     0x471DE0 SCEA_GJKdistance (contact tests and contact times)
    SimplexDistance, //      0x46EA50 DistanceSqr of a simplex pair
    Impulse,        //     0x46BAE0 Contact::CollisionImpulse
    AiCar,          //   0x44B1B0 AICarController::Think
    AiDriver,       //   0x462430 AIDriver::Think
    ParticleUpdate, //   0x403A60 particle systems: update and quad buffers (also called by the frame render)
    Topology,       //   0x577010 MobileTopologyUpdate
    Avoidables,     //   0x57A530 Avoidable::Update (each avoidable)
    RideInfo,       //   0x42A790 ride physics info rebuild
    Draft,          //   0x426450 Car::CalculateDraftFactor
    CarForces,      //   0x429620 Car::CalcForceParameters
    BeginMove,      //   0x4267A0 Car::BeginMovement
    FinishMove,     //   0x426930 Car::FinishMovement
    Smokeables,     //   0x595670 SmokeableThink
    CarEvents,      //   0x4281E0 Car::TriggerEvents
    TerrainEffects, //   0x4286A0 Car::TriggerCarTerrainEffects
    Skids,          //   0x589F00 ServiceFlamingSkids
    EventManager,   //   0x58E040 emProcessAllEvents
    PlayerState,    //   0x430730 Player::UpdateGameState
    SpaceNodes,     //   0x5793E0 ServiceSpaceNodes
    RaceUpdate,     //   0x435570 Race::Update
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
    Clans,          //     0x58A240 RenderClans (in views)
    WorldModels,    //     0x592100 RenderWorldModels (in views)
    CarReflections, //     0x570DB0 RenderCarReflections (in views)
    MiniWorld,      //     0x4AE650 MiniWorld::Render (in views)
    CarFlares,      //     0x570D00 RenderFlares (cars) (in views)
    CarRender,      //       0x56DC30 CarRenderInfo::Render (cars, reflections)
    CarShadow,      //       0x56CF10 0x56D0C0 0x56D700 car shadow hull, corners and setup
    PartCull,       //       0x569460 CarPartCuller::CullParts
    LightContext,   //       0x5437F0 elSetupLightContext
    LightClone,     //       0x544390 elCloneLightContext
    WorldModel,     //       0x591E40 WorldModel::Render
    SpaceNode,      //       0x5791C0 SpaceNode::Update
    Submit,         //       0x40C910 model submit (every caller)
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

// Reads timing_phases; before guest code runs, after pf::perf::init.
void configure();

// Game thread, after each frame render: hands the frame's phase sums to the hitch log, counts the frame and logs
// every ReportFrames frames.
void frame();

// Collision frames (timing_phases=on): the frames with at least one contact resolve. Reset starts the count, log
// writes the average and maximum of the step and collision phases in those frames.
void resetCollisionFrames();
void logCollisionFrames();

}}

#endif

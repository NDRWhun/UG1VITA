"""Generates the C++ recompilation of Need for Speed Underground (Speed.exe) into one directory.

Usage: generate.py --binaries <dir> --out <dir> [--analysis <file.json>]

<dir> must hold Speed.exe. Its SHA-256 is checked: the tables below fit only this file. The routines of the audio
banks in <dir>/Sound are translated too (bankcode.py).

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import collections
import filecmp
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
import bankcode  # noqa: E402
from disasm import analysis, codegen, discover, fpustack, image as pe_image, writer  # noqa: E402

EXE_NAME = 'Speed.exe'
EXE_SHA256 = 'c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7'
MODULE = 'nfsu'

# MSVC CRT setjmp (_setjmp3) and the indirect jump in longjmp that resumes after it. Calls to setjmp save a host
# jump buffer; the longjmp jump restores it (include/recomp.h). Speed.exe links _setjmp3 (0x6844ac) and longjmp
# (0x684430, resuming at 0x6844a5 through edx), but no code or data refers to either.
SETJMP = None
LONGJMP_JUMPS = {}                      # jump address -> register holding the guest jmp_buf

# Functions that never return although their code has a path to ret: the CRT exits leave through doexit and a
# function pointer. Code after calls to them is padding.
NO_RETURN = [
    0x6720ea,   # exit
    0x6720fb,   # _exit
    0x670c6c,   # _amsg_exit
]

# Guest functions replaced by hand-written native code (native::sub_<address>, src/nfsu/native.cpp); the translated
# body stays as sub_<address>_guest.
NATIVE_OVERRIDES = [
    0x672fc8,   # _ftol: st0 to a 64-bit integer, truncating (sets and restores the x87 control word)
    0x674898,   # _ftol2: the same result without a control word change
    0x6734a0,   # _chkstk / _alloca_probe: page-probing stack allocation
    0x647cb0,   # movie YUV420 -> ARGB converter: the guest has only an MMX path
    0x413270,   # movie frame to the movie texture: converted straight into the locked texture
    # Movie decoder kernels (src/nfsu/movie_decode.cpp): the inverse DCT, the intra coefficient decoder and the block
    # moves between bytes and the 16.16 work buffers.
    0x64abc0,   # 8x8 inverse DCT in 16.16 fixed point
    0x64b110,   # intra coefficients of a block
    0x64a694,   # 8x8 block of the DC value
    0x64a71f,   # 8x8 bytes to 16.16 values in a 16-wide buffer
    0x64a7f0,   # 8x8 bytes to 16.16 values in an 8-wide buffer
    0x64a8c1,   # 16x16 16.16 values to clamped bytes
    0x64aaa5,   # 8x8 16.16 values to clamped bytes
    0x4086b0,   # back buffer size: the display's with widescreen=on, and the 2D mapping the patches below read
    0x4f5730,   # end of an FE package's rendering: places the HUD packages' objects before the render list is drawn
    0x514b90,   # FE render object draw: its polygons at the object's place (centred, screen edge or full width)
    0x58e810,   # track map draw: the whole map at its place
    0x4f3030,   # FE mouse pointer draw: keeps the pointer state, draws nothing (no pointer on the Vita)
    0x4070a0,   # device read: the pad's action tables of the active input map only (src/nfsu/main.cpp)
    0x409cd0,   # FE background view: cleared first on the magazine screen
    0x4322a0,   # race parameters: add a driver (benchmark_tuned fits the ride first)
    0x46d1c0,   # drive train shift points: results kept per input (physics.cpp)
    0x580240,   # theoretical top speed of a ride (physics.cpp)
    0x478570,   # PhysicsMover::IterateForces: wheel ground forces and clutch torque per substep (physics.cpp)
    0x585810,   # RigidBody::ComputeAuxiliaries per substep (physics.cpp)
    0x585410,   # RigidBody::GetDerivative per substep (physics.cpp)
    0x588580,   # Suspension::ComputeAuxiliaries per wheel and substep (physics.cpp)
    0x628b30,   # character animation: bone matrices of a pose (anim.cpp)
    0x6315f0,   # character animation: keyframe decoder of codec 7 (anim.cpp)
    0x425150,   # movement mode of a racing car (the benchmark keeps the AI-driven player car on full physics)
    0x416a10,   # Customize Display Settings: value change of a row (Motion Blur: off, on, enhanced; display.cpp)
    0x4169b0,   # Customize Display Settings: the rows' values (Motion Blur: Enhanced; display.cpp)
    0x410a00,   # default display settings for the graphics card, then Motion Blur and Anti-Aliasing off (display.cpp)
    0x415120,   # Display Settings: value change of the Resolution row (Anti-Aliasing with widescreen=on; display.cpp)
    0x4151c0,   # Display Settings: the Resolution row's value (Anti-Aliasing with widescreen=on; display.cpp)
    # D3DX (include/winapi/d3dx9.h): effects without the guest HLSL compiler; the math entries are jumps through
    # the PSGP dispatch table.
    0x5af9e5,   # D3DXCreateEffectPool
    0x5afb2f,   # D3DXCreateEffect
    0x5ac69c,   # D3DXMatrixMultiply
    0x5abdaf,   # D3DXVec3Transform
    0x5ac443,   # D3DXVec4Transform
    0x5ac8e5,   # D3DXMatrixTranspose
    # EA SND mixer (src/nfsu/audio_mixer.cpp): the render loop without the execution lock, the parts that check
    # what they reach, the x87 kernels of the voice render.
    0x654050,   # render loop: ticks, millisecond callbacks, voice render and stream notifications
    0x64e330,   # SND tick
    0x64e2f0,   # millisecond callbacks (the bank tick)
    0x655880,   # stream notifications to the game's callbacks
    0x64bbd0,   # object release: listeners, then the free at the last reference
    0x656d70,   # render frames into the DirectSound buffer
    0x65a880,   # dst += src * gain
    0x65a9a0,   # 16-bit -> float
    0x661f60,   # linear interpolation resampler
    0x65fad0,   # EA-XA decoder
    0x65b3f0,   # float -> 16-bit output module
    # Scenery culling (src/nfsu/cull.cpp): the per-frame scenery culler, its visibility of each (view, section) is
    # computed on worker threads; the box against frustum test is native for every caller.
    0x446620,   # scenery culler: every view, every visible section
    0x40db20,   # bounding box against the view frustum: 0 outside, 1 partial, 2 inside
    # Track topology queries (src/nfsu/coll.cpp): ground height and polygon tests for wheels, cameras, AI and
    # effects, and the polygon cache rebuild around a point.
    0x440a40,   # ground height under a point from a topology coordinate's polygon cache
    0x4400e0,   # distance of a point outside a track polygon
    0x4407b0,   # interest box and polygon cache of a topology coordinate
    # Math helpers (src/nfsu/mathfn.cpp): trigonometry by 16-bit angle, lookups, vectors and boxes.
    0x564b10,   # sine of a 16-bit angle
    0x564bb0,   # sine of a float in radians
    0x564bd0,   # sine and cosine of a 16-bit angle
    0x564cd0,   # arcsine as a 16-bit angle
    0x564db0,   # angle of a 2D vector
    0x4387e0,   # float truncated to an integer, as a float
    0x583ea0,   # table value, linear between entries
    0x583f20,   # graph value, linear between points
    0x567370,   # normalise a vector
    0x5416e0,   # irradiance: add a coloured direction's spherical harmonic terms
    0x567450,   # normalise a vector with the w term
    0x567310,   # normalise a 2D vector to a length
    0x540830,   # length with the w term
    0x5677d0,   # grow a box by a point and radius
    0x567740,   # grow a box by a point
    0x567710,   # box of one point
    0x458750,   # first overlap time of two moving boxes
    0x4733a0,   # box of a collision body's vertices
    # AI planning (src/nfsu/aiplan.cpp).
    0x5a5c30,   # future points: lateral limits of each point from the one after it
    # Render preparation (src/nfsu/prep.cpp): per-instance light-context fills on the workers.
    0x446730,   # StuffScenery: fill light contexts and submit the view's instances
    0x40a580,   # frame render and present
    # Light contexts (src/nfsu/lights.cpp): elSetupLightContext and elCloneLightContext are listed with the phases.
    0x542e40,   # elSetupLights: table and database lights, spherical harmonic coefficients
    0x542c20,   # elSetupEnvMap: target direction and environment matrix of a light context
    0x542400,   # light context copied with its directions and coefficients rotated
    # Resource loading (src/nfsu/load.cpp): the JDLZ decompressor, the 8-bit palette texture expansion and the car
    # skin compositor with its pixel blend.
    0x58d190,   # JDLZ decompressor
    0x407db0,   # texture creation: palette textures expanded to A8R8G8B8, levels copied into the D3D texture
    0x5a8a50,   # CompositeSkin32: car skin layers blended into a 32-bit texture
    0x5a8640,   # weighted sum of up to n pixels
    # Light flares (src/nfsu/flares.cpp): the world pass with per-flare visibility on the workers, one flare, and
    # one flare quad.
    0x544f30,   # world light flares of a view
    0x5447c0,   # one light flare: intensity, visibility and its quads
    0x5443f0,   # one flare quad and its submission
    # Draw queue flush setup functions (src/nfsu/draw.cpp): transforms, states, effect constants and slot-pool
    # releases of each packet and node, with direct device and effect calls (include/winapi/d3d9_native.h).
    0x410400,   # world, view and projection transforms of a packet
    0x410550,   # blend enable, source and destination
    0x410650,   # alpha test enable and reference
    0x410700,   # packets of a node back to the packet pool
    0x40ed10,   # a node's packets back to the pool
    0x40e6d0,   # effect group switch
    0x4102f0,   # bone palette of a skinned mesh
    0x40f3d0,   # model texture, alpha, blend, cull and z-write states, texture offset
    0x40f7c0,   # light context: fixed-function lights or the harmonic coefficients
    0x4100a0,   # reflection matrices
    0x40fae0,   # material: fixed-function material or the diffuse, specular and envmap terms
    # Effect group functions and model texture swaps (src/nfsu/groups.cpp).
    0x40e740,   # group start: headlight and black textures
    0x40e7b0,   # group start: white texture, gloss and the view's texture projection
    0x40e900,   # group start: headlight, black and white textures, projection or gloss
    0x40eb30,   # group start: overlay texture
    0x40eb60,   # group start: texture transforms off
    0x53f540,   # a model's replacement textures into its solid
    0x53f500,   # the solid's own textures back
    # Parts of the world views (src/nfsu/viewparts.cpp).
    0x408f00,   # scenery culler setup and call
    0x409180,   # full-screen quad of the rear-view mirror frame and mask
    0x408b20,   # screen pass states after the mirror and the player view
    0x4090a0,   # fog of a view (0x588f40)
    0x40cd40,   # motion blur
    0x40d090,   # motion blur pass: a texture n times with stepped coordinates into a target
    0x40d370,   # motion blur pass: a texture n times with texel offsets into a target
    0x40d6e0,   # screen filter pass
    0x409b70,   # screen pass of the player view's flares
    # Phase log (src/nfsu/phases.cpp, timing_phases): the guest bodies timed per call.
    0x420b40,   # World::DoTimestep
    0x420310,   # World::DoTimestepMove
    0x4774a0,   # PhysicsMover::Move
    0x474a90,   # MellowMover::Move
    0x420380,   # World::DoTimestepCollisions
    0x420890,   # World::DoPredictiveCollision
    0x44b1b0,   # AICarController::Think
    0x462430,   # AIDriver::Think
    0x48e8b0,   # UpdateCameraMovers
    0x51d240,   # EAXSound::Update
    0x445050,   # TrackStreamer::Service
    0x4483c0,   # ServiceResourceLoading
    0x409580,   # world views of the frame render
    0x571050,   # RenderCars
    0x58a240,   # RenderClans (phase timing)
    0x592100,   # RenderWorldModels (phase timing)
    0x570db0,   # RenderCarReflections (phase timing)
    0x570950,   # the cars of the render list (src/nfsu/cars.cpp)
    0x4ae650,   # MiniWorld::Render (phase timing)
    0x570d00,   # RenderFlares (cars) (phase timing)
    0x56dc30,   # CarRenderInfo::Render (cars, reflections) (src/nfsu/cars.cpp)
    0x40f0f0,   # user-pointer packet enqueue of the car shadows (src/nfsu/cars.cpp)
    0x56cf10,   # car shadow hull, corners and setup (phase timing)
    0x56d0c0,   # car shadow hull, corners and setup (phase timing)
    0x56d700,   # car shadow hull, corners and setup (phase timing)
    0x569460,   # CarPartCuller::CullParts (phase timing)
    0x5437f0,   # elSetupLightContext: native light context (lights.cpp), phase timing
    0x544390,   # elCloneLightContext: native (lights.cpp), phase timing
    0x591e40,   # WorldModel::Render (phase timing)
    0x5791c0,   # SpaceNode::Update (phase timing)
    0x577010,   # MobileTopologyUpdate (phase timing)
    0x57a530,   # Avoidable::Update (phase timing)
    0x42a790,   # ride physics info rebuild (phase timing)
    0x426450,   # Car::CalculateDraftFactor (phase timing)
    0x429620,   # Car::CalcForceParameters (phase timing)
    0x4267a0,   # Car::BeginMovement (phase timing)
    0x426930,   # Car::FinishMovement (phase timing)
    0x595670,   # SmokeableThink (phase timing)
    0x4281e0,   # Car::TriggerEvents (phase timing)
    0x4286a0,   # Car::TriggerCarTerrainEffects (phase timing)
    0x589f00,   # ServiceFlamingSkids (phase timing)
    0x58e040,   # emProcessAllEvents (phase timing)
    0x430730,   # Player::UpdateGameState (phase timing)
    0x5793e0,   # ServiceSpaceNodes (phase timing)
    0x435570,   # Race::Update (phase timing)
    0x469580,   # DynamicCollisionBody::MaybeCreateWitnesses (phase timing)
    0x468060,   # CollisionBodyWitness::TestForContact (phase timing)
    0x46b5e0,   # Contact::Resolve (phase timing)
    0x471de0,   # SCEA_GJKdistance (gjk.cpp, phase timing)
    0x46ea50,   # DistanceSqr of a simplex pair (gjk.cpp, phase timing)
    0x472f30,   # support point of a polytope (gjk.cpp)
    0x473a90,   # support point of a wafer (gjk.cpp)
    0x468fe0,   # DynamicCollisionBody::Interpolate (contact.cpp)
    0x472830,   # the matrices of a witness (contact.cpp)
    0x46bae0,   # Contact::CollisionImpulse (phase timing)
    # Particle draw (src/nfsu/particles.cpp): the quads of every particle on the workers, the draws in order.
    0x404110,   # particle system list draw for a view
    0x403a60,   # particle system update: emitters, then every particle's motion
    # Draw queue (src/nfsu/flush.cpp): the model submission, the packet enqueue with its slot-pool pops and the
    # flush of the queue with direct device and effect calls.
    0x40c910,   # model submit: cull and enqueue each mesh part
    0x40f1e0,   # enqueue a packet under its model's node
    0x40ed50,   # flush the draw queue
    # Scene parts of the world views (src/nfsu/worldmodels.cpp; 0x592100, 0x591e40, 0x58a240, 0x4ae650 are listed
    # with the phases): the textured quad submit, the FE flares, and the car flare recorded in shadow compares.
    0x40c300,   # textured quad: transforms, texture and a fan of four vertices
    0x570c70,   # FE flares: the car flares of the FE cars of a view
    0x4ae7d0,   # MiniWorld::RenderFlares
    0x56d8d0,   # car flare of a car (guest body; recorded by the shadow compares of the FE flares)
]

# Byte patches applied to the code before discovery: (address, original bytes, new bytes) in hex. Generation stops
# when the executable's bytes differ from the original ones.
PATCHES = [
    # Projection builder 0x40DD00: views 1, 2, 4, 5 (players and their reflections) keep the vertical angle of a 4:3
    # screen (aspect 0.75 [0x6CCA08]) and scale the horizontal term _11 by k = (back buffer height / width) / 0.75;
    # k stays on the x87 stack below the far distance until _11 is stored. Other views keep their target's aspect
    # (types 8 and 9 the fixed 0.6002) with k = 1.
    (0x40ddcf, '8b4558 db4018 c74424200000803f da7014 8b4504 83f801 740f 83f804 740a 83f802 7405 83f805 7518'
               ' d905747c6b00 8b0d707c6b00 d8356c7c6b00 894c2420 dec9 83f808 7405 83f809 7508 ddd8 d905f0cc6c00',
               '8b4504 b936000000 0fa3c1 721f d9e8 d9542420 2c08 3c01 760b 8b4d58 db4118 da7114 eb2e'
               ' d905f0cc6c00 eb26 db0538107000 da3534107000 d90508ca6c00 dcf9 d905747c6b00 d9542420'
               ' d8356c7c6b00 dec9'),
    (0x40de59, 'd8e9', 'd8ea'),                                 # far - near: far is st2
    (0x40de65, 'd8f9', 'd8fa'),                                 # far / (far - near)
    (0x40de9b, 'ddd8', 'ddd9'),                                 # drops far, keeps k
    (0x40debb, 'd83dbcc76c00 d91f', 'd8f9 d91f ddd8 9090'),     # _11 = k / tan, then k is popped
    # 0x40DCB0, the view's pixel scale (scenery LOD, flares): (height / 2) / (0.75 tan) instead of (width / 2) / tan.
    (0x40dcdb, 'a134107000 d1f8 89442400 da7c2400', 'd80d0ccc6c00 da3d38107000 909090'),
    # 0x42F020, camera framing: the vertical extent is 0.75 of the horizontal one instead of height / width.
    (0x42f09e, 'db0538107000 d8ca da3534107000', 'd90508ca6c00 d8ca 909090909090'),
    # 2D polygons 0x40BEF0: x of the 640x480 space times [0x6D3440] minus [0x6D3444] (sub_4086b0 in native.cpp;
    # sub_514b90 and sub_58e810 change them around the HUD's draws).
    (0x40bf07, 'd80d14c96c00', 'd80d40346d00'),
    (0x40bf15, 'd825bcc76c00', 'd82544346d00'),
    (0x40bf98, 'd80d14c96c00', 'd80d40346d00'),
    (0x40bf9e, 'd825bcc76c00', 'd82544346d00'),
    (0x40bffd, 'd80d14c96c00', 'd80d40346d00'),
    (0x40c009, 'd825bcc76c00', 'd82544346d00'),
    (0x40c051, 'd80d14c96c00', 'd80d40346d00'),
    (0x40c062, 'd825bcc76c00', 'd82544346d00'),
    # Full-screen quad of the rear-view mirror frame and mask 0x409180: world _11 = [0x6D3444], as the 2D polygons.
    (0x409274, 'c74424200000803f c744242400000000', '8b0d44346d00 894c2420 8364242400 90'),
    # Env cube face schedule 0x40AD60: the update divisor per reflection level [0x702AAC] and the face rows
    # [0x702AC0] are read from a copy at 0x6D3800 / 0x6D3810 (nfsu::envmap, car_reflection_interval).
    (0x40af38, 'f7348dac2a7000', 'f7348d00386d00'),
    (0x40af64, '8d04c5c02a7000', '8d04c510386d00'),
    # Magazine screen: the 3D pass clear test 0x4098A2 reads a copy at 0x6D3A00 of [0x73645C], 0 while the magazine is
    # shown (phases.cpp sub_409580), so the background the FE view cleared and drew stays (native.cpp sub_409cd0).
    (0x4098a2, '833d5c64730001', '833d003a6d0001'),
    # Display setting limits 0x410B70: with motion blur available, the limit of MOTIONBLUR [0x701064] is 2 instead of 1
    # (enhanced, display.cpp); the limit is only compared with 0 and clamps the setting (0x410CE0, 0x415F90).
    (0x410c3e, '893564107000 893568107000 5e c3 90909090', 'c70564107000 02000000 893568107000 5e c3'),
]

# Guest addresses in or after the read-only data that the port writes at run time (HUD mapping 0x6D3440 in
# native.cpp, the env cube schedule copies at 0x6D3800, the magazine clear copy at 0x6D3A00): x87 operands there are
# loaded from memory. Operands at other fixed addresses of read-only sections become constants (--load-constants).
MUTABLE_CONSTANTS = [(0x6d3440, 8), (0x6d3800, 0x200), (0x6d3a00, 4)]

# Busy-wait loops: the generated code releases the execution lock there so other guest threads can run.
THREAD_SEGMENTS = []

# Net x87 pushes of functions the depth analysis cannot follow (they leave through a computed jump).
FPU_DELTAS = {}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = []

# Entries that no indirect call reaches although a data dword points at them.
NOT_INDIRECT = [
    0x4e4f4e,   # 'NON\0' tag in the string table at 0x6b159c, inside code
]

# Targets of indirect jumps whose callers read flags (site -> entries).
JUMP_TARGETS = {
    # CRT x87 class dispatch: jmp [edx + 0x10 + class] through the descriptors 0x6ebe4a, 0x6ec04a
    0x67a465: [0x67be80, 0x67a4fc, 0x67a512, 0x67bef4, 0x67daf1, 0x67a50d, 0x67dc31],
    # and through descriptor 0x6eb490 (two operands)
    0x67a4f1: [0x673824, 0x67a5a6, 0x67a53f, 0x67a504, 0x67a53d, 0x67a567],
}

# SIMD code exists only behind CPU feature tests (docs: cpu_threads.md 1.2); the runtime reports no MMX, SSE or
# 3DNow!, so these ranges trap if reached. A SIMD instruction outside them counts as unsupported.
SIMD_GATED = [
    (0x5b0000, 0x620000, 'D3DX: PSGP tables 0x6d4ac0, MMX blitter probe'),
    (0x624000, 0x627000, 'EA render command kernels (SSE/3DNow! opcodes chosen at 0x6212a0)'),
    (0x63f000, 0x649000, 'EA memcpy/memset dispatchers, EA MMX helpers, movie MMX kernels'),
    (0x64f000, 0x663000, 'audio library mixers (flags 0x713eeb, 0x7172f4)'),
    (0x673000, 0x680000, 'CRT libm SSE2 paths (0x792fb8)'),
    (0x682f00, 0x683100, 'D3DX MMX blitter (0x6ecd04)'),
]

# Names of the generated files: <module>.cpp/.h, <module>.<bucket>.cpp, the layout header and the bank routines.
GENERATED_FILE = re.compile(r'^%s(\.[0-9a-f]+)?\.(cpp|h)$|^%s_layout\.h$|^%s_(bank|image)\.cpp$' % (MODULE, MODULE, MODULE))


class GameModule:
    """The executable as the analyses and the writer see it."""
    def __init__(self, image, subroutines):
        self.application_name = MODULE
        self.image = image
        self.entry_point = image.entry_point
        self.exported_symbols = []
        self.subroutines = subroutines


def check_binary(directory):
    path = os.path.join(directory, EXE_NAME)
    if not os.path.isfile(path):
        sys.exit('missing %s' % path)
    # Checked before parsing: other files are refused whatever they contain.
    with open(path, 'rb') as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    if sha256 != EXE_SHA256:
        sys.exit('%s: SHA-256 %s, expected %s' % (path, sha256, EXE_SHA256))
    return pe_image.Image(path)


def write_image(directory, stage):
    """Writes <module>_image.cpp: the executable the code was generated from, byte for byte, behind NFSU_EMBED_EXE.
    With it the port also runs from an executable whose data it cannot read itself (a copy-protected 1.4 build,
    main.cpp embeddedFallback): the data then comes from this copy, the player's executable only identifies the
    build. Without the define exe() returns no executable."""
    with open(os.path.join(directory, EXE_NAME), 'rb') as f:
        data = f.read()
    padded = data + bytes(-len(data) % 4)
    words = [int.from_bytes(padded[i:i + 4], 'little') for i in range(0, len(padded), 4)]
    lines = ['// Generated by tools/recompiler: %s (SHA-256 %s), embedded with NFSU_EMBED_EXE.' % (EXE_NAME, EXE_SHA256),
             '#include "embedded.h"', '', 'namespace %s { namespace image {' % MODULE, '',
             '#ifdef NFSU_EMBED_EXE',
             '// Little-endian words, read as bytes (the guest and the hosts are little-endian).',
             'alignas(4) static const uint32_t s_words[] = {']
    for i in range(0, len(words), 8):
        lines.append('    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
    lines += ['};', '',
              'const uint8_t* exe(uint32_t& size)', '{',
              '    size = %d;' % len(data),
              '    return reinterpret_cast<const uint8_t*>(s_words);', '}',
              '#else',
              'const uint8_t* exe(uint32_t& size)', '{', '    size = 0;', '    return nullptr;', '}',
              '#endif', '', '}}', '']
    with open(os.path.join(stage, '%s_image.cpp' % MODULE), 'w', newline='\n') as f:
        f.write('\n'.join(lines))


def apply_patches(image):
    for address, original, patched in PATCHES:
        original, patched = bytes.fromhex(original), bytes.fromhex(patched)
        s = image.section_of(address)
        if s is None or len(original) != len(patched):
            sys.exit('patch 0x%x: no section or lengths differ' % address)
        offset = address - s.address
        if s.data[offset:offset + len(original)] != original:
            sys.exit('patch 0x%x: bytes %s, expected %s'
                     % (address, s.data[offset:offset + len(original)].hex(), original.hex()))
        s.data = s.data[:offset] + patched + s.data[offset + len(patched):]


def simd_range(address):
    return next((r for r in SIMD_GATED if r[0] <= address < r[1]), None)


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions),
                          'no_return': a in discovery.no_return, 'address_taken': a in program.address_taken})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    rejected = [{'address': a, 'origin': o, 'reason': r} for a, (o, r) in sorted(discovery.rejected.items())]
    sources = {'code_refs': {a: sorted(s) for a, s in discovery.code_refs.items()},
               'data_pointers': {a: sorted(s) for a, s in discovery.data_pointers.items()},
               'string_pointers': {a: sorted(s) for a, s in discovery.string_pointers.items()}}
    traps = [{'address': a, 'mnemonic': m, 'class': k} for a, m, k in sorted(set(codegen.simd_sites))]
    code_ranges = []
    for a in sorted(discovery.code):
        end = a + discovery.decoded[a].size
        if code_ranges and a <= code_ranges[-1][1]:
            code_ranges[-1][1] = max(code_ranges[-1][1], end)
        else:
            code_ranges.append([a, end])
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables, 'gaps': discovery.gaps(),
                   'rejected': rejected, 'sources': sources, 'simd_traps': traps,
                   'unsupported': sorted(set(codegen.unhandled_sites)), 'rounds': discovery.rounds,
                   'index_tables': discovery.index_tables, 'code_ranges': code_ranges,
                   'mnemonics': dict(mnemonics), 'no_return': sorted(discovery.no_return)}, f)


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert GENERATED_FILE.match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and GENERATED_FILE.match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


def report(discovery, module, program):
    """Statistics and every place the generated code traps. Returns the number of problems: unsupported
    instructions in reachable code, SIMD outside the gated ranges and sites breaking the flag convention."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.decoded[a].size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s); %d no-return, %d address-taken; %d discovery rounds'
          % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items())),
             len(discovery.no_return), len(program.address_taken), discovery.rounds))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    reasons = collections.Counter(discovery.rejected.values())
    print('pointer candidates rejected: %d (%s); text dwords skipped: %d'
          % (len(discovery.rejected), ', '.join('%s/%s %d' % (o, r, n) for (o, r), n in sorted(reasons.items())),
             len(discovery.string_pointers)))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    calls = fpustack.audit(program)
    print('indirect calls: %d; import table %d (host, no check); float result inferred %d, none inferred %d '
          '(checked: %s); open %d and no static x87 depth %d (stack in cpu.fpu, no check)'
          % (sum(calls.values()), calls.get(('import', 0), 0), calls.get(('inferred', 1), 0),
             calls.get(('inferred', 0), 0), 'yes' if program.indirect_fpu_checked else 'no',
             sum(n for (k, _), n in calls.items() if k == 'open'),
             sum(n for (k, _), n in calls.items() if k == 'unknown')))
    problems = 0
    if program.abi_flags:
        flags = analysis.C.ALL_FLAGS
        readers = sorted(a for a, m in program.live_in.items() if m & flags)
        producers = sorted(a for a, m in program.ret_live.items() if m & flags)
        contract = program.flag_contract()
        print('flag convention: %d functions read flags at entry, %d return flags; indirect jumps with flags: %d '
              '(%d functions write them back)' % (len(readers), len(producers), len(contract['indirect_tails']),
                                                  len(program.indirect_returners)))
        for kind, sites in contract.items():
            if kind == 'indirect_tails':
                continue
            for a, m in sites:
                print('FLAG CONVENTION BROKEN (%s): 0x%x %s' % (kind, a, analysis.state_names(m)))
                problems += 1
    traps = collections.defaultdict(set)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[getattr(ins, 'reason', None) or ins.mnemonic].add(ins.address)
    simd = set(a for a, _, _ in codegen.simd_sites)
    for name, sites in sorted(traps.items()):
        if sites & simd:
            continue
        sites = sorted(sites)
        print('trap: %-28s %4d site(s): %s' % (name, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    by_range = collections.Counter()
    for a, m, kind in sorted(set(codegen.simd_sites)):
        r = simd_range(a)
        if r is None:
            print('UNGATED SIMD instruction: 0x%x %s (%s)' % (a, m, kind))
            problems += 1
        else:
            by_range[(r[2], kind)] += 1
    for (where, kind), n in sorted(by_range.items()):
        print('gated SIMD trap: %-6s %5d in %s' % (kind, n, where))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in set(codegen.unhandled_sites))
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))
        problems += len(set(codegen.unhandled_sites))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with %s' % EXE_NAME)
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--flags-across-calls', action='store_true',
                        help='pass flags through calls and returns as any code may read them (for comparisons)')
    parser.add_argument('--partial-reads', action='store_true',
                        help='count partial register writes as reads of the whole register (for comparisons)')
    parser.add_argument('--load-constants', action='store_true',
                        help='x87 operands of read-only data loaded from memory, not folded (for comparisons)')
    parser.add_argument('--preempt-call', action='store_true',
                        help='loop preemption points call the yield directly, not through the stub (for comparisons)')
    parser.add_argument('--alias-cpu', action='store_true',
                        help='no restrict on the cpu parameter of translated functions (for comparisons)')
    parser.add_argument('--flag-audit', action='store_true',
                        help='check at run time that every flag read from cpu was stored there (logs the reader)')
    parser.add_argument('--fpu-check', action='store_true',
                        help='assert the x87 depth after every call whose depth is known (finds leaking callees)')
    args = parser.parse_args()
    started = time.time()
    image = check_binary(args.binaries)
    apply_patches(image)
    print('%s: SHA-256 ok, %d sections, %d relocations, %d imports, %d patches'
          % (EXE_NAME, len(image.sections), len(image.relocations), len(image.imports), len(PATCHES)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    discovery = discover.Discovery(image, cs, no_return=NO_RETURN)
    discovery.discover(extra_entries=NATIVE_OVERRIDES + ([SETJMP] if SETJMP is not None else []))
    print('discovery took %.0f s' % (time.time() - started))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    module = GameModule(image, subroutines)
    mutable = MUTABLE_CONSTANTS + [(a, len(bytes.fromhex(o))) for a, o, _ in PATCHES]
    program = analysis.Program([module], THREAD_SEGMENTS, [], {}, FLAGS_PRESERVED, NOT_INDIRECT, NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=SETJMP, longjmp_jumps=LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=FPU_DELTAS, preempt_loops=True,
                               address_taken=discovery.address_taken, abi_flags=not args.flags_across_calls,
                               jump_targets=JUMP_TARGETS, flag_audit=args.flag_audit, partial_reads=args.partial_reads,
                               fold_constants=not args.load_constants, mutable_constants=mutable,
                               restrict_cpu=not args.alias_cpu, preempt_stub=not args.preempt_call)
    program.analyze()
    # Every direct call and tail call must reach a generated function.
    missing = sorted(set(x.target for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                         if x.kind in ('call', 'tail') and x.target not in program.functions))
    if missing:
        sys.exit('calls to addresses that are no function: %s' % ' '.join('0x%x' % a for a in missing[:20]))
    with tempfile.TemporaryDirectory(prefix='nfsugen-') as stage:
        writer.Writer(module, program, stage).write()
        bankcode.write(args.binaries, image, capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32), stage)
        write_image(args.binaries, stage)
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    problems = report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))
    if problems:
        sys.exit('%d unsupported or ungated instruction(s) or flag convention break(s) in reachable code' % problems)


if __name__ == '__main__':
    main()

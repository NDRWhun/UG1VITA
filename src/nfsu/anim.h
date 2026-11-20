#ifndef NFSU_ANIM_H_
#define NFSU_ANIM_H_

namespace nfsu { namespace anim
{

// Native character animation of the game (the bone matrices of a pose, the keyframe decoder of codec 7),
// bit-exact with the translated guest code: every x87 chain in double in the guest's order, floats where the guest
// stores floats. The overrides (generate.py NATIVE_OVERRIDES) also leave the registers and x87 state the translated
// code leaves. Settings: anim_native=off runs the guest code, shadow_anim=on runs both and logs the first
// differences.

}}

#endif

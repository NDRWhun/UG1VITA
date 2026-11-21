#ifndef NFSU_VIEWPARTS_H_
#define NFSU_VIEWPARTS_H_

#include <lib/winapp.h>

namespace nfsu { namespace viewparts
{

// Native parts of the world views 0x409580, bit-exact with the translated guest code: the scenery culler setup
// 0x408f00 (with 0x53e290 / 0x408e30; the culler 0x446620 runs through its entry), the fog of the player view 0x4090a0
// (with FogVolume::CalculateDistantFogInfo 0x588f40), the screen state reset 0x408b20, the rear-view mirror quad
// 0x409180, the motion blur 0x40cd40 with its passes 0x40d370 and 0x40d090, the screen filter pass 0x40d6e0 (also the
// frame render's) and the flare screen pass 0x409b70 (the view begin 0x40abd0 inline). The queue flush 0x40ed50, the
// flare draws 0x544f30, 0x4ae7d0, 0x570d00 and 0x570c70 and the culler run through their entries and are recorded
// callees of the shadow compare. Their entries are the generated ones (native overrides). view_parts_native=off
// runs the guest code; shadow_view_parts=on runs each call as guest code first, then natively against it
// (sceneshadow.h), and logs the differences. Under another capture (shadow_views) they run as guest code.

// At the end of a benchmark: the shadow compare totals.
void logShadowTotal();

}}

#endif

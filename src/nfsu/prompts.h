#ifndef NFSU_PROMPTS_H_
#define NFSU_PROMPTS_H_

#include <x86.h>

// Button prompts: button_prompts=vita (default) shows the Vita buttons of the port's fixed layout where the game
// shows PC keys, button_prompts=pc keeps the game's own prompts. Glyph art: app0:glyphs.bin.
//  - Menu bar: the key-cap textures of Next (Enter) and Back (Backspace) become Cross and Circle; the other key
//    caps become empty and the menu bar items no Vita button reaches are not shown.
//  - Fonts conduitmditc_tt21i and conduitmditc_tt14i get glyph characters (codes 0x81-0x95) drawn into the free
//    rows of their textures; strings that name PC keys are replaced with ones that use them, and the joystick's
//    button and axis names (the controls screen) are glyphs.

namespace nfsu { namespace prompts
{

// Reads button_prompts and the glyph art and patches the menu bar table; after the image is mapped, before guest
// code runs.
void configure();

// Texture record t (0x407db0's eax) before its texture is created: replaces the pixel data of the key caps and
// draws the glyphs into the font textures.
void texture(x86::reg32 t);

// Game thread, at each pad poll: adds the glyphs to loaded fonts and replaces strings of a loaded language table.
void poll();

}}

#endif

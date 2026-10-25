#ifndef NFSU_PROMPTS_H_
#define NFSU_PROMPTS_H_

#include <x86.h>

// Button prompts: button_prompts=vita (default) shows the Vita buttons of the port's fixed layout where the game
// shows PC keys, button_prompts=pc keeps the game's own prompts. Glyph art: app0:glyphs.bin.
//  - Menu bar: the key-cap textures of Next (Enter) and Back (Backspace) become Cross and Circle, the letter keys of
//    the screens' extra items Triangle and Quit's Select (menuButton gives the menu bar those keys); change colour,
//    which has no key, is not shown.
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

// Game thread, at each pad poll: adds the glyphs to loaded fonts and replaces strings of a loaded language table;
// a menu bar key from menuButton that the menu bar did not read since the last poll is dropped.
void poll();

enum MenuButton { MenuTriangle, MenuSelect };

// Game thread, a press of Triangle or Select while the menu input map is active: gives the menu bar the key of the
// current screen's item for that button (Triangle: its extra item, or the tutorial in the quick race mode select;
// Select: Quit), when the item is shown and no text field is being edited.
void menuButton(MenuButton button);

}}

#endif

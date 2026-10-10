#pragma once
// Batman City: freeze DirectInput GAME CONTROLLER reads while the VR menu is open (the game
// reads pads through DirectInput as well as XInput). Keyboard and mouse are never touched.

#include <cstdint>

namespace bvr::input::dinput_block {

void install();            // once, at mod start
uint32_t blocked_reads();  // controller reads frozen / event batches dropped

} // namespace bvr::input::dinput_block

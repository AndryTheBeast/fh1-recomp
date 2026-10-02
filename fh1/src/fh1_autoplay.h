// fh1 - scripted controller for unattended test runs.
//
// --fh1_autoplay="<entry>;<entry>;..." adds a virtual pad that holds buttons on a timetable, so
// tools/auto_test.ps1 can get from the title screen into the game with nobody at the PC.
// Each entry is  START+DURATION=CONTROLS  in seconds from launch, CONTROLS joined with ','.
// Controls: a b x y start back lb rb ls rs up down left right, lt / rt (full trigger),
// lx=V ly=V rx=V ry=V (stick, V from -1 to 1). Example: tap Start at 30 s, then hold the
// accelerator from 60 s to 180 s:
//   --fh1_autoplay="30+0.3=start;60+120=rt"
// Its input is merged with any real controller (see fh1_merge_controllers).

#pragma once

#include <memory>
#include <string>

#include <rex/input/input_driver.h>

namespace fh1 {

// Parses the script; returns nullptr (and logs why) if it is empty or invalid.
std::unique_ptr<rex::input::InputDriver> CreateAutoplayDriver(const std::string& script);

}  // namespace fh1

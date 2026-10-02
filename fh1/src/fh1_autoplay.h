// fh1 - scripted controller for unattended test runs.
//
// Two ways to drive the virtual pad (it is merged with any real controller, see
// fh1_merge_controllers):
//
// 1. --fh1_autoplay="<entry>;<entry>;..." - a timetable from launch. Each entry is
//    START+DURATION=CONTROLS in seconds, CONTROLS joined with ','. Example: tap Start at 30 s,
//    hold the accelerator from 60 s to 180 s:  --fh1_autoplay="30+0.3=start;60+120=rt"
//
// 2. --fh1_autoplay_file=<script> - a script run line by line (tools/autoplay/*.txt):
//      wait S                    wait S seconds (decimals allowed)
//      tap CONTROLS [xN] [gap S] press 0.12 s, release, wait 0.25 s (or S); N times
//      hold CONTROLS S           hold for S seconds, then release them
//      set CONTROLS              hold until 'clear' (sticks and triggers too)
//      clear                     release everything set
//      shot NAME                 ask tools/auto_test.ps1 for a screenshot named NAME
//      log TEXT                  write TEXT to the log ([autoplay])
//      def NAME ... end          define a macro;  do NAME [xN]  runs it
//      include FILE              run another script (path relative to this one)
//      quit                      end the test now (auto_test stops the game)
//    '#' starts a comment. CONTROLS: a b x y start back lb rb ls rs up down left right,
//    lt rt (full trigger) or lt=V rt=V (0..1), lx=V ly=V rx=V ry=V (stick, -1..1).
//    Screenshots and quit go through request files in --fh1_autoplay_dir (set by auto_test).

#pragma once

#include <memory>
#include <string>

#include <rex/input/input_driver.h>

namespace fh1 {

// Parses the timetable; returns nullptr (and logs why) if it is empty or invalid.
std::unique_ptr<rex::input::InputDriver> CreateAutoplayDriver(const std::string& script);

// Loads a script file; returns nullptr (and logs why) if it cannot be read or parsed.
std::unique_ptr<rex::input::InputDriver> CreateAutoplayScriptDriver(const std::string& path,
                                                                    const std::string& dir);

}  // namespace fh1

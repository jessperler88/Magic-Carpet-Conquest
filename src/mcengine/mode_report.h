// Match report of a mode run (port-only, read-only): a plain-text summary of a Conquest match for whoever tunes
// the bots and the world - the setup, the result, one row per wizard, what every computer wizard spent its ticks
// on, a timeline (castle level / castle mana / mana total every 30 s of match time), the events (castles founded,
// lost and grown, deaths with the killer, eliminations, spells learnt) and a census of the world at the end.
//
// The recorder watches the simulation from outside (mode_report_tick after every tick of a mode run); it never
// writes game state and is not part of it (no checksum, no save state: a loaded state's report starts at the load).
// The console's `report` command and the end of a match (mcport) write mode_report_text() to
// <save dir>/reports/ and the clipboard.
#pragma once
#include <string>

// After every simulation tick of a mode run. Starts a new recording by itself when a new match began (the mode
// tick went back or the seed changed).
void mode_report_tick();
// The report of the match being recorded (still running or just over): false when nothing was recorded.
bool mode_report_text(std::string *out);
// "conquest_s<seed>_<size>_<bots>bots_t<tick>" - a file name stem for the recorded match ("" = none).
std::string mode_report_name();

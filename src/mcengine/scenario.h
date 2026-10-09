// Debug command language and scenario files (Phase 4, round 10 task B; docs/analysis/port_console.md - the
// user documentation of every command). Engine side, no SDL: the in-game console (mcport/console.*), the
// stdin reader of headless runs, `run file.scn` / MC_SCENARIO in mcport and scenario_test all go through it.
//
// One line = one command. A command is one of
//   - a simulation command (teleport, spawn, give, god, kill, claim, heal, spells, win, lose, damage, cmd,
//     cheat, book, close, packet): becomes a command packet (debug_cmd.h builders) that the host queues
//     (debug_cmd_queue) - it changes the game only inside a tick, recorded / replayed / lockstep-safe;
//   - a host command (time, shot, save, load, sync, dump, cam, overlay, inspect, run, quit, echo): a HostCmd
//     the host carries out (mcport main.cpp; headless hosts may ignore some);
//   - a query (where, find, count, players, queue) or an assertion (assert_*): reads the state between ticks,
//     never writes it;
//   - in scenario files only: wait N and the local player's input verbs of reference_player_test (steer, keys,
//     cast, left, right, respawn, rebuild, face, fly, faceth, face_class).
// `help` / `help <command>` print the table below (cmd_help).
//
// Coordinates of debug commands are cells (0..255, the cell centre) unless written with a `w` suffix (world
// units: 0x100 per cell, e.g. `3200w`); heights (teleport z) are world units. The input verbs keep
// reference_player_test's world units. Numbers may be decimal or 0x hex. `@N` (or `@me`) anywhere selects the
// target player of a simulation command (default: the local player who sends it).
#pragma once
#include "mc_types.h"
#include "mode.h"
#include <cstdint>
#include <string>
#include <vector>

// ---- host commands ------------------------------------------------------------------------------------------
enum class HostOp { NONE, TIME, SHOT, SAVE, LOAD, SYNC, DUMP, CAM, OVERLAY, INSPECT, RUN, QUIT, ECHO };
struct HostCmd {
    HostOp      op = HostOp::NONE;
    // TIME: "pause" "resume" "toggle" "step" "speed" "normal"; CAM: "free" "follow" "to" "off";
    // INSPECT: "slot" "cursor" "off"; OVERLAY: the overlay name ("list" = list them)
    std::string sub;
    std::string text;   // SHOT: file name ("" = automatic); RUN: path; ECHO: text; TIME speed: "4", "1/2", "max"...
    int  n = 0;         // TIME step: ticks; SAVE / LOAD: slot; CAM follow / INSPECT slot: Thing slot (-1 = none)
    int  x = 0, y = 0;  // CAM to: world units
    int  on = -1;       // OVERLAY: 1 on, 0 off, -1 toggle
};

// ---- assertions -----------------------------------------------------------------------------------------------
// Who an assertion / query looks at.
struct CmdWho {
    enum Kind { PLAYER, CASTLE, SLOT } kind = PLAYER;
    int n = -1;         // PLAYER / CASTLE: player record (-1 = the local player); SLOT: Thing slot
};
enum CmdOp { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
enum class AssertKind { HEALTH, MANA, COUNT, ALIVE, DEAD, POS, STATUS, GOD };
struct Assertion {
    AssertKind kind = AssertKind::HEALTH;
    CmdWho who;
    int     op = OP_EQ;
    int64_t value = 0;
    bool    value_max = false;   // HEALTH / MANA: compare with max_health / mana_total
    int     cls = -1, type = -1; // COUNT (-1 = any type)
    int     x = 0, y = 0, r = 0; // POS: world units, radius (Chebyshev, world units)
    int     status = 0;          // STATUS: 0 running (no won / lost bit), 2 won, 4 lost
    bool    on = true;           // GOD
};

// ---- input verbs (reference_player_test grammar) ------------------------------------------------------------
enum class InputVerb { STEER, KEYS, LEFT, RIGHT, RESPAWN, REBUILD, FACE, FLY, FACE_THING, FACE_CLASS };
struct InputLine {
    InputVerb verb = InputVerb::STEER;
    int a = 0, b = 0, c = 0;
    bool has_c = false;
};

// ---- one parsed line -----------------------------------------------------------------------------------------
enum class CmdKind { EMPTY, INVALID, PACKET, HOST, QUERY, ASSERT, INPUT, WAIT, TEXT };
struct ParsedCommand {
    CmdKind     kind = CmdKind::EMPTY;
    std::string verb;           // the first word ("spawn", "assert_count", ...)
    std::string error;          // INVALID: what is wrong (usage included)
    CmdPacket   packet{};       // PACKET
    HostCmd     host;           // HOST
    Assertion   check;          // ASSERT
    InputLine   input;          // INPUT
    int         wait = 0;       // WAIT: ticks
    std::string text;           // TEXT (help); QUERY: the whole line (cmd_query parses it again)
    std::string source;         // the line as written (comment stripped)
};

// Parses one line (no tick prefix; '#' starts a comment). Pure: reads no game state, so a scenario can be
// parsed before its level exists.
ParsedCommand cmd_parse(const std::string &line);
// Runs a QUERY against the current state (between ticks; read-only). Returns the output text (may be several
// lines).
std::string cmd_query(const ParsedCommand &q);
// Evaluates an assertion against the current state (read-only). `detail` gets "health of p0 = 10000 (== max)".
bool cmd_check(const Assertion &a, std::string *detail);
// Help: the command table ("" = the overview), from the same table the parser uses.
std::string cmd_help(const std::string &topic);
// Command names starting with `prefix` (console Tab completion).
std::vector<std::string> cmd_complete(const std::string &prefix);

// Class / type names: "creature", "dragon", "mana_ball", "castle", numbers ... (cls / type -1 = not found).
bool cmd_parse_class_type(const std::vector<std::string> &w, size_t *i, int *cls, int *type, bool type_required);
std::string cmd_thing_name(int cls, int type);           // "Creature Dragon (5,0)"

// ---- scenario files ------------------------------------------------------------------------------------------
// Grammar (docs/analysis/port_console.md has the full description):
//   # comment                         header lines (anywhere):
//   level L                           campaign level L (0-based) from its first tick
//   rts seed S [bots B] [size N] [map M] [humans H] [reseed]
//       [teams T] [win W] [mana-pct P] [hold S] [time M] [respawn S] [respawn-level S] [desecrate S|off]
//       [bot-skill K] [result]
//                                     a Conquest mode run (debug commands always on); the match rules as mcport's
//                                     `rts --teams ...` (rts_run.h, mode_level.h mode_match_option); `result`: mcport
//                                     shows the result screen when the level ends instead of quitting
//   stop T                            end after tick T (default: when the last line ran)
//   name TEXT                         shown in reports
//   T <command>                       the command in tick T (simulation commands are queued before tick T,
//   T-T2 <command>                    run in tick T, or later while the command slot is busy; assertions /
//                                     host commands / queries see the state after tick T, once every packet of
//                                     an earlier line ran). A range repeats a simulation command every tick
//                                     and holds an input verb over the ticks.
//   <command>                         untimed: at the cursor (starts at 1), which `wait N` advances by N
// Ticks count from the scenario start: with a `level` / `rts` header they are PlayerRec.tick (tick 1 = the
// first tick of the level, which carries the join packets, so a packet queued for it runs in tick 2).
struct ScenarioItem {
    int tick = 0, tick_end = 0;  // tick_end > tick only for input verbs
    int line = 0;
    int order = 0;               // file order (stable sort key)
    int packets_before = 0;      // packet items before this one (sorted order): they must have run first
    ParsedCommand cmd;
};
struct Scenario {
    std::string name;            // file name (failures are reported as name:line)
    std::string title;           // `name` header ("" = none)
    int  level = -1;             // `level L`
    bool rts = false;            // `rts ...`
    ModeParams rts_params;
    bool rts_result = false;     // `rts ... result` (round 12)
    int  stop = 0;              // 0 = when the items are done
    std::vector<ScenarioItem> items;     // sorted by (tick, order); no INPUT items
    std::vector<ScenarioItem> inputs;    // the input verbs (ranges)
    std::string error;           // parse error ("name:line: ...")
};
bool scenario_parse(const std::string &text, const std::string &name, Scenario *out);
bool scenario_load(const std::string &path, Scenario *out);

// What one step of a running scenario produced.
struct ScenarioStep {
    std::vector<CmdPacket>   packets;    // queued (debug_cmd_queue) by this step
    std::vector<HostCmd>     host;       // for the host (shot, dump, time, save, ...)
    std::vector<std::string> failures;   // "spawn_kill.scn:12: assert_count creature == 3: found 2"
    std::vector<std::string> output;     // passed assertions, queries, echo, refused packets
    bool done = false;                   // the scenario has ended (this step or earlier)
};

class ScenarioRunner {
public:
    void start(const Scenario &s);
    void stop();                          // abandon (the host quits, `run` of another file)
    // After tick `tick` of the scenario (ticks run since start; call once with 0 before the first tick):
    // evaluates what is due and queues the packets of the next tick (debug_cmd_queue).
    ScenarioStep step(int tick);
    bool running() const { return running_; }
    bool done() const { return done_; }
    int  failures() const { return failures_; }
    int  passed() const { return passed_; }
    int  tick() const { return tick_; }
    const Scenario &scenario() const { return s_; }
    // The scenario drives the local player (it has input verbs): the host installs scenario_local_input as
    // g_hook_player_local_input while it runs (and restores its own afterwards).
    bool has_input() const { return !s_.inputs.empty(); }
    void local_input();                   // the local player's packet of the tick being run (tick() + 1)
    std::string summary() const;          // "spawn_kill.scn: 12 assertions passed, 0 failed"
private:
    Scenario s_;
    size_t next_ = 0;
    int tick_ = 0;
    bool running_ = false, done_ = false;
    int failures_ = 0, passed_ = 0;
    int queued_ = 0;              // packets this run queued (debug_cmd_queue)
    int refused_ = 0;             // ... and packets refused (counted as run)
};

// The runner the hosts share (console `run`, MC_SCENARIO) and its input hook.
ScenarioRunner &scenario_runner();
void scenario_local_input();              // scenario_runner().local_input()

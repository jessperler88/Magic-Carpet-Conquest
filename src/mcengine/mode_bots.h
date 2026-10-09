// Bots v1 of the Conquest mode (Phase 4 round 12, task C; docs/port/BRIEFING_round12.md). Port-only.
//
// The computer wizards of a mode run are the original's wizard AI (ai_wizard.cpp, `is_computer`) with the base
// game's castle loop. This module adapts them to a Conquest match without changing the original's AI for the
// campaign / network game: the skill (MatchRules.bot_skill -> the player block's aggression / accuracy / reaction),
// allies (same_side: no attacks on a team mate, its castle / mana / creatures), and whatever a bot needs to play a
// generated world end to end (castle site, respawn through mode_rules, roaming for mana on a big world).
#pragma once
#include <cstdint>

// The level start of a mode run, after the player blocks are set up (mode_level.cpp's hook): bot skill.
void mode_bots_level_start();
// The skill a MatchRules.bot_skill value plays at (0 -> 2 normal; 1 easy .. 4 hard) and its name.
int  mode_bot_skill(uint8_t bot_skill);
const char *mode_bot_skill_name(int skill);     // "easy", "normal", "hard", "brutal"
// The skill applied to one level player block (0xd8 bytes: +4 aggression, +8 reaction, +0xc accuracy, int16 each,
// 0..255). Skill 2 changes nothing; 1 lowers every knob, 3 / 4 raise them toward 255 (mode_bots.cpp).
void mode_bots_apply_skill(uint8_t *blk, int skill);

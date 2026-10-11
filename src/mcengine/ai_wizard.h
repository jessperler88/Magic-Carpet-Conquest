// Computer-controlled wizard (class 3 type 1, Table A state 1): player_type1_s1_update_11de0 and the
// ai_* functions 0x11f20..0x15590. Owner: ai_wizard.cpp (round 4). Report: docs/analysis/port_ai_wizard.md.
//
// Every tick the handler runs player_ai_wizard_tick (housekeeping, movement, spell acquisition,
// dodging), dispatches on the AI mode byte P.ai_mode (+0x19f) and finally ai_choose_goal, which may
// switch the mode. Modes: 0 choose a goal, 1 upgrade own castle, 3 fly to the chosen castle site,
// 4 approach target, 6 collect a mana ball, 7 attack a castle, 8 / 9 / 0xd attack a wizard / type-3
// player / creature, 0xb return home, 0xc idle cruise; 2, 5 and 0xa do nothing.
// Target = Thing.target (+0x92) with the signature of the targeted Thing in Thing.unk94 (+0x94, see
// thing_signature); Thing.home (+0x96) = the castle site chosen by ai_find_castle_site.
// Spell ids (class-12 types): 0 fireball, 1 heal, 2 speed-up, 3 possession, 4 shield, 7 meteor,
// 8 volcano, 0xc invisible, 0xe rebound, 0xf lightning, 0x10 castle, 0x11 skeleton army.
#pragma once
#include "thing.h"

// Binds the class-3 type-1 Table A handler (0x11de0) and installs g_hook_ai_record_threat
// (ai_record_threat_from_projectiles_150f0, called by thing_update_all once per sub-step).
void ai_wizard_register_handlers();

// ---- the handler and its tick ------------------------------------------------------------------
void player_type1_s1_update(Thing *t);          // player_type1_s1_update_11de0 (Table A class 3 state 1)
int  player_ai_wizard_tick(Thing *t);           // player_ai_wizard_tick_11f20: 0 when the wizard died (state 2), else 1
int  ai_choose_goal(Thing *t);                  // ai_choose_goal_12330 (always 1)

// ---- mode handlers (the return values are ignored by the handler) --------------------------------
int ai_mode1_upgrade_castle(Thing *t);          // ai_mode1_upgrade_castle_12470
int ai_mode3_fly_to_castle_site(Thing *t);      // ai_mode3_fly_to_castle_site_12560
int ai_mode4_approach_target(Thing *t);         // ai_mode4_approach_target_12600
int ai_mode12_idle(Thing *t);                   // ai_mode12_idle_12680 (casts speed-up when ready)
int ai_mode11_return_home(Thing *t);            // ai_mode11_return_home_126f0
int ai_mode6_collect_mana(Thing *t);            // ai_mode6_collect_mana_12830
int ai_mode7_attack_castle(Thing *t);           // ai_mode7_attack_castle_12950
int ai_mode8_attack_wizard(Thing *t);           // ai_mode8_attack_wizard_12a90 (modes 8, 9, 0xd through the thunk 12a80)

// ---- goals (1 = target chosen; the caller sets the mode) -----------------------------------------
int ai_find_castle_site(Thing *t);              // ai_find_castle_site_12bd0 -> Thing.home
int ai_goal_repair_castle(Thing *t);            // ai_goal_repair_castle_12d70 (no caller in retail)
int ai_goal_upgrade_castle(Thing *t);           // ai_goal_upgrade_castle_12df0
int ai_goal_collect_mana(Thing *t);             // ai_goal_collect_mana_12e90
int ai_goal_retreat(Thing *t);                  // ai_goal_retreat_12f70
int ai_goal_attack_castle(Thing *t);            // ai_goal_attack_castle_13000
int ai_goal_attack_wizard(Thing *t);            // ai_goal_attack_wizard_13210
int ai_goal_attack_type3(Thing *t);             // ai_goal_attack_type3_13440
int ai_goal_creature_near_rival(Thing *t);      // ai_goal_creature_near_rival_13600 (no caller in retail)
int ai_goal_hunt_creature(Thing *t);            // ai_goal_hunt_creature_13770
int ai_goal_default(Thing *t);                  // ai_goal_default_13a20 (sets mode 0xb or 0xc itself)
// Port (round 12, mode runs only; 0 otherwise): a visible live enemy wizard (returns 2, mode 8) or else a live
// creature of another side (returns 1, mode 0xd) within 24 cells of the own castle -> target; between the upgrade
// and the castle attack goals.
int ai_goal_defend_castle(Thing *t);
// Port (mode runs only; 0 otherwise): a spell urn of a spell the AI casts and the wizard lacks, within reach of its
// castle (further with every castle level) -> target, mode 4; between the attack goals and the mana.
int ai_goal_fetch_spell(Thing *t);
// Port (mode runs only; 0 otherwise): ai_goal_collect_mana for a ball within 48 cells of the own castle; after the
// defence, before the castle attacks.
int ai_goal_collect_near(Thing *t);
// Port (mode runs only; 0 otherwise): ai_goal_defend_castle for any creature of another side within 24 cells of the
// castle, attacking or not; after ai_goal_collect_near.
int ai_goal_farm_creatures(Thing *t);
int ai_set_mode(Thing *t, int mode);            // the 14 setters ai_set_mode0_13880 .. ai_set_mode11_13a00 (always 1)

// ---- spells --------------------------------------------------------------------------------------
Thing *ai_get_spell_thing(const Thing *t, int spell);   // ai_get_spell_thing_13ac0: P.spell_thing[spell] or null
int    ai_spell_ready(Thing *t, int spell);             // ai_spell_ready_14640
int    ai_castle_spell_ready(Thing *t);                 // ai_castle_spell_ready_14980 (upgrade variant: mana_total, castle site)
int    ai_spell_active(Thing *t, int spell);            // ai_spell_active_14aa0: owned and cast_ticks > 0
int    ai_can_afford_spell(Thing *t, int spell);        // ai_can_afford_spell_14ad0: spell.mana_total <= wizard.mana_total
int    ai_cast_spell(Thing *t, int spell);              // ai_cast_spell_14240: 1 when the cast was started
void   ai_spawn_spells(Thing *t);                       // ai_spawn_spells_14b00: P.ai_want_spell countdowns -> new spell Things
Thing *ai_spell_in_progress(const Thing *t, int spell); // ai_spell_in_progress_14c40: the spell Thing while cast_ticks > 0
int    ai_choose_attack_spell(Thing *t);                // ai_choose_attack_spell_14c70: spell id or 0xff
int    ai_choose_castle_attack_spell(Thing *t);         // ai_choose_castle_attack_spell_14f00: spell id or 0xff
int    ai_has_any_attack_spell(const Thing *t);         // ai_has_any_attack_spell_154e0: owns one of 0, 0xf, 8, 0x11, 7
// Aim tolerance of ai_spell_ready / ai_castle_spell_ready in angle units: ((255 - accuracy) / 4 + 20) degrees.
inline int ai_aim_tolerance(int accuracy) { return ((((255 - accuracy) / 4) + 0x14) << 11) / 360; }
// Think period of ai_choose_goal / the dodge check: 0x40 - reaction / 4 ticks.
inline int ai_think_period(int reaction) { return 0x40 - reaction / 4; }

// ---- movement and finders ------------------------------------------------------------------------
int    ai_wizard_move(Thing *t);                                        // ai_wizard_move_13b10
int    ai_approach_target(Thing *t, const Thing *target, int near_dist, int far_dist);   // ai_approach_target_140d0 (target null: distance to Thing.home)
Thing *ai_find_mana_ball_target(Thing *t);                              // ai_find_mana_ball_target_13ce0
Thing *player_find_nearest_by_type(const Thing *t, int type);           // player_find_nearest_by_type_13ec0 (0, 2, 3 or 0xff = any)
Thing *player_find_nearest_wizard_excl(const Thing *t, const Thing *excl);   // player_find_nearest_wizard_excl_13fa0
Thing *player_find_nearest_castle_excl(const Thing *t, const Thing *excl);   // player_find_nearest_castle_excl_14010
uint16_t thing_signature(const Thing *t);                               // thing_signature_14080: cls * 0x80 + type + owner
int    ai_target_valid(const Thing *t, const Thing *target);            // ai_target_valid_140a0

// ---- threat tracking -----------------------------------------------------------------------------
void   ai_record_threat_from_projectiles();         // ai_record_threat_from_projectiles_150f0 (g_hook_ai_record_threat)
Thing *ai_find_incoming_projectile(const Thing *t); // ai_find_incoming_projectile_153b0
void   ai_set_dodge_steer(Thing *t, const Thing *proj);     // ai_set_dodge_steer_15420: P.strafe_speed = 0x50
void   ai_counter_projectile(Thing *t, const Thing *proj);  // ai_counter_projectile_15460
int    ai_cache_human_wizard(const Thing *t);       // ai_cache_human_wizard_15540 -> g_ai_human_wizard

// DAT_000acac0: the last human flyer (class 3 type 0) of the player list, written every AI tick and
// read by nothing else in the image. Thing index, 0 = none.
extern uint16_t g_ai_human_wizard;
// DAT_0009e5c8: the C runtime's rand() seed (initial value 1, never re-seeded by the game). The only
// caller of rand() is ai_choose_attack_spell. Saved games / snapshots do not contain it.
extern uint32_t g_ai_rand_seed;
int ai_rand();                                      // crt_rand_5aff8: ANSI rand(), 0..0x7fff

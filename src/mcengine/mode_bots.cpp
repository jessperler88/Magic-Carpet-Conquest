// Bots v1 (mode_bots.h). Round 12 task C. STUB from the integrator: task C replaces it.
#include "mode_bots.h"

void mode_bots_level_start() {}

int mode_bot_skill(uint8_t bot_skill) { return bot_skill == 0 ? 2 : bot_skill > 4 ? 4 : bot_skill; }

const char *mode_bot_skill_name(int skill) {
    switch (skill) {
    case 1: return "easy";
    case 2: return "normal";
    case 3: return "hard";
    case 4: return "brutal";
    default: return "?";
    }
}

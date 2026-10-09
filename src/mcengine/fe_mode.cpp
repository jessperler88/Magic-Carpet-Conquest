// The Conquest mode's front-end screens (fe_mode.h). Round 12 task A. STUB from the integrator: task A replaces it.
#include "fe_mode.h"

namespace {
ModeParams make_default_params() {
    ModeParams p;
    p.seed = 1;
    p.world_size = 1024;
    p.bots = 3;
    p.humans = 1;
    return p;
}
ModeParams s_params = make_default_params();
ModeState s_result;
int s_result_local = 0;
}

const ModeParams &fe_mode_params() { return s_params; }
void fe_mode_set_params(const ModeParams &p) { s_params = p; }

void fe_mode_set_result(const ModeState &final_state, int local_record) {
    s_result = final_state;
    s_result_local = local_record;
}
const ModeState &fe_mode_result() { return s_result; }
int fe_mode_result_local() { return s_result_local; }

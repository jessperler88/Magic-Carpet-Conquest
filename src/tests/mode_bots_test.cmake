# Port round 12 (task C): bots v1 of the Conquest mode - skill, allies, a generated world end to end, determinism;
# `mode_bots_test <dir> --harness K ...` is the bot-vs-bot balance harness (docs/analysis/port_r12_bots.md).
mc_unit_test(mode_bots_test tests/mode_bots_test.cpp ${MC_SIM_ALL})

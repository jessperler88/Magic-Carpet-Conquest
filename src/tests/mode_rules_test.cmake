# Port round 12 task B: the match rules v1 of a Conquest run (sides / teams, respawn, elimination, desecration, win
# conditions, level end on one tick, determinism) on a small generated world (docs/analysis/port_r12_rules.md).
mc_unit_test(mode_rules_test tests/mode_rules_test.cpp ${MC_SIM_ALL})

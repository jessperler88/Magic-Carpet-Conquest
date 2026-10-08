# Round 11 session 3 (task F): the 256 world equals the pre-round-11 code (golden file tests/data/world_levels_256.txt,
# docs/analysis/port_world_test.md), 512 / 2048 / 4096 probes, a generated 1024 world run twice.
mc_unit_test(world_test tests/world_test.cpp ${MC_SIM_ALL})

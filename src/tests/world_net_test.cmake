# Round 11 session 3: a Conquest mode run (generated 1024 world, 2 humans + 3 bots) in lockstep over TCP, two processes.
mc_unit_test(world_net_test tests/world_net_test.cpp ${MC_SIM_ALL} mcport/net_tcp.cpp)
target_include_directories(world_net_test PRIVATE mcport)
if(WIN32)
  target_link_libraries(world_net_test PRIVATE ws2_32)
endif()
# Listen on loopback only: no Windows Firewall prompt (net_tcp.cpp MC_NET_BIND).
set_tests_properties(world_net_test PROPERTIES ENVIRONMENT "MC_NET_BIND=127.0.0.1")

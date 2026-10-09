mc_unit_test(net_test tests/net_test.cpp ${MC_SIM_ALL} mcengine/frontend.cpp mcengine/fe_mode.cpp mcengine/savegame.cpp
    mcengine/ui_draw.cpp mcengine/fli.cpp mcport/net_tcp.cpp)
target_include_directories(net_test PRIVATE mcport)
if(WIN32)
  target_link_libraries(net_test PRIVATE ws2_32)
endif()
# Listen on loopback only: no Windows Firewall prompt when the TCP parts run (net_tcp.cpp MC_NET_BIND).
set_tests_properties(net_test PROPERTIES ENVIRONMENT "MC_NET_BIND=127.0.0.1")

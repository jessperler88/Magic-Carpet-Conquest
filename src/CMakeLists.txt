cmake_minimum_required(VERSION 3.25)
project(MagicCarpetPort LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MC_WARN_NARROWING "Diagnostic build: report C4244 narrowing conversions (round 11 world sweep)" OFF)
if(MSVC)
  add_compile_options(/W4 /utf-8 /wd4245 /wd4267)
  if(NOT MC_WARN_NARROWING)
    add_compile_options(/wd4244)
  endif()
  # Incremental linking produced executables that crashed before main (game_test, effects_test, round 8);
  # a full link of the same objects ran fine.
  add_link_options(/INCREMENTAL:NO)
else()
  add_compile_options(-Wall -Wextra -Wno-sign-compare)
endif()

set(MC_GAME_DIR "${CMAKE_SOURCE_DIR}/../MagicCarpet/magic" CACHE PATH "Retail game directory (read-only)")
file(TO_CMAKE_PATH "${MC_GAME_DIR}" MC_GAME_DIR_CMAKE)

# ---- tables extracted from the user's own carpet.exe (never committed: they are Bullfrog's data) ----
# tools/port/gen_exe_tables.py (specs: mcengine/tables/*.tables) and tools/port/gen_dispatch.py (Thing class
# tables, names from ghidra/export/carpet_functions.csv) write <build>/gen/*.h at configure time; the sources
# include them as "gen/xxx_tables.h" through the build directory. Re-run when a spec, a script or the exe changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(MC_CARPET_EXE "${MC_GAME_DIR_CMAKE}/carpet.exe")
if(NOT EXISTS "${MC_CARPET_EXE}")
  message(FATAL_ERROR "carpet.exe not found in MC_GAME_DIR (${MC_GAME_DIR_CMAKE}). Point -DMC_GAME_DIR=<dir> at the "
                      "folder of your own Magic Carpet copy that holds carpet.exe (the DOS retail version).")
endif()
set(MC_REPO_DIR "${CMAKE_SOURCE_DIR}/..")
set(MC_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB MC_TABLE_SPECS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/mcengine/tables/*.tables")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MC_TABLE_SPECS} "${MC_CARPET_EXE}"
  "${MC_REPO_DIR}/tools/port/gen_exe_tables.py" "${MC_REPO_DIR}/tools/port/gen_dispatch.py"
  "${MC_REPO_DIR}/tools/analysis/img.py" "${MC_REPO_DIR}/ghidra/export/carpet_functions.csv")
foreach(gen_script gen_exe_tables.py gen_dispatch.py)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "MC_CARPET_EXE=${MC_CARPET_EXE}" "MC_EXE_CACHE=${CMAKE_BINARY_DIR}/carpet_flat.bin"
            "${Python3_EXECUTABLE}" "${MC_REPO_DIR}/tools/port/${gen_script}" --out "${MC_GEN_DIR}"
    RESULT_VARIABLE gen_result OUTPUT_QUIET ERROR_VARIABLE gen_error)
  if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "tools/port/${gen_script} failed (${gen_result}): ${gen_error}")
  endif()
endforeach()
include_directories("${CMAKE_BINARY_DIR}")   # "gen/xxx_tables.h"
# The repository root: tests find extracted/reference dumps, tools/ and their scenarios under it.
get_filename_component(MC_REPO_ABS "${MC_REPO_DIR}" ABSOLUTE)
add_compile_definitions(MC_REPO_DIR="${MC_REPO_ABS}")

# ---- mcdata: plain C data layer (RNC, file helpers, levels, sprites, tmaps) ----
add_library(mcdata STATIC
  mcdata/rnc.c
  mcdata/mcfile.c
  mcdata/level.c
  mcdata/sprite.c
  mcdata/tmaps.c
  mcdata/sndbank.c
  mcdata/hmp.c
)
target_include_directories(mcdata PUBLIC mcdata)

# ---- mcengine: the translated game (C++17, no SDL dependency) ----
# Every .cpp under mcengine/ is part of the library so subsystems can be added without touching
# this file. Generated tables live in mcengine/gen (python tools/port/gen_exe_tables.py).
file(GLOB MCENGINE_SOURCES CONFIGURE_DEPENDS mcengine/*.cpp)
add_library(mcengine STATIC ${MCENGINE_SOURCES})
target_include_directories(mcengine PUBLIC mcengine)
target_link_libraries(mcengine PUBLIC mcdata)
target_compile_definitions(mcengine PUBLIC MC_DEFAULT_GAME_DIR="${MC_GAME_DIR_CMAKE}")

# ---- mcport: the native game executable (SDL2 platform layer) ----
find_package(SDL2 CONFIG REQUIRED)
# Every .cpp under mcport/ (platform, audio, ...) is part of the executable.
file(GLOB MCPORT_SOURCES CONFIGURE_DEPENDS mcport/*.cpp)
add_executable(mcport WIN32 ${MCPORT_SOURCES})
target_link_libraries(mcport PRIVATE mcengine $<$<BOOL:${WIN32}>:winmm> $<$<BOOL:${WIN32}>:ws2_32>
  $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
  $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)

# ---- tests ----
# Two kinds:
#  * mc_unit_test(name test.cpp engine_src...): compiles only the listed engine sources (+ globals),
#    so a subsystem can be built and tested while other subsystems are still being written.
#    Declared one per file in tests/*.cmake (GLOBbed below) so files can be added independently.
#  * mc_test(name src): links the whole mcengine library (integration tests).
# Every test exe takes the game dir as argv[1] and returns 0 on success.
enable_testing()
function(mc_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE mcengine $<$<BOOL:${WIN32}>:dbghelp>)
  add_test(NAME ${name} COMMAND ${name} "${MC_GAME_DIR_CMAKE}")
endfunction()
function(mc_unit_test name)
  add_executable(${name} ${ARGN} mcengine/mc_globals.cpp mcengine/mode.cpp mcengine/world_set.cpp)
  target_include_directories(${name} PRIVATE mcengine)
  target_link_libraries(${name} PRIVATE mcdata $<$<BOOL:${WIN32}>:dbghelp>)
  target_compile_definitions(${name} PRIVATE MC_DEFAULT_GAME_DIR="${MC_GAME_DIR_CMAKE}")
  add_test(NAME ${name} COMMAND ${name} "${MC_GAME_DIR_CMAKE}")
endfunction()
# The renderer-free simulation core (see mcengine/sim.h). A game-logic unit test lists
# ${MC_SIM_CORE} plus the sources of the subsystem under test.
set(MC_SIM_CORE mcengine/thing.cpp mcengine/terrain_gen.cpp mcengine/terrain_paint.cpp mcengine/spatial.cpp
    mcengine/level_features.cpp mcengine/constructors.cpp mcengine/player.cpp mcengine/demo.cpp mcengine/sim.cpp
    mcengine/text.cpp mcengine/palette_fx.cpp mcengine/net.cpp mcengine/worldgen.cpp)
# The whole simulation without the renderer: the core plus every gameplay subsystem (sim_register_gameplay).
set(MC_SIM_ALL ${MC_SIM_CORE} mcengine/sim_all.cpp mcengine/effects.cpp mcengine/projectiles.cpp mcengine/spells.cpp
    mcengine/castle.cpp mcengine/scenery.cpp mcengine/creature_common.cpp mcengine/creatures.cpp mcengine/input.cpp
    mcengine/creatures2.cpp mcengine/creatures3.cpp mcengine/ai_wizard.cpp mcengine/sound.cpp
    mcengine/debug_cmd.cpp mcengine/mode_level.cpp mcengine/mode_dump.cpp mcengine/scenario.cpp
    mcengine/mode_rules.cpp mcengine/mode_bots.cpp)
mc_test(mcdata_tests tests/mcdata_tests.c)
file(GLOB MC_TEST_CMAKE CONFIGURE_DEPENDS tests/*.cmake)
foreach(f ${MC_TEST_CMAKE})
  include(${f})
endforeach()

cmake_minimum_required(VERSION 3.16)
project(NetRescue VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The complete simulation engine: graph, routing, topologies, ARQ/fault/queue engine, experiments, API.
set(NETRESCUE_CORE_SOURCES
  src/network.c src/cidr.c src/persistence.c src/arq.c src/topology.c src/congestion.c src/validate.c
  src/json.c src/experiment.c src/presets.c src/api.c)

if(MSVC)
  set(NETRESCUE_WARNINGS /W4)
else()
  set(NETRESCUE_WARNINGS -Wall -Wextra -Wpedantic)
endif()

if(EMSCRIPTEN)
  # WebAssembly build: emcmake cmake -S . -B build-wasm && cmake --build build-wasm
  add_executable(netrescue_engine ${NETRESCUE_CORE_SOURCES})
  target_include_directories(netrescue_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(netrescue_engine PRIVATE ${NETRESCUE_WARNINGS} -O2)
  target_link_options(netrescue_engine PRIVATE -O2 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNetRescueEngine
    -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=2097152
    "-sEXPORTED_FUNCTIONS=['_nr_api_call','_nr_api_shutdown','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']")
  set_target_properties(netrescue_engine PROPERTIES SUFFIX ".mjs")
  return()
endif()

add_library(netrescue_core OBJECT ${NETRESCUE_CORE_SOURCES})
target_include_directories(netrescue_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(UNIX)
  target_link_libraries(netrescue_core PUBLIC m)
endif()
target_compile_options(netrescue_core PRIVATE ${NETRESCUE_WARNINGS})

function(netrescue_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE netrescue_core)
  target_compile_options(${name} PRIVATE ${NETRESCUE_WARNINGS})
endfunction()

if(WIN32)
  add_executable(NetRescue WIN32 src/main_win32.c)
  target_link_libraries(NetRescue PRIVATE netrescue_core comctl32 comdlg32 user32 gdi32)
  target_compile_options(NetRescue PRIVATE ${NETRESCUE_WARNINGS})
  netrescue_executable(NetRescueCLI src/main_cli.c)
else()
  message(STATUS "Native Win32 dashboard is Windows-only; building the portable simulator CLI.")
  netrescue_executable(NetRescue src/main_cli.c)
endif()

# Native host for the JSON API used by the website (C-backend fallback and scripting tool).
netrescue_executable(netrescue_api src/main_api.c)

enable_testing()
foreach(test_name test_core test_engine test_api)
  netrescue_executable(${test_name} tests/${test_name}.c)
  if(MSVC)
    target_compile_options(${test_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
  endif()
endforeach()
add_test(NAME core COMMAND test_core)
add_test(NAME engine COMMAND test_engine)
add_test(NAME api COMMAND test_api)

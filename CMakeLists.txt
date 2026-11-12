cmake_minimum_required(VERSION 3.20)
project(ets2_truck_dirt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The DLL and probetest need MSVC and the SCS Telemetry SDK headers, which are not in the repository
# (redistribution licence not confirmed): extract the SDK so that third_party/scs_sdk/include/scssdk.h
# exists. Without them (or with gcc/clang) only the portable tests are built: rigtest, dusttest, statetest,
# washtest, paneltest, langtest and compattest (plus dustpreview with MSVC, which needs D3D11 but not the SDK).
set(DIRT_FULL_BUILD OFF)
if(MSVC)
    set(DIRT_WARNINGS /W4 /permissive- /utf-8)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/scs_sdk/include/scssdk.h")
        set(DIRT_FULL_BUILD ON)
    else()
        message(WARNING "SCS SDK headers not found (third_party/scs_sdk/include/scssdk.h): "
                        "building only the offline tests, without the DLL and probetest")
    endif()
else()
    set(DIRT_WARNINGS -Wall -Wextra)
endif()

if(DIRT_FULL_BUILD)
    # MinHook (BSD-2, v1.3.4) as a static library.
    add_library(minhook STATIC
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde64.c)
    target_include_directories(minhook PUBLIC third_party/minhook/include)

    # D7: the version (DIRT_VERSION in dirt_version.h, and out/version.txt for tools/release/make_release.sh),
    # from git at every build: the last tag v* on HEAD, else DIRT_VERSION_BASE plus the commit.
    set(DIRT_VERSION_BASE "0.1.0-alpha")
    set(DIRT_VERSION_DIR "${CMAKE_BINARY_DIR}/version")
    add_custom_target(dirt_version
        COMMAND "${CMAKE_COMMAND}" -DBASE=${DIRT_VERSION_BASE} "-DSRC=${CMAKE_SOURCE_DIR}"
                "-DHEADER=${DIRT_VERSION_DIR}/dirt_version.h" "-DTEXT=${CMAKE_BINARY_DIR}/out/version.txt"
                -P "${CMAKE_SOURCE_DIR}/tools/release/version.cmake"
        BYPRODUCTS "${DIRT_VERSION_DIR}/dirt_version.h" "${CMAKE_BINARY_DIR}/out/version.txt"
        COMMENT "Plugin version from git"
        VERBATIM)
    # D10: the texts of lang/*.txt built into the DLL (src/lang_embed.cpp includes this file).
    file(GLOB DIRT_LANG_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lang/*.txt")
    set(DIRT_LANG_INC "${CMAKE_BINARY_DIR}/generated/lang_texts.inc")
    add_custom_command(OUTPUT "${DIRT_LANG_INC}"
        COMMAND ${CMAKE_COMMAND} -DLANG_DIR=${CMAKE_SOURCE_DIR}/lang -DOUT=${DIRT_LANG_INC}
                -P ${CMAKE_SOURCE_DIR}/tools/embed_lang.cmake
        DEPENDS ${DIRT_LANG_FILES} ${CMAKE_SOURCE_DIR}/tools/embed_lang.cmake
        COMMENT "Embedding lang/*.txt")

    # src/generated/shader_catalog.inc comes from tools/shader_catalog/build_catalog.py.
    add_library(ets2_truck_dirt SHARED src/plugin.cpp src/gpu_probe.cpp src/tint.cpp src/rig_tracker.cpp src/dirt_model.cpp
        src/dirt_state.cpp src/test_panel.cpp src/panel_draw.cpp src/compat.cpp src/pattern.cpp src/trigger_probe.cpp
        src/wiper.cpp src/wash.cpp src/overlay.cpp src/lang.cpp src/lang_embed.cpp src/payment.cpp src/station.cpp src/input_hook.cpp src/menu.cpp src/premap.cpp src/cargo_dirt.cpp "${DIRT_LANG_INC}"
        src/exports.def)
    target_include_directories(ets2_truck_dirt PRIVATE third_party/scs_sdk/include src "${DIRT_VERSION_DIR}"
        "${CMAKE_BINARY_DIR}/generated")
    add_dependencies(ets2_truck_dirt dirt_version)
    target_compile_definitions(ets2_truck_dirt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ets2_truck_dirt PRIVATE ${DIRT_WARNINGS})
    target_link_libraries(ets2_truck_dirt PRIVATE minhook)
    set_target_properties(ets2_truck_dirt PROPERTIES OUTPUT_NAME "ets2_truck_dirt")

    # Output stays inside the project; nothing is copied to the game folder.
    set_target_properties(ets2_truck_dirt PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/out")

    # Offline test of the probe (tools/probetest): runs the DLL with a fake SDK.
    add_executable(probetest tools/probetest/probetest.cpp)
    target_include_directories(probetest PRIVATE third_party/scs_sdk/include)
    target_compile_definitions(probetest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(probetest PRIVATE d3d11 d3dcompiler minhook)
    set_target_properties(probetest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
endif()

# Offline test of the rig tracker on V3a captures (tools/rigtest, CSV from tools/v3a/export_rig.py).
add_executable(rigtest tools/rigtest/rigtest.cpp src/rig_tracker.cpp)
target_include_directories(rigtest PRIVATE src)
target_compile_definitions(rigtest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rigtest PRIVATE ${DIRT_WARNINGS})
set_target_properties(rigtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# N47: offline test of the rig maps (tools/maptest) on the V3a captures.
add_executable(maptest tools/maptest/maptest.cpp src/rig_tracker.cpp)
target_include_directories(maptest PRIVATE src)
target_compile_definitions(maptest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(maptest PRIVATE ${DIRT_WARNINGS})
set_target_properties(maptest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# Offline test of the D3 dust model (tools/dusttest).
add_executable(dusttest tools/dusttest/dusttest.cpp src/dirt_model.cpp)
target_include_directories(dusttest PRIVATE src)
target_compile_definitions(dusttest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dusttest PRIVATE ${DIRT_WARNINGS})
set_target_properties(dusttest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# Offline test of the D5 saved dirt (tools/statetest).
add_executable(statetest tools/statetest/statetest.cpp src/dirt_state.cpp)
target_include_directories(statetest PRIVATE src)
target_compile_definitions(statetest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(statetest PRIVATE ${DIRT_WARNINGS})
set_target_properties(statetest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
find_package(Threads REQUIRED)  # D11n: the writer thread (src/file_writer.h)
target_link_libraries(statetest PRIVATE Threads::Threads)

# Offline test of the D5 wash at the service station (tools/washtest).
# N36: the game's refuel offer located in the games' executables, offline (not in run_tests: needs the games).
add_executable(stationtest tools/stationtest/stationtest.cpp src/station.cpp src/pattern.cpp)
target_include_directories(stationtest PRIVATE src)
target_compile_definitions(stationtest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(stationtest PRIVATE ${DIRT_WARNINGS})
set_target_properties(stationtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

add_executable(washtest tools/washtest/washtest.cpp src/wash.cpp)
target_include_directories(washtest PRIVATE src)
target_compile_definitions(washtest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(washtest PRIVATE ${DIRT_WARNINGS})
set_target_properties(washtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# N67: offline test of the wiper learner (tools/wipertest).
add_executable(wipertest tools/wipertest/wipertest.cpp src/wiper.cpp)
target_include_directories(wipertest PRIVATE src)
target_compile_definitions(wipertest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wipertest PRIVATE ${DIRT_WARNINGS})
set_target_properties(wipertest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# Offline test of the N10 test panel and the forced levels (tools/paneltest).
add_executable(paneltest tools/paneltest/paneltest.cpp src/test_panel.cpp src/dirt_model.cpp src/menu.cpp
    src/overlay.cpp src/lang.cpp)
target_include_directories(paneltest PRIVATE src)
target_compile_definitions(paneltest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(paneltest PRIVATE ${DIRT_WARNINGS})
set_target_properties(paneltest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# Offline test of the D10 texts (tools/langtest): src/lang and every lang/<code>.txt.
add_executable(langtest tools/langtest/langtest.cpp src/lang.cpp src/wash.cpp)
target_include_directories(langtest PRIVATE src)
target_compile_definitions(langtest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(langtest PRIVATE ${DIRT_WARNINGS})
set_target_properties(langtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# Offline test of the N11 self-check of the render assumptions (tools/compattest).
add_executable(compattest tools/compattest/compattest.cpp src/compat.cpp)
target_include_directories(compattest PRIVATE src)
target_compile_definitions(compattest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(compattest PRIVATE ${DIRT_WARNINGS})
set_target_properties(compattest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")


# N57: offline test of the cargo's dirt (tools/cargotest).
add_executable(cargotest tools/cargotest/cargotest.cpp src/cargo_dirt.cpp)
target_include_directories(cargotest PRIVATE src)
target_compile_definitions(cargotest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(cargotest PRIVATE ${DIRT_WARNINGS})
set_target_properties(cargotest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

# P1: offline test of the pre-map's table (tools/premaptest).
add_executable(premaptest tools/premaptest/premaptest.cpp src/premap.cpp)
target_include_directories(premaptest PRIVATE src)
target_compile_definitions(premaptest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(premaptest PRIVATE ${DIRT_WARNINGS})
set_target_properties(premaptest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
if(MSVC)
    # Offline preview of the dust on a ray-traced truck (tools/dustpreview): PNGs.
    add_executable(dustpreview tools/dustpreview/dustpreview.cpp src/panel_draw.cpp src/test_panel.cpp src/dirt_model.cpp
        src/overlay.cpp src/lang.cpp)
    target_include_directories(dustpreview PRIVATE src)
    target_compile_definitions(dustpreview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(dustpreview PRIVATE d3d11 d3dcompiler windowscodecs ole32)
    set_target_properties(dustpreview PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

    # N64j: the windscreen's dirt rendered offline, the GLASS variant over the measured pane (tools/glasspreview).
    add_executable(glasspreview tools/glasspreview/glasspreview.cpp)
    target_include_directories(glasspreview PRIVATE src)
    target_compile_definitions(glasspreview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(glasspreview PRIVATE d3d11 d3dcompiler windowscodecs ole32)
    set_target_properties(glasspreview PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

    # D5: the player's overlay (wash at the station, keys menu) drawn into PNGs with the real font (tools/overlaypreview).
    add_executable(overlaypreview tools/overlaypreview/overlaypreview.cpp src/panel_draw.cpp src/test_panel.cpp src/menu.cpp
        src/dirt_model.cpp src/overlay.cpp src/lang.cpp)
    target_include_directories(overlaypreview PRIVATE src)
    target_compile_definitions(overlaypreview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(overlaypreview PRIVATE ${DIRT_WARNINGS})
    target_link_libraries(overlaypreview PRIVATE d3d11 d3dcompiler windowscodecs ole32)
    set_target_properties(overlaypreview PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
endif()

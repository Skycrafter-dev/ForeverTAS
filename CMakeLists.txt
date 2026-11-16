cmake_minimum_required(VERSION 3.21)

if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "ForeverTAS does not support macOS")
endif()

project(ForeverTAS
    VERSION 0.3.0
    DESCRIPTION "Native TrackMania Nations Forever tool-assisted search tool"
    HOMEPAGE_URL "https://github.com/Skycrafter-dev/ForeverTAS"
    LANGUAGES CXX)

option(FOREVERTAS_ENABLE_CUDA
    "Build ForeverTAS with the optional ForeverValidator CUDA backend" OFF)
option(FOREVERTAS_ENABLE_HIP
    "Build ForeverTAS with the optional ForeverValidator HIP backend" OFF)
option(FOREVERTAS_ENABLE_TWIN
    "Build ForeverTAS with ForeverValidator's twin engine (Approximate physics: CPU threads, CUDA or HIP)" OFF)
# A GPU module (cuda or hip): only the search core with that backend, as a
# library the app loads at run time; no Qt, no app. See src/compute.
set(FOREVERTAS_COMPUTE_MODULE "" CACHE STRING "Build only a GPU module: cuda or hip")
if(FOREVERTAS_COMPUTE_MODULE STREQUAL "cuda")
    set(FOREVERTAS_ENABLE_CUDA ON CACHE BOOL "" FORCE)
    set(FOREVERTAS_ENABLE_HIP OFF CACHE BOOL "" FORCE)
elseif(FOREVERTAS_COMPUTE_MODULE STREQUAL "hip")
    set(FOREVERTAS_ENABLE_CUDA OFF CACHE BOOL "" FORCE)
    set(FOREVERTAS_ENABLE_HIP ON CACHE BOOL "" FORCE)
elseif(FOREVERTAS_COMPUTE_MODULE)
    message(FATAL_ERROR "FOREVERTAS_COMPUTE_MODULE must be cuda or hip")
endif()
if(FOREVERTAS_COMPUTE_MODULE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    # every GPU generation of a file at once (one module carries them all
    # until it is split per generation)
    if(FOREVERTAS_COMPUTE_MODULE STREQUAL "cuda")
        # ptxas takes up to ~3.7 GB per generation on the twin's kernels:
        # nvcc runs as many generations side by side as fit in ~5 GB each of
        # the computer's memory (the rest is the computer's; all cores when
        # it can: --threads=0), and ninja one CUDA file at a time
        set(FOREVERTAS_CUDA_GENERATIONS_AT_ONCE "" CACHE STRING
            "GPU generations nvcc compiles side by side (0: one per core; empty: from the memory)")
        set(cuda_threads "${FOREVERTAS_CUDA_GENERATIONS_AT_ONCE}")
        if(cuda_threads STREQUAL "")
            cmake_host_system_information(RESULT memory_mb QUERY TOTAL_PHYSICAL_MEMORY)
            cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
            math(EXPR cuda_threads "${memory_mb} / 5120")
            if(cuda_threads LESS 1)
                set(cuda_threads 1)
            elseif(cuda_threads GREATER_EQUAL cores)
                set(cuda_threads 0)
            endif()
        endif()
        set(FOREVERTAS_CUDA_FILES_AT_ONCE 1 CACHE STRING
            "CUDA files compiled at the same time")
        string(APPEND CMAKE_CUDA_FLAGS " --threads=${cuda_threads} --compress-mode=size")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(CMAKE_CUDA_COMPILER_LAUNCHER
            "${Python3_EXECUTABLE};${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_slots.py;${CMAKE_BINARY_DIR}/cuda-slots;${FOREVERTAS_CUDA_FILES_AT_ONCE};${CMAKE_CUDA_COMPILER_LAUNCHER}")
    endif()
endif()
option(FOREVERTAS_QML_MODULE_EXTERNAL
    "Use the release's shared QML module instead of building it here" OFF)
set(FOREVERTAS_COMMON_QML_DIR "" CACHE PATH
    "Directory containing the shared ForeverTAS QML module")
set(FOREVERTAS_PREBUILT_VALIDATOR_DIR "" CACHE PATH
    "Directory containing a verified, matching Windows Validator build")
set(FOREVERTAS_WINDOWS_RUNTIME_DIR "" CACHE PATH
    "Directory containing non-Qt runtime DLLs for Windows packaging")
set(FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR "" CACHE PATH
    "Directory containing redistributable MSVC runtime DLLs")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
if(NOT FOREVERTAS_COMPUTE_MODULE)
find_package(SDL3 3.2 QUIET CONFIG)
if(NOT SDL3_FOUND)
    set(SDL_SHARED OFF CACHE BOOL "Build bundled SDL statically")
    set(SDL_STATIC ON CACHE BOOL "Build bundled SDL statically")
    set(SDL_TESTS OFF CACHE BOOL "Disable SDL tests")
    set(SDL_EXAMPLES OFF CACHE BOOL "Disable SDL examples")
    set(SDL_INSTALL OFF CACHE BOOL "Install only the app's SDL notice")
    # no precompiled header: ccache and sccache cannot keep files built with one
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc)
    FetchContent_MakeAvailable(SDL3)
    install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/forevertas/SDL3")
endif()
find_program(FOREVERTAS_LLDB_EXECUTABLE NAMES lldb)
find_program(FOREVERTAS_SCRIPT_EXECUTABLE NAMES script)
find_program(FOREVERTAS_WINPTY_EXECUTABLE NAMES winpty)

set(FOREVERTAS_APP_ID "dev.skycrafter.forevertas")
set(FOREVERTAS_DISPLAY_NAME "ForeverTAS")

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Concurrent
    Core
    Gui
    Network
    Qml
    Quick
    Quick3D
    QuickControls2
    WebSockets
    Widgets)
# The glass panels are drawn with custom shaders.
find_package(Qt6 6.5 REQUIRED COMPONENTS ShaderTools)
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.5 REQUIRED COMPONENTS DBus)
endif()
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
    find_package(Qt6 QUIET COMPONENTS GuiPrivate ShaderTools)
endif()
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)
endif()

set(FOREVERVALIDATOR_BUILD_CLI OFF CACHE BOOL
    "Do not build the embedded ForeverValidator CLI" FORCE)
set(FOREVERVALIDATOR_BUILD_TESTS OFF CACHE BOOL
    "Do not build embedded ForeverValidator tests" FORCE)
set(FOREVERVALIDATOR_ENABLE_CUDA ${FOREVERTAS_ENABLE_CUDA} CACHE BOOL
    "Enable the embedded ForeverValidator CUDA backend" FORCE)
set(FOREVERVALIDATOR_ENABLE_HIP ${FOREVERTAS_ENABLE_HIP} CACHE BOOL
    "Enable the embedded ForeverValidator HIP backend" FORCE)
# ForeverValidator's Vulkan backend is not used by ForeverTAS.
set(FOREVERVALIDATOR_ENABLE_VULKAN OFF CACHE BOOL
    "Enable the embedded ForeverValidator Vulkan backend" FORCE)
# The twin runs on CPU threads in every build, and on the GPU with CUDA or HIP.
set(FOREVERVALIDATOR_BUILD_TWIN ${FOREVERTAS_ENABLE_TWIN} CACHE BOOL
    "Build the embedded ForeverValidator twin engine" FORCE)
if(FOREVERTAS_ENABLE_TWIN AND (FOREVERTAS_ENABLE_CUDA OR FOREVERTAS_ENABLE_HIP))
    set(forevertas_twin_gpu ON)
else()
    set(forevertas_twin_gpu OFF)
endif()
set(FOREVERVALIDATOR_BUILD_TWIN_GPU ${forevertas_twin_gpu} CACHE BOOL
    "Build the embedded ForeverValidator twin's GPU engine" FORCE)
if(WIN32 AND FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "nvidia")
    set(CMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreaded
        -Xcompiler=-MT)
    set(CMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL
        -Xcompiler=-MD)
    set(CMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebug
        -Xcompiler=-MTd)
    set(CMAKE_HIP_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebugDLL
        -Xcompiler=-MDd)
    set(CMAKE_HIP_ARCHIVE_CREATE
        "<CMAKE_AR> /nologo /out:<TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_HIP_ARCHIVE_APPEND
        "<CMAKE_AR> /nologo /out:<TARGET> <LINK_FLAGS> <TARGET> <OBJECTS>")
    set(CMAKE_HIP_ARCHIVE_FINISH "")
endif()

if(FOREVERTAS_PREBUILT_VALIDATOR_DIR)
    if(NOT WIN32 OR NOT FETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR)
        message(FATAL_ERROR "Prebuilt Validator requires Windows and pinned local source")
    endif()
    file(TO_CMAKE_PATH "${FETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR}"
        forevervalidator_SOURCE_DIR)
    foreach(component IN ITEMS core native)
        set(archive
            "${FOREVERTAS_PREBUILT_VALIDATOR_DIR}/forevervalidator_${component}.lib")
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Prebuilt Validator archive is missing: ${archive}")
        endif()
        add_library(forevervalidator_${component} STATIC IMPORTED GLOBAL)
        set_target_properties(forevervalidator_${component} PROPERTIES
            IMPORTED_LOCATION "${archive}"
            INTERFACE_INCLUDE_DIRECTORIES "${forevervalidator_SOURCE_DIR}/include")
        add_library(ForeverValidator::${component} ALIAS
            forevervalidator_${component})
    endforeach()
    set_target_properties(forevervalidator_native PROPERTIES
        INTERFACE_LINK_LIBRARIES forevervalidator_core)
    set(validator_libraries OpenSSL::Crypto ZLIB::ZLIB)
    if(FOREVERTAS_ENABLE_CUDA OR
            (FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "nvidia"))
        find_package(CUDAToolkit REQUIRED)
        list(APPEND validator_libraries CUDA::cudart)
    endif()
    if(FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "amd")
        file(TO_CMAKE_PATH "$ENV{HIP_PATH}/lib/amdhip64.lib" hip_runtime_library)
        if(NOT EXISTS "${hip_runtime_library}")
            message(FATAL_ERROR "AMD HIP runtime import library is missing")
        endif()
        list(APPEND validator_libraries "${hip_runtime_library}")
    endif()
    set_target_properties(forevervalidator_core PROPERTIES
        INTERFACE_LINK_LIBRARIES "${validator_libraries}"
        INTERFACE_COMPILE_DEFINITIONS
            "FOREVERVALIDATOR_HAS_CUDA=$<BOOL:${FOREVERTAS_ENABLE_CUDA}>;FOREVERVALIDATOR_HAS_HIP=$<BOOL:${FOREVERTAS_ENABLE_HIP}>;FOREVERVALIDATOR_HAS_VULKAN=0")
    if(FOREVERTAS_ENABLE_HIP)
        find_path(FOREVERVALIDATOR_HIP_INCLUDE_DIR hip/hip_runtime.h
            HINTS "$ENV{HIP_PATH}/include" "$ENV{ROCM_PATH}/include" REQUIRED)
    endif()
else()
    if(WIN32 AND FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "amd")
        # In cl driver mode, clang names every architecture's device image
        # after the source when the object comes from /Fo, so each
        # --offload-arch overwrites the previous image and every fat binary
        # slot carries the last architecture's code. -o keeps one temporary
        # image per architecture; the Validator inherits this rule.
        enable_language(HIP)
        string(REPLACE "/Fo<OBJECT>" "-o <OBJECT>"
            CMAKE_HIP_COMPILE_OBJECT "${CMAKE_HIP_COMPILE_OBJECT}")
    endif()
    FetchContent_Declare(
        ForeverValidator
        GIT_REPOSITORY https://github.com/Skycrafter-dev/ForeverValidator.git
        GIT_TAG 543b94e64a47d40b007c221684d7b42d8042d074
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(ForeverValidator)
endif()
if(NOT FOREVERTAS_PREBUILT_VALIDATOR_DIR AND FOREVERTAS_ENABLE_HIP AND UNIX)
    if(CMAKE_HIP_PLATFORM STREQUAL "nvidia")
        target_compile_options(forevervalidator_core PRIVATE
            $<$<COMPILE_LANGUAGE:HIP>:-Xcompiler=-fPIC>)
    else()
        target_compile_options(forevervalidator_core PRIVATE
            $<$<COMPILE_LANGUAGE:HIP>:-fPIC>)
    endif()
endif()

add_library(forevertas_core STATIC
    src/input_timeline_time.h
    src/physics_backend.h
    src/replay_file_io.cpp
    src/replay_file_io.h
    src/evaluators/iteration_evaluator.h
    src/evaluators/custom_volume_entry_evaluator.cpp
    src/evaluators/custom_volume_entry_evaluator.h
    src/evaluators/gate_entry_evaluator.cpp
    src/evaluators/gate_entry_evaluator.h
    src/evaluators/evaluator_utils.h
    src/evaluators/precise_finish_time_evaluator.cpp
    src/evaluators/precise_finish_time_evaluator.h
    src/evaluators/checkpoint_time_evaluator.cpp
    src/evaluators/around_walls_distance.cpp
    src/evaluators/around_walls_distance.h
    src/evaluators/distance_target_evaluator.cpp
    src/evaluators/checkpoint_time_evaluator.h
    src/evaluators/distance_target_evaluator.h
    src/evaluators/time_evaluator.cpp
    src/evaluators/time_evaluator.h
    src/evaluators/point_target_evaluator.cpp
    src/evaluators/point_target_evaluator.h
    src/evaluators/pose_target_evaluator.cpp
    src/evaluators/pose_target_evaluator.h
    src/evaluators/scripted_target_evaluator.cpp
    src/evaluators/scripted_target_evaluator.h
    src/evaluators/stunt_points_evaluator.cpp
    src/evaluators/stunt_points_evaluator.h
    src/evaluators/velocity_evaluator.cpp
    src/evaluators/velocity_evaluator.h
    src/evaluators/volume_entry_evaluator.cpp
    src/evaluators/volume_entry_evaluator.h
    src/mutations/composite_input_mutator.cpp
    src/mutations/composite_input_mutator.h
    src/mutations/existing_event_perturbation_mutator.cpp
    src/mutations/existing_event_perturbation_mutator.h
    src/mutations/input_deletion_mutator.cpp
    src/mutations/input_deletion_mutator.h
    src/mutations/input_event_formatter.cpp
    src/mutations/input_event_formatter.h
    src/mutations/replay_input_script.cpp
    src/mutations/replay_input_script.h
    src/mutations/input_event_utils.cpp
    src/mutations/input_event_utils.h
    src/mutations/input_insertion_mutator.cpp
    src/mutations/input_insertion_mutator.h
    src/mutations/input_mutator.h
    src/mutations/modifier_utils.h
    src/mutations/random_steering_mutator.cpp
    src/mutations/random_steering_mutator.h
    src/mutations/smooth_steering_mutator.cpp
    src/mutations/smooth_steering_mutator.h
    src/conditions/condition_program.cpp
    src/conditions/condition_program.h
    src/conditions/condition_catalog.cpp
    src/conditions/condition_catalog.h
    src/searches/algorithm_registry.cpp
    src/searches/setting_labels.cpp
    src/searches/algorithm_registry.h
    src/searches/option_configuration.h
    src/searches/option_settings_utils.h
    src/searches/basic_brute_force_search.cpp
    src/searches/basic_brute_force_search.h
    src/searches/tree_search.cpp
    src/searches/tree_search.h
    src/searches/cuda_batch_calibrator.cpp
    src/searches/cuda_batch_calibrator.h
    src/searches/cuda_calibration_safety.cpp
    src/searches/cuda_calibration_safety.h
    src/searches/cuda_search_configuration.cpp
    src/searches/cuda_search_configuration.h
    src/searches/search_runner.cpp
    src/compute/compute_local.cpp
    src/compute/compute_local.h
    src/compute/compute_module_api.h
    src/diagnostics/diagnostics_log.cpp
    src/diagnostics/diagnostics_log.h
    src/searches/result_input_script.h
    src/time_format.h
    src/searches/search_runner.h)
target_include_directories(forevertas_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(forevertas_core PUBLIC
    ForeverValidator::core
    ForeverValidator::native)
if(FOREVERTAS_ENABLE_HIP)
    target_sources(forevertas_core PRIVATE
        src/searches/hip_calibration_limits.cpp
        src/searches/hip_calibration_limits.h)
    target_include_directories(forevertas_core PRIVATE
        "${FOREVERVALIDATOR_HIP_INCLUDE_DIR}")
    if(CMAKE_HIP_PLATFORM STREQUAL "nvidia")
        target_compile_definitions(forevertas_core PRIVATE
            __HIP_PLATFORM_NVIDIA__=1)
    else()
        target_compile_definitions(forevertas_core PRIVATE
            __HIP_PLATFORM_AMD__=1)
    endif()
endif()

if(FOREVERTAS_COMPUTE_MODULE)
    include(cmake/ComputeModule.cmake)
    return()
endif()
include(cmake/ComputeAbi.cmake)

add_library(forevertas_gui STATIC
    src/app/compact_number_format.cpp
    src/app/compact_number_format.h
    src/app/system_file_dialog.cpp
    src/app/system_file_dialog.h
    src/viewer/gpu_ray_tracing_view.cpp
    src/viewer/gpu_ray_tracing_view.h
    src/viewer/custom_volume_geometry.cpp
    src/viewer/custom_volume_geometry.h
    src/viewer/material_classifier.cpp
    src/viewer/material_classifier.h
    src/viewer/car_instancing.cpp
    src/viewer/car_instancing.h
    src/viewer/race_geometry.cpp
    src/viewer/race_geometry.h
    src/viewer/render_module_api.h
    src/viewer/render_module_library.cpp
    src/viewer/render_module_library.h
    src/viewer/render_module_view.cpp
    src/viewer/return_freed_memory.h
    src/viewer/render_module_view.h
    src/viewer/video_export_controller.cpp
    src/viewer/video_export_controller.h
    src/viewer/ray_tracing_scene.cpp
    src/viewer/ray_tracing_scene.h
    src/viewer/race_timeline_item.cpp
    src/viewer/pointer_lock.cpp
    src/viewer/pointer_lock.h
    src/viewer/race_timeline_item.h
    src/viewer/race_viewer_controller.cpp
    src/viewer/race_viewer_controller.h
    src/viewer/map_identity.cpp
    src/viewer/map_identity.h
    src/app/search_session_store.cpp
    src/app/search_session_store.h
    src/app/shutdown_controller.cpp
    src/app/shutdown_controller.h
    src/viewer/simulation_debugger_model.cpp
    src/viewer/simulation_debugger_model.h
    src/viewer/debugger_tools.cpp
    src/viewer/debugger_tools.h
    src/viewer/visual_scene_pipeline.cpp
    src/viewer/visual_scene_pipeline.h
    src/viewer/whiteboard_canvas_item.cpp
    src/viewer/whiteboard_canvas_item.h
    src/viewer/whiteboard_model.cpp
    src/viewer/whiteboard_model.h
    src/app/search_configuration_model.cpp
    src/app/search_configuration_model.h
    src/app/cuboid_target_model.cpp
    src/app/cuboid_target_model.h
    src/app/custom_volume_target_model.cpp
    src/app/custom_volume_target_model.h
    src/app/pose_target_model.cpp
    src/app/pose_target_model.h
    src/app/search_completion.h
    src/app/keyboard_layout.cpp
    src/app/keyboard_layout.h
    src/app/input_preview_binding.cpp
    src/app/input_preview_binding.h
    src/app/mcp_bridge.cpp
    src/app/mcp_bridge.h
    src/app/mcp_protocol.cpp
    src/app/mcp_protocol.h
    src/app/mcp_relay.cpp
    src/app/remote/remote_codec.cpp
    src/app/remote/remote_hub.cpp
    src/app/remote/group_search.cpp
    src/app/remote/remote_groups.cpp
    src/app/remote/remote_search.cpp
    src/app/remote/remote_worker.cpp
    src/app/mcp_relay.h
    src/app/search_controller.cpp
    src/app/compute_modules.cpp
    src/app/compute_modules.h
    src/app/compute_archive.cpp
    src/app/compute_archive.h
    src/app/compute_check.cpp
    src/app/compute_check.h
    src/app/search_controller.h
    src/app/app_settings.cpp
    src/app/app_settings.h
    src/app/search_diagnostic.cpp
    src/app/search_diagnostic.h
    src/app/packs_directory_finder.cpp
    src/app/packs_directory_finder.h
    src/app/dock_layout.cpp
    src/app/dock_layout.h
    src/app/panel_wheel_redirector.cpp
    src/app/panel_wheel_redirector.h
    src/app/rolling_throughput.h
    src/app/search_worker.cpp
    src/app/search_worker.h)
target_include_directories(forevertas_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(forevertas_gui forevertas_compute_abi)
# GPU modules arrive as .tar.zst
find_path(FOREVERTAS_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_library(FOREVERTAS_ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)
target_include_directories(forevertas_gui PRIVATE "${FOREVERTAS_ZSTD_INCLUDE_DIR}")
target_link_libraries(forevertas_gui PUBLIC "${FOREVERTAS_ZSTD_LIBRARY}")
target_sources(forevertas_gui PRIVATE src/viewer/gamepad_input.cpp src/viewer/gamepad_input.h)
target_sources(forevertas_gui PRIVATE src/viewer/go_to.cpp src/viewer/go_to.h)
target_link_libraries(forevertas_gui PUBLIC SDL3::SDL3)
if(WIN32)
    # lending in an AppContainer (src/app/remote/remote_groups.cpp)
    target_link_libraries(forevertas_gui PUBLIC userenv advapi32 psapi ole32)
endif()

file(GLOB_RECURSE FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES
    CONFIGURE_DEPENDS
    "${forevervalidator_SOURCE_DIR}/src/engine/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/engine/*.h"
    "${forevervalidator_SOURCE_DIR}/src/simulation/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/simulation/*.h"
    "${forevervalidator_SOURCE_DIR}/src/validation/api/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/validation/api/*.h")
list(FILTER FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES EXCLUDE REGEX
    "/src/simulation/backends/hip/generated/.*")
list(SORT FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES)
set(FOREVERTAS_SIMULATION_DEBUG_SOURCE_ENTRIES "")
foreach(debug_source IN LISTS FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES)
    file(RELATIVE_PATH debug_source_path
        "${forevervalidator_SOURCE_DIR}" "${debug_source}")
    string(REPLACE "\\" "/" debug_source_path "${debug_source_path}")
    string(APPEND FOREVERTAS_SIMULATION_DEBUG_SOURCE_ENTRIES
        "    \"${debug_source_path}\",\n")
    set_source_files_properties("${debug_source}" PROPERTIES
        QT_RESOURCE_ALIAS "${debug_source_path}")
endforeach()
list(LENGTH FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES
    FOREVERTAS_SIMULATION_DEBUG_SOURCE_COUNT)
configure_file(
    cmake/SimulationDebugSources.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/simulation_debug_sources.h"
    @ONLY)
qt_add_resources(forevertas_gui "simulation_debug_sources"
    PREFIX "/simulation-debug"
    FILES ${FOREVERTAS_SIMULATION_DEBUG_SOURCE_FILES})
target_compile_definitions(forevertas_gui PRIVATE
    FOREVERTAS_SIMULATION_DEBUG_SOURCE_ROOT="${forevervalidator_SOURCE_DIR}")
target_include_directories(forevertas_gui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(forevertas_gui PUBLIC
    forevertas_core
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Quick
    Qt6::Quick3D
    Qt6::WebSockets
    Qt6::Widgets)
if(UNIX AND NOT APPLE)
    target_link_libraries(forevertas_gui PUBLIC Qt6::DBus)
elseif(WIN32)
    target_link_libraries(forevertas_gui PUBLIC ole32 shell32 uuid)
endif()

# The live-code worker is a separate, unoptimized instance of the real
# ForeverValidator reference engine. Running it out of process keeps native
# source stepping and injected edits from ever blocking the UI process.
file(GLOB_RECURSE FOREVERTAS_DEBUG_WORKER_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "${forevervalidator_SOURCE_DIR}/src/engine/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/format/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/simulation/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/validation/api/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/validation/evaluation/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/validation/planning/*.cpp"
    "${forevervalidator_SOURCE_DIR}/src/platform/native/*.cpp")
# The debugger executes the reference/native engine and deliberately does not
# link CUDA. Driver dispatch is a CUDA host-side facility, not part of the
# editable reference simulation path.
list(FILTER FOREVERTAS_DEBUG_WORKER_ENGINE_SOURCES EXCLUDE REGEX
    "/src/simulation/backends/cuda/cuda_driver_shim\.cpp$")
list(FILTER FOREVERTAS_DEBUG_WORKER_ENGINE_SOURCES EXCLUDE REGEX
    "/src/simulation/backends/hip/.*\.cpp$")
add_executable(forevertas-simulation-debug-worker
    src/debugger/simulation_debug_worker.cpp
    src/mutations/input_event_formatter.cpp
    src/mutations/input_event_utils.cpp
    "${forevervalidator_SOURCE_DIR}/src/simulation/backends/hip/generated/hip_backend.cpp"
    ${FOREVERTAS_DEBUG_WORKER_ENGINE_SOURCES})
target_include_directories(forevertas-simulation-debug-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${forevervalidator_SOURCE_DIR}/include"
    "${forevervalidator_SOURCE_DIR}/src")
target_link_libraries(forevertas-simulation-debug-worker PRIVATE
    OpenSSL::Crypto
    ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(forevertas-simulation-debug-worker PRIVATE
        ws2_32
        gdi32
        crypt32)
endif()
target_compile_definitions(forevertas-simulation-debug-worker PRIVATE
    NOMINMAX
    OPENSSL_SUPPRESS_DEPRECATED)
if(MSVC)
    target_compile_options(forevertas-simulation-debug-worker PRIVATE
        /W4 /Od /Zi /Ob0 /Oy- /fp:strict)
else()
    target_compile_options(forevertas-simulation-debug-worker PRIVATE
        -Wall
        -Wextra
        -Wno-deprecated-declarations
        -O0
        -g2
        -gdwarf-4
        -fno-inline
        -fno-omit-frame-pointer
        -ffp-contract=off)
endif()
set_target_properties(forevertas-simulation-debug-worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
# Wayland: the pointer held during the timeline's cursor drag (pointer
# constraints and relative pointer, which Qt does not offer).
if(UNIX AND NOT APPLE AND TARGET Qt6::GuiPrivate)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FOREVERTAS_WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
        pkg_get_variable(FOREVERTAS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    endif()
    find_program(FOREVERTAS_WAYLAND_SCANNER wayland-scanner)
    if(FOREVERTAS_WAYLAND_CLIENT_FOUND AND FOREVERTAS_WAYLAND_SCANNER AND FOREVERTAS_WAYLAND_PROTOCOLS_DIR)
        enable_language(C)   # the generated protocol code
        set(forevertas_wayland_generated "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
        file(MAKE_DIRECTORY "${forevertas_wayland_generated}")
        foreach(protocol
                "unstable/pointer-constraints/pointer-constraints-unstable-v1"
                "unstable/relative-pointer/relative-pointer-unstable-v1")
            get_filename_component(protocol_name "${protocol}" NAME)
            set(protocol_xml "${FOREVERTAS_WAYLAND_PROTOCOLS_DIR}/${protocol}.xml")
            add_custom_command(
                OUTPUT "${forevertas_wayland_generated}/${protocol_name}-client-protocol.h"
                       "${forevertas_wayland_generated}/${protocol_name}-protocol.c"
                COMMAND "${FOREVERTAS_WAYLAND_SCANNER}" client-header "${protocol_xml}"
                        "${forevertas_wayland_generated}/${protocol_name}-client-protocol.h"
                COMMAND "${FOREVERTAS_WAYLAND_SCANNER}" private-code "${protocol_xml}"
                        "${forevertas_wayland_generated}/${protocol_name}-protocol.c"
                DEPENDS "${protocol_xml}"
                VERBATIM)
            target_sources(forevertas_gui PRIVATE
                "${forevertas_wayland_generated}/${protocol_name}-client-protocol.h"
                "${forevertas_wayland_generated}/${protocol_name}-protocol.c")
        endforeach()
        target_include_directories(forevertas_gui PRIVATE "${forevertas_wayland_generated}")
        target_link_libraries(forevertas_gui PRIVATE PkgConfig::FOREVERTAS_WAYLAND_CLIENT)
        target_compile_definitions(forevertas_gui PRIVATE FOREVERTAS_WAYLAND_POINTER_LOCK=1)
    else()
        message(STATUS "Wayland pointer lock off (needs wayland-client, wayland-protocols and wayland-scanner)")
    endif()
endif()
# the car geometries' scene references, for the vanishing-car reports
find_package(Qt6 QUIET COMPONENTS Quick3DPrivate)
if(TARGET Qt6::Quick3DPrivate)
    target_link_libraries(forevertas_gui PRIVATE Qt6::Quick3DPrivate)
    target_compile_definitions(forevertas_gui PRIVATE FOREVERTAS_QUICK3D_PRIVATE=1)
endif()
if(TARGET Qt6::GuiPrivate AND TARGET Qt6::ShaderTools)
    target_compile_definitions(forevertas_gui PUBLIC
        FOREVERTAS_GPU_RAY_TRACING=1)
    target_link_libraries(forevertas_gui PRIVATE Qt6::GuiPrivate)
else()
    target_compile_definitions(forevertas_gui PUBLIC
        FOREVERTAS_GPU_RAY_TRACING=0)
endif()

qt_add_executable(forevertas
    src/app/main.cpp
    src/app/diagnostics_startup.cpp
    src/app/diagnostics_startup.h
    src/app/stall_watch.cpp
    src/app/stall_watch.h
    src/app/update_controller.cpp
    src/app/update_controller.h
    src/app/update_hardware.cpp
    src/app/update_hardware.h
    src/app/update_release.cpp
    src/app/update_release.h)
add_dependencies(forevertas forevertas-simulation-debug-worker forevertas_compute_abi)
target_include_directories(forevertas PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(forevertas_icon_svg
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/${FOREVERTAS_APP_ID}.svg")
set_source_files_properties("${forevertas_icon_svg}" PROPERTIES
    QT_RESOURCE_ALIAS "icons/forevertas.svg")
qt_add_resources(forevertas "forevertas_app_resources"
    PREFIX "/"
    FILES "${forevertas_icon_svg}")

# The documentation the MCP bridge serves (get_documentation): the topics it
# names, and the README.
set(forevertas_mcp_docs
    MCP.md GATES.md SEARCH_COMPONENTS.md CONDITIONS.md CHECKPOINT_TIME.md
    INPUT_INSERTION.md GPU_BACKENDS.md GPU_SAFETY.md)
list(TRANSFORM forevertas_mcp_docs PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/docs/")
qt_add_resources(forevertas "forevertas_mcp_docs"
    PREFIX "/mcp-docs"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/docs"
    FILES ${forevertas_mcp_docs})
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/README.md" PROPERTIES
    QT_RESOURCE_ALIAS "README.md")
qt_add_resources(forevertas "forevertas_mcp_readme"
    PREFIX "/mcp-docs"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# The stdio MCP server for Windows (a console program: the app is a GUI one,
# whose standard input and output Windows does not wire); elsewhere it is
# ForeverTAS --mcp-stdio.
if(WIN32)
    add_executable(forevertas-mcp src/tools/forevertas_mcp_main.cpp)
    target_link_libraries(forevertas-mcp PRIVATE forevertas_gui Qt6::Core Qt6::Network)
    target_compile_definitions(forevertas-mcp PRIVATE FOREVERTAS_VERSION="${PROJECT_VERSION}")
    set_target_properties(forevertas-mcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
    add_dependencies(forevertas forevertas-mcp)
    install(TARGETS forevertas-mcp RUNTIME DESTINATION ".")
endif()

if(WIN32)
    configure_file(
        packaging/windows/forevertas.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/forevertas.rc"
        @ONLY)
    configure_file(
        packaging/windows/forevertas.exe.manifest.in
        "${CMAKE_CURRENT_BINARY_DIR}/forevertas.exe.manifest"
        @ONLY)
    target_sources(forevertas PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/forevertas.rc"
        "${CMAKE_CURRENT_BINARY_DIR}/forevertas.exe.manifest")
endif()

set_target_properties(forevertas PROPERTIES
    OUTPUT_NAME "${FOREVERTAS_DISPLAY_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
    WIN32_EXECUTABLE TRUE)
set_source_files_properties(qml/AppTheme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/DurationDisplay.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/SteeringDisplay.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)
if(NOT FOREVERTAS_QML_MODULE_EXTERNAL)
qt_add_qml_module(forevertas_qml
    PLUGIN_TARGET forevertas_qml
    URI ForeverTAS
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/qml/ForeverTAS"
    QML_FILES
        qml/ConditionReference.qml
        qml/AppTheme.qml
        qml/DockFrame.qml
        qml/DockHeaderButton.qml
        qml/DockPanel.qml
        qml/DockSpace.qml
        qml/DurationDisplay.qml
        qml/settings/DurationField.qml
        qml/EvaluationOverlay.qml
        qml/EditHistory.qml
        qml/FailureNotice.qml
        qml/Glass.qml
        qml/GlassMenu.qml
        qml/GlassMenuBar.qml
        qml/GlassPopupBackground.qml
        qml/GlassDialog.qml
        qml/Gloss.qml
        qml/GraphicsInspector.qml
        qml/VideoExportDialog.qml
        qml/Main.qml
        qml/PreferencesDialog.qml
        qml/RemoteSettings.qml
        qml/RaceControls.qml
        qml/SearchSessionHistory.qml
        qml/ResultFormat.js
        qml/ResultTable.qml
        qml/SearchInputs.qml
        qml/SimulationDebuggerPanel.qml
        qml/SliderValueField.qml
        qml/GoToField.qml
        qml/SteeringDisplay.qml
        qml/ThemedButton.qml
        qml/ThemedCheckBox.qml
        qml/ThemedIconButton.qml
        qml/ThemedItemDelegate.qml
        qml/ThemedMenuItem.qml
        qml/ThemedSlider.qml
        qml/ThemedSwitch.qml
        qml/ThemedTabButton.qml
        qml/ThemedToolButton.qml
        qml/WhiteboardOverlay.qml
        qml/WhiteboardPlanes.qml
        qml/settings/AdaptiveEscalationSearchSettings.qml
        qml/settings/AlgorithmSelector.qml
        qml/settings/BasicBruteForceSearchSettings.qml
        qml/settings/TreeSearchSettings.qml
        qml/settings/ConfigurationSection.qml
        qml/settings/ExistingEventPerturbationSettings.qml
        qml/settings/PreciseFinishTimeEvaluationSettings.qml
        qml/settings/CheckpointTimeEvaluationSettings.qml
        qml/settings/InfoTip.qml
        qml/settings/InputDeletionSettings.qml
        qml/settings/InputInsertionSettings.qml
        qml/settings/ModifierComposition.qml
        qml/settings/ModifierPassControls.qml
        qml/settings/GateEntryEvaluationSettings.qml
        qml/settings/PointTargetEvaluationSettings.qml
        qml/settings/PoseTargetEvaluationSettings.qml
        qml/settings/RandomSteeringMutationSettings.qml
        qml/settings/ScrubNumberField.qml
        qml/settings/ScriptedTargetEvaluationSettings.qml
        qml/settings/SettingCombo.qml
        qml/settings/SettingSlider.qml
        qml/settings/PoseWeightsEditor.qml
        qml/settings/DistanceTargetEvaluationSettings.qml
        qml/settings/WindowChainButton.qml
        qml/settings/SteeringUnits.qml
        qml/settings/SettingSwitch.qml
        qml/settings/SettingTextField.qml
        qml/settings/SmoothSteeringSettings.qml
        qml/settings/StuntPointsEvaluationSettings.qml
        qml/settings/StyledComboBox.qml
        qml/settings/TimeEvaluationSettings.qml
        qml/settings/TimeUnits.qml
        qml/settings/TimeWindowSettings.qml
        qml/settings/Vector3Settings.qml
        qml/settings/VelocityEvaluationSettings.qml
        qml/settings/VolumeEntryEvaluationSettings.qml)
# Bytecode only, no QML compiled to C++: Qt 6.9's garbage collector marks the
# values compiled functions hold and can read one that is gone, then hangs or
# aborts (Enter in a settings field, after a search;
# forevertas-qml-module-tests).
set_target_properties(forevertas_qml PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
file(GLOB FOREVERTAS_REPLACEMENT_TEXTURES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/*.png")
qt_add_resources(forevertas_qml "replacement-materials"
    PREFIX "/materials"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials"
    FILES ${FOREVERTAS_REPLACEMENT_TEXTURES})
# The map material of a render module's mode (RenderModuleView).
qt_add_resources(forevertas_qml "module-screen-material"
    PREFIX "/materials"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    FILES shaders/module_screen.frag)
file(GLOB FOREVERTAS_LUCIDE_ICONS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.svg")
qt_add_resources(forevertas_qml "lucide-icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
    FILES ${FOREVERTAS_LUCIDE_ICONS})
file(GLOB FOREVERTAS_ENVIRONMENT_TEXTURES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment/*.png")
qt_add_resources(forevertas_qml "environment-textures"
    PREFIX "/environment"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment"
    FILES ${FOREVERTAS_ENVIRONMENT_TEXTURES})
qt_add_shaders(forevertas_qml "glass-shaders"
    PREFIX "/glass"
    FILES
        shaders/ambient.frag
        shaders/glass.frag
        shaders/gloss.frag)
if(TARGET Qt6::GuiPrivate AND TARGET Qt6::ShaderTools)
    qt_add_shaders(forevertas_qml "ray-tracing-shaders"
        PREFIX "/raytracing"
        GLSL "310 es,430"
        FILES
            shaders/raytrace.comp
            shaders/raytrace_present.vert
            shaders/raytrace_present.frag)
endif()
endif()
target_link_libraries(forevertas PRIVATE
    forevertas_gui
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Quick3D
    Qt6::QuickControls2)
target_compile_definitions(forevertas PRIVATE
    FOREVERTAS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # The crash handler: stacks, module versions, display adapters, minidumps.
    target_link_libraries(forevertas PRIVATE dbghelp version psapi dxgi)
endif()
if(MSVC)
    # ForeverTAS.pdb stays with the build (not shipped) to read crash stacks
    # and dumps from users' logs; the code is the same as without it.
    target_link_options(forevertas PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
endif()

if(WIN32)
    set(forevertas_runtime_destination ".")
else()
    set(forevertas_runtime_destination "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS forevertas
    RUNTIME DESTINATION "${forevertas_runtime_destination}")
if(WIN32 AND TARGET SDL3::SDL3-shared)
    install(FILES "$<TARGET_FILE:SDL3::SDL3-shared>" DESTINATION ".")
endif()
install(TARGETS forevertas-simulation-debug-worker
    RUNTIME DESTINATION "${forevertas_runtime_destination}")
# Optional files a Windows build ships beside the executable, such as an
# optional render module and the DLLs it needs (the Linux AppImage takes
# FOREVERTAS_EXTRA_LIBRARIES instead). Plain builds have none.
set(FOREVERTAS_EXTRA_INSTALL_FILES "" CACHE STRING
    "Files installed beside the executable on Windows (a list)")
if(WIN32 AND FOREVERTAS_EXTRA_INSTALL_FILES)
    install(FILES ${FOREVERTAS_EXTRA_INSTALL_FILES} DESTINATION ".")
endif()
if(NOT FOREVERTAS_QML_MODULE_EXTERNAL)
    install(TARGETS forevertas_qml
        LIBRARY DESTINATION "${forevertas_runtime_destination}/qml/ForeverTAS"
        RUNTIME DESTINATION "${forevertas_runtime_destination}/qml/ForeverTAS")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/qml/ForeverTAS/qmldir"
        DESTINATION "${forevertas_runtime_destination}/qml/ForeverTAS")
else()
    if(NOT IS_DIRECTORY "${FOREVERTAS_COMMON_QML_DIR}")
        message(FATAL_ERROR "External QML module directory is missing")
    endif()
    install(DIRECTORY "${FOREVERTAS_COMMON_QML_DIR}/"
        DESTINATION "${forevertas_runtime_destination}/qml/ForeverTAS")
endif()

if(WIN32)
    set(forevertas_deploy_options NO_TRANSLATIONS)
    list(APPEND forevertas_deploy_options NO_COMPILER_RUNTIME)
    qt_generate_deploy_app_script(
        TARGET forevertas
        OUTPUT_SCRIPT forevertas_deploy_script
        DEPLOY_TOOL_OPTIONS --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
        ${forevertas_deploy_options})
    install(CODE
        "set(QT_DEPLOY_BIN_DIR \".\")\ninclude(\"${forevertas_deploy_script}\")")
endif()

if(WIN32)
    if(NOT FOREVERTAS_WINDOWS_RUNTIME_DIR
            AND DEFINED VCPKG_INSTALLED_DIR
            AND DEFINED VCPKG_TARGET_TRIPLET)
        set(FOREVERTAS_WINDOWS_RUNTIME_DIR
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    if(NOT IS_DIRECTORY "${FOREVERTAS_WINDOWS_RUNTIME_DIR}")
        message(FATAL_ERROR
            "FOREVERTAS_WINDOWS_RUNTIME_DIR must point to the vcpkg runtime "
            "directory containing OpenSSL and zlib DLLs")
    endif()
    if(NOT FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR
            AND DEFINED ENV{VCToolsRedistDir})
        set(FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR
            "$ENV{VCToolsRedistDir}/x64/Microsoft.VC143.CRT")
    endif()
    if(NOT IS_DIRECTORY "${FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR}")
        message(FATAL_ERROR
            "FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR must point to the "
            "Microsoft.VC143.CRT directory")
    endif()

    file(GLOB forevertas_openssl_runtime_dlls
        LIST_DIRECTORIES FALSE
        "${FOREVERTAS_WINDOWS_RUNTIME_DIR}/libcrypto-*.dll")
    file(GLOB forevertas_zlib_runtime_dlls
        LIST_DIRECTORIES FALSE
        "${FOREVERTAS_WINDOWS_RUNTIME_DIR}/z.dll"
        "${FOREVERTAS_WINDOWS_RUNTIME_DIR}/zlib.dll"
        "${FOREVERTAS_WINDOWS_RUNTIME_DIR}/zlib1.dll")
    # GPU modules arrive as .tar.zst
    file(GLOB forevertas_zstd_runtime_dlls
        LIST_DIRECTORIES FALSE
        "${FOREVERTAS_WINDOWS_RUNTIME_DIR}/zstd.dll")
    file(GLOB forevertas_msvc_runtime_dlls
        LIST_DIRECTORIES FALSE
        "${FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR}/*.dll")
    list(LENGTH forevertas_openssl_runtime_dlls
        forevertas_openssl_runtime_dll_count)
    list(LENGTH forevertas_zlib_runtime_dlls
        forevertas_zlib_runtime_dll_count)
    list(LENGTH forevertas_msvc_runtime_dlls
        forevertas_msvc_runtime_dll_count)
    if(NOT forevertas_openssl_runtime_dll_count EQUAL 1
            OR NOT forevertas_zlib_runtime_dll_count EQUAL 1)
        message(FATAL_ERROR
            "Expected exactly one OpenSSL crypto DLL and one zlib DLL in "
            "${FOREVERTAS_WINDOWS_RUNTIME_DIR}; found "
            "${forevertas_openssl_runtime_dll_count} OpenSSL and "
            "${forevertas_zlib_runtime_dll_count} zlib DLLs")
    endif()
    if(forevertas_msvc_runtime_dll_count EQUAL 0)
        message(FATAL_ERROR
            "No redistributable MSVC runtime DLLs found in "
            "${FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR}")
    endif()
    install(FILES
        ${forevertas_openssl_runtime_dlls}
        ${forevertas_zlib_runtime_dlls}
        ${forevertas_zstd_runtime_dlls}
        ${forevertas_msvc_runtime_dlls}
        DESTINATION ".")
endif()

if(WIN32)
    set(forevertas_license_destination "licenses")
else()
    set(forevertas_license_destination
        "${CMAKE_INSTALL_DATAROOTDIR}/doc/${FOREVERTAS_DISPLAY_NAME}/licenses")
endif()

install(FILES
    LICENSE
    packaging/licenses/LGPL-3.0-only.txt
    packaging/licenses/Qt-GPL-exception-1.0.txt
    packaging/licenses/THIRD_PARTY_NOTICES.md
    DESTINATION "${forevertas_license_destination}")
if(EXISTS "${forevervalidator_SOURCE_DIR}/LICENSE")
    install(FILES "${forevervalidator_SOURCE_DIR}/LICENSE"
        DESTINATION "${forevertas_license_destination}"
        RENAME ForeverValidator-LICENSE.txt)
endif()
# The licenses of the extra files a release ships (its In-game render module's
# libraries). Plain builds have none.
set(FOREVERTAS_EXTRA_LICENSE_FILES "" CACHE STRING
    "License files installed with the app's, under render-module (a list)")
if(FOREVERTAS_EXTRA_LICENSE_FILES)
    install(FILES ${FOREVERTAS_EXTRA_LICENSE_FILES}
        DESTINATION "${forevertas_license_destination}/render-module")
endif()

if(UNIX)
    install(FILES packaging/linux/${FOREVERTAS_APP_ID}.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    install(FILES packaging/linux/${FOREVERTAS_APP_ID}.metainfo.xml
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo"
        RENAME ${FOREVERTAS_APP_ID}.appdata.xml)
    install(FILES packaging/icons/${FOREVERTAS_APP_ID}.svg
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")
    install(FILES packaging/icons/${FOREVERTAS_APP_ID}.png
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps")
endif()

set(forevertas_package_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(forevertas_package_arch MATCHES "^(AMD64|amd64|x86_64)$")
    set(forevertas_package_arch "x86_64")
elseif(forevertas_package_arch MATCHES "^(ARM64|arm64|aarch64)$")
    set(forevertas_package_arch "arm64")
endif()

if(WIN32)
    set(forevertas_package_platform "windows")
    set(CPACK_GENERATOR "ZIP")
else()
    set(forevertas_package_platform "linux")
    set(CPACK_GENERATOR "TGZ")
endif()

# One app per platform; its GPU code comes as a module (src/compute).
set(forevertas_update_asset_parts "${forevertas_package_platform};universal")
list(APPEND forevertas_update_asset_parts "${forevertas_package_arch}")
list(JOIN forevertas_update_asset_parts "-" FOREVERTAS_UPDATE_ASSET_ID)
set(FOREVERTAS_UPDATE_ASSET_ID "${FOREVERTAS_UPDATE_ASSET_ID}" CACHE INTERNAL
    "Platform and backend identity of public update artifacts" FORCE)
target_compile_definitions(forevertas PRIVATE
    FOREVERTAS_UPDATE_PLATFORM="${forevertas_package_platform}"
    FOREVERTAS_UPDATE_ARCH="${forevertas_package_arch}")

set(CPACK_PACKAGE_NAME "${FOREVERTAS_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "Skycrafter-dev")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${FOREVERTAS_DISPLAY_NAME}-${PROJECT_VERSION}-${forevertas_package_platform}-${forevertas_package_arch}")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
        AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(FOREVERTAS_WARNING_OPTIONS /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(FOREVERTAS_WARNING_OPTIONS -Wall -Wextra -Werror)
endif()

if(FOREVERTAS_WARNING_OPTIONS)
    target_compile_options(forevertas_core PRIVATE
        ${FOREVERTAS_WARNING_OPTIONS})
    target_compile_options(forevertas_gui PRIVATE
        ${FOREVERTAS_WARNING_OPTIONS})
    target_compile_options(forevertas PRIVATE
        ${FOREVERTAS_WARNING_OPTIONS})
endif()

if(BUILD_TESTING)
    if(Python3_Interpreter_FOUND)
        add_test(NAME forevertas-release-tools
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_tools_tests.py")
    endif()
    add_executable(forevertas-update-release-tests
        src/app/update_release.cpp
        tests/update_release_tests.cpp)
    target_include_directories(forevertas-update-release-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forevertas-update-release-tests PRIVATE
        Qt6::Core Qt6::Test)
    add_test(NAME forevertas-update-release
             COMMAND forevertas-update-release-tests)
    if(UNIX AND NOT APPLE)
        add_test(NAME forevertas-appimage-update-smoke
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/update_appimage_smoke.sh
                         $<TARGET_FILE:forevertas>)
    endif()
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-update-release-tests PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
    endif()

    add_executable(forevertas-strategy-tests
        tests/search_component_tests.cpp)
    target_link_libraries(forevertas-strategy-tests PRIVATE forevertas_core)
    add_executable(forevertas-condition-catalog-tests tests/condition_catalog_tests.cpp)
    target_link_libraries(forevertas-condition-catalog-tests PRIVATE forevertas_core)
    target_compile_definitions(forevertas-condition-catalog-tests PRIVATE
        FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-condition-catalog-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-condition-catalog COMMAND forevertas-condition-catalog-tests)

    add_executable(forevertas-gate-entry-tests tests/gate_entry_tests.cpp)
    target_link_libraries(forevertas-gate-entry-tests PRIVATE forevertas_core)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-gate-entry-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-gate-entry COMMAND forevertas-gate-entry-tests)

    add_executable(forevertas-go-to-tests tests/go_to_tests.cpp)
    target_link_libraries(forevertas-go-to-tests PRIVATE forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-go-to-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-go-to COMMAND forevertas-go-to-tests)

    # the vanishing car's cause (needs a display: run under xvfb-run when there is one)
    add_executable(forevertas-geometry-return-tests tests/geometry_return_tests.cpp)
    target_link_libraries(forevertas-geometry-return-tests PRIVATE forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-geometry-return-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    find_program(FOREVERTAS_XVFB_RUN xvfb-run)
    if(FOREVERTAS_XVFB_RUN)
        add_test(NAME forevertas-geometry-return
                 COMMAND ${FOREVERTAS_XVFB_RUN} -a $<TARGET_FILE:forevertas-geometry-return-tests>)
        set_tests_properties(forevertas-geometry-return PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=xcb;QSG_RHI_BACKEND=opengl;LIBGL_ALWAYS_SOFTWARE=1;__GLX_VENDOR_LIBRARY_NAME=mesa"
            TIMEOUT 60)
    endif()

    add_executable(forevertas-dock-layout-tests tests/dock_layout_tests.cpp)
    target_link_libraries(forevertas-dock-layout-tests PRIVATE forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-dock-layout-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-dock-layout COMMAND forevertas-dock-layout-tests)

    # Manual benchmarks: need a copy of the persisted settings and game packs.
    add_executable(forevertas-node-cost-benchmark
        tests/node_cost_benchmark.cpp)
    target_link_libraries(forevertas-node-cost-benchmark PRIVATE
        forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-node-cost-benchmark PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_executable(forevertas-search-lab
        tests/search_lab.cpp)
    target_link_libraries(forevertas-search-lab PRIVATE
        forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-search-lab PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_executable(forevertas-search-strategy-comparison
        tests/search_strategy_comparison.cpp)
    target_link_libraries(forevertas-search-strategy-comparison PRIVATE
        forevertas_gui)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-search-strategy-comparison PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
    endif()

    add_executable(forevertas-controller-tests
        tests/search_controller_tests.cpp)
    add_executable(forevertas-shutdown-tests tests/shutdown_tests.cpp src/app/shutdown_controller.cpp)
    add_executable(forevertas-debugger-tools-tests tests/debugger_tools_tests.cpp src/viewer/debugger_tools.cpp)
    target_include_directories(forevertas-debugger-tools-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forevertas-debugger-tools-tests PRIVATE Qt6::Core)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-debugger-tools-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-debugger-tools COMMAND forevertas-debugger-tools-tests)
    add_executable(forevertas-compute-archive-tests tests/compute_archive_tests.cpp src/app/compute_archive.cpp)
    target_include_directories(forevertas-compute-archive-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        "${FOREVERTAS_ZSTD_INCLUDE_DIR}")
    target_link_libraries(forevertas-compute-archive-tests PRIVATE Qt6::Core "${FOREVERTAS_ZSTD_LIBRARY}")
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-compute-archive-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-compute-archive COMMAND forevertas-compute-archive-tests)
    target_include_directories(forevertas-shutdown-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forevertas-shutdown-tests PRIVATE Qt6::Core)
    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-shutdown-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
    endif()
    add_test(NAME forevertas-shutdown COMMAND forevertas-shutdown-tests)
    target_link_libraries(forevertas-controller-tests PRIVATE
        forevertas_gui
        Qt6::Test)

    add_executable(forevertas-renderer-tests
        tests/renderer_tests.cpp)
    target_compile_definitions(forevertas-renderer-tests PRIVATE
        FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(forevertas-renderer-tests PRIVATE
        forevertas_gui
        Qt6::Gui)

    add_executable(forevertas-whiteboard-tests
        tests/whiteboard_tests.cpp)
    target_compile_definitions(forevertas-whiteboard-tests PRIVATE
        FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(forevertas-whiteboard-tests PRIVATE
        forevertas_gui
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2)

    if(FOREVERTAS_WARNING_OPTIONS)
        target_compile_options(forevertas-strategy-tests PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
        target_compile_options(forevertas-controller-tests PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
        target_compile_options(forevertas-renderer-tests PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
        target_compile_options(forevertas-whiteboard-tests PRIVATE
            ${FOREVERTAS_WARNING_OPTIONS})
    endif()

    add_test(NAME forevertas-strategies
             COMMAND forevertas-strategy-tests)
    add_test(NAME forevertas-no-replay-duration-control
             COMMAND ${CMAKE_COMMAND}
                -DFOREVERTAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_replay_duration_control.cmake)
    add_test(NAME forevertas-system-file-dialog-control
             COMMAND ${CMAKE_COMMAND}
                -DFOREVERTAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/system_file_dialog_control.cmake)
    add_test(NAME forevertas-controller
             COMMAND forevertas-controller-tests)
    set_tests_properties(forevertas-controller PROPERTIES
        RESOURCE_LOCK controller-settings)
    add_test(NAME forevertas-renderer
             COMMAND forevertas-renderer-tests)
    add_test(NAME forevertas-whiteboard
             COMMAND forevertas-whiteboard-tests)
    set_tests_properties(forevertas-renderer PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(forevertas-whiteboard PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_SCALE_FACTOR=1")
    else()
        set_tests_properties(forevertas-whiteboard PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    add_test(NAME forevertas-qml-smoke
             COMMAND forevertas --qml-smoke-test)
    set_tests_properties(forevertas-qml-smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software")

    add_test(NAME forevertas-install-smoke
        COMMAND "${CMAKE_COMMAND}"
            -DFOREVERTAS_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DFOREVERTAS_STAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-smoke
            -DFOREVERTAS_CONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_smoke.cmake)

    set(FOREVERTAS_TEST_PACK_DIRECTORY "" CACHE PATH
        "Installed TMUF Packs directory used by the optional search smoke test")
    set(FOREVERTAS_TEST_REPLAY "" CACHE FILEPATH
        "Replay used by the optional search smoke test")
    set(FOREVERTAS_TEST_SECOND_REPLAY "" CACHE FILEPATH
        "Distinct replay used by optional viewer reload tests")
    set(FOREVERTAS_TEST_CHALLENGE_REPLAY "" CACHE FILEPATH
        "Replay paired with FOREVERTAS_TEST_CHALLENGE")
    set(FOREVERTAS_TEST_CHALLENGE "" CACHE FILEPATH
        "Standalone Challenge.Gbx used by optional scenario tests")
    if(FOREVERTAS_ENABLE_TWIN)
        # forevertas-approximate-search PACKS REPLAY BRANCH_MS WINDOW_MS [ATTEMPTS]
        add_executable(forevertas-approximate-search
            tests/approximate_search_tests.cpp)
        target_link_libraries(forevertas-approximate-search PRIVATE
            forevertas_core)
    endif()
    if(FOREVERTAS_TEST_PACK_DIRECTORY AND FOREVERTAS_TEST_REPLAY)
        add_executable(forevertas-search-smoke
            tests/search_smoke.cpp)
        target_link_libraries(forevertas-search-smoke PRIVATE
            forevertas_core)
        if(FOREVERTAS_ENABLE_CUDA)
            target_include_directories(forevertas-search-smoke PRIVATE
                "${forevervalidator_SOURCE_DIR}/src")
        endif()
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-search-smoke PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        set(FOREVERTAS_SEARCH_SMOKE_ARGUMENTS
            "${FOREVERTAS_TEST_PACK_DIRECTORY}"
            "${FOREVERTAS_TEST_REPLAY}")
        if(FOREVERTAS_TEST_CHALLENGE_REPLAY AND
           FOREVERTAS_TEST_CHALLENGE)
            list(APPEND FOREVERTAS_SEARCH_SMOKE_ARGUMENTS
                "${FOREVERTAS_TEST_CHALLENGE_REPLAY}"
                "${FOREVERTAS_TEST_CHALLENGE}")
        endif()
        add_test(NAME forevertas-search-smoke
                 COMMAND forevertas-search-smoke
                     ${FOREVERTAS_SEARCH_SMOKE_ARGUMENTS})
        add_test(NAME forevertas-finish-export
                 COMMAND forevertas-search-smoke --finish-export-only
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-finish-export PROPERTIES TIMEOUT 120)
        if(FOREVERTAS_ENABLE_HIP)
            add_executable(forevertas-hip-search-parity
                tests/hip_search_parity_tests.cpp)
            target_link_libraries(forevertas-hip-search-parity PRIVATE
                forevertas_core)
            if(FOREVERTAS_WARNING_OPTIONS)
                target_compile_options(forevertas-hip-search-parity PRIVATE
                    ${FOREVERTAS_WARNING_OPTIONS})
            endif()
            add_test(NAME forevertas-hip-search-parity
                COMMAND forevertas-hip-search-parity
                    "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                    "${FOREVERTAS_TEST_REPLAY}")
            add_test(NAME forevertas-hip-precise-finish-parity
                COMMAND forevertas-hip-search-parity --precise-finish-only
                    "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                    "${FOREVERTAS_TEST_REPLAY}")
            add_test(NAME forevertas-hip-calibration
                COMMAND forevertas-hip-search-parity --calibration-only
                    "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                    "${FOREVERTAS_TEST_REPLAY}")
            set_tests_properties(
                forevertas-hip-search-parity
                forevertas-hip-precise-finish-parity
                forevertas-hip-calibration PROPERTIES
                    LABELS "hip;parity;stadium"
                    RESOURCE_LOCK hip
                    RUN_SERIAL TRUE
                    TIMEOUT 300)
        endif()
        if(FOREVERTAS_ENABLE_CUDA)
            set_tests_properties(forevertas-search-smoke PROPERTIES
                RESOURCE_LOCK cuda)
            add_executable(forevertas-cuda-search-parity
                tests/cuda_search_parity_tests.cpp)
            target_link_libraries(forevertas-cuda-search-parity PRIVATE
                forevertas_core)
            if(FOREVERTAS_WARNING_OPTIONS)
                target_compile_options(forevertas-cuda-search-parity PRIVATE
                    ${FOREVERTAS_WARNING_OPTIONS})
            endif()
            set(FOREVERTAS_CUDA_SEARCH_PARITY_ARGUMENTS
                "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                "${FOREVERTAS_TEST_REPLAY}")
            if(FOREVERTAS_TEST_CHALLENGE_REPLAY)
                list(APPEND FOREVERTAS_CUDA_SEARCH_PARITY_ARGUMENTS
                    "${FOREVERTAS_TEST_CHALLENGE_REPLAY}")
            endif()
            add_test(NAME forevertas-cuda-search-parity
                     COMMAND forevertas-cuda-search-parity
                         ${FOREVERTAS_CUDA_SEARCH_PARITY_ARGUMENTS})
            set_tests_properties(forevertas-cuda-search-parity PROPERTIES
                LABELS "cuda;parity;performance;stadium"
                RESOURCE_LOCK cuda
                RUN_SERIAL TRUE
                TIMEOUT 240)
            add_test(NAME forevertas-cuda-precise-finish-parity
                     COMMAND forevertas-cuda-search-parity
                         --precise-finish-only
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}")
            set_tests_properties(
                forevertas-cuda-precise-finish-parity PROPERTIES
                LABELS "cuda;parity;precise-finish;stadium"
                RESOURCE_LOCK cuda
                RUN_SERIAL TRUE
                TIMEOUT 300)
            add_test(NAME forevertas-cuda-calibration
                     COMMAND forevertas-cuda-search-parity
                         --calibration-only
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}")
            set_tests_properties(forevertas-cuda-calibration PROPERTIES
                LABELS "cuda;calibration;parity;stadium"
                RESOURCE_LOCK cuda
                TIMEOUT 600)
            add_test(NAME forevertas-gpu-pool
                     COMMAND forevertas-cuda-search-parity
                         --gpu-pool
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}")
            set_tests_properties(forevertas-gpu-pool PROPERTIES
                LABELS "cuda;stadium"
                RESOURCE_LOCK cuda
                TIMEOUT 60)
        endif()
        add_test(NAME forevertas-controller-lifecycle
                 COMMAND forevertas-controller-tests
                     --lifecycle
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-controller-lifecycle PROPERTIES
            RESOURCE_LOCK controller-settings)

        add_executable(forevertas-render-scene-data-smoke
            tests/render_scene_data_smoke.cpp)
        target_link_libraries(forevertas-render-scene-data-smoke PRIVATE
            forevertas_core)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-render-scene-data-smoke PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-render-scene-data
                 COMMAND forevertas-render-scene-data-smoke
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-render-scene-data PROPERTIES
            TIMEOUT 180)

        add_executable(forevertas-viewer-reload-smoke
            tests/viewer_reload_smoke.cpp)
        add_executable(forevertas-lazy-ray-tracing-smoke tests/lazy_ray_tracing_smoke.cpp)
        add_executable(forevertas-search-preview-tests tests/search_preview_tests.cpp)
        add_executable(forevertas-car-presence-tests tests/car_presence_tests.cpp)
        target_link_libraries(forevertas-car-presence-tests PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-car-presence-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-car-presence COMMAND forevertas-car-presence-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-car-presence PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
        add_executable(forevertas-viewer-failure-tests tests/viewer_failure_tests.cpp)
        target_link_libraries(forevertas-viewer-failure-tests PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-viewer-failure-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-viewer-failure COMMAND forevertas-viewer-failure-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-viewer-failure PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
        add_executable(forevertas-gamepad-tests tests/gamepad_tests.cpp)
        target_link_libraries(forevertas-gamepad-tests PRIVATE forevertas_gui Qt6::Gui)
        add_test(NAME forevertas-gamepad COMMAND forevertas-gamepad-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-gamepad PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        add_executable(forevertas-live-race-tests tests/live_race_tests.cpp)
        target_link_libraries(forevertas-live-race-tests PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-live-race-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-live-race COMMAND forevertas-live-race-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-live-race PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        add_executable(forevertas-live-retention-tests tests/live_retention_tests.cpp)
        target_link_libraries(forevertas-live-retention-tests PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-live-retention-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-live-retention COMMAND forevertas-live-retention-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-live-retention PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        target_link_libraries(forevertas-search-preview-tests PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-search-preview-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-search-preview COMMAND forevertas-search-preview-tests
                 "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-search-preview PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        add_executable(forevertas-time-target-tests tests/time_target_tests.cpp)
        target_link_libraries(forevertas-time-target-tests PRIVATE forevertas_core)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-time-target-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-time-target
                 COMMAND forevertas-time-target-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-time-target PROPERTIES TIMEOUT 180)
        add_executable(forevertas-checkpoint-time-tests tests/checkpoint_time_tests.cpp)
        target_link_libraries(forevertas-checkpoint-time-tests PRIVATE forevertas_core)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-checkpoint-time-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-checkpoint-time
                 COMMAND forevertas-checkpoint-time-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-checkpoint-time PROPERTIES TIMEOUT 240)
        add_executable(forevertas-distance-around-walls-tests tests/distance_around_walls_tests.cpp)
        target_link_libraries(forevertas-distance-around-walls-tests PRIVATE forevertas_core)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-distance-around-walls-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-distance-around-walls
                 COMMAND forevertas-distance-around-walls-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-distance-around-walls PROPERTIES TIMEOUT 60)
        # car.localvel on each CPU backend, also as the condition program reads it
        add_executable(forevertas-local-velocity-tests tests/local_velocity_tests.cpp)
        target_link_libraries(forevertas-local-velocity-tests PRIVATE forevertas_core Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-local-velocity-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-local-velocity
                 COMMAND forevertas-local-velocity-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-local-velocity PROPERTIES TIMEOUT 60)
        if(FOREVERTAS_ENABLE_CUDA)
            # Both compare CPU results with CUDA sessions, whose memory
            # headroom check fails while other tests use the GPU.
            set_tests_properties(forevertas-time-target forevertas-checkpoint-time
                PROPERTIES RESOURCE_LOCK cuda RUN_SERIAL TRUE)
        endif()
        add_executable(forevertas-volume-completion-tests tests/volume_completion_tests.cpp)
        target_link_libraries(forevertas-volume-completion-tests PRIVATE forevertas_core)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-volume-completion-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-volume-completion
                 COMMAND forevertas-volume-completion-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-volume-completion PROPERTIES TIMEOUT 240)
        target_link_libraries(forevertas-lazy-ray-tracing-smoke PRIVATE forevertas_gui Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-lazy-ray-tracing-smoke PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-lazy-ray-tracing
                 COMMAND forevertas-lazy-ray-tracing-smoke
                    "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-lazy-ray-tracing PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        target_link_libraries(forevertas-viewer-reload-smoke PRIVATE
            forevertas_gui
            Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-viewer-reload-smoke PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-viewer-reload-same
                 COMMAND forevertas-viewer-reload-smoke
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-viewer-reload-same PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 180)

        if(FOREVERTAS_TEST_SECOND_REPLAY)
            add_test(NAME forevertas-viewer-reload
                     COMMAND forevertas-viewer-reload-smoke
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}"
                         "${FOREVERTAS_TEST_SECOND_REPLAY}")
            set_tests_properties(forevertas-viewer-reload PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 180)

        endif()

        add_executable(forevertas-viewer-smoke
            tests/viewer_smoke.cpp)
        target_link_libraries(forevertas-viewer-smoke PRIVATE
            forevertas_gui
            Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-viewer-smoke PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-viewer
                 COMMAND forevertas-viewer-smoke
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        add_test(NAME forevertas-base-preview
                 COMMAND forevertas-viewer-smoke --base-preview
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-base-preview PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
        set_tests_properties(forevertas-viewer PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 180)
        if(FOREVERTAS_TEST_CHALLENGE)
            add_test(NAME forevertas-viewer-challenge
                     COMMAND forevertas-viewer-smoke
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_CHALLENGE}")
            set_tests_properties(forevertas-viewer-challenge PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 180)
        endif()

        add_executable(forevertas-simulation-debugger-tests
            tests/simulation_debugger_tests.cpp)
        target_link_libraries(forevertas-simulation-debugger-tests PRIVATE
            forevertas_gui
            Qt6::Gui)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(
                forevertas-simulation-debugger-tests PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        if(FOREVERTAS_LLDB_EXECUTABLE AND ((WIN32 AND FOREVERTAS_WINPTY_EXECUTABLE) OR
                                         (NOT WIN32 AND FOREVERTAS_SCRIPT_EXECUTABLE)))
            add_test(NAME forevertas-simulation-debugger
                     COMMAND forevertas-simulation-debugger-tests
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}")
            set_tests_properties(forevertas-simulation-debugger PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 180)
        endif()

        if(FOREVERTAS_TEST_SECOND_REPLAY)
            add_executable(forevertas-viewer-reload-qml-smoke
                tests/viewer_reload_qml_smoke.cpp)
            target_compile_definitions(forevertas-viewer-reload-qml-smoke PRIVATE
                FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
            target_link_libraries(forevertas-viewer-reload-qml-smoke PRIVATE
                forevertas_gui
                Qt6::Qml
                Qt6::Quick
                Qt6::QuickControls2
                Qt6::Widgets)
            if(FOREVERTAS_WARNING_OPTIONS)
                target_compile_options(forevertas-viewer-reload-qml-smoke PRIVATE
                    ${FOREVERTAS_WARNING_OPTIONS})
            endif()
            add_test(NAME forevertas-viewer-reload-qml
                     COMMAND forevertas-viewer-reload-qml-smoke
                         "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                         "${FOREVERTAS_TEST_REPLAY}"
                         "${FOREVERTAS_TEST_SECOND_REPLAY}")
            set_tests_properties(forevertas-viewer-reload-qml PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software"
                TIMEOUT 180)

        endif()

        add_executable(forevertas-viewer-qml-smoke
            tests/viewer_qml_smoke.cpp)
        qt_add_resources(forevertas-viewer-qml-smoke
            "replacement-materials-test"
            PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials"
            FILES ${FOREVERTAS_REPLACEMENT_TEXTURES})
        qt_add_resources(forevertas-viewer-qml-smoke
            "module-screen-material-test"
            PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            FILES shaders/module_screen.frag)
        qt_add_resources(forevertas-viewer-qml-smoke
            "environment-textures-test"
            PREFIX "/environment"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment"
            FILES ${FOREVERTAS_ENVIRONMENT_TEXTURES})
        qt_add_resources(forevertas-viewer-qml-smoke
            "lucide-icons-test"
            PREFIX "/icons"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
            FILES ${FOREVERTAS_LUCIDE_ICONS})
        qt_add_shaders(forevertas-viewer-qml-smoke "glass-shaders-test"
            PREFIX "/glass"
            FILES
                shaders/ambient.frag
                shaders/glass.frag
                shaders/gloss.frag)
        target_compile_definitions(forevertas-viewer-qml-smoke PRIVATE
            FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(forevertas-viewer-qml-smoke PRIVATE
            forevertas_gui
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::Quick3D
            Qt6::QuickControls2
            Qt6::Widgets)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-viewer-qml-smoke PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-viewer-qml
                 COMMAND forevertas-viewer-qml-smoke
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-viewer-qml PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software"
            TIMEOUT 180)
        # UI checks in the QML module the app runs: hidden headers come back;
        # Enter in every settings field after a search: no hang, no runaway memory.
        add_executable(forevertas-qml-module-tests tests/qml_module_tests.cpp)
        add_dependencies(forevertas-qml-module-tests forevertas_qml)
        target_compile_definitions(forevertas-qml-module-tests PRIVATE
            FOREVERTAS_QML_IMPORT_DIR="${CMAKE_CURRENT_BINARY_DIR}/bin/qml")
        target_link_libraries(forevertas-qml-module-tests PRIVATE
            forevertas_gui Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-qml-module-tests PRIVATE
                ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-qml-module
                 COMMAND forevertas-qml-module-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-qml-module PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software"
            TIMEOUT 60)
        # MCP: the bridge and relay in process, and on a map.
        # Remote searching (docs/REMOTE.md): workers, rooms and groups
        add_executable(forevertas-remote-tests tests/remote_tests.cpp)
        target_link_libraries(forevertas-remote-tests PRIVATE forevertas_gui Qt6::Core Qt6::WebSockets)
        target_compile_definitions(forevertas-remote-tests PRIVATE FOREVERTAS_VERSION="${PROJECT_VERSION}")
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-remote-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-remote
                 COMMAND forevertas-remote-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-remote PROPERTIES TIMEOUT 60)

        add_executable(forevertas-mcp-tests tests/mcp_tests.cpp)
        target_link_libraries(forevertas-mcp-tests PRIVATE
            forevertas_gui Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::Widgets)
        if(FOREVERTAS_WARNING_OPTIONS)
            target_compile_options(forevertas-mcp-tests PRIVATE ${FOREVERTAS_WARNING_OPTIONS})
        endif()
        add_test(NAME forevertas-mcp
                 COMMAND forevertas-mcp-tests
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-mcp PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 60)
        # The window thread's time per mouse move while dragging (not a test):
        # forevertas-drag-benchmark PACKS REPLAY
        add_executable(forevertas-drag-benchmark tests/drag_benchmark.cpp)
        add_dependencies(forevertas-drag-benchmark forevertas_qml)
        target_compile_definitions(forevertas-drag-benchmark PRIVATE
            FOREVERTAS_QML_IMPORT_DIR="${CMAKE_CURRENT_BINARY_DIR}/bin/qml")
        target_link_libraries(forevertas-drag-benchmark PRIVATE
            forevertas_gui Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)
        # Screenshots of UI states, by eye (not a test): forevertas-ui-capture PACKS REPLAY OUT_DIR STATE...
        add_executable(forevertas-ui-capture tests/ui_capture.cpp src/app/diagnostics_startup.cpp)
        qt_add_resources(forevertas-ui-capture "module-screen-material-capture" PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders" FILES shaders/module_screen.frag)
        qt_add_resources(forevertas-ui-capture "environment-textures-capture" PREFIX "/environment"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment" FILES ${FOREVERTAS_ENVIRONMENT_TEXTURES})
        qt_add_resources(forevertas-ui-capture "replacement-materials-capture" PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials" FILES ${FOREVERTAS_REPLACEMENT_TEXTURES})
        qt_add_resources(forevertas-ui-capture "lucide-icons-capture" PREFIX "/icons"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons" FILES ${FOREVERTAS_LUCIDE_ICONS})
        qt_add_shaders(forevertas-ui-capture "glass-shaders-capture" PREFIX "/glass"
            FILES shaders/ambient.frag shaders/glass.frag shaders/gloss.frag)
        target_compile_definitions(forevertas-ui-capture PRIVATE FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(forevertas-ui-capture PRIVATE FOREVERTAS_VERSION="${PROJECT_VERSION}")
        if(WIN32)
            target_link_libraries(forevertas-ui-capture PRIVATE dbghelp version psapi dxgi)
        endif()
        target_link_libraries(forevertas-ui-capture PRIVATE forevertas_gui Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Quick3D
            Qt6::QuickControls2 Qt6::Widgets)
        # The Export dialog's video export, by hand on a platform: forevertas-video-export PACKS REPLAY OUT.mp4 [W H FPS]
        add_executable(forevertas-video-export tests/video_export_test.cpp)
        target_link_libraries(forevertas-video-export PRIVATE forevertas_gui Qt6::Gui Qt6::Widgets)
        # The vanishing car, hunted: forevertas-car-stress PACKS REPLAY OUT_DIR [STEPS [SEED]] (under a display)
        add_executable(forevertas-car-stress tests/car_stress.cpp)
        # the car geometries' scene references (a Qt Quick 3D reference count)
        find_package(Qt6 QUIET COMPONENTS Quick3DPrivate)
        if(TARGET Qt6::Quick3DPrivate)
            target_link_libraries(forevertas-car-stress PRIVATE Qt6::Quick3DPrivate)
            target_compile_definitions(forevertas-car-stress PRIVATE FOREVERTAS_QUICK3D_PRIVATE=1)
        endif()
        qt_add_resources(forevertas-car-stress "module-screen-material-stress" PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders" FILES shaders/module_screen.frag)
        qt_add_resources(forevertas-car-stress "environment-textures-stress" PREFIX "/environment"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment" FILES ${FOREVERTAS_ENVIRONMENT_TEXTURES})
        qt_add_resources(forevertas-car-stress "replacement-materials-stress" PREFIX "/materials"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials" FILES ${FOREVERTAS_REPLACEMENT_TEXTURES})
        qt_add_resources(forevertas-car-stress "lucide-icons-stress" PREFIX "/icons"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons" FILES ${FOREVERTAS_LUCIDE_ICONS})
        qt_add_shaders(forevertas-car-stress "glass-shaders-stress" PREFIX "/glass"
            FILES shaders/ambient.frag shaders/glass.frag shaders/gloss.frag)
        target_compile_definitions(forevertas-car-stress PRIVATE FOREVERTAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(forevertas-car-stress PRIVATE forevertas_gui Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Quick3D
            Qt6::QuickControls2 Qt6::Widgets)
        add_test(NAME forevertas-pass-management-qml
                 COMMAND forevertas-viewer-qml-smoke
                     "${FOREVERTAS_TEST_PACK_DIRECTORY}"
                     "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-pass-management-qml PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;FOREVERTAS_PASS_UI_ONLY=1"
            TIMEOUT 30)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(forevertas-gpu-backend-tests tests/gpu_backend_tests.cpp)
    target_link_libraries(forevertas-gpu-backend-tests PRIVATE forevertas_core)
    add_test(NAME forevertas-gpu-backends COMMAND forevertas-gpu-backend-tests)
endif()

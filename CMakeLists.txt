cmake_minimum_required(VERSION 3.21)

if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "ForeverTAS does not support macOS")
endif()

project(ForeverTAS
    VERSION 0.2.4
    DESCRIPTION "Native TrackMania Nations Forever tool-assisted search tool"
    HOMEPAGE_URL "https://github.com/Skycrafter-dev/ForeverTAS"
    LANGUAGES CXX)

option(FOREVERTAS_ENABLE_VULKAN
    "Build ForeverTAS with the optional ForeverValidator Vulkan backend" ON)
option(FOREVERTAS_ENABLE_CUDA
    "Build ForeverTAS with the optional ForeverValidator CUDA backend" OFF)
option(FOREVERTAS_ENABLE_HIP
    "Build ForeverTAS with the optional ForeverValidator HIP backend" OFF)
option(FOREVERTAS_QML_MODULE_EXTERNAL
    "Use the release's shared QML module instead of building it here" OFF)
set(FOREVERTAS_COMMON_QML_DIR "" CACHE PATH
    "Directory containing the shared ForeverTAS QML module")
set(FOREVERTAS_PREBUILT_VALIDATOR_DIR "" CACHE PATH
    "Directory containing a verified, matching Windows Validator build")
set(FOREVERTAS_DISTRIBUTION_FLAVOR "" CACHE STRING
    "Public package flavor: universal, nvidia-smNN, or amd-rx7000-rx9000")
set(FOREVERTAS_WINDOWS_RUNTIME_DIR "" CACHE PATH
    "Directory containing non-Qt runtime DLLs for Windows packaging")
set(FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR "" CACHE PATH
    "Directory containing redistributable MSVC runtime DLLs")
set(FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR "" CACHE PATH
    "Directory containing redistributable CUDA runtime DLLs")
set(FOREVERTAS_WINDOWS_HIP_RUNTIME_DIR "" CACHE PATH
    "Directory containing redistributable HIP runtime DLLs")

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
find_program(FOREVERTAS_LLDB_EXECUTABLE NAMES lldb)
find_program(FOREVERTAS_SCRIPT_EXECUTABLE NAMES script)

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
    Widgets)
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

set(FOREVERVALIDATOR_BUILD_CLI OFF CACHE BOOL
    "Do not build the embedded ForeverValidator CLI" FORCE)
set(FOREVERVALIDATOR_BUILD_TESTS OFF CACHE BOOL
    "Do not build embedded ForeverValidator tests" FORCE)
set(FOREVERVALIDATOR_ENABLE_CUDA ${FOREVERTAS_ENABLE_CUDA} CACHE BOOL
    "Enable the embedded ForeverValidator CUDA backend" FORCE)
set(FOREVERVALIDATOR_ENABLE_HIP ${FOREVERTAS_ENABLE_HIP} CACHE BOOL
    "Enable the embedded ForeverValidator HIP backend" FORCE)
set(FOREVERVALIDATOR_ENABLE_VULKAN ${FOREVERTAS_ENABLE_VULKAN} CACHE BOOL
    "Enable the embedded ForeverValidator Vulkan backend" FORCE)
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
    if(FOREVERTAS_ENABLE_VULKAN)
        find_package(Vulkan 1.2 REQUIRED)
        list(APPEND validator_libraries Vulkan::Vulkan)
    endif()
    if(FOREVERTAS_ENABLE_CUDA OR
            (FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "nvidia"))
        find_package(CUDAToolkit REQUIRED)
        list(APPEND validator_libraries CUDA::cudart)
        if(FOREVERTAS_ENABLE_CUDA)
            list(APPEND validator_libraries CUDA::nvrtc CUDA::nvJitLink)
        endif()
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
            "FOREVERVALIDATOR_HAS_CUDA=$<BOOL:${FOREVERTAS_ENABLE_CUDA}>;FOREVERVALIDATOR_HAS_HIP=$<BOOL:${FOREVERTAS_ENABLE_HIP}>;FOREVERVALIDATOR_HAS_VULKAN=$<BOOL:${FOREVERTAS_ENABLE_VULKAN}>")
    if(FOREVERTAS_ENABLE_HIP)
        find_path(FOREVERVALIDATOR_HIP_INCLUDE_DIR hip/hip_runtime.h
            HINTS "$ENV{HIP_PATH}/include" "$ENV{ROCM_PATH}/include" REQUIRED)
    endif()
else()
    FetchContent_Declare(
        ForeverValidator
        GIT_REPOSITORY https://github.com/Skycrafter-dev/ForeverValidator.git
        GIT_TAG a89a8e80d1835826b401e757e2159c501544e8d4
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
    src/evaluators/evaluator_utils.h
    src/evaluators/precise_finish_time_evaluator.cpp
    src/evaluators/precise_finish_time_evaluator.h
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
    src/searches/algorithm_registry.cpp
    src/searches/algorithm_registry.h
    src/searches/option_configuration.h
    src/searches/option_settings_utils.h
    src/searches/basic_brute_force_search.cpp
    src/searches/basic_brute_force_search.h
    src/searches/cuda_batch_calibrator.cpp
    src/searches/cuda_batch_calibrator.h
    src/searches/cuda_calibration_safety.cpp
    src/searches/cuda_calibration_safety.h
    src/searches/cuda_search_configuration.cpp
    src/searches/cuda_search_configuration.h
    src/searches/search_runner.cpp
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
    src/viewer/race_geometry.cpp
    src/viewer/race_geometry.h
    src/viewer/ray_tracing_scene.cpp
    src/viewer/ray_tracing_scene.h
    src/viewer/race_timeline_item.cpp
    src/viewer/race_timeline_item.h
    src/viewer/race_viewer_controller.cpp
    src/viewer/race_viewer_controller.h
    src/viewer/map_identity.cpp
    src/viewer/map_identity.h
    src/app/search_session_store.cpp
    src/app/search_session_store.h
    src/viewer/simulation_debugger_model.cpp
    src/viewer/simulation_debugger_model.h
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
    src/app/input_preview_binding.cpp
    src/app/input_preview_binding.h
    src/app/search_controller.cpp
    src/app/search_controller.h
    src/app/packs_directory_finder.cpp
    src/app/packs_directory_finder.h
    src/app/panel_wheel_redirector.cpp
    src/app/panel_wheel_redirector.h
    src/app/rolling_throughput.h
    src/app/search_worker.cpp
    src/app/search_worker.h)

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
if(FOREVERTAS_LLDB_EXECUTABLE)
    target_compile_definitions(forevertas_gui PRIVATE
        FOREVERTAS_LLDB_EXECUTABLE="${FOREVERTAS_LLDB_EXECUTABLE}")
endif()
if(FOREVERTAS_SCRIPT_EXECUTABLE)
    target_compile_definitions(forevertas_gui PRIVATE
        FOREVERTAS_SCRIPT_EXECUTABLE="${FOREVERTAS_SCRIPT_EXECUTABLE}")
endif()
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
    Qt6::Quick
    Qt6::Quick3D
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
# link CUDA. Driver dispatch and session specialization are CUDA host-side
# facilities, not part of the editable reference simulation path.
list(FILTER FOREVERTAS_DEBUG_WORKER_ENGINE_SOURCES EXCLUDE REGEX
    "/src/simulation/backends/cuda/(cuda_driver_shim|cuda_session_specialization)\.cpp$")
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
    src/app/update_controller.cpp
    src/app/update_controller.h
    src/app/update_hardware.cpp
    src/app/update_hardware.h
    src/app/update_release.cpp
    src/app/update_release.h)
add_dependencies(forevertas forevertas-simulation-debug-worker)

set(forevertas_icon_svg
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/${FOREVERTAS_APP_ID}.svg")
set_source_files_properties("${forevertas_icon_svg}" PROPERTIES
    QT_RESOURCE_ALIAS "icons/forevertas.svg")
qt_add_resources(forevertas "forevertas_app_resources"
    PREFIX "/"
    FILES "${forevertas_icon_svg}")

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
if(NOT FOREVERTAS_QML_MODULE_EXTERNAL)
qt_add_qml_module(forevertas_qml
    PLUGIN_TARGET forevertas_qml
    URI ForeverTAS
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/qml/ForeverTAS"
    QML_FILES
        qml/AppTheme.qml
        qml/EvaluationOverlay.qml
        qml/EditHistory.qml
        qml/GraphicsInspector.qml
        qml/Main.qml
        qml/SearchSessionHistory.qml
        qml/SearchInputs.qml
        qml/SimulationDebuggerPanel.qml
        qml/SliderValueField.qml
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
        qml/settings/AlgorithmSelector.qml
        qml/settings/BasicBruteForceSearchSettings.qml
        qml/settings/ConfigurationSection.qml
        qml/settings/ExistingEventPerturbationSettings.qml
        qml/settings/PreciseFinishTimeEvaluationSettings.qml
        qml/settings/InputDeletionSettings.qml
        qml/settings/InputInsertionSettings.qml
        qml/settings/ModifierComposition.qml
        qml/settings/ModifierPassControls.qml
        qml/settings/PointTargetEvaluationSettings.qml
        qml/settings/PoseTargetEvaluationSettings.qml
        qml/settings/RandomSteeringMutationSettings.qml
        qml/settings/ScrubNumberField.qml
        qml/settings/ScriptedTargetEvaluationSettings.qml
        qml/settings/SettingCombo.qml
        qml/settings/SettingSlider.qml
        qml/settings/SteeringUnits.qml
        qml/settings/SettingSwitch.qml
        qml/settings/SettingTextField.qml
        qml/settings/SmoothSteeringSettings.qml
        qml/settings/StuntPointsEvaluationSettings.qml
        qml/settings/StyledComboBox.qml
        qml/settings/TimeWindowSettings.qml
        qml/settings/Vector3Settings.qml
        qml/settings/VelocityEvaluationSettings.qml
        qml/settings/VolumeEntryEvaluationSettings.qml)
file(GLOB FOREVERTAS_REPLACEMENT_TEXTURES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/*.png")
qt_add_resources(forevertas_qml "replacement-materials"
    PREFIX "/materials"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/materials"
    FILES ${FOREVERTAS_REPLACEMENT_TEXTURES})
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
    set(forevertas_runtime_destination ".")
else()
    set(forevertas_runtime_destination "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS forevertas
    RUNTIME DESTINATION "${forevertas_runtime_destination}")
install(TARGETS forevertas-simulation-debug-worker
    RUNTIME DESTINATION "${forevertas_runtime_destination}")
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
    file(GLOB forevertas_msvc_runtime_dlls
        LIST_DIRECTORIES FALSE
        "${FOREVERTAS_WINDOWS_MSVC_RUNTIME_DIR}/*.dll")
    if(FOREVERTAS_ENABLE_CUDA)
        if(NOT FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR
                AND CMAKE_CUDA_COMPILER)
            get_filename_component(FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR
                "${CMAKE_CUDA_COMPILER}" DIRECTORY)
        endif()
        if(NOT IS_DIRECTORY "${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}")
            message(FATAL_ERROR
                "FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR must point to the "
                "CUDA toolkit bin directory")
        endif()
        file(GLOB forevertas_cudart_runtime_dlls
            LIST_DIRECTORIES FALSE
            "${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}/cudart64_*.dll")
        file(GLOB forevertas_nvrtc_runtime_dlls
            LIST_DIRECTORIES FALSE
            "${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}/nvrtc64_120_0.dll")
        file(GLOB forevertas_nvrtc_builtins_runtime_dlls
            LIST_DIRECTORIES FALSE
            "${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}/nvrtc-builtins64_*.dll")
        file(GLOB forevertas_nvjitlink_runtime_dlls
            LIST_DIRECTORIES FALSE
            "${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}/nvJitLink_*.dll")
        foreach(forevertas_cuda_runtime_component IN ITEMS
                cudart nvrtc nvrtc_builtins nvjitlink)
            list(LENGTH
                forevertas_${forevertas_cuda_runtime_component}_runtime_dlls
                forevertas_${forevertas_cuda_runtime_component}_runtime_dll_count)
            if(NOT forevertas_${forevertas_cuda_runtime_component}_runtime_dll_count EQUAL 1)
                message(FATAL_ERROR
                    "Expected exactly one ${forevertas_cuda_runtime_component} "
                    "runtime DLL in ${FOREVERTAS_WINDOWS_CUDA_RUNTIME_DIR}; "
                    "found ${forevertas_${forevertas_cuda_runtime_component}_runtime_dll_count}")
            endif()
        endforeach()
        set(forevertas_cuda_runtime_dlls
            ${forevertas_cudart_runtime_dlls}
            ${forevertas_nvrtc_runtime_dlls}
            ${forevertas_nvrtc_builtins_runtime_dlls}
            ${forevertas_nvjitlink_runtime_dlls})
    endif()
    if(FOREVERTAS_ENABLE_HIP AND CMAKE_HIP_PLATFORM STREQUAL "amd")
        find_file(forevertas_hip_runtime_dll
            NAMES amdhip64_7.dll amdhip64_6.dll amdhip64.dll
            HINTS "${FOREVERTAS_WINDOWS_HIP_RUNTIME_DIR}"
                  "$ENV{HIP_PATH}/bin"
                  "$ENV{ROCM_PATH}/bin"
            REQUIRED)
        install(FILES "${forevertas_hip_runtime_dll}" DESTINATION ".")
    endif()
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
    if(FOREVERTAS_ENABLE_VULKAN)
        find_file(forevertas_vulkan_runtime_dll
            NAMES vulkan-1.dll
            HINTS "${FOREVERTAS_WINDOWS_RUNTIME_DIR}" "$ENV{VULKAN_SDK}/Bin"
            NO_DEFAULT_PATH REQUIRED)
        install(FILES "${forevertas_vulkan_runtime_dll}" DESTINATION ".")
    endif()
    install(FILES
        ${forevertas_openssl_runtime_dlls}
        ${forevertas_zlib_runtime_dlls}
        ${forevertas_msvc_runtime_dlls}
        ${forevertas_cuda_runtime_dlls}
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

if(FOREVERTAS_DISTRIBUTION_FLAVOR)
    if(FOREVERTAS_DISTRIBUTION_FLAVOR STREQUAL "universal")
        if(FOREVERTAS_ENABLE_CUDA OR FOREVERTAS_ENABLE_HIP OR
           NOT FOREVERTAS_ENABLE_VULKAN)
            message(FATAL_ERROR "Universal packages require Vulkan without CUDA or HIP")
        endif()
    elseif(FOREVERTAS_DISTRIBUTION_FLAVOR MATCHES
           "^nvidia-sm(50|52|53|60|61|62|70|72|75|80|86|87|89|90|100|101|120)$")
        set(forevertas_package_sm "${CMAKE_MATCH_1}")
        if(NOT FOREVERTAS_ENABLE_CUDA OR NOT FOREVERTAS_ENABLE_HIP OR
           NOT FOREVERTAS_ENABLE_VULKAN OR
           NOT CMAKE_HIP_PLATFORM STREQUAL "nvidia")
            message(FATAL_ERROR "NVIDIA packages require CUDA, NVIDIA HIP, and Vulkan")
        endif()
        if(NOT CMAKE_CUDA_ARCHITECTURES STREQUAL
               "${forevertas_package_sm}-real;${forevertas_package_sm}-virtual" OR
           NOT CMAKE_HIP_ARCHITECTURES STREQUAL "${forevertas_package_sm}")
            message(FATAL_ERROR "NVIDIA package must contain only its named SM target")
        endif()
    elseif(FOREVERTAS_DISTRIBUTION_FLAVOR STREQUAL "amd-rx7000-rx9000")
        if(FOREVERTAS_ENABLE_CUDA OR NOT FOREVERTAS_ENABLE_HIP OR
           NOT FOREVERTAS_ENABLE_VULKAN OR
           NOT CMAKE_HIP_PLATFORM STREQUAL "amd")
            message(FATAL_ERROR "AMD packages require AMD HIP and Vulkan without CUDA")
        endif()
        if(NOT CMAKE_HIP_ARCHITECTURES STREQUAL
               "gfx1100;gfx1101;gfx1102;gfx1200;gfx1201")
            message(FATAL_ERROR "AMD package must contain the RX 7000/9000 gfx targets")
        endif()
    else()
        message(FATAL_ERROR "Unknown ForeverTAS distribution flavor")
    endif()
    set(forevertas_package_id "${FOREVERTAS_DISTRIBUTION_FLAVOR}")
    set(forevertas_update_asset_parts
        "${forevertas_package_platform};${forevertas_package_id}")
else()
    set(forevertas_package_id "legacy")
    set(forevertas_update_asset_parts "${forevertas_package_platform}")
    if(FOREVERTAS_ENABLE_CUDA)
        list(APPEND forevertas_update_asset_parts "cuda")
    endif()
    if(FOREVERTAS_ENABLE_VULKAN)
        list(APPEND forevertas_update_asset_parts "vulkan")
    endif()
    if(FOREVERTAS_ENABLE_HIP)
        if(NOT CMAKE_HIP_PLATFORM MATCHES "^(amd|nvidia)$")
            message(FATAL_ERROR "HIP updates require an AMD or NVIDIA HIP platform")
        endif()
        list(APPEND forevertas_update_asset_parts "hip-${CMAKE_HIP_PLATFORM}")
    endif()
endif()
list(APPEND forevertas_update_asset_parts "${forevertas_package_arch}")
list(JOIN forevertas_update_asset_parts "-" FOREVERTAS_UPDATE_ASSET_ID)
set(FOREVERTAS_UPDATE_ASSET_ID "${FOREVERTAS_UPDATE_ASSET_ID}" CACHE INTERNAL
    "Platform and backend identity of public update artifacts" FORCE)
target_compile_definitions(forevertas PRIVATE
    FOREVERTAS_PACKAGE_ID="${forevertas_package_id}"
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
    if(WIN32)
        find_program(FOREVERTAS_POWERSHELL_EXECUTABLE NAMES pwsh.exe powershell.exe)
        if(FOREVERTAS_POWERSHELL_EXECUTABLE)
            add_test(NAME forevertas-windows-distribution-plan
                COMMAND "${FOREVERTAS_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_distribution_plan.ps1")
            add_test(NAME forevertas-windows-template-assembly
                COMMAND "${FOREVERTAS_POWERSHELL_EXECUTABLE}" -NoProfile -File
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_template_assembly.ps1")
        endif()
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME forevertas-distribution-matrix
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/distribution_tests.py")
        add_test(NAME forevertas-warm-build-seed
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/warm_builds_tests.py")
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

    add_executable(forevertas-controller-tests
        tests/search_controller_tests.cpp)
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
    add_test(NAME forevertas-release-cache-control
             COMMAND ${CMAKE_COMMAND}
                -DFOREVERTAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_cache_control.cmake)
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
        if(FOREVERTAS_LLDB_EXECUTABLE AND FOREVERTAS_SCRIPT_EXECUTABLE)
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
            "environment-textures-test"
            PREFIX "/environment"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/environment"
            FILES ${FOREVERTAS_ENVIRONMENT_TEXTURES})
        qt_add_resources(forevertas-viewer-qml-smoke
            "lucide-icons-test"
            PREFIX "/icons"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
            FILES ${FOREVERTAS_LUCIDE_ICONS})
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
    if(FOREVERTAS_ENABLE_VULKAN AND FOREVERTAS_TEST_PACK_DIRECTORY AND
       FOREVERTAS_TEST_REPLAY)
        add_executable(forevertas-vulkan-search-parity
            tests/vulkan_search_parity_tests.cpp)
        target_link_libraries(forevertas-vulkan-search-parity PRIVATE forevertas_core)
        add_test(NAME forevertas-vulkan-search-parity
            COMMAND forevertas-vulkan-search-parity
                "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        add_test(NAME forevertas-vulkan-precise-finish-parity
            COMMAND forevertas-vulkan-search-parity --precise-finish-only
                "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        add_test(NAME forevertas-vulkan-calibration
            COMMAND forevertas-vulkan-search-parity --calibration-only
                "${FOREVERTAS_TEST_PACK_DIRECTORY}" "${FOREVERTAS_TEST_REPLAY}")
        set_tests_properties(forevertas-vulkan-search-parity
            forevertas-vulkan-precise-finish-parity forevertas-vulkan-calibration
            PROPERTIES LABELS "vulkan;parity;stadium" RUN_SERIAL TRUE TIMEOUT 1200)
    endif()
endif()

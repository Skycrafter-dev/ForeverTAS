if(NOT DEFINED FOREVERTAS_BUILD_DIR)
    message(FATAL_ERROR "FOREVERTAS_BUILD_DIR is required")
endif()
if(NOT DEFINED FOREVERTAS_STAGE_DIR)
    message(FATAL_ERROR "FOREVERTAS_STAGE_DIR is required")
endif()

file(REMOVE_RECURSE "${FOREVERTAS_STAGE_DIR}")

set(install_command
    "${CMAKE_COMMAND}" --install "${FOREVERTAS_BUILD_DIR}"
    --prefix "${FOREVERTAS_STAGE_DIR}")
if(DEFINED FOREVERTAS_CONFIG AND NOT FOREVERTAS_CONFIG STREQUAL "")
    list(APPEND install_command --config "${FOREVERTAS_CONFIG}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Installing the portable tree failed: ${install_result}")
endif()

if(WIN32)
    set(executable "${FOREVERTAS_STAGE_DIR}/ForeverTAS.exe")
    set(debug_worker
        "${FOREVERTAS_STAGE_DIR}/forevertas-simulation-debug-worker.exe")
    set(license "${FOREVERTAS_STAGE_DIR}/licenses/LICENSE")
else()
    set(executable "${FOREVERTAS_STAGE_DIR}/bin/ForeverTAS")
    set(debug_worker
        "${FOREVERTAS_STAGE_DIR}/bin/forevertas-simulation-debug-worker")
    set(license
        "${FOREVERTAS_STAGE_DIR}/share/doc/ForeverTAS/licenses/LICENSE")
    foreach(required_file IN ITEMS
        "${FOREVERTAS_STAGE_DIR}/share/applications/dev.skycrafter.forevertas.desktop"
        "${FOREVERTAS_STAGE_DIR}/share/metainfo/dev.skycrafter.forevertas.appdata.xml"
        "${FOREVERTAS_STAGE_DIR}/share/icons/hicolor/256x256/apps/dev.skycrafter.forevertas.png")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Missing installed metadata: ${required_file}")
        endif()
    endforeach()
endif()

foreach(required_file IN ITEMS "${executable}" "${debug_worker}" "${license}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing installed file: ${required_file}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        QT_QPA_PLATFORM=offscreen
        QSG_RHI_BACKEND=software
        "${executable}" --qml-smoke-test
    RESULT_VARIABLE smoke_result
    TIMEOUT 60)
if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "Installed application smoke test failed: ${smoke_result}")
endif()

# The application must keep running after startup: a runtime-broken QML
# component (for example an attached control) can make the process exit
# itself quietly, which the immediate-quit smoke flag above cannot
# catch. Success here is being killed by the timeout while still alive.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        QT_QPA_PLATFORM=offscreen
        QSG_RHI_BACKEND=software
        "${executable}"
    RESULT_VARIABLE alive_result
    TIMEOUT 8)
if(NOT alive_result STREQUAL "Process terminated due to timeout")
    message(FATAL_ERROR
        "Application exited on its own after startup "
        "(result: ${alive_result})")
endif()

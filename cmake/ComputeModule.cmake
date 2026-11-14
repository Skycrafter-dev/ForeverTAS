# The GPU module: forevertas-compute-cuda / forevertas-compute-hip.
include(cmake/ComputeAbi.cmake)
add_library(forevertas_compute MODULE src/compute/compute_module_entry.cpp)
set_target_properties(forevertas_compute PROPERTIES
    OUTPUT_NAME "forevertas-compute-${FOREVERTAS_COMPUTE_MODULE}"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # its runtime libraries (cudart, the HIP runtime) sit next to it
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
target_include_directories(forevertas_compute PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(forevertas_compute forevertas_compute_abi)
target_link_libraries(forevertas_compute PRIVATE forevertas_core)
if(UNIX)
    # Only the entry is exported, and the module's code calls its own copy of
    # everything, never the app's (which has the same functions without GPU).
    target_link_options(forevertas_compute PRIVATE
        "LINKER:-Bsymbolic" "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
endif()
if(WIN32)
    # no link time in the DLL: the same code gives the same module, and the
    # same module the same archive name (compute_modules.py bundle)
    target_link_options(forevertas_compute PRIVATE "LINKER:/Brepro")
endif()

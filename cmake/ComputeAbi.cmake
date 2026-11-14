# kComputeAbi: a hash of every header an app and a GPU module share (the
# search core's and ForeverValidator's public ones) and of the compiler. A
# module is used only by an app with the same hash.
file(GLOB_RECURSE forevertas_compute_abi_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${forevervalidator_SOURCE_DIR}/include/*.h")
list(FILTER forevertas_compute_abi_headers EXCLUDE REGEX "/src/(app|viewer|debugger)/")
list(SORT forevertas_compute_abi_headers)
set(forevertas_compute_abi_header
    "${CMAKE_CURRENT_BINARY_DIR}/generated/forevertas_compute_abi.h")
add_custom_command(
    OUTPUT "${forevertas_compute_abi_header}"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT=${forevertas_compute_abi_header}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_SYSTEM_NAME}"
        "-DHEADERS=${forevertas_compute_abi_headers}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteComputeAbi.cmake"
    DEPENDS ${forevertas_compute_abi_headers}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteComputeAbi.cmake"
    VERBATIM)
add_custom_target(forevertas_compute_abi DEPENDS "${forevertas_compute_abi_header}")

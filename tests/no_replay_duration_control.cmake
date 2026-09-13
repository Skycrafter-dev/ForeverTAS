if(NOT DEFINED FOREVERTAS_SOURCE_DIR)
    message(FATAL_ERROR "FOREVERTAS_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE sources
    "${FOREVERTAS_SOURCE_DIR}/src/*.cpp"
    "${FOREVERTAS_SOURCE_DIR}/src/*.h")
set(forbidden
    "\\.durationMs"
    "replay duration")
foreach(source IN LISTS sources)
    file(READ "${source}" contents)
    if(source STREQUAL "${FOREVERTAS_SOURCE_DIR}/src/blocks/visual_state_horizon.h")
        string(REPLACE "\r\n" "\n" contents "${contents}")
        # Only these complete, typed sandbox-state accessors are exempt.
        # Unrelated member accesses in the same header remain forbidden.
        set(read_accessor "inline std::uint64_t VisualStateHorizonMs(const VisualState &state) noexcept {\n  return state.durationMs;\n}")
        set(write_accessor "inline void SetVisualStateHorizonMs(VisualState &state,std::uint64_t horizonMs) noexcept {\n  state.durationMs = horizonMs;\n}")
        foreach(name IN ITEMS read_accessor write_accessor)
            set(accessor "${${name}}")
            string(FIND "${contents}" "${accessor}" position)
            if(position EQUAL -1)
                message(FATAL_ERROR "Sandbox horizon accessor boundary changed in ${source}")
            endif()
            string(REPLACE "${accessor}" "" contents "${contents}")
        endforeach()
    endif()
    foreach(pattern IN LISTS forbidden)
        if(contents MATCHES "${pattern}")
            message(FATAL_ERROR
                "Recorded replay duration controls ForeverTAS in ${source}: ${pattern}")
        endif()
    endforeach()
endforeach()

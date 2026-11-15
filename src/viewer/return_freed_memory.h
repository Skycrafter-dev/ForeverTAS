#ifndef FOREVERTAS_VIEWER_RETURN_FREED_MEMORY_H
#define FOREVERTAS_VIEWER_RETURN_FREED_MEMORY_H

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace forevertas::viewer {

// Gives memory freed by a map load (the previous map, the load's working
// copies) back to the system. glibc otherwise keeps much of it for later
// allocations: hundreds of MB per map loaded.
inline void ReturnFreedMemory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

}  // namespace forevertas::viewer

#endif

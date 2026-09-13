#ifndef FOREVERTAS_BLOCKS_VISUAL_STATE_HORIZON_H
#define FOREVERTAS_BLOCKS_VISUAL_STATE_HORIZON_H

#include "blocks/visual_runtime.h"

namespace forevertas::blocks {
// The sandbox state's legacy field is the programmable horizon, not the
// recording length. Keep that distinction explicit at the typed boundary.
inline std::uint64_t VisualStateHorizonMs(const VisualState &state) noexcept {
  return state.durationMs;
}

inline void SetVisualStateHorizonMs(VisualState &state,std::uint64_t horizonMs) noexcept {
  state.durationMs = horizonMs;
}
}
#endif

#pragma once

namespace forevertas::app {

struct UpdateHardware {
    int nvidiaComputeCapability = 0;
    bool amdRadeonRx7000Or9000 = false;
};

UpdateHardware DetectUpdateHardware();

} // namespace forevertas::app

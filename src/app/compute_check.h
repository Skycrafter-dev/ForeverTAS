#pragma once

namespace forevertas::app {

// ForeverTAS --check-compute-module ARCHIVE [PACKS REPLAY]: a GPU module
// unpacked and loaded the way the app installs it (the release checks every
// module with the app it ships). argv[1] is the flag.
int CheckComputeModule(int argc, char **argv);

}  // namespace forevertas::app

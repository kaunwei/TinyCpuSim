#include "tinyarmsim/common.hpp"

namespace tinyarmsim {

const char* get_build_profile() noexcept {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

} // namespace tinyarmsim

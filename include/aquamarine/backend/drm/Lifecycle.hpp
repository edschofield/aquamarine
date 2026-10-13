#pragma once
#include <cstddef>

namespace Aquamarine {
    enum class eDRMRetirementState {
        RUNNING,
        DEFERRED,
        RETIRED,
        QUARANTINED,
    };

    struct SDRMShutdownStatus {
        eDRMRetirementState state   = eDRMRetirementState::RUNNING;
        size_t              buffers = 0, pins = 0, cleanupErrors = 0;
        int                 error = 0;
    };
}

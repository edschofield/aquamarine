#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Aquamarine {
    enum class eDRMTileFrameEvent {
        IGNORED,
        PENDING,
        COMPLETE,
    };

    // A completion barrier for one frame submitted to multiple CRTCs. The
    // owner must retain scanout resources separately. Cancelling notifications
    // does not establish that a buffer is no longer being scanned out.
    class CDRMTileFrame {
      public:
        bool               begin(uint64_t token, std::span<const uint32_t> crtcs);
        eDRMTileFrameEvent presented(uint64_t token, uint32_t crtc);
        bool               cancel(uint64_t token);
        bool               pending() const;

      private:
        uint64_t              m_token = 0;
        std::vector<uint32_t> m_remaining;
    };
}

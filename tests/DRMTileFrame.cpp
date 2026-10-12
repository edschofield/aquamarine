#include "DRMTileFrame.hpp"
#include "shared.hpp"
#include <array>

using namespace Aquamarine;

int main() {
    int                           ret = 0;
    CDRMTileFrame                 frame;
    const std::array<uint32_t, 2> crtcs{92, 102};
    EXPECT(frame.begin(0, crtcs), false);
    EXPECT(frame.begin(1, {}), false);
    EXPECT(frame.begin(1, std::array<uint32_t, 2>{92, 92}), false);
    EXPECT(frame.begin(1, std::array<uint32_t, 2>{92, 0}), false);
    EXPECT(frame.begin(1, crtcs), true);
    EXPECT(frame.begin(2, crtcs), false);
    EXPECT(frame.presented(2, 92) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.presented(1, 97) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.presented(1, 102) == eDRMTileFrameEvent::PENDING, true);
    EXPECT(frame.pending(), true);
    EXPECT(frame.presented(1, 102) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.presented(1, 92) == eDRMTileFrameEvent::COMPLETE, true);
    EXPECT(frame.pending(), false);
    EXPECT(frame.presented(1, 92) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.begin(1, crtcs), false);
    EXPECT(frame.begin(2, crtcs), true);
    EXPECT(frame.presented(1, 102) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.presented(2, 92) == eDRMTileFrameEvent::PENDING, true);
    EXPECT(frame.cancel(1), false);
    EXPECT(frame.pending(), true);
    EXPECT(frame.cancel(2), true);
    EXPECT(frame.presented(2, 102) == eDRMTileFrameEvent::IGNORED, true);
    EXPECT(frame.begin(2, crtcs), false);
    EXPECT(frame.begin(3, crtcs), true);
    EXPECT(frame.presented(3, 92) == eDRMTileFrameEvent::PENDING, true);
    // A missing second event must never complete the frame.
    EXPECT(frame.pending(), true);
    EXPECT(frame.begin(4, crtcs), false);
    EXPECT(frame.presented(3, 102) == eDRMTileFrameEvent::COMPLETE, true);
    return ret;
}

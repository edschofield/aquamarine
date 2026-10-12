#include "DRMTileFrame.hpp"
#include <algorithm>

using namespace Aquamarine;

bool CDRMTileFrame::begin(uint64_t token, std::span<const uint32_t> crtcs) {
    if (pending() || token <= m_token || crtcs.empty())
        return false;
    std::vector<uint32_t> remaining(crtcs.begin(), crtcs.end());
    std::ranges::sort(remaining);
    if (remaining.front() == 0 || std::ranges::adjacent_find(remaining) != remaining.end())
        return false;
    m_token     = token;
    m_remaining = std::move(remaining);
    return true;
}

eDRMTileFrameEvent CDRMTileFrame::presented(uint64_t token, uint32_t crtc) {
    if (token != m_token)
        return eDRMTileFrameEvent::IGNORED;
    const auto found = std::ranges::find(m_remaining, crtc);
    if (found == m_remaining.end())
        return eDRMTileFrameEvent::IGNORED;
    m_remaining.erase(found);
    return pending() ? eDRMTileFrameEvent::PENDING : eDRMTileFrameEvent::COMPLETE;
}

bool CDRMTileFrame::cancel(uint64_t token) {
    if (!pending() || token != m_token)
        return false;
    m_remaining.clear();
    return true;
}

bool CDRMTileFrame::pending() const {
    return !m_remaining.empty();
}

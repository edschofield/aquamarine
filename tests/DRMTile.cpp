#include "DRMTile.hpp"
#include "shared.hpp"
#include <algorithm>
#include <array>
#include <string>

using namespace Aquamarine;

static drmModeModeInfo nativeMode() {
    return {.clock       = 483250,
            .hdisplay    = 2560,
            .hsync_start = 2608,
            .hsync_end   = 2640,
            .htotal      = 2720,
            .vdisplay    = 2880,
            .vsync_start = 2883,
            .vsync_end   = 2893,
            .vtotal      = 2962,
            .vrefresh    = 60,
            .flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
            .type        = DRM_MODE_TYPE_PREFERRED};
}

static std::array<SDRMTileMember, 2> imac() {
    return {SDRMTileMember{.device = 1, .connector = 114, .tile = *parseDRMTile("1:1:2:1:0:0:2560:2880"), .modes = {nativeMode()}},
            SDRMTileMember{.device = 1, .connector = 123, .tile = *parseDRMTile("1:1:2:1:1:0:2560:2880"), .modes = {nativeMode()}}};
}

int main() {
    int               ret   = 0;
    const std::string valid = "1:1:2:1:0:0:2560:2880";
    EXPECT(parseDRMTile(valid).has_value(), true);
    EXPECT(parseDRMTile(valid + '\0').has_value(), true);
    // No trailing byte may be read when handed a non-NUL-terminated view.
    EXPECT(parseDRMTile(std::string_view(valid + "garbage").substr(0, valid.size())).has_value(), true);
    for (const auto& invalid : std::vector<std::string>{"",
                                                        "1:1:2:1:0:0:2560",
                                                        valid + ":0",
                                                        valid + "x",
                                                        valid + '\0' + "x",
                                                        valid + '\0' + '\0',
                                                        "0:1:2:1:0:0:2560:2880",
                                                        "1:2:2:1:0:0:2560:2880",
                                                        "1:1:0:1:0:0:2560:2880",
                                                        "1:1:2:0:0:0:2560:2880",
                                                        "1:1:2:1:2:0:2560:2880",
                                                        "1:1:2:1:0:1:2560:2880",
                                                        "1:1:2:1:-1:0:2560:2880",
                                                        "1:1:2:1:0:0:0:2880",
                                                        "1:1:2:1:0:0:2560:0",
                                                        "1:1:2:1:0:0:2147483647:2880",
                                                        "4294967296:1:2:1:0:0:2560:2880",
                                                        "1:1:4294967295:1:0:0:2560:2880",
                                                        "1:1:2:1: 0:0:2560:2880",
                                                        "+1:1:2:1:0:0:2560:2880"}) {
        EXPECT(parseDRMTile(invalid).has_value(), false);
    }
    EXPECT(parseDRMTile("1:0:2:1:0:0:2560:2880").has_value(), true);

    auto members = imac();
    auto plan    = planDRMTiles(members);
    EXPECT(plan.has_value(), true);
    if (!plan)
        return 1;
    EXPECT(plan->width, 5120U);
    EXPECT(plan->height, 2880U);
    EXPECT(plan->placements[0].x, 0U);
    EXPECT(plan->placements[1].x, 2560U);
    EXPECT(plan->placements[1].y, 0U);
    EXPECT(plan->modes.size(), 1U);
    EXPECT(plan->modes[0].heads[1].hdisplay, 2560U);
    EXPECT(plan->modes[0].preferred, true);
    std::ranges::reverse(members);
    plan = planDRMTiles(members);
    EXPECT(plan.has_value(), true);
    if (!plan)
        return 1;
    EXPECT(plan->placements[0].connector, 114U);
    EXPECT(plan->placements[1].connector, 123U);

    const auto errorIs = [](const auto& m, eDRMTileError error) {
        const auto result = planDRMTiles(m);
        return !result && result.error() == error;
    };
    EXPECT(errorIs(std::span<const SDRMTileMember>{}, eDRMTileError::INVALID_TOPOLOGY), true);
    EXPECT(errorIs(std::span(members).first(1), eDRMTileError::INCOMPLETE_GROUP), true);
    members           = imac();
    members[1].device = 2;
    EXPECT(errorIs(members, eDRMTileError::DIFFERENT_DEVICE), true);
    members                 = imac();
    members[1].tile.groupId = 2;
    EXPECT(errorIs(members, eDRMTileError::INVALID_TOPOLOGY), true);
    members                  = imac();
    members[1].tile.tileHLoc = 0;
    EXPECT(errorIs(members, eDRMTileError::DUPLICATE_TILE), true);
    members              = imac();
    members[1].connector = members[0].connector;
    EXPECT(errorIs(members, eDRMTileError::DUPLICATE_TILE), true);
    members                         = imac();
    members[1].tile.isSingleMonitor = false;
    EXPECT(errorIs(members, eDRMTileError::NOT_SINGLE_MONITOR), true);
    members                   = imac();
    members[1].tile.tileHSize = 2500;
    EXPECT(errorIs(members, eDRMTileError::INVALID_TOPOLOGY), true);
    members = imac();
    members[1].modes.clear();
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members = imac();
    members[1].modes[0].clock++;
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members = imac();
    members[1].modes[0].flags ^= DRM_MODE_FLAG_PHSYNC;
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members                      = imac();
    members[1].modes[0].hdisplay = 2048;
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members = imac();
    for (auto& member : members)
        member.modes[0].clock = 0;
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members = imac();
    for (auto& member : members)
        member.modes[0].flags |= DRM_MODE_FLAG_INTERLACE;
    EXPECT(errorIs(members, eDRMTileError::NO_COMMON_MODE), true);
    members                  = imac();
    members[1].modes[0].type = 0;
    plan                     = planDRMTiles(members);
    EXPECT(plan.has_value(), true);
    if (!plan)
        return 1;
    EXPECT(plan->modes[0].preferred, false);
    members = imac();
    members[0].modes.push_back(nativeMode());
    plan = planDRMTiles(members);
    EXPECT(plan.has_value(), true);
    if (!plan)
        return 1;
    EXPECT(plan->modes.size(), 1U);
    members = imac();
    for (size_t i = 0; i < members.size(); ++i) {
        members[i].tile.numHTile = 1;
        members[i].tile.numVTile = 2;
        members[i].tile.tileHLoc = 0;
        members[i].tile.tileVLoc = i;
    }
    plan = planDRMTiles(members);
    EXPECT(plan.has_value(), true);
    if (!plan)
        return 1;
    EXPECT(plan->width, 2560U);
    EXPECT(plan->height, 5760U);
    EXPECT(plan->placements[1].y, 2880U);
    return ret;
}

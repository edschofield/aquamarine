#pragma once

#include <aquamarine/backend/DRM.hpp>
#include <expected>
#include <span>
#include <string_view>

namespace Aquamarine {
    // Internal helpers. TILE group IDs are only meaningful within one DRM device.
    std::optional<SDRMConnector::STileInfo> parseDRMTile(std::string_view blob);
    bool                                    hasDRMTileAggregateMode(const SDRMConnector::STileInfo& tile, std::span<const drmModeModeInfo> modes);

    struct SDRMTileMember {
        uint64_t                     device    = 0;
        uint32_t                     connector = 0;
        SDRMConnector::STileInfo     tile;
        std::vector<drmModeModeInfo> modes;
    };

    struct SDRMTilePlacement {
        uint32_t connector = 0;
        uint32_t x = 0, y = 0, width = 0, height = 0;
    };

    struct SDRMTiledMode {
        // In the same row-major order as SDRMTilePlan::placements.
        std::vector<drmModeModeInfo> heads;
        bool                         preferred = false;
    };

    struct SDRMTilePlan {
        uint32_t                       width = 0, height = 0;
        std::vector<SDRMTilePlacement> placements;
        std::vector<SDRMTiledMode>     modes;
    };

    enum class eDRMTileError {
        INVALID_TOPOLOGY,
        DIFFERENT_DEVICE,
        NOT_SINGLE_MONITOR,
        INCOMPLETE_GROUP,
        DUPLICATE_TILE,
        NO_COMMON_MODE,
    };

    std::expected<SDRMTilePlan, eDRMTileError> planDRMTiles(std::span<const SDRMTileMember> members);
}

#include "DRMTile.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

using namespace Aquamarine;

static bool validTile(const SDRMConnector::STileInfo& tile) {
    if (!tile.groupId || tile.numHTile <= 0 || tile.numVTile <= 0 || tile.tileHSize <= 0 || tile.tileVSize <= 0)
        return false;
    if (tile.tileHLoc < 0 || tile.tileHLoc >= tile.numHTile || tile.tileVLoc < 0 || tile.tileVLoc >= tile.numVTile)
        return false;
    return uint64_t(tile.numHTile) * tile.tileHSize <= std::numeric_limits<int>::max() && uint64_t(tile.numVTile) * tile.tileVSize <= std::numeric_limits<int>::max();
}

std::optional<SDRMConnector::STileInfo> Aquamarine::parseDRMTile(std::string_view blob) {
    // Kernel blobs normally include a final NUL; do not read beyond blobLen or
    // accept embedded NULs/trailing fields as sscanf would.
    if (blob.ends_with('\0'))
        blob.remove_suffix(1);
    std::array<uint32_t, 8> fields{};
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto separator = blob.find(':');
        const auto field     = blob.substr(0, separator);
        if (field.empty() || (i + 1 < fields.size()) != (separator != std::string_view::npos))
            return std::nullopt;
        const auto result = std::from_chars(field.data(), field.data() + field.size(), fields[i]);
        if (result.ec != std::errc{} || result.ptr != field.data() + field.size())
            return std::nullopt;
        if (separator != std::string_view::npos)
            blob.remove_prefix(separator + 1);
    }
    if (fields[1] > 1 || std::ranges::any_of(std::span(fields).subspan(2), [](uint32_t v) { return v > std::numeric_limits<int>::max(); }))
        return std::nullopt;
    SDRMConnector::STileInfo tile{
        .groupId         = fields[0],
        .isSingleMonitor = fields[1] != 0,
        .numHTile        = static_cast<int>(fields[2]),
        .numVTile        = static_cast<int>(fields[3]),
        .tileHLoc        = static_cast<int>(fields[4]),
        .tileVLoc        = static_cast<int>(fields[5]),
        .tileHSize       = static_cast<int>(fields[6]),
        .tileVSize       = static_cast<int>(fields[7]),
    };
    if (!validTile(tile))
        return std::nullopt;
    return tile;
}

bool Aquamarine::hasDRMTileAggregateMode(const SDRMConnector::STileInfo& tile, std::span<const drmModeModeInfo> modes) {
    if (!validTile(tile))
        return false;
    const auto width  = uint64_t(tile.numHTile) * tile.tileHSize;
    const auto height = uint64_t(tile.numVTile) * tile.tileVSize;
    return std::ranges::any_of(modes, [width, height](const auto& mode) { return !(mode.flags & DRM_MODE_FLAG_INTERLACE) && mode.hdisplay >= width && mode.vdisplay >= height; });
}

static bool sameTiming(const drmModeModeInfo& a, const drmModeModeInfo& b) {
    return a.clock == b.clock && a.hdisplay == b.hdisplay && a.hsync_start == b.hsync_start && a.hsync_end == b.hsync_end && a.htotal == b.htotal && a.hskew == b.hskew &&
        a.vdisplay == b.vdisplay && a.vsync_start == b.vsync_start && a.vsync_end == b.vsync_end && a.vtotal == b.vtotal && a.vscan == b.vscan && a.flags == b.flags;
}

std::vector<SDRMFormat> Aquamarine::intersectDRMTileFormats(std::span<const std::vector<SDRMFormat>> heads) {
    std::vector<SDRMFormat> result;
    if (heads.empty())
        return result;
    for (const auto& first : heads.front()) {
        for (const auto modifier : first.modifiers) {
            const bool common = std::ranges::all_of(heads.subspan(1), [&](const auto& formats) {
                return std::ranges::any_of(
                    formats, [&](const auto& format) { return format.drmFormat == first.drmFormat && std::ranges::find(format.modifiers, modifier) != format.modifiers.end(); });
            });
            if (!common)
                continue;
            auto format = std::ranges::find(result, first.drmFormat, &SDRMFormat::drmFormat);
            if (format == result.end()) {
                result.push_back({.drmFormat = first.drmFormat, .modifiers = {modifier}});
            } else if (std::ranges::find(format->modifiers, modifier) == format->modifiers.end())
                format->modifiers.push_back(modifier);
        }
    }
    return result;
}

static bool nativeTiming(const drmModeModeInfo& mode, const SDRMConnector::STileInfo& tile) {
    return mode.clock && mode.hdisplay == tile.tileHSize && mode.vdisplay == tile.tileVSize && mode.hdisplay <= mode.hsync_start && mode.hsync_start <= mode.hsync_end &&
        mode.hsync_end <= mode.htotal && mode.vdisplay <= mode.vsync_start && mode.vsync_start <= mode.vsync_end && mode.vsync_end <= mode.vtotal &&
        !(mode.flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) && mode.vscan <= 1;
}

std::expected<SDRMTilePlan, eDRMTileError> Aquamarine::planDRMTiles(std::span<const SDRMTileMember> members) {
    if (members.empty() || !validTile(members.front().tile))
        return std::unexpected(eDRMTileError::INVALID_TOPOLOGY);
    const auto& origin = members.front();
    const auto& tile   = origin.tile;
    if (uint64_t(tile.numHTile) * tile.numVTile != members.size())
        return std::unexpected(eDRMTileError::INCOMPLETE_GROUP);

    std::vector<size_t>   order(members.size(), members.size());
    std::vector<uint32_t> connectorIDs;
    for (size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        const auto& other  = member.tile;
        if (member.device != origin.device)
            return std::unexpected(eDRMTileError::DIFFERENT_DEVICE);
        if (!validTile(other) || !member.connector || other.groupId != tile.groupId || other.numHTile != tile.numHTile || other.numVTile != tile.numVTile ||
            other.tileHSize != tile.tileHSize || other.tileVSize != tile.tileVSize)
            return std::unexpected(eDRMTileError::INVALID_TOPOLOGY);
        if (!other.isSingleMonitor)
            return std::unexpected(eDRMTileError::NOT_SINGLE_MONITOR);
        const auto pos = size_t(other.tileVLoc) * tile.numHTile + other.tileHLoc;
        if (order[pos] != members.size() || std::ranges::find(connectorIDs, member.connector) != connectorIDs.end())
            return std::unexpected(eDRMTileError::DUPLICATE_TILE);
        order[pos] = i;
        connectorIDs.push_back(member.connector);
    }

    SDRMTilePlan plan{.width = uint32_t(tile.numHTile) * tile.tileHSize, .height = uint32_t(tile.numVTile) * tile.tileVSize};
    for (auto index : order) {
        const auto& member = members[index];
        plan.placements.push_back({.connector = member.connector,
                                   .x         = uint32_t(member.tile.tileHLoc) * member.tile.tileHSize,
                                   .y         = uint32_t(member.tile.tileVLoc) * member.tile.tileVSize,
                                   .width     = uint32_t(member.tile.tileHSize),
                                   .height    = uint32_t(member.tile.tileVSize)});
    }
    for (const auto& mode : members[order.front()].modes) {
        if (!nativeTiming(mode, tile) || std::ranges::any_of(plan.modes, [&](const auto& existing) { return sameTiming(existing.heads.front(), mode); }))
            continue;
        SDRMTiledMode candidate{.preferred = true};
        for (auto index : order) {
            const auto& modes    = members[index].modes;
            const auto  matching = std::ranges::find_if(modes, [&](const auto& other) { return sameTiming(mode, other); });
            if (matching == modes.end())
                break;
            candidate.heads.push_back(*matching);
            candidate.preferred = candidate.preferred && (matching->type & DRM_MODE_TYPE_PREFERRED);
        }
        if (candidate.heads.size() == members.size())
            plan.modes.push_back(std::move(candidate));
    }
    if (plan.modes.empty())
        return std::unexpected(eDRMTileError::NO_COMMON_MODE);
    return plan;
}

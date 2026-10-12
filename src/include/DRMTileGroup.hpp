#pragma once
#include "DRMTile.hpp"
#include "DRMTileFrame.hpp"

namespace Aquamarine {
    struct SDRMTileGroup {
        SDRMTilePlan                                                  plan;
        std::vector<SDRMFormat>                                       formats;
        bool                                                          reprobe = false;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMConnector>> heads;
        bool                                                          poweredOff = false;
        size_t                                                        activeMode = 0;
        CDRMTileFrame                                                 frame;
        unsigned                                                      referenceSeq  = 0;
        timespec                                                      referenceTime = {};
        bool                                                          disconnecting = false;
        bool                                                          needsModeset  = true;
        bool                                                          recovering    = false;
        Hyprutils::OS::CFileDescriptor                                watchdog;
    };
}

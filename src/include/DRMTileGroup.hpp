#pragma once
#include "DRMTile.hpp"
#include "DRMTileFrame.hpp"

namespace Aquamarine {
    struct SDRMTileGroup {
        bool reprobe = false;
        SDRMTilePlan                                                  plan;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMConnector>> heads;
        bool                                                          poweredOff = false;
        size_t                                                        activeMode = 0;
        CDRMTileFrame                                                 frame;
        uintptr_t                                                     token         = 0;
        unsigned                                                      referenceSeq  = 0;
        timespec                                                      referenceTime = {};
        bool                                                          disconnecting = false;
        bool                                                          needsModeset  = true;
        bool                                                          recovering    = false;
        Hyprutils::OS::CFileDescriptor                                watchdog;
    };
}

#include "DRMTileGroup.hpp"
#include "Shared.hpp"
#include <aquamarine/backend/drm/Atomic.hpp>
#include <aquamarine/backend/Session.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <unordered_map>
#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
using Hyprutils::Utils::CScopeGuard;
template <typename T>
using SP = CSharedPointer<T>;

void CDRMBackend::configureTiledOutputs() {
    if (!envEnabled("AQ_DRM_TILED_OUTPUTS") || !atomic || shouldBlit())
        return;

    std::unordered_map<uint32_t, std::vector<SP<SDRMConnector>>> groups;
    for (const auto& head : connectors) {
        if (head->output && !head->output->announced && !head->tiledOwner && !head->tilingRedundant && head->status == DRM_MODE_CONNECTED && head->tileInfo.isSingleMonitor &&
            head->tileInfo.numHTile == 2 && head->tileInfo.numVTile == 1)
            groups[head->tileInfo.groupId].push_back(head);
    }
    for (auto& [id, heads] : groups) {
        std::vector<SDRMTileMember> members;
        for (const auto& head : heads) {
            auto raw = drmModeGetConnectorCurrent(drmFD(), head->id);
            if (!raw)
                continue;
            SDRMTileMember member{.device = uint64_t(drmFD()), .connector = head->id, .tile = head->tileInfo};
            if (raw->count_modes)
                member.modes.assign(raw->modes, raw->modes + raw->count_modes);
            drmModeFreeConnector(raw);
            members.push_back(std::move(member));
        }
        auto plan = planDRMTiles(members);
        if (!plan || members.size() != heads.size() || plan->width > 65535 || plan->height > 65535)
            continue;
        std::ranges::sort(heads, {}, [](const auto& h) { return h->tileInfo.tileHLoc; });
        if (std::ranges::any_of(heads, [](const auto& h) { return !h->crtc || !h->crtc->primary; }) || heads[0]->crtc == heads[1]->crtc)
            continue;
        auto output             = heads.front()->output;
        output->tiled           = std::make_shared<SDRMTileGroup>();
        output->tiled->watchdog = Hyprutils::OS::CFileDescriptor{timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)};
        if (!output->tiled->watchdog.isValid()) {
            output->tiled.reset();
            continue;
        }
        output->tiled->heads = heads;
        output->tiled->plan  = std::move(*plan);
        output->modes.clear();
        for (const auto& mode : output->tiled->plan.modes) {
            auto logical         = makeShared<SOutputMode>();
            logical->pixelSize   = {double(output->tiled->plan.width), double(output->tiled->plan.height)};
            const auto& timing   = mode.heads.front();
            logical->refreshRate = (uint64_t(timing.clock) * 1000000 / timing.htotal + timing.vtotal / 2) / timing.vtotal;
            logical->preferred   = mode.preferred;
            // This is a logical size, never a modeline sent to either link.
            output->modes.push_back(logical);
        }
        for (size_t i = 0; i < heads.size(); ++i) {
            const auto& rect                   = output->tiled->plan.placements[i];
            heads[i]->tiledOwner               = output;
            heads[i]->tiledSource              = CBox{double(rect.x), double(rect.y), double(rect.width), double(rect.height)};
            heads[i]->canDoVrr                 = false;
            heads[i]->output->vrrCapable       = false;
            heads[i]->output->supportsExplicit = false;
            heads[i]->output->cursorVisible    = false;
        }
        output->state->setMode(output->preferredMode());
        output->parsedEDID.hdrMetadata.reset();
        output->parsedEDID.supportsBT2020 = false;
        log(AQ_LOG_DEBUG,
            std::format("drm: tiled output {}: {}x{}, {} physical heads (experimental)", output->name, output->tiled->plan.width, output->tiled->plan.height, heads.size()));
        if (backend->ready)
            backend->events.pollFDsChanged.emit();
    }
}

void CDRMOutput::invalidateTiledFrame() {
    if (!tiled)
        return;
    tiled->frame.cancel(tiled->token);
    const itimerspec disarm{};
    timerfd_settime(tiled->watchdog.get(), 0, &disarm, nullptr);
    tiled->needsModeset = true;
    for (const auto& head : tiled->heads) {
        head->sched.invalidate();
        if (head->crtc)
            head->crtc->disarmPageFlip();
    }
}

bool CDRMOutput::commitTiled(bool onlyTest) {
    if (!tiled || !backend->sessionActive() || tiled->disconnecting)
        return false;
    const auto group = tiled;
    for (const auto& head : group->heads) {
        if (head->status != DRM_MODE_CONNECTED || !head->crtc || !head->output || head->tiledOwner != self)
            return false;
    }
    const auto  snapshot = state->snapshot();
    const auto& pending  = snapshot.state();
    const auto  mode     = pending.mode ? pending.mode.lock() : pending.customMode;
    const auto  found    = std::ranges::find(modes, mode);
    if (snapshot.error() || found == modes.end() || pending.adaptiveSync || pending.presentationMode != AQ_OUTPUT_PRESENTATION_VSYNC || pending.wideColorGamut ||
        pending.hdrMetadata.hdmi_metadata_type1.eotf || !pending.gammaLut.empty() || !pending.degammaLut.empty() || !(pending.ctm == Mat3x3::identity()))
        return false;
    const size_t modeIndex = found - modes.begin();
    const bool   modeset   = snapshot.needsReconfig() || group->needsModeset || lastCommitNoBuffer;
    if (!onlyTest && group->frame.pending() && !modeset)
        return false;

    SP<CDRMFB>  fb;
    bool        acquired = false;
    CScopeGuard rewind([&] {
        if (acquired && swapchain)
            swapchain->rollback();
    });
    if (pending.enabled) {
        auto buffer = pending.buffer;
        if ((!buffer || buffer->size != mode->pixelSize) && swapchain && swapchain->currentOptions().size == mode->pixelSize) {
            buffer   = swapchain->next(nullptr);
            acquired = !!buffer;
        }
        if (!buffer || buffer->size != mode->pixelSize)
            return false;
        const auto attrs   = buffer->dmabuf();
        const auto formats = getRenderFormats();
        const auto format  = std::ranges::find(formats, attrs.format, &SDRMFormat::drmFormat);
        if (!attrs.success || format == formats.end() || std::ranges::find(format->modifiers, attrs.modifier) == format->modifiers.end())
            return false;
        fb = CDRMFB::create(buffer, backend, nullptr);
        if (!fb || fb->dead)
            return false;
    }
    // The public output does not advertise explicit synchronization yet. If a
    // caller nevertheless supplies an input fence, wait before either head
    // can read the buffer. No single-head output fence is exposed.
    if (!onlyTest && pending.enabled && pending.explicitInFence >= 0) {
        pollfd fence{pending.explicitInFence, POLLIN, 0};
        int    result;
        do {
            result = poll(&fence, 1, 2000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || !(fence.revents & POLLIN))
            return false;
    }
    std::vector<uint64_t> paused;
    CScopeGuard           resume([&] {
        for (auto crtc : paused)
            backend->resumeCommitQueue(crtc);
    });
    for (const auto& head : group->heads) {
        if (!backend->pauseCommitQueue(head->crtc->id))
            return false;
        paused.push_back(head->crtc->id);
    }
    const bool wantsFlip = !onlyTest && !group->recovering && pending.enabled && fb && (pending.committed & COutputState::AQ_OUTPUT_STATE_BUFFER);
    uint32_t   flags     = onlyTest ? DRM_MODE_ATOMIC_TEST_ONLY : wantsFlip ? DRM_MODE_PAGE_FLIP_EVENT : 0;
    if (modeset)
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    else if (wantsFlip)
        flags |= DRM_MODE_ATOMIC_NONBLOCK;
    std::vector<std::pair<SP<SDRMConnector>, SDRMConnectorCommitData>> commits;
    for (size_t i = 0; i < group->heads.size(); ++i) {
        SDRMConnectorCommitData data{};
        data.outputState = pending;
        data.mainFB      = fb;
        data.enabled     = pending.enabled;
        data.modeset     = modeset;
        data.blocking    = modeset || !wantsFlip;
        data.test        = onlyTest;
        data.flags       = wantsFlip ? DRM_MODE_PAGE_FLIP_EVENT : 0;
        data.committed   = pending.committed | COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE;
        data.committed &= ~(COutputState::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE | COutputState::AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE | COutputState::AQ_OUTPUT_STATE_DAMAGE);
        data.outputState.committed = data.committed;
        data.modeInfo              = group->plan.modes[modeIndex].heads[i];
        data.primarySource         = group->heads[i]->tiledSource;
        data.hdrMetadata           = hdr_output_metadata{};
        if (modeset)
            data.ctm = Mat3x3::identity();
        commits.emplace_back(group->heads[i], std::move(data));
    }
    const auto token = wantsFlip ? backend->nextPageFlipID() : uintptr_t{0};
    // Save notification state so a rejected modeset cannot lose the earlier
    // frame's completion. No event dispatch occurs within the atomic ioctl.
    const auto                                   oldFrame = group->frame;
    const auto                                   oldToken = group->token;
    std::vector<decltype(SDRMCRTC::pendingFlip)> oldFlips;
    if (wantsFlip) {
        std::vector<uint32_t> crtcs;
        for (const auto& head : group->heads)
            crtcs.push_back(head->crtc->id);
        group->frame.cancel(oldToken);
        if (!group->frame.begin(token, crtcs)) {
            group->frame = oldFrame;
            return false;
        }
        group->token = token;
        for (const auto& head : group->heads) {
            oldFlips.push_back(head->crtc->pendingFlip);
            head->crtc->armPageFlip(head, false, token);
        }
    }
    auto       atomicImpl = dynamic_cast<CDRMAtomicImpl*>(backend->impl.get());
    const bool ok         = atomicImpl && atomicImpl->commitGroup(commits, flags, token);
    if (!ok && wantsFlip) {
        group->frame = oldFrame;
        group->token = oldToken;
        for (size_t i = 0; i < group->heads.size(); ++i)
            group->heads[i]->crtc->pendingFlip = oldFlips[i];
    }
    if (!ok || onlyTest)
        return ok;
    if (!wantsFlip)
        invalidateTiledFrame();
    DRMFBList retired;
    for (auto& [head, data] : commits) {
        if (wantsFlip)
            head->sched.onFrameSubmitted();
        head->applyCommit(data);
        if (!wantsFlip)
            head->onPresent(data.primaryFBChanged, data.cursorFBChanged, &data.retiredFBs);
        for (auto& old : data.retiredFBs)
            retired.push_back(std::move(old));
        head->refresh = mode->refreshRate;
    }
    // Each head owns a pin. Retire them only after all heads have adopted
    // the accepted state; no half-updated group is visible to release handlers.
    for (const auto& old : retired)
        connector->releaseFBBuffer(old);
    group->needsModeset = false;
    if (wantsFlip) {
        const itimerspec timeout{.it_value = {.tv_sec = 2}};
        timerfd_settime(group->watchdog.get(), 0, &timeout, nullptr);
    }
    lastCommitNoBuffer = !fb;
    needsFrame         = false;
    if (fb)
        state->internalState.drmFormat = fb->buffer->dmabuf().format;
    state->consume(snapshot);
    acquired = false;
    events.commit.emit();
    return true;
}

void CDRMOutput::tiledPageFlip(uintptr_t token, uint32_t crtc, unsigned seq, unsigned sec, unsigned usec) {
    const auto group = tiled;
    if (!group)
        return;
    const auto result = group->frame.presented(token, crtc);
    if (result == eDRMTileFrameEvent::IGNORED)
        return;
    if (crtc == connector->crtc->id) {
        group->referenceSeq  = seq;
        group->referenceTime = {.tv_sec = time_t(sec), .tv_nsec = long(usec) * 1000};
    }
    if (result != eDRMTileFrameEvent::COMPLETE)
        return;
    const itimerspec disarm{};
    timerfd_settime(group->watchdog.get(), 0, &disarm, nullptr);
    DRMFBList retired;
    for (const auto& head : group->heads) {
        head->sched.onFrameComplete();
        head->onPresent(true, true, &retired);
    }
    for (const auto& old : retired)
        connector->releaseFBBuffer(old);
    CFrameRunningGuard running(connector->sched);
    events.present.emit(SPresentEvent{
        .presented = backend->sessionActive(),
        .when      = &group->referenceTime,
        .seq       = group->referenceSeq,
        .refresh   = int(connector->refresh ? 1000000000000LL / connector->refresh : 0),
        .flags     = AQ_OUTPUT_PRESENT_HW_CLOCK | AQ_OUTPUT_PRESENT_HW_COMPLETION | AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_ZEROCOPY,
    });
    if (backend->sessionActive() && enabledState && !connector->sched.frameScheduled())
        connector->sched.frameReady.emit();
}

void CDRMOutput::recoverTiledFrame() {
    if (!tiled)
        return;
    uint64_t expirations = 0;
    if (read(tiled->watchdog.get(), &expirations, sizeof(expirations)) != sizeof(expirations) || !tiled->frame.pending() || !backend->sessionActive())
        return;
    backend->log(AQ_LOG_WARNING, "drm: tiled frame timed out; restoring the complete group with a blocking modeset");
    tiled->needsModeset = true;
    tiled->recovering   = true;
    const bool restored = commitTiled();
    tiled->recovering   = false;
    if (!restored) {
        backend->log(AQ_LOG_ERROR, "drm: tiled recovery failed; disabling the complete group before releasing buffers");
        disconnectTiled();
        return;
    }
    timespec when{};
    clock_gettime(CLOCK_MONOTONIC, &when);
    events.present.emit(SPresentEvent{.presented = false, .when = &when});
    scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);
}

bool CDRMOutput::disconnectTiled() {
    const auto group = tiled;
    if (!group || group->disconnecting)
        return false;
    // Retain scanout pins until a blocking disable has actually succeeded.
    if (!backend->sessionActive())
        return false;
    std::vector<std::pair<SP<SDRMConnector>, SDRMConnectorCommitData>> commits;
    for (const auto& head : group->heads) {
        if (!head->crtc)
            return false;
        SDRMConnectorCommitData data{};
        data.modeset  = true;
        data.blocking = true;
        commits.emplace_back(head, std::move(data));
    }
    auto atomicImpl = dynamic_cast<CDRMAtomicImpl*>(backend->impl.get());
    if (!atomicImpl || !atomicImpl->commitGroup(commits, DRM_MODE_ATOMIC_ALLOW_MODESET, 0))
        return false;
    group->disconnecting = true;
    invalidateTiledFrame();
    // Clear ownership before calling the ordinary per-head teardown.
    for (const auto& head : group->heads) {
        head->tiledOwner.reset();
        head->tiledSource.reset();
        head->output->enabledState = false;
    }
    for (const auto& head : group->heads) {
        const auto status = head->status;
        head->disconnect();
        head->status = status;
    }
    tiled.reset();
    return true;
}

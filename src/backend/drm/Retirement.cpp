#include "Retirement.hpp"
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/Session.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <format>
#include <utility>
#include <xf86drm.h>
#include <xf86drmMode.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

namespace {
    int drmError(int result) {
        return result == 0 ? 0 : (result == -1 ? (errno ? errno : EIO) : -result);
    }

    class CDRMRetirementOps final : public IDRMRetirementOps {
      public:
        int disable(int fd, bool atomic, const std::vector<SDRMDisableHead>& heads) override {
            if (!atomic) {
                for (const auto& h : heads) {
                    if (h.cursor) {
                        if (const int error = drmError(drmModeSetCursor(fd, h.crtc, 0, 0, 0)); error)
                            return error;
                    }
                    if (const int error = drmError(drmModeSetCrtc(fd, h.crtc, 0, 0, 0, nullptr, 0, nullptr)); error)
                        return error;
                }
                return 0;
            }
            auto* request = drmModeAtomicAlloc();
            if (!request)
                return ENOMEM;
            int        error = 0;
            const auto add   = [&](uint32_t object, uint32_t property) {
                if (error)
                    return;
                if (!object || !property)
                    error = EINVAL;
                else if (const int result = drmModeAtomicAddProperty(request, object, property, 0); result < 0)
                    error = drmError(result);
            };
            for (const auto& h : heads) {
                add(h.connector, h.connectorCrtc);
                add(h.crtc, h.active);
                add(h.crtc, h.mode);
                add(h.primary, h.primaryFB);
                add(h.primary, h.primaryCrtc);
                if (h.cursor) {
                    add(h.cursor, h.cursorFB);
                    add(h.cursor, h.cursorCrtc);
                }
            }
            if (!error)
                error = drmError(drmModeAtomicCommit(fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr));
            drmModeAtomicFree(request);
            return error;
        }
        bool isMaster(int fd) override {
            return drmIsMaster(fd);
        }
        int closeFB(int fd, uint32_t id, bool remove) override {
            return drmError(remove ? drmModeRmFB(fd, id) : drmModeCloseFB(fd, id));
        }
        int closeHandle(int fd, uint32_t handle) override {
            return drmError(drmCloseBufferHandle(fd, handle));
        }
        int importHandle(int fd, int dmaFD, uint32_t* handle) override {
            return drmError(drmPrimeFDToHandle(fd, dmaFD, handle));
        }
        int addFB(int fd, const SDMABUFAttrs& attrs, const std::array<uint32_t, 4>& handles, bool modifiers, uint32_t* id) override {
            if (modifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
                // ADDFB2 requires unused plane slots to stay zero. Dumb buffers
                // use modifier zero, so only real GBM imports expose this rule.
                std::array<uint64_t, 4> mods{};
                std::fill_n(mods.begin(), attrs.planes, attrs.modifier);
                return drmError(drmModeAddFB2WithModifiers(fd, attrs.size.x, attrs.size.y, attrs.format, handles.data(), attrs.strides.data(), attrs.offsets.data(), mods.data(),
                                                           id, DRM_MODE_FB_MODIFIERS));
            }
            if (attrs.modifier != DRM_FORMAT_MOD_INVALID && attrs.modifier != DRM_FORMAT_MOD_LINEAR)
                return EINVAL;
            return drmError(drmModeAddFB2(fd, attrs.size.x, attrs.size.y, attrs.format, handles.data(), attrs.strides.data(), attrs.offsets.data(), id, 0));
        }
    };

    bool retryable(int error) {
        return error == EINTR || error == EAGAIN || error == EBUSY || error == ENOMEM || error == ENOSPC || error == EACCES || error == EPERM;
    }
}

// All contexts are registered before they can import/submit. Terminal records
// deliberately remain reachable until process exit; no destructor may unpin them.
class Aquamarine::CDRMRetirementManager {
  public:
    static CDRMRetirementManager& get() {
        static auto* manager = new CDRMRetirementManager;
        return *manager;
    }
    std::list<std::shared_ptr<CDRMDeviceContext>> contexts;
    bool                                          quarantined = false;
};

CDRMDeviceContext::CDRMDeviceContext(int fd, std::shared_ptr<IDRMRetirementOps> ops) : m_fd(fd), m_ops(std::move(ops)) {}
CDRMDeviceContext::~CDRMDeviceContext() = default;

std::shared_ptr<CDRMDeviceContext> CDRMDeviceContext::create(CSharedPointer<CSessionDevice> device, CSharedPointer<CSession> session, CWeakPointer<CBackend> backend) {
    auto context       = std::shared_ptr<CDRMDeviceContext>(new CDRMDeviceContext(device->fd, std::make_shared<CDRMRetirementOps>()));
    context->m_device  = device;
    context->m_session = session;
    context->m_backend = backend;
    context->m_path    = device->path;
    context->attachRecovery(session);
    context->registerWithManager();
    device->drmContext = context;
    return context;
}

void CDRMDeviceContext::registerWithManager() {
    CDRMRetirementManager::get().contexts.push_back(shared_from_this());
}

void CDRMDeviceContext::attachRecovery(CSharedPointer<CSession> session) {
    const std::weak_ptr<CDRMDeviceContext> weak = shared_from_this();
    m_activate                                  = session->events.changeActive.listen([weak] {
        if (const auto context = weak.lock()) {
            if (const auto session = context->m_session.lock())
                context->retry(session->active);
        }
    });
    m_sessionDestroy                            = session->events.destroy.listen([weak] {
        if (const auto context = weak.lock())
            context->sessionClosing();
    });
}

bool CDRMDeviceContext::canOpen(const std::string& path) {
    const auto& manager = CDRMRetirementManager::get();
    return !manager.quarantined && std::ranges::none_of(manager.contexts, [&](const auto& c) { return c->m_path == path && !c->m_closed; });
}

int CDRMDeviceContext::fd() const {
    return m_fd;
}
bool CDRMDeviceContext::running() const {
    return !m_quiescing && !m_closed;
}
bool CDRMDeviceContext::sessionUsable() const {
    return m_session && !m_session->shuttingDown;
}
bool CDRMDeviceContext::sessionActive() const {
    return sessionUsable() && m_session->active;
}
IDRMRetirementOps& CDRMDeviceContext::ops() {
    return *m_ops;
}

SDRMShutdownStatus CDRMDeviceContext::status() const {
    auto result = m_status;
    for (auto it = m_imports.begin(); it != m_imports.end(); ++it) {
        const auto& i = *it;
        if (!i->pins)
            continue;
        result.pins += i->pins;
        if (std::none_of(m_imports.begin(), it, [&](const auto& previous) { return previous->pins && previous->heldBuffer == i->heldBuffer; }))
            ++result.buffers;
    }
    return result;
}

std::shared_ptr<SDRMImport> CDRMDeviceContext::registerImport(CSharedPointer<CDRMFB> fb, CSharedPointer<IBuffer> buffer) {
    if (!running() || !buffer || buffer->backendRetirementPending())
        return nullptr;
    m_imports.remove_if([](const auto& i) { return i->fb.expired() && !i->pins && !i->id; });
    auto entry    = std::make_shared<SDRMImport>();
    entry->fb     = fb;
    entry->buffer = buffer;
    m_imports.push_back(entry);
    return entry;
}

bool CDRMDeviceContext::rememberHead(const SDRMConnector& c, bool atomic) {
    if (!running() || !c.crtc || !c.crtc->primary)
        return false;
    const auto&     crtc = *c.crtc;
    SDRMDisableHead h{c.id,
                      c.props.values.crtc_id,
                      crtc.id,
                      crtc.props.values.active,
                      crtc.props.values.mode_id,
                      crtc.primary->id,
                      crtc.primary->props.values.fb_id,
                      crtc.primary->props.values.crtc_id};
    if (crtc.cursor) {
        h.cursor     = crtc.cursor->id;
        h.cursorFB   = crtc.cursor->props.values.fb_id;
        h.cursorCrtc = crtc.cursor->props.values.crtc_id;
    }
    m_atomic = atomic;
    // Keep every submitted routing, including a connector moved to a new CRTC.
    auto it = std::ranges::find_if(m_heads, [&](const auto& old) { return old.connector == h.connector && old.crtc == h.crtc; });
    try {
        if (it == m_heads.end())
            m_heads.push_back(h);
        else
            *it = h;
    } catch (const std::bad_alloc&) { return false; }
    return true;
}

void CDRMDeviceContext::block(SDRMImport& i) {
    if (i.pins && i.heldBuffer && !i.blocked) {
        ++i.heldBuffer->retirementHolds;
        i.blocked = true;
    }
}

void CDRMDeviceContext::pin(const std::shared_ptr<SDRMImport>& i) {
    if (!i || m_detached)
        return;
    auto buffer = i->buffer.lock();
    if (!buffer)
        return;
    i->heldBuffer = buffer;
    ++i->pins;
    buffer->backendPin();
    if (m_quiescing)
        block(*i); // Submitted async work can still be reconciled during drain.
}

void CDRMDeviceContext::release(SDRMImport& i, size_t count) {
    auto buffer = i.heldBuffer;
    count       = std::min(count, i.pins);
    i.pins -= count;
    if (!i.pins) {
        if (i.blocked && buffer) {
            --buffer->retirementHolds;
            i.blocked = false;
        }
        i.heldBuffer.reset();
    }
    while (count-- && buffer)
        buffer->backendUnpin();
}

void CDRMDeviceContext::unpin(const std::shared_ptr<SDRMImport>& i) {
    if (!i || m_detached || i->pins <= i->deferredUnpins)
        return;
    if (m_quiescing && !m_detached) {
        ++i->deferredUnpins;
        return;
    }
    release(*i, 1);
}

void CDRMDeviceContext::closeHandles(SDRMImport& i) {
    const auto handles = std::exchange(i.handles, {});
    if (m_fd < 0)
        return;
    for (size_t n = 0; n < handles.size(); ++n) {
        if (!handles[n] || std::find(handles.begin(), handles.begin() + n, handles[n]) != handles.begin() + n)
            continue;
        if (m_ops->closeHandle(m_fd, handles[n]))
            ++m_status.cleanupErrors; // File close owns the unresolved handle; never retry a possibly reused number.
    }
}

void CDRMDeviceContext::dropImport(SDRMImport& i, bool remove, bool runtimeFallback) {
    closeHandles(i);
    if (!i.id || m_fd < 0)
        return;
    int error = m_ops->closeFB(m_fd, i.id, remove);
    if (error == EINVAL && !remove && runtimeFallback)
        error = m_ops->closeFB(m_fd, i.id, true);
    if (error)
        ++m_status.cleanupErrors;
    // A failed cleanup remains a file-owned obligation until close. Do not
    // retry its numeric ID after other imports have had a chance to reuse it.
    i.id = 0;
}

void CDRMDeviceContext::beginShutdown() {
    if (m_quiescing)
        return;
    m_quiescing    = true;
    m_status.state = eDRMRetirementState::DEFERRED;
    for (auto& i : m_imports)
        block(*i);
}

void CDRMDeviceContext::invalidateImports() {
    for (const auto& i : m_imports) {
        if (const auto fb = i->fb.lock())
            fb->invalidate();
    }
}

int CDRMDeviceContext::attemptDisable() {
    if (m_fd < 0)
        return ENODEV;
    if (m_heads.empty() && !status().pins)
        return 0;
    if (!m_ops->isMaster(m_fd))
        return EACCES;
    if (m_heads.empty() && status().pins)
        return EINVAL;
    return m_heads.empty() ? 0 : m_ops->disable(m_fd, m_atomic, m_heads);
}

void CDRMDeviceContext::finishShutdown(bool sessionActive, bool canRetry, int permanentError) {
    if (m_detached)
        return;
    beginShutdown();
    m_detached = true;
    invalidateImports();
    // These pins retired during the drain, before final teardown began.
    for (auto& i : m_imports) {
        const auto count = std::exchange(i->deferredUnpins, 0);
        release(*i, count);
    }
    const int error = m_heads.empty() && !status().pins ? 0 : (permanentError ? permanentError : (sessionActive ? attemptDisable() : EACCES));
    if (!error) {
        complete(true, 0);
        return;
    }
    m_status.error = error;
    if (permanentError || !canRetry || !retryable(error)) {
        complete(false, error);
        return;
    }
    report();
    if (sessionActive && error != EACCES && error != EPERM) {
        if (const auto backend = m_backend.lock()) {
            const std::weak_ptr<CDRMDeviceContext> weak = shared_from_this();
            backend->addIdleEvent(makeShared<std::function<void()>>([weak] {
                if (const auto c = weak.lock()) {
                    if (const auto s = c->m_session.lock())
                        c->retry(s->active);
                }
            }));
        }
    }
}

void CDRMDeviceContext::retry(bool sessionActive) {
    if (!m_detached || m_closed || m_completing || m_status.state != eDRMRetirementState::DEFERRED || !sessionActive || m_retryUsed)
        return;
    if (!m_ops->isMaster(m_fd))
        return;
    m_retryUsed     = true;
    const int error = attemptDisable();
    complete(!error, error);
}

void CDRMDeviceContext::complete(bool retired, int error) {
    if (m_completing || m_status.state == eDRMRetirementState::QUARANTINED || m_status.state == eDRMRetirementState::RETIRED)
        return;
    const auto keepAlive = shared_from_this();
    m_completing         = true;
    m_status.state       = retired ? eDRMRetirementState::RETIRED : eDRMRetirementState::QUARANTINED;
    m_status.error       = error;
    m_activate.reset();
    m_sessionDestroy.reset();
    if (!retired)
        CDRMRetirementManager::get().quarantined = true;
    for (auto& i : m_imports)
        dropImport(*i, retired, false);
    closeDevice();
    if (retired) {
        for (auto& i : m_imports)
            release(*i, i->pins);
        CDRMRetirementManager::get().contexts.remove(keepAlive);
    }
    report();
    m_completing = false;
}

void CDRMDeviceContext::closeDevice() {
    if (m_closed)
        return;
    m_closed    = true;
    m_fd        = -1;
    auto device = std::exchange(m_device, {});
    if (device) {
        if (device->close())
            ++m_status.cleanupErrors;
        if (const auto session = m_session.lock())
            std::erase(session->sessionDevices, device);
    }
}

void CDRMDeviceContext::deviceClosing() {
    if (m_closed)
        return;
    // The session owns the actual close; do not recursively close its device.
    m_closed = true;
    m_fd     = -1;
    if (m_status.state != eDRMRetirementState::RETIRED) {
        beginShutdown();
        invalidateImports();
        complete(false, ENODEV);
    }
    m_device.reset();
}

void CDRMDeviceContext::sessionClosing() {
    if (m_status.state == eDRMRetirementState::DEFERRED)
        complete(false, ECANCELED);
}

void CDRMDeviceContext::report() const {
    const auto  s     = status();
    const char* state = "RUNNING";
    switch (s.state) {
        case eDRMRetirementState::DEFERRED: state = "DEFERRED"; break;
        case eDRMRetirementState::RETIRED: state = "RETIRED"; break;
        case eDRMRetirementState::QUARANTINED: state = "QUARANTINED"; break;
        default: break;
    }
    const auto message = std::format("drm: retirement {} state {}: {} buffers, {} pins, error {}, cleanup errors {}", m_path, state, s.buffers, s.pins, s.error, s.cleanupErrors);
    if (const auto backend = m_backend.lock())
        backend->log(s.state == eDRMRetirementState::RETIRED ? AQ_LOG_DEBUG : AQ_LOG_ERROR, message);
    else
        std::fprintf(stderr, "aquamarine: %s\n", message.c_str());
}

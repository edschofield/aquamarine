// Exercises production lifetime objects with injected DRM operations. No DRM
// node, libseat session, or renderer is opened by this fixture. Its commit
// worker uses an injected submitter.
#include "Retirement.hpp"
#include "AsyncCommit.hpp"
#include "DRMTileGroup.hpp"
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/Session.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

namespace {
    void require(bool value, const char* message) {
        if (!value) {
            std::fprintf(stderr, "DRMShutdown: %s\n", message);
            std::abort();
        }
    }

    class CBuffer final : public IBuffer {
      public:
        CBuffer() {
            size = {64, 64};
        }
        ~CBuffer() override {
            events.destroy.emit();
        }
        eBufferCapability caps() override {
            return BUFFER_CAPABILITY_NONE;
        }
        eBufferType type() override {
            return BUFFER_TYPE_DMABUF;
        }
        void update(const Hyprutils::Math::CRegion&) override {}
        bool isSynchronous() override {
            return true;
        }
        bool good() override {
            return true;
        }
        SDMABUFAttrs dmabuf() override {
            SDMABUFAttrs attrs;
            attrs.success = true;
            attrs.size    = size;
            attrs.planes  = planes;
            return attrs;
        }
        int planes = 1;
    };

    class CSubmitter final : public IDRMCommitSubmitter {
      public:
        SResult submit(uint64_t, const IDRMCommitThreadData&) noexcept override {
            return {.submitted = true};
        }
    };

    class COps final : public IDRMRetirementOps {
      public:
        int disable(int fd, bool atomic, const std::vector<SDRMDisableHead>& heads) override {
            require(fd >= 0 && !heads.empty(), "disable needs live context and routing");
            ++disables;
            atomicDisable = atomic;
            headCount     = heads.size();
            for (const auto& h : heads)
                require(h.connector && h.crtc && h.primary && (!atomic || (h.active && h.mode)), "complete head inventory");
            return disableError;
        }
        bool isMaster(int fd) override {
            require(fd >= 0, "master on closed descriptor");
            return master;
        }
        int closeFB(int fd, uint32_t id, bool remove) override {
            require(fd >= 0 && id, "FB cleanup on closed descriptor");
            ++closes;
            removes += remove;
            return closeError;
        }
        int closeHandle(int fd, uint32_t handle) override {
            require(fd >= 0 && handle, "handle cleanup on closed descriptor");
            ++handleCloses;
            return 0;
        }
        int importHandle(int fd, int, uint32_t* handle) override {
            require(fd >= 0, "import on closed descriptor");
            if (++imports == failImport)
                return EIO;
            *handle = 7; // Multi-plane aliases must close only once per import.
            return 0;
        }
        int addFB(int fd, const SDMABUFAttrs&, const std::array<uint32_t, 4>&, bool, uint32_t* id) override {
            require(fd >= 0, "ADDFB on closed descriptor");
            ++adds;
            if (addError)
                return addError;
            *id = 100 + adds;
            return 0;
        }
        int    disableError = 0, closeError = 0, addError = 0, failImport = -1;
        int    disables = 0, closes = 0, removes = 0, imports = 0, adds = 0, handleCloses = 0;
        size_t headCount = 0;
        bool   master = true, atomicDisable = false;
    };
}

namespace Aquamarine {
    class CDRMShutdownTest {
      public:
        class CStartFailure final : public CDRMBackend {
          public:
            CStartFailure(CSharedPointer<CBackend> owner) : CDRMBackend(owner) {}
            bool start() override {
                return false;
            }
        };

        CDRMShutdownTest(bool failStartup = false) {
            root             = CSharedPointer<CBackend>(new CBackend);
            root->self       = root;
            session          = makeShared<CSession>();
            session->self    = session;
            session->backend = root;
            session->active  = true;
            root->session    = session;
            device           = CSharedPointer<CSessionDevice>(new CSessionDevice);
            device->session  = session;
            device->path     = "fixture";
            int pipes[2];
            require(pipe(pipes) == 0, "pipe allocation");
            device->fd = pipes[0];
            close(pipes[1]);
            session->sessionDevices.push_back(device);
            backend            = failStartup ? CSharedPointer<CDRMBackend>(new CStartFailure(root)) : CSharedPointer<CDRMBackend>(new CDRMBackend(root));
            backend->self      = backend;
            backend->gpu       = device;
            backend->atomic    = true;
            ops                = std::make_shared<COps>();
            context            = std::shared_ptr<CDRMDeviceContext>(new CDRMDeviceContext(device->fd, ops));
            context->m_device  = device;
            context->m_session = session;
            context->m_backend = root;
            context->m_path    = device->path;
            context->registerWithManager();
            context->attachRecovery(session);
            device->drmContext     = context;
            backend->deviceContext = context;
        }
        ~CDRMShutdownTest() {
            if (backend)
                backend->shutdown();
        }

        void attach(bool shared = true) {
            auto group      = std::make_shared<SDRMTileGroup>();
            group->watchdog = Hyprutils::OS::CFileDescriptor(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
            require(group->watchdog.isValid(), "watchdog allocation");
            weakGroup = group;
            for (uint32_t n = 0; n < 2; ++n) {
                auto h                                 = makeShared<SDRMConnector>();
                h->self                                = h;
                h->backend                             = backend;
                h->id                                  = 10 + n;
                h->status                              = DRM_MODE_CONNECTED;
                h->props.values.crtc_id                = 1;
                h->crtc                                = makeShared<SDRMCRTC>();
                h->crtc->id                            = 20 + n;
                h->crtc->props.values.active           = 2;
                h->crtc->props.values.mode_id          = 3;
                h->crtc->primary                       = makeShared<SDRMPlane>();
                h->crtc->primary->id                   = 30 + n;
                h->crtc->primary->props.values.fb_id   = 4;
                h->crtc->primary->props.values.crtc_id = 5;
                h->crtc->cursor                        = makeShared<SDRMPlane>();
                h->crtc->cursor->id                    = 40 + n;
                h->crtc->cursor->props.values.fb_id    = 6;
                h->crtc->cursor->props.values.crtc_id  = 7;
                h->output                              = CSharedPointer<CDRMOutput>(new CDRMOutput("fixture", backend, h));
                h->output->self                        = h->output;
                h->output->announced                   = n == 0;
                if (n == 0) {
                    output        = h->output;
                    output->tiled = group;
                }
                h->tiledOwner = output;
                backend->connectors.push_back(h);
                heads.push_back(h);
                group->heads.push_back(h);
                require(backend->rememberScanout(*h), "provision head before scanout");
                auto buffer = n && shared ? buffers.front() : makeShared<CBuffer>();
                buffers.push_back(buffer);
                auto fb = CDRMFB::create(buffer, backend);
                require(!!fb, "production import");
                fbs.push_back(fb);
                fb->pin();
                h->crtc->primary->front = fb;
                h->crtc->primary->back  = fb; // One pin for identical front/back.
            }
        }

        void detached() {
            require(weakGroup.expired(), "tile group retained after shutdown");
            for (const auto& h : heads) {
                require(!h->output && !h->tiledOwner, "head still attached in callback");
                require(!h->crtc->primary->front && !h->crtc->primary->back, "plane references retained");
            }
            require(!output->connector && !output->tiled, "external output retained routing");
            require(!output->commit() && !output->test() && !output->getBackend(), "detached output accepted work");
            output->scheduleFrame();
            output->recoverTiledFrame();
        }

        static void success(bool shared, bool cleanupFailure, bool externalPin, bool cursor = false) {
            CDRMShutdownTest f;
            f.attach(shared);
            if (cursor) {
                for (size_t i = 0; i < f.heads.size(); ++i) {
                    f.heads[i]->crtc->cursor->front = f.fbs[i];
                    f.heads[i]->crtc->cursor->back  = f.fbs[i];
                    f.fbs[i]->pin();
                }
                require(f.context->status().pins == 4 && f.context->status().buffers == 1, "shared primary/cursor pin multiplicity");
            }
            f.ops->closeError = cleanupFailure ? EIO : 0;
            if (externalPin)
                f.buffers.front()->backendPin();
            int  destroyed = 0, hiddenDestroyed = 0, releases = 0;
            auto destroy       = f.output->events.destroy.listen([&] {
                require(++destroyed == 1, "duplicate logical destroy");
                f.detached();
                f.backend->shutdown();
            });
            auto hiddenDestroy = f.heads.back()->output->events.destroy.listen([&] { ++hiddenDestroyed; });
            auto release       = f.buffers.front()->events.backendRelease.listen([&] {
                ++releases;
                f.detached();
            });
            auto polls         = f.backend->pollFDs();
            f.backend->shutdown();
            f.detached();
            require(destroyed == 1 && hiddenDestroyed == 0, "exactly one logical destroy, hidden head silent");
            require(f.ops->disables == 1 && f.ops->headCount == 2 && f.ops->atomicDisable, "one full atomic disable");
            require(f.context->status().state == eDRMRetirementState::RETIRED && !f.context->status().pins, "successful retirement accounting");
            require(f.ops->closes == (shared ? 1 : 2), "deduplicated FB cleanup");
            require((f.context->status().cleanupErrors != 0) == cleanupFailure, "cleanup status independent of retirement");
            require(f.buffers.front()->backendPinCount() == (externalPin ? 1u : 0u), "only owned pins removed");
            require(releases == (externalPin ? 0 : 1), "release event count");
            require(!f.buffers.front()->backendRetirementPending(), "retired buffer reusable");
            f.fbs.front()->reimport();
            require(!f.fbs.front()->id && f.ops->adds == (shared ? 1 : 2), "old FB inert");
            f.backend.reset();
            for (const auto& poll : polls)
                poll->onSignal();
            require(f.ops->disables == 1, "saved poll callback touched retired device");
            if (externalPin)
                f.buffers.front()->backendUnpin();
            require(releases == 1, "last external pin releases exactly once");
            f.output.reset();
            require(destroyed == 1 && hiddenDestroyed == 0, "destructors repeated output notifications");
        }

        static void failure(bool inactive, bool recover, bool sessionEnd, bool legacy) {
            CDRMShutdownTest f;
            f.attach();
            f.context->m_atomic = !legacy;
            f.session->active   = !inactive;
            f.ops->disableError = EBUSY;
            int  releases       = 0;
            auto release        = f.buffers.front()->events.backendRelease.listen([&] { ++releases; });
            f.backend->shutdown();
            f.detached();
            require(f.context->status().state == eDRMRetirementState::DEFERRED, "transient failure must defer");
            require(f.context->status().pins == 2 && f.context->status().buffers == 1, "shared buffer multiplicity");
            require(!releases && f.buffers.front()->backendRetirementPending(), "unproven buffer released");
            require(!CDRMDeviceContext::canOpen("fixture"), "pending generation reopened");
            const auto importCount = f.ops->imports;
            f.buffers.front()->attachments.clear();
            require(!CDRMFB::create(f.buffers.front(), f.backend), "attachment removal bypassed quarantine");
            f.fbs.front()->reimport();
            require(f.ops->imports == importCount, "old attachment reimported");
            f.backend.reset(); // Retry must not require the original backend.
            if (sessionEnd)
                f.session->shutdown();
            else {
                f.ops->disableError = recover ? 0 : EIO;
                f.session->active   = true;
                f.session->events.changeActive.emit();
            }
            require(f.context->status().state == (recover ? eDRMRetirementState::RETIRED : eDRMRetirementState::QUARANTINED), "retry disposition");
            require(releases == (recover ? 1 : 0), "retry release decision");
            require(f.device->fd == -1, "device FD not closed");
            f.fbs.front()->unpin(); // Stale calls cannot defeat terminal quarantine.
            require(f.context->status().pins == (recover ? 0u : 2u), "stale unpin changed quarantine");
            if (!recover) {
                require(!CDRMDeviceContext::canOpen("another GPU"), "terminal failure admitted a new generation");
                CWeakPointer<IBuffer> weak = f.buffers.front();
                f.buffers.clear();
                f.fbs.clear();
                f.output.reset();
                f.heads.clear();
                f.context.reset();
                require(!weak.expired(), "terminal buffer lost its process-lifetime owner");
            }
        }

        static void imports() {
            CDRMShutdownTest f;
            auto             b = makeShared<CBuffer>();
            b->planes          = 3;
            f.ops->failImport  = 3;
            require(!CDRMFB::create(b, f.backend), "partial import unexpectedly succeeded");
            require(f.ops->handleCloses == 1 && !f.ops->closes, "partial handles leaked or double closed");
            f.ops->failImport = -1;
            f.ops->addError   = EIO;
            require(!CDRMFB::create(b, f.backend), "failed ADDFB unexpectedly succeeded");
            require(f.ops->handleCloses == 2 && !f.ops->closes, "ADDFB failure lost handle obligations");
            f.ops->addError = 0;
            auto fb         = CDRMFB::create(b, f.backend);
            require(!!fb && f.ops->handleCloses == 3, "multi-plane aliases not deduplicated");
            f.backend->shutdown();
            require(fb->dead && !fb->id && f.ops->closes == 1, "external FB not invalidated");
        }

        static void asyncShutdown(bool failDisable) {
            CDRMShutdownTest f;
            f.attach();
            f.ops->disableError     = failDisable ? EIO : 0;
            f.backend->impl         = makeShared<CDRMAtomicImpl>(f.backend);
            f.backend->commitThread = CDRMCommitThread::create(makeUnique<CSubmitter>());
            require(!!f.backend->commitThread, "commit worker creation");
            auto submitted = makeShared<CBuffer>();
            auto cancelled = makeShared<CBuffer>();
            auto enqueue   = [&](size_t head, CSharedPointer<CBuffer> buffer, bool future) {
                auto data                  = makeUnique<CDRMAsyncCommitData>(f.heads[head]->output->state->snapshot());
                data->mainBuffer           = buffer;
                data->connector            = f.heads[head];
                data->output               = data->connector->output;
                data->request              = makeUnique<CDRMAtomicRequest>(f.backend);
                data->commitData.mainFB    = CDRMFB::create(buffer, f.backend);
                data->commitData.enabled   = true;
                data->commitData.committed = COutputState::AQ_OUTPUT_STATE_BUFFER;
                data->pinBuffers();
                auto request      = makeUnique<SDRMCommitThreadRequest>();
                request->ownerID  = data->output->asyncOwnerID;
                request->queueKey = data->connector->crtc->id;
                if (future)
                    request->submitAt = std::chrono::steady_clock::now() + std::chrono::hours(1);
                request->data = std::move(data);
                const auto id = f.backend->commitThread->enqueue(std::move(request));
                require(id.has_value(), "enqueue production async request");
                f.heads[head]->output->pendingAsyncCommit = *id;
            };
            int  submittedReleases = 0, cancelledReleases = 0, results = 0;
            auto submittedRelease = submitted->events.backendRelease.listen([&] {
                ++submittedReleases;
                f.detached();
            });
            auto cancelledRelease = cancelled->events.backendRelease.listen([&] {
                ++cancelledReleases;
                require(!f.output->commit(), "cancel callback resubmitted while quiescing");
                f.backend->shutdown();
            });
            auto submittedResult  = f.output->events.commitResult.listen([&](const IOutput::SCommitResult& result) {
                require(result.status == IOutput::AQ_OUTPUT_COMMIT_SUBMITTED && !f.output->commit(), "submitted result during shutdown");
                ++results;
            });
            auto cancelledResult  = f.heads.back()->output->events.commitResult.listen([&](const IOutput::SCommitResult& result) {
                require(result.status == IOutput::AQ_OUTPUT_COMMIT_CANCELLED && !f.output->commit(), "cancelled result during shutdown");
                ++results;
            });
            enqueue(0, submitted, false);
            pollfd ready{f.backend->commitThread->completionFD(), POLLIN, 0};
            require(poll(&ready, 1, 2000) == 1, "worker completion timeout");
            enqueue(1, cancelled, true);
            f.backend->shutdown();
            f.detached();
            require(results == 2 && cancelledReleases == 1 && !cancelled->backendPinCount(), "cancelled request accounting");
            require(submittedReleases == (failDisable ? 0 : 1), "submitted request release decision");
            require(submitted->backendPinCount() == (failDisable ? 1u : 0u), "request pin transferred to scanout ownership");
            require(f.context->status().pins == (failDisable ? 3u : 0u), "scanout pins after async drain");
        }

        static void idleRecovery() {
            CDRMShutdownTest f;
            f.attach();
            f.root->idle.fd     = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            f.ops->disableError = EBUSY;
            f.backend->shutdown();
            require(f.context->status().state == eDRMRetirementState::DEFERRED, "idle retry not scheduled");
            f.ops->disableError = 0;
            f.backend.reset();
            pollfd ready{f.root->idle.fd, POLLIN, 0};
            require(poll(&ready, 1, 2000) == 1, "idle retry timeout");
            f.root->dispatchIdle();
            require(f.context->status().state == eDRMRetirementState::RETIRED && f.ops->disables == 2, "idle retry without DRM backend");
        }

        static void descriptorReuse() {
            CDRMShutdownTest f;
            f.attach();
            const int original = f.device->fd;
            auto      polls    = f.backend->pollFDs();
            f.backend->shutdown();
            f.backend.reset();
            int pipes[2];
            require(pipe(pipes) == 0, "replacement pipe");
            // The previous read end is the lowest released descriptor.
            require(pipes[0] == original, "fixture did not reuse original descriptor");
            const char byte = 'x';
            require(write(pipes[1], &byte, 1) == 1, "replacement pipe write");
            for (const auto& fd : polls)
                fd->onSignal();
            f.fbs.front()->reimport();
            f.fbs.front()->drop();
            f.device->close();
            char readByte = 0;
            require(read(pipes[0], &readByte, 1) == 1 && readByte == byte, "stale callback consumed or closed reused FD");
            close(pipes[0]);
            close(pipes[1]);
        }

        static void partialStartup(bool mandatory) {
            CDRMShutdownTest f(true);
            f.attach();
            f.root->implementations.push_back(f.backend);
            SBackendImplementationOptions option;
            option.backendType        = AQ_BACKEND_DRM;
            option.backendRequestMode = mandatory ? AQ_BACKEND_REQUEST_MANDATORY : AQ_BACKEND_REQUEST_IF_AVAILABLE;
            f.root->implementationOptions.push_back(option);
            require(!f.root->start(), "failed backend startup accepted");
            f.detached();
            require(f.context->status().state == eDRMRetirementState::RETIRED && f.device->fd == -1, "partial startup retained scanout ownership");
        }

        static void lastOwner(bool rootOwner) {
            CDRMShutdownTest f;
            f.attach();
            if (rootOwner) {
                f.root->implementations.push_back(f.backend);
                f.backend.reset();
                f.root.reset();
            } else
                f.backend.reset();
            f.detached();
            require(f.context->status().state == eDRMRetirementState::RETIRED && !f.context->status().pins, "last owner failed to shut down");
        }

        static void requestPins() {
            auto                b = makeShared<CBuffer>();
            COutputState        state;
            CDRMAsyncCommitData data(state.snapshot());
            data.mainBuffer   = b;
            data.cursorBuffer = b;
            int  releases     = 0;
            auto listener     = b->events.backendRelease.listen([&] {
                ++releases;
                data.releasePins();
            });
            data.pinBuffers();
            require(b->backendPinCount() == 1, "shared request buffer double pinned");
            data.releasePins();
            data.releasePins();
            require(!b->backendPinCount() && releases == 1, "reentrant request release imbalance");
        }

        CSharedPointer<CBackend>                   root;
        CSharedPointer<CSession>                   session;
        CSharedPointer<CSessionDevice>             device;
        CSharedPointer<CDRMBackend>                backend;
        CSharedPointer<CDRMOutput>                 output;
        std::shared_ptr<COps>                      ops;
        std::shared_ptr<CDRMDeviceContext>         context;
        std::weak_ptr<SDRMTileGroup>               weakGroup;
        std::vector<CSharedPointer<SDRMConnector>> heads;
        std::vector<CSharedPointer<CBuffer>>       buffers;
        std::vector<CSharedPointer<CDRMFB>>        fbs;
    };
}

int main() {
    // Quarantine intentionally survives process lifetime. Isolate cases so that
    // its global admission ban is exercised rather than reset through a test hook.
    const std::vector<std::function<void()>> cases{
        [] { CDRMShutdownTest::success(true, false, false); },
        [] { CDRMShutdownTest::success(false, false, false); },
        [] { CDRMShutdownTest::success(true, true, false); },
        [] { CDRMShutdownTest::success(true, false, true); },
        [] { CDRMShutdownTest::failure(true, true, false, false); },
        [] { CDRMShutdownTest::failure(false, false, false, false); },
        [] { CDRMShutdownTest::failure(true, false, true, false); },
        [] { CDRMShutdownTest::failure(false, false, false, true); },
        [] { CDRMShutdownTest::imports(); },
        [] { CDRMShutdownTest::requestPins(); },
        [] { CDRMShutdownTest::asyncShutdown(false); },
        [] { CDRMShutdownTest::asyncShutdown(true); },
        [] { CDRMShutdownTest::idleRecovery(); },
        [] { CDRMShutdownTest::descriptorReuse(); },
        [] { CDRMShutdownTest::lastOwner(false); },
        [] { CDRMShutdownTest::lastOwner(true); },
        [] { CDRMShutdownTest::partialStartup(false); },
        [] { CDRMShutdownTest::partialStartup(true); },
        [] { CDRMShutdownTest::success(true, false, false, true); },
    };
    size_t caseNumber = 0;
    for (const auto& run : cases) {
        std::fprintf(stderr, "DRMShutdown case %zu\n", ++caseNumber);
        const pid_t child = fork();
        require(child >= 0, "fork");
        if (!child) {
            run();
            std::exit(0); // Allow sanitizer exit hooks; the quarantine root has no destructor.
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child case failed");
    }
    return 0;
}

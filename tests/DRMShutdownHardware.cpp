// Manual, bounded KMS test. Run in an otherwise unused, active local seat.
// Never register this executable with CTest: it changes the physical display.
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/DRMDumb.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static int trial(const std::string& mode) {
    SBackendOptions options;
    options.logFunction = [](eBackendLogLevel, const std::string& text) { std::cerr << text << '\n'; };
    SBackendImplementationOptions drm;
    drm.backendType        = AQ_BACKEND_DRM;
    drm.backendRequestMode = AQ_BACKEND_REQUEST_MANDATORY;
    auto root              = CBackend::create({drm}, options);
    check(!!root, "backend creation");
    std::vector<CSharedPointer<IOutput>> outputs;
    auto                                 newOutput = root->events.newOutput.listen([&](CSharedPointer<IOutput> output) { outputs.push_back(output); });
    check(root->start(), "backend startup");
    CSharedPointer<CDRMBackend> backend;
    for (const auto& implementation : root->getImplementations()) {
        if (auto raw = dynamic_cast<CDRMBackend*>(implementation.get()))
            backend = raw->self.lock();
    }
    check(!!backend && !!root->session, "DRM backend/session");
    CSharedPointer<IAllocator> allocator;
    for (const auto& a : backend->getAllocators()) {
        if (a && a->type() == AQ_ALLOCATOR_TYPE_DRM_DUMB)
            allocator = a;
    }
    check(!!allocator, "dumb allocator");
    std::vector<CSharedPointer<IBuffer>>                buffers;
    std::vector<Hyprutils::Signal::CHyprSignalListener> listeners;
    size_t                                              destroyed = 0, released = 0;
    bool                                                nativePanel = false;
    for (const auto& output : outputs) {
        const auto preferred = output->preferredMode();
        check(!!preferred, "preferred mode");
        check(output->name != "DP-1", "hidden tile announced");
        if (output->name == "eDP-1")
            nativePanel = preferred->pixelSize == Hyprutils::Math::Vector2D{5120, 2880};
        auto buffer = allocator->acquire({.size = preferred->pixelSize, .format = DRM_FORMAT_XRGB8888, .scanout = true}, nullptr);
        check(!!buffer && buffer->good(), "dumb allocation");
        auto [pixels, format, length] = buffer->beginDataPtr(0);
        check(pixels && length, "dumb mapping");
        std::memset(pixels, 0x40, length);
        buffer->endDataPtr();
        buffers.push_back(buffer);
        listeners.push_back(buffer->events.backendRelease.listen([&] { ++released; }));
        listeners.push_back(output->events.destroy.listen([&] { ++destroyed; }));
        output->state->setEnabled(true);
        output->state->setMode(preferred);
        output->state->setFormat(DRM_FORMAT_XRGB8888);
        output->state->setBuffer(buffer);
        check(output->commit(), "initial output commit");
        std::cout << "OUTPUT " << output->name << ' ' << preferred->pixelSize.x << 'x' << preferred->pixelSize.y << std::endl;
    }
    check(nativePanel, "native two-head 5K panel missing");
    std::cout << "READY " << mode << std::endl;
    const auto start    = std::chrono::steady_clock::now();
    bool       shutdown = false;
    while (std::chrono::steady_clock::now() - start < 20s) {
        const auto          fds = root->getPollFDs();
        std::vector<pollfd> polls;
        for (const auto& fd : fds)
            polls.push_back({fd->fd, POLLIN, 0});
        const int ready = poll(polls.data(), polls.size(), 100);
        check(ready >= 0 || errno == EINTR, "poll");
        if (ready > 0) {
            for (size_t i = 0; i < polls.size(); ++i) {
                if (polls[i].revents & POLLIN)
                    fds[i]->onSignal();
            }
        }
        if (!shutdown && ((mode == "active" && std::chrono::steady_clock::now() - start > 3s) || (mode != "active" && !root->session->active))) {
            shutdown = true;
            if (mode == "recover") {
                backend->shutdown();
                check(backend->shutdownStatus().state == eDRMRetirementState::DEFERRED, "inactive device did not defer");
                std::cout << "WAITING_FOR_ACTIVATION" << std::endl;
            } else
                root->shutdown();
        }
        if (shutdown && backend->shutdownStatus().state != eDRMRetirementState::DEFERRED)
            break;
    }
    const auto status     = backend->shutdownStatus();
    const bool quarantine = mode == "inactive";
    std::cout << "RESULT state=" << static_cast<int>(status.state) << " buffers=" << status.buffers << " pins=" << status.pins << " error=" << status.error
              << " cleanupErrors=" << status.cleanupErrors << " destroyed=" << destroyed << " released=" << released << std::endl;
    check(shutdown, "seat transition timeout");
    check(status.state == (quarantine ? eDRMRetirementState::QUARANTINED : eDRMRetirementState::RETIRED), "unexpected retirement disposition");
    check(destroyed == outputs.size(), "logical destroy count");
    check(released == (quarantine ? 0 : buffers.size()), "release count");
    check(status.cleanupErrors == 0, "DRM cleanup errors");
    check(quarantine ? status.pins >= outputs.size() : status.pins == 0, "scanout pin balance");
    for (const auto& output : outputs)
        check(!output->commit(), "retained output accepted work");
    root->shutdown();
    const auto count = destroyed;
    outputs.clear();
    buffers.clear();
    check(destroyed == count, "duplicate destroy on final reference");
    CWeakPointer<CBackend> weakRoot    = root;
    CWeakPointer<CSession> weakSession = root->session;
    backend.reset();
    root.reset();
    check(weakRoot.expired() && weakSession.expired(), "backend/session retained after last owner");
    std::cout << "PASS " << mode << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2 || (std::string(argv[1]) != "active" && std::string(argv[1]) != "inactive" && std::string(argv[1]) != "recover")) {
        std::cerr << "Manual display test: drmShutdownHardware active|inactive|recover\n";
        return 2;
    }
    try {
        return trial(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}

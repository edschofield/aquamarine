#include <aquamarine/allocator/DRMDumb.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include "../backend/drm/Retirement.hpp"
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <unistd.h>
#include <cstring>
#include <sys/mman.h>
#include "../backend/drm/Renderer.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer
#define WP CWeakPointer

Aquamarine::CDRMDumbBuffer::CDRMDumbBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CDRMDumbAllocator> allocator_,
                                           Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) : allocator(allocator_.lock()) {
    if (!allocator || allocator->drmFD() < 0 || !allocator->backend)
        return;
    attrs.format = params.format;

    if (int ret = drmModeCreateDumbBuffer(allocator->drmFD(), params.size.x, params.size.y, 32, 0, &handle, &stride, &bufferLen); ret < 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("failed to create a drm_dumb buffer: {}", strerror(-ret)));
        return;
    }

    pixelSize = {(double)params.size.x, (double)params.size.y};

    attrs.size          = pixelSize;
    attrs.strides.at(0) = stride;
    attrs.planes        = 1;
    size                = pixelSize;

    uint64_t offset = 0;
    if (int ret = drmModeMapDumbBuffer(allocator->drmFD(), handle, &offset); ret < 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("failed to map a drm_dumb buffer: {}", strerror(-ret)));
        return;
    }

    data = (uint8_t*)mmap(nullptr, bufferLen, PROT_READ | PROT_WRITE, MAP_SHARED, allocator->drmFD(), offset);
    if (data == MAP_FAILED) {
        allocator->backend->log(AQ_LOG_ERROR, "failed to mmap a drm_dumb buffer");
        return;
    }

    // set the entire buffer so we dont get garbage
    memset(data, 0xFF, bufferLen);

    if (int ret = drmPrimeHandleToFD(allocator->drmFD(), handle, DRM_CLOEXEC, &primeFD); ret < 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("failed to map a drm_dumb buffer: {}", strerror(-ret)));
        return;
    }

    attrs.fds.at(0) = primeFD;

    attrs.success = true;

    allocator->backend->log(AQ_LOG_DEBUG, std::format("DRM Dumb: Allocated a new buffer with primeFD {}, size {} and format {}", primeFD, attrs.size, fourccToName(attrs.format)));
}

Aquamarine::CDRMDumbBuffer::~CDRMDumbBuffer() {
    events.destroy.emit();
    if (data && data != MAP_FAILED)
        munmap(data, bufferLen);
    if (primeFD >= 0)
        close(primeFD);
    // The buffer retains its allocator and its separate DRM file description.
    // KMS import handles cannot alias this allocation handle.
    if (handle && allocator && allocator->drmFD() >= 0)
        drmModeDestroyDumbBuffer(allocator->drmFD(), handle);
}

eBufferCapability Aquamarine::CDRMDumbBuffer::caps() {
    return eBufferCapability::BUFFER_CAPABILITY_DATAPTR;
}

eBufferType Aquamarine::CDRMDumbBuffer::type() {
    return eBufferType::BUFFER_TYPE_DMABUF;
}

void Aquamarine::CDRMDumbBuffer::update(const Hyprutils::Math::CRegion& damage) {
    ; // nothing to do
}

bool Aquamarine::CDRMDumbBuffer::isSynchronous() {
    return true;
}

bool Aquamarine::CDRMDumbBuffer::good() {
    return attrs.success && data && data != MAP_FAILED;
}

SDMABUFAttrs Aquamarine::CDRMDumbBuffer::dmabuf() {
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::CDRMDumbBuffer::beginDataPtr(uint32_t flags) {
    return {data, attrs.format, bufferLen};
}

void Aquamarine::CDRMDumbBuffer::endDataPtr() {
    ; // nothing to do
}

Aquamarine::CDRMDumbAllocator::~CDRMDumbAllocator() {
    if (ownsFD && drmfd >= 0)
        close(drmfd);
}

SP<CDRMDumbAllocator> Aquamarine::CDRMDumbAllocator::create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_, std::shared_ptr<CDRMDeviceContext> context) {
    if (!backend_)
        return nullptr;
    if (drmGetNodeTypeFromFd(drmfd_) != DRM_NODE_PRIMARY) {
        backend_->log(AQ_LOG_ERROR, "DRM Dumb: Cannot create allocator when drmfd is not the primary node");
        return nullptr;
    }

    uint64_t hasDumb = 0;
    if (drmGetCap(drmfd_, DRM_CAP_DUMB_BUFFER, &hasDumb) < 0) {
        backend_->log(AQ_LOG_ERROR, "DRM Dumb: Failed to query hasDumb");
        return nullptr;
    }

    if (!hasDumb) {
        backend_->log(AQ_LOG_ERROR, "DRM Dumb: hasDumb is false, gpu driver doesn't support dumb buffers!");
        return nullptr;
    }

    auto a  = SP<CDRMDumbAllocator>(new CDRMDumbAllocator(drmfd_, backend_));
    a->self = a;
    a->deviceContext = std::move(context);
    // PRIME imports are not reference-counted within a DRM file description.
    // A dup would share the KMS handle namespace: importing this buffer there
    // and closing the import could close the allocator's own dumb handle.
    a->drmfd  = backend_->reopenDRMNode(drmfd_, false);
    a->ownsFD = true;
    if (a->drmfd < 0)
        return nullptr;

    backend_->log(AQ_LOG_DEBUG, "DRM Dumb: created a dumb allocator");

    return a;
}

SP<IBuffer> Aquamarine::CDRMDumbAllocator::acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain_) {
    if (!backend || (deviceContext && !deviceContext->running()))
        return nullptr;
    auto buf = SP<IBuffer>(new CDRMDumbBuffer(params, self, swapchain_));
    if (!buf->good())
        return nullptr;
    return buf;
}

SP<CBackend> Aquamarine::CDRMDumbAllocator::getBackend() {
    return backend.lock();
}

int Aquamarine::CDRMDumbAllocator::drmFD() {
    return drmfd;
}

eAllocatorType Aquamarine::CDRMDumbAllocator::type() {
    return eAllocatorType::AQ_ALLOCATOR_TYPE_DRM_DUMB;
}

Aquamarine::CDRMDumbAllocator::CDRMDumbAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) : backend(backend_), drmfd(fd_) {
    ; // nothing to do
}

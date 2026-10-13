#pragma once

#include <aquamarine/backend/drm/Lifecycle.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <array>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace Aquamarine {
    class CDRMRetirementManager;
    class CDRMFB;
    class CBackend;
    class CSession;
    class CSessionDevice;
    struct SDRMConnector;

    struct SDRMDisableHead {
        uint32_t connector = 0, connectorCrtc = 0;
        uint32_t crtc = 0, active = 0, mode = 0;
        uint32_t primary = 0, primaryFB = 0, primaryCrtc = 0;
        uint32_t cursor = 0, cursorFB = 0, cursorCrtc = 0;
    };

    // The production implementation is the only place retirement issues DRM calls.
    // Tests inject these operations, never a real GPU descriptor.
    class IDRMRetirementOps {
      public:
        virtual ~IDRMRetirementOps()                                                                                                = default;
        virtual int  disable(int fd, bool atomic, const std::vector<SDRMDisableHead>& heads)                                        = 0;
        virtual bool isMaster(int fd)                                                                                               = 0;
        virtual int  closeFB(int fd, uint32_t id, bool remove)                                                                      = 0;
        virtual int  closeHandle(int fd, uint32_t handle)                                                                           = 0;
        virtual int  importHandle(int fd, int dmaFD, uint32_t* handle)                                                              = 0;
        virtual int  addFB(int fd, const SDMABUFAttrs& attrs, const std::array<uint32_t, 4>& handles, bool modifiers, uint32_t* id) = 0;
    };

    struct SDRMImport {
        Hyprutils::Memory::CWeakPointer<CDRMFB>    fb;
        Hyprutils::Memory::CWeakPointer<IBuffer>   buffer;
        Hyprutils::Memory::CSharedPointer<IBuffer> heldBuffer;
        uint32_t                                   id      = 0;
        std::array<uint32_t, 4>                    handles = {};
        size_t                                     pins = 0, deferredUnpins = 0;
        bool                                       blocked = false;
    };

    class CDRMDeviceContext : public std::enable_shared_from_this<CDRMDeviceContext> {
      public:
        static std::shared_ptr<CDRMDeviceContext> create(Hyprutils::Memory::CSharedPointer<CSessionDevice> device, Hyprutils::Memory::CSharedPointer<CSession> session,
                                                         Hyprutils::Memory::CWeakPointer<CBackend> backend);
        static bool                               canOpen(const std::string& path);
        ~CDRMDeviceContext();

        int                         fd() const;
        bool                        running() const;
        bool                        sessionActive() const;
        bool                        sessionUsable() const;
        SDRMShutdownStatus          status() const;
        std::shared_ptr<SDRMImport> registerImport(Hyprutils::Memory::CSharedPointer<CDRMFB> fb, Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        bool                        rememberHead(const SDRMConnector& connector, bool atomic);
        void                        pin(const std::shared_ptr<SDRMImport>& import);
        void                        unpin(const std::shared_ptr<SDRMImport>& import);
        void                        closeHandles(SDRMImport& import);
        void                        dropImport(SDRMImport& import, bool remove = false, bool runtimeFallback = true);
        IDRMRetirementOps&          ops();

        void                        beginShutdown();
        void                        finishShutdown(bool sessionActive, bool canRetry, int permanentError = 0);
        void                        retry(bool sessionActive);
        void                        deviceClosing();
        void                        sessionClosing();

      private:
        CDRMDeviceContext(int fd, std::shared_ptr<IDRMRetirementOps> ops);
        void                                              invalidateImports();
        void                                              complete(bool retired, int error);
        void                                              closeDevice();
        void                                              report() const;
        int                                               attemptDisable();
        void                                              block(SDRMImport& import);
        void                                              release(SDRMImport& import, size_t count);
        void                                              registerWithManager();
        void                                              attachRecovery(Hyprutils::Memory::CSharedPointer<CSession> session);

        int                                               m_fd        = -1;
        bool                                              m_quiescing = false, m_detached = false, m_closed = false, m_atomic = true;
        bool                                              m_retryUsed = false, m_completing = false;
        SDRMShutdownStatus                                m_status;
        std::string                                       m_path;
        std::shared_ptr<IDRMRetirementOps>                m_ops;
        std::list<std::shared_ptr<SDRMImport>>            m_imports;
        std::vector<SDRMDisableHead>                      m_heads;
        Hyprutils::Memory::CSharedPointer<CSessionDevice> m_device;
        Hyprutils::Memory::CWeakPointer<CSession>         m_session;
        Hyprutils::Memory::CWeakPointer<CBackend>         m_backend;
        Hyprutils::Signal::CHyprSignalListener            m_activate, m_sessionDestroy;
        friend class CDRMRetirementManager;
        friend class CDRMShutdownTest;
    };
}

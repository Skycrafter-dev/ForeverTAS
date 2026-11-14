#include "viewer/pointer_lock.h"

#include "diagnostics/diagnostics_log.h"

#include <QGuiApplication>
#include <QQuickWindow>

#if defined(FOREVERTAS_WAYLAND_POINTER_LOCK)
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>

#include <wayland-client.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <cstring>
#endif

namespace forevertas::viewer {

#if defined(FOREVERTAS_WAYLAND_POINTER_LOCK)

// The compositor's objects, on an event queue of our own (Qt's thread reads
// the socket; dispatch() runs ours on the GUI thread).
struct PointerLock::Wayland {
    PointerLock *owner = nullptr;
    wl_display *display = nullptr;
    wl_pointer *pointer = nullptr;
    wl_event_queue *queue = nullptr;
    wl_display *displayOnQueue = nullptr;   // a wrapper on our queue
    wl_registry *registry = nullptr;
    zwp_pointer_constraints_v1 *constraints = nullptr;
    zwp_relative_pointer_manager_v1 *relativeManager = nullptr;
    zwp_relative_pointer_v1 *relative = nullptr;
    zwp_locked_pointer_v1 *lockedPointer = nullptr;
    bool active = false;   // the compositor said locked
    bool moving = false;   // raw motion seen since the lock

    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t version) {
        auto *self = static_cast<Wayland *>(data);
        if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
            self->constraints = static_cast<zwp_pointer_constraints_v1 *>(
                    wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
        } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
            self->relativeManager = static_cast<zwp_relative_pointer_manager_v1 *>(
                    wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
        }
        (void)version;
    }
    static void globalRemove(void *, wl_registry *, uint32_t) {}
    static constexpr wl_registry_listener registryListener{&global, &globalRemove};

    static void relativeMotion(void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t dx,
                               wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
        auto *self = static_cast<Wayland *>(data);
        if (self->lockedPointer == nullptr) return;
        if (!self->moving) {
            self->moving = true;
            diagnostics::Log("pointer lock: raw motion coming in");
        }
        emit self->owner->moved(QPointF(wl_fixed_to_double(dx), wl_fixed_to_double(dy)));
    }
    static constexpr zwp_relative_pointer_v1_listener relativeListener{&relativeMotion};

    static void locked(void *data, zwp_locked_pointer_v1 *) {
        static_cast<Wayland *>(data)->active = true;
        diagnostics::Log("pointer lock: locked by the compositor");
    }
    static void unlocked(void *data, zwp_locked_pointer_v1 *) {
        static_cast<Wayland *>(data)->active = false;
        diagnostics::Log("pointer lock: unlocked by the compositor");
    }
    static constexpr zwp_locked_pointer_v1_listener lockedListener{&locked, &unlocked};

    bool start() {
        auto *native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (native == nullptr) return false;
        display = native->display();
        pointer = native->pointer();
        if (display == nullptr || pointer == nullptr) {
            diagnostics::Log("pointer lock: no Wayland %s", display == nullptr ? "display" : "pointer");
            return false;
        }
        queue = wl_display_create_queue(display);
        displayOnQueue = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(displayOnQueue), queue);
        registry = wl_display_get_registry(displayOnQueue);
        wl_registry_add_listener(registry, &registryListener, this);
        wl_display_roundtrip_queue(display, queue);
        if (constraints == nullptr || relativeManager == nullptr) {
            diagnostics::Log("pointer lock: the compositor offers %s%s%s",
                             constraints == nullptr ? "no pointer constraints" : "",
                             constraints == nullptr && relativeManager == nullptr ? ", " : "",
                             relativeManager == nullptr ? "no relative pointer" : "");
            return false;
        }
        return true;
    }

    // Raw motion only while locked: its events would pile up in our queue
    // otherwise.
    void startMotion() {
        auto *pointerOnQueue = static_cast<wl_pointer *>(wl_proxy_create_wrapper(pointer));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(pointerOnQueue), queue);
        relative = zwp_relative_pointer_manager_v1_get_relative_pointer(relativeManager, pointerOnQueue);
        wl_proxy_wrapper_destroy(pointerOnQueue);
        zwp_relative_pointer_v1_add_listener(relative, &relativeListener, this);
    }
    void stopMotion() {
        if (relative != nullptr) zwp_relative_pointer_v1_destroy(relative);
        relative = nullptr;
    }

    ~Wayland() {
        if (lockedPointer != nullptr) zwp_locked_pointer_v1_destroy(lockedPointer);
        if (relative != nullptr) zwp_relative_pointer_v1_destroy(relative);
        if (relativeManager != nullptr) zwp_relative_pointer_manager_v1_destroy(relativeManager);
        if (constraints != nullptr) zwp_pointer_constraints_v1_destroy(constraints);
        if (registry != nullptr) wl_registry_destroy(registry);
        if (displayOnQueue != nullptr) wl_proxy_wrapper_destroy(displayOnQueue);
        if (queue != nullptr) wl_event_queue_destroy(queue);
        if (display != nullptr) wl_display_flush(display);
    }
};

#else

struct PointerLock::Wayland {};

#endif

PointerLock::PointerLock(QObject *parent) : QObject(parent) {
    pump_.setInterval(4);
    connect(&pump_, &QTimer::timeout, this, &PointerLock::dispatch);
}

PointerLock::~PointerLock() {
    delete wayland_;
}

bool PointerLock::lock(QQuickWindow *window) {
#if defined(FOREVERTAS_WAYLAND_POINTER_LOCK)
    if (window == nullptr || !QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        return false;
    }
    if (wayland_ == nullptr) {
        wayland_ = new Wayland;
        wayland_->owner = this;
        if (!wayland_->start()) {
            // the compositor cannot: not asked again
            wayland_->owner = nullptr;
            return false;
        }
    }
    if (wayland_->owner == nullptr) return false;
    auto *native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    wl_surface *surface = native != nullptr ? native->surface() : nullptr;
    if (surface == nullptr) return false;
    wayland_->lockedPointer = zwp_pointer_constraints_v1_lock_pointer(
            wayland_->constraints, surface, wayland_->pointer, nullptr,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wayland_->lockedPointer), wayland_->queue);
    zwp_locked_pointer_v1_add_listener(wayland_->lockedPointer, &Wayland::lockedListener, wayland_);
    wayland_->startMotion();
    wayland_->moving = false;
    wl_display_flush(wayland_->display);
    diagnostics::Log("pointer lock: asked");
    locked_ = true;
    pump_.start();
    return true;
#else
    (void)window;
    return false;
#endif
}

void PointerLock::unlock(const QPointF &showAt) {
    if (!locked_) return;
    locked_ = false;
    pump_.stop();
#if defined(FOREVERTAS_WAYLAND_POINTER_LOCK)
    if (wayland_ != nullptr && wayland_->lockedPointer != nullptr) {
        dispatch();
        // where the pointer shows again (applied with the surface's next commit)
        zwp_locked_pointer_v1_set_cursor_position_hint(wayland_->lockedPointer,
                                                       wl_fixed_from_double(showAt.x()),
                                                       wl_fixed_from_double(showAt.y()));
        zwp_locked_pointer_v1_destroy(wayland_->lockedPointer);
        wayland_->lockedPointer = nullptr;
        wayland_->stopMotion();
        dispatch();   // what was still queued
        wayland_->active = false;
        wl_display_flush(wayland_->display);
    }
#else
    (void)showAt;
#endif
}

void PointerLock::dispatch() {
#if defined(FOREVERTAS_WAYLAND_POINTER_LOCK)
    if (wayland_ != nullptr && wayland_->queue != nullptr) {
        wl_display_dispatch_queue_pending(wayland_->display, wayland_->queue);
    }
#endif
}

}  // namespace forevertas::viewer

#ifndef FOREVERTAS_VIEWER_POINTER_LOCK_H
#define FOREVERTAS_VIEWER_POINTER_LOCK_H

#include <QObject>
#include <QPointF>
#include <QTimer>

class QQuickWindow;

namespace forevertas::viewer {

// The pointer held still while it drags something, as games hold it: it
// stays where it is and moved() says how far it would have gone. On Wayland
// (where no application may move the pointer) this asks the compositor for
// a pointer lock and raw motion (pointer-constraints and relative-pointer).
// Elsewhere lock() returns false: the caller puts the pointer back itself.
class PointerLock : public QObject {
    Q_OBJECT

public:
    explicit PointerLock(QObject *parent = nullptr);
    ~PointerLock() override;

    // True when the compositor locked the pointer to window.
    bool lock(QQuickWindow *window);
    // Lets it go; on Wayland the pointer reappears at showAt (window
    // coordinates) where the compositor allows.
    void unlock(const QPointF &showAt);
    bool locked() const { return locked_; }

signals:
    // How far the pointer would have moved, in window coordinates.
    void moved(const QPointF &delta);

private:
    void dispatch();

    struct Wayland;
    Wayland *wayland_ = nullptr;
    bool locked_ = false;
    QTimer pump_;
};

}  // namespace forevertas::viewer

#endif

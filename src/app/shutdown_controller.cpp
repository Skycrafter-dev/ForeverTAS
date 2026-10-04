#include "app/shutdown_controller.h"

#include <algorithm>
#include <utility>

namespace forevertas::app {

ShutdownController::ShutdownController(std::function<void()> begin, std::function<bool()> idle,
        std::function<void()> persist, std::function<void()> forceExit, int graceMs, QObject *parent)
    : QObject(parent), begin_(std::move(begin)), persist_(std::move(persist)),
      forceExit_(std::move(forceExit)), idle_(std::move(idle)), graceMs_(std::max(0, graceMs)) {
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &ShutdownController::poll);
}

bool ShutdownController::requestClose() {
    if (ready_) return true;
    if (closing_) return false;
    closing_ = true;
    elapsed_.start();
    persist_();
    begin_();
    ready_ = idle_();
    emit stateChanged();
    if (!ready_) timer_.start();
    return ready_;
}

void ShutdownController::poll() {
    if (idle_()) {
        ready_ = true;
        timer_.stop();
        emit readyToClose();
    } else if (!delayed_ && elapsed_.elapsed() >= graceMs_) {
        delayed_ = true;
        emit stateChanged();
    }
}

void ShutdownController::forceExit() {
    if (!closing_ || !delayed_ || ready_ || forced_) return;
    forced_ = true;
    persist_();
    forceExit_();
}

}

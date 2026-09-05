#include "blocks/visual_debugger.h"
#include "blocks/block_value.h"

#include <algorithm>

namespace forevertas::blocks {

void VisualDebugger::enable(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) resume();
}

void VisualDebugger::begin(bool pauseAtStart) {
  std::lock_guard<std::mutex> lock(mutex_);
  pauseRequested_ = pauseAtStart;
  paused_ = false;
  step_ = Step::Run;
  lastUpdate_ = {};
}

void VisualDebugger::pause() {
  enabled_.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  pauseRequested_ = true;
}

void VisualDebugger::resume(Step step) {
  std::lock_guard<std::mutex> lock(mutex_);
  step_ = step;
  paused_ = false;
  pauseRequested_ = false;
  wake_.notify_all();
}

void VisualDebugger::setBreakpoints(std::set<VisualNodeId> blocks) {
  std::lock_guard<std::mutex> lock(mutex_);
  breakpoints_ = std::move(blocks);
}

void VisualDebugger::setObserver(Observer observer) {
  std::lock_guard<std::mutex> lock(mutex_);
  observer_ = std::move(observer);
}

bool VisualDebugger::visit(VisualNodeId block, std::size_t depth, const VisualState &state,
                          const std::map<std::string, VisualValue> &variables,
                          const std::vector<VisualCallFrame> &frames,
                          const std::function<bool()> &stopRequested,
                          bool executablePoint) {
  if (!enabled()) return true;
  std::unique_lock<std::mutex> lock(mutex_);
  const bool stopHere = pauseRequested_ || (executablePoint && (breakpoints_.count(block) ||
      step_ == Step::Into || (step_ == Step::Over && depth <= pausedDepth_) ||
      (step_ == Step::Out && depth < pausedDepth_))) ||
      (step_ == Step::Tick && state.tick != pausedTick_);
  if (stopHere) {
    paused_ = true;
    pauseRequested_ = false;
    pausedDepth_ = depth;
    pausedTick_ = state.tick;
    step_ = Step::Run;
  }
  const auto now = std::chrono::steady_clock::now();
  if (stopHere || now - lastUpdate_ >= std::chrono::milliseconds(100)) {
    lastUpdate_ = now;
    const auto observer = observer_;
    if (observer) {
      VisualDebugSnapshot snapshot{block, depth, state, variables, frames, paused_, false};
      lock.unlock();
      observer(snapshot);
      lock.lock();
    }
  }
  while (paused_ && enabled()) {
    lock.unlock();
    const bool stop = stopRequested && stopRequested();
    lock.lock();
    if (stop) { paused_ = false; return false; }
    wake_.wait_for(lock, std::chrono::milliseconds(25), [&] { return !paused_; });
  }
  return true;
}

void VisualDebugger::finish(const VisualState &state,
                            const std::map<std::string, VisualValue> &variables) {
  Observer observer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
    step_ = Step::Run;
    observer = observer_;
    wake_.notify_all();
  }
  if (observer && enabled()) observer({0, 0, state, variables, {}, false, true});
}

std::string DescribeVisualValue(const VisualValue &value, std::size_t limit) {
  const auto number = [](double n) { return FormatNumberValue(n); };
  std::string text = std::visit([&](const auto &v) -> std::string {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, std::monostate>) return "absent";
    else if constexpr (std::is_same_v<T, double>) return number(v);
    else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else if constexpr (std::is_same_v<T, std::string>) return v.substr(0, limit);
    else if constexpr (std::is_same_v<T, VisualVector>) return "("+number(v.x)+", "+number(v.y)+", "+number(v.z)+")";
    else if constexpr (std::is_same_v<T, VisualRotation>) return "rotation ("+number(v.x)+", "+number(v.y)+", "+number(v.z)+", "+number(v.w)+")";
    else if constexpr (std::is_same_v<T, VisualRange>) return "["+number(v.minimum)+" … "+number(v.maximum)+"]";
    else if constexpr (std::is_same_v<T, VisualState>) return "state at "+std::to_string(v.timeMs)+" ms";
    else if constexpr (std::is_same_v<T, VisualInputs>) return std::to_string(v.size())+" inputs";
    else if constexpr (std::is_same_v<T, VisualProcedure>) return "block "+v.name;
    else if constexpr (std::is_same_v<T, VisualPolygon>) return std::to_string(v.size())+" vertices";
    else if constexpr (std::is_same_v<T, VisualVolume>) return v.plane.empty() ? "box" : "prism ("+v.plane+")";
    else if constexpr (std::is_same_v<T, std::shared_ptr<const VisualSnapshot>>) return v ? "snapshot at "+std::to_string(v->state.timeMs)+" ms" : "absent snapshot";
    else {
      std::string out = "[";
      if (v) for (std::size_t i=0; i<v->size() && i<8 && out.size()<limit; ++i) {
        if (i) out += ", ";
        out += DescribeVisualValue((*v)[i], std::min<std::size_t>(limit-out.size(), 40));
      }
      if (v && v->size()>8) out += ", …";
      return out+"]";
    }
  }, value.data);
  if (text.size()>limit) text = text.substr(0, limit)+"…";
  return text;
}

} // namespace forevertas::blocks

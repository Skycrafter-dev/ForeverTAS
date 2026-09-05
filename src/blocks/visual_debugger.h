#ifndef FOREVERTAS_BLOCKS_VISUAL_DEBUGGER_H
#define FOREVERTAS_BLOCKS_VISUAL_DEBUGGER_H

#include "blocks/visual_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>

namespace forevertas::blocks {

struct VisualDebugSnapshot {
  VisualNodeId block = 0;
  std::size_t depth = 0;
  VisualState state;
  std::map<std::string, VisualValue> variables;
  std::vector<VisualCallFrame> frames;
  bool paused = false;
  bool finished = false;
};

// Shared by the UI and execution thread. A paused program still polls Stop;
// no queued QObject slot is needed to wake a blocked interpreter.
class VisualDebugger {
public:
  enum class Step { Run, Into, Over, Out, Tick };
  using Observer = std::function<void(const VisualDebugSnapshot &)>;

  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  void enable(bool enabled);
  void begin(bool pauseAtStart = false);
  void pause();
  void resume(Step step = Step::Run);
  void setBreakpoints(std::set<VisualNodeId> blocks);
  void setObserver(Observer observer);
  bool visit(VisualNodeId block, std::size_t depth, const VisualState &state,
             const std::map<std::string, VisualValue> &variables,
             const std::vector<VisualCallFrame> &frames,
             const std::function<bool()> &stopRequested,
             bool executablePoint = true);
  void finish(const VisualState &state,
              const std::map<std::string, VisualValue> &variables);

private:
  std::atomic_bool enabled_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool pauseRequested_ = false, paused_ = false;
  Step step_ = Step::Run;
  std::size_t pausedDepth_ = 0;
  std::uint64_t pausedTick_ = 0;
  std::set<VisualNodeId> breakpoints_;
  Observer observer_;
  std::chrono::steady_clock::time_point lastUpdate_{};
};

std::string DescribeVisualValue(const VisualValue &value, std::size_t limit = 200);

} // namespace forevertas::blocks
#endif

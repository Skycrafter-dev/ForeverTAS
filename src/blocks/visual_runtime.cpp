#include "blocks/visual_runtime.h"

#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_debugger.h"
#include "blocks/parallel_executor.h"
#include "blocks/program_value_codec.h"
#include "input_timeline_time.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <mutex>

namespace forevertas::blocks {

namespace {
class DeferredHistory final : public VisualHistorySpan {
public:
  explicit DeferredHistory(std::function<std::vector<VisualState>()> sampler) : sampler_(std::move(sampler)) {}
  void appendTo(std::vector<VisualState> &states) const override {
    std::call_once(once_,[&] { samples_=sampler_(); sampler_={}; });
    states.insert(states.end(),samples_.begin(),samples_.end());
  }
private:
  mutable std::function<std::vector<VisualState>()> sampler_;
  mutable std::once_flag once_;
  mutable std::vector<VisualState> samples_;
};
}

std::shared_ptr<const VisualHistorySpan> DeferredVisualHistory(std::function<std::vector<VisualState>()> sample) {
  return std::make_shared<DeferredHistory>(std::move(sample));
}

VisualAdvance VisualSimulationHost::advanceMany(std::uint32_t ticks) {
  VisualAdvance result{read(),read(),{}};
  std::vector<VisualState> states;
  states.reserve(ticks);
  for (std::uint32_t i=0;i<ticks;++i) {
    result.previous=result.state; result.state=advance(); states.push_back(result.state);
  }
  if (ticks>1) result.history=DeferredVisualHistory([states=std::move(states)] { return states; });
  return result;
}

std::vector<VisualState> VisualHistoryStates(
    const std::shared_ptr<const VisualHistoryNode> &history) {
  if (!history) return {};
  std::vector<const VisualHistoryNode *> chunks;
  for (auto current=history.get();current;current=current->previous.get()) chunks.push_back(current);
  std::vector<VisualState> states;
  states.reserve(history->size);
  for (auto it=chunks.rbegin();it!=chunks.rend();++it) {
    if ((*it)->span) (*it)->span->appendTo(states);
    else states.push_back((*it)->state);
    if (states.size()!=(*it)->size) throw std::runtime_error("Invalid sampled history span.");
  }
  return states;
}

namespace {

using Snapshot = std::shared_ptr<const VisualSnapshot>;
using List = std::shared_ptr<const VisualList>;
constexpr double kMaximumInteger = 9007199254740991.0;
constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;
constexpr std::uint32_t kMaximumHorizonMs = 2147481040;

struct Stop {};
enum class Flow { Next, Break, Continue, Return };
using Frame = VisualCallFrame;

std::string Field(const VisualNode &node, const std::string &key) {
  const auto found = node.fields.find(key);
  if (found != node.fields.end()) return found->second;
  for (const auto &field : FindVisualBlock(node.definitionId)->fields)
    if (field.key == key) return field.defaultValue;
  throw std::runtime_error("Missing field '" + key + "'.");
}

template<class T> T As(const VisualValue &value, const char *expected) {
  const auto *typed = std::get_if<T>(&value.data);
  if (!typed) throw std::runtime_error(std::string("Expected ") + expected + ".");
  return *typed;
}

double Number(const VisualValue &value) {
  const double number = As<double>(value, "a number");
  if (!std::isfinite(number)) throw std::runtime_error("Expected a finite number.");
  return number;
}

VisualValue Scalar(double value) {
  if (!std::isfinite(value)) throw std::runtime_error("The calculation did not produce a finite number.");
  return VisualValue(value);
}

std::int64_t Integer(double value, double minimum = -kMaximumInteger,
                     double maximum = kMaximumInteger) {
  if (!std::isfinite(value) || value < minimum || value > maximum || std::floor(value) != value)
    throw std::runtime_error("Expected a whole number between " + FormatNumberValue(minimum) +
                             " and " + FormatNumberValue(maximum) + ".");
  return static_cast<std::int64_t>(value);
}

double Length(VisualVector v) { return std::hypot(v.x, v.y, v.z); }
VisualVector Subtract(VisualVector a, VisualVector b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
VisualVector Scale(VisualVector v, double scale) { return {v.x*scale, v.y*scale, v.z*scale}; }
VisualVector Normalize(VisualVector v) {
  const double length = Length(v);
  if (!std::isfinite(length) || length <= 1e-12) throw std::runtime_error("Cannot normalize a zero or invalid vector.");
  return Scale(v, 1.0 / length);
}

VisualRotation Rotation(double yaw, double pitch, double roll) {
  const double cy = std::cos(yaw*kRadiansPerDegree/2), sy = std::sin(yaw*kRadiansPerDegree/2);
  const double cp = std::cos(pitch*kRadiansPerDegree/2), sp = std::sin(pitch*kRadiansPerDegree/2);
  const double cr = std::cos(roll*kRadiansPerDegree/2), sr = std::sin(roll*kRadiansPerDegree/2);
  // Match the viewer's Y-up car pose: yaw about Y, pitch about X, roll about Z.
  return {sp*cy*cr+cp*sy*sr, cp*sy*cr-sp*cy*sr, cp*cy*sr-sp*sy*cr, cp*cy*cr+sp*sy*sr};
}

bool Inside(VisualVector position, const VisualVolume &volume) {
  const auto p = Subtract(position, volume.origin);
  if (volume.plane.empty())
    return std::abs(p.x) <= volume.size.x/2 && std::abs(p.y) <= volume.size.y/2 && std::abs(p.z) <= volume.size.z/2;
  const double x = volume.plane == "yz" ? p.y : p.x;
  const double y = volume.plane == "xy" ? p.y : p.z;
  const double depth = volume.plane == "xy" ? p.z : volume.plane == "xz" ? p.y : p.x;
  if (depth < 0 || depth > volume.depth) return false;
  bool inside = false;
  for (std::size_t i = 0, j = volume.polygon.size()-1; i < volume.polygon.size(); j = i++) {
    const auto [ax, ay] = volume.polygon[j];
    const auto [bx, by] = volume.polygon[i];
    const double cross = (x-ax)*(by-ay)-(y-ay)*(bx-ax);
    if (std::abs(cross) < 1e-9 && x >= std::min(ax,bx) && x <= std::max(ax,bx) &&
        y >= std::min(ay,by) && y <= std::max(ay,by)) return true;
    if ((ay > y) != (by > y) && x < (bx-ax)*(y-ay)/(by-ay)+ax) inside = !inside;
  }
  return inside;
}

const std::vector<std::pair<std::string, SandboxInputAction>> &Actions() {
  static const std::vector<std::pair<std::string, SandboxInputAction>> actions{
      {"steer", SandboxInputAction::Steer}, {"accelerate", SandboxInputAction::Accelerate},
      {"brake", SandboxInputAction::Brake}, {"gas", SandboxInputAction::Gas},
      {"left", SandboxInputAction::SteerLeft}, {"right", SandboxInputAction::SteerRight},
      {"respawn", SandboxInputAction::Respawn}, {"race-running", SandboxInputAction::RaceRunning},
      {"finish-line", SandboxInputAction::FinishLine}, {"unmapped", SandboxInputAction::Unmapped}};
  return actions;
}

class Runtime {
public:
  Runtime(const VisualProgram &program, VisualSimulationHost &host, const VisualRuntimeControl &control)
      : program_(program), host_(host), control_(control), current_(host.read()), previous_(current_),
        inputs_(std::make_shared<const VisualInputs>(host.inputs())),
        history_(std::make_shared<VisualHistoryNode>(VisualHistoryNode{current_, {}, 1})),
        horizon_(control.horizonMs) {
    if (!control.tickMs || !control.collectionLimit || !control.workerCount || control.workerCount > 256 || control.horizonMs < control.tickMs ||
        control.horizonMs > kMaximumHorizonMs || control.horizonMs % control.tickMs)
      throw std::invalid_argument("Invalid visual runtime limits.");
    initial_ = save();
    if (control.operationLimit) operationCounter_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    for (auto id : program.topLevel) {
      const auto &node = *program.find(id);
      if (node.enabled && node.definitionId == "procedures/define") procedures_[Field(node, "name")] = &node;
    }
  }

  VisualExecutionResult run() {
    try {
      for (auto id : program_.topLevel) {
        const auto &node = *program_.find(id);
        if (node.enabled && node.definitionId == "flow/when-start") stack(node, "body");
      }
    } catch (const Stop &) { result_.stopped = true; }
      catch (...) {
        if (control_.debugger) control_.debugger->finish(current_, result_.variables);
        throw;
      }
    result_.finalSnapshot = save();
    if (control_.debugger) control_.debugger->finish(current_, result_.variables);
    return std::move(result_);
  }

private:
  const VisualProgram &program_;
  VisualSimulationHost &host_;
  const VisualRuntimeControl &control_;
  VisualExecutionResult result_;
  std::map<std::string, const VisualNode *> procedures_;
  std::vector<Frame> frames_;
  std::mt19937_64 random_{1};
  VisualState current_, previous_;
  std::shared_ptr<const VisualInputs> inputs_;
  std::shared_ptr<const VisualHistoryNode> history_;
  Snapshot initial_;
  std::uint32_t horizon_;
  const VisualNode *active_ = nullptr;
  std::size_t depth_ = 0;
  std::shared_ptr<std::atomic<std::uint64_t>> operationCounter_;
  struct Event { VisualValue value; VisualState state; };
  std::vector<Event> events_;
  bool mapped_ = false;
  ParallelExecutor executor_;
  std::vector<std::unique_ptr<VisualSimulationHost>> mapHosts_;
  std::vector<HostBytecodeStorage> mapStorage_;
  std::map<std::string,BytecodeCompilation> bytecode_;
  std::set<std::string> nonUniform_;

  void poll(bool inspect = false, bool executablePoint = true) {
    if (control_.stopRequested && control_.stopRequested()) throw Stop{};
    if (operationCounter_ && operationCounter_->fetch_add(1, std::memory_order_relaxed) >= control_.operationLimit)
      throw std::runtime_error("The program exceeded its operation limit.");
    ++result_.operations;
    if ((result_.operations & 1023u) == 0 && control_.progress) control_.progress();
    if (inspect && control_.debugger && !control_.debugger->visit(
          active_ ? active_->id : 0, depth_, state(), result_.variables, frames_, control_.stopRequested, executablePoint))
      throw Stop{};
  }

  template<class Function> auto at(const VisualNode &node, Function function) -> decltype(function()) {
    const auto *saved = active_;
    active_ = &node;
    ++depth_;
    struct Reset { Runtime &runtime; const VisualNode *saved;
      ~Reset() { runtime.active_ = saved; --runtime.depth_; } } reset{*this, saved};
    try {
      poll(true);
      if (depth_ > 256) throw std::runtime_error("The execution stack is too deep.");
      return function();
    } catch (const std::runtime_error &error) {
      const std::string message = error.what();
      if (message.rfind("Block #", 0) == 0) throw;
      throw std::runtime_error("Block #" + std::to_string(node.id) + " (" +
          FindVisualBlock(node.definitionId)->label + "): " + message);
    }
  }

  const VisualNode &input(const VisualNode &node, const std::string &key) const {
    const auto found = node.inputs.find(key);
    if (found == node.inputs.end()) throw std::runtime_error("Connect input '" + key + "'.");
    return *program_.find(found->second);
  }
  VisualValue value(const VisualNode &node, const std::string &key) { return eval(input(node,key)); }
  double number(const VisualNode &node, const std::string &key) { return Number(value(node,key)); }
  bool boolean(const VisualNode &node, const std::string &key) { return As<bool>(value(node,key), "a boolean"); }
  VisualVector vector(const VisualNode &node, const std::string &key) {
    auto v = As<VisualVector>(value(node,key), "a vector");
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
      throw std::runtime_error("Expected a finite vector.");
    return v;
  }
  void collectionSize(std::size_t count) const {
    if (count > control_.collectionLimit) throw std::runtime_error("The collection exceeds the runtime item limit.");
  }
  std::size_t index(const VisualNode &node, std::size_t size) {
    return static_cast<std::size_t>(Integer(number(node,"index"),1,static_cast<double>(size))-1);
  }
  std::uint32_t time(double ms, bool horizon = false) const {
    const auto result = Integer(ms, horizon ? control_.tickMs : 0, kMaximumHorizonMs);
    if (result % control_.tickMs) throw std::runtime_error("Time must be aligned to " + std::to_string(control_.tickMs) + " ms.");
    return static_cast<std::uint32_t>(result);
  }
  const VisualState &state() const { return current_; }
  Snapshot save() const {
    return std::make_shared<VisualSnapshot>(VisualSnapshot{
        host_.capture(), current_, previous_, inputs_, history_, horizon_, result_.candidates});
  }
  void restore(const Snapshot &snapshot) {
    if (!snapshot || !snapshot->native || !snapshot->inputs || !snapshot->history)
      throw std::runtime_error("Invalid snapshot.");
    // A serial mapped job can change the shared host's horizon. Establish a
    // horizon valid for both cursors before restoring, then shrink afterwards.
    host_.setHorizon(std::max(snapshot->horizonMs, static_cast<std::uint32_t>(host_.read().timeMs)));
    current_ = host_.restore(*snapshot->native);
    host_.replaceInputs(*snapshot->inputs);
    inputs_ = snapshot->inputs;
    horizon_ = snapshot->horizonMs;
    current_ = host_.setHorizon(horizon_);
    previous_ = snapshot->previous;
    history_ = snapshot->history;
  }
  void restart() {
    const auto inputs = inputs_;
    const auto horizon = horizon_;
    restore(initial_);
    host_.replaceInputs(*inputs);
    inputs_ = inputs;
    horizon_ = horizon;
    current_ = host_.setHorizon(horizon_);
    history_ = std::make_shared<VisualHistoryNode>(VisualHistoryNode{current_, {}, 1});
  }
  void tick() {
    poll(true, false);
    if (current_.timeMs >= horizon_)
      throw std::runtime_error("Simulation reached its horizon; restore, restart, or extend the horizon.");
    std::vector<std::pair<const VisualNode *, bool>> edges;
    for (auto id : program_.topLevel) {
      const auto &node = *program_.find(id);
      if (node.enabled && node.definitionId == "events/when") edges.emplace_back(&node, boolean(node,"condition"));
    }
    previous_ = current_;
    current_ = host_.advance();
    if (current_.timeMs <= previous_.timeMs) throw std::runtime_error("The simulation did not advance.");
    collectionSize(history_->size + 1);
    history_ = std::make_shared<VisualHistoryNode>(
        VisualHistoryNode{current_, history_, history_->size + 1});
    poll(true, false);
    const auto emitted = current_;
    const bool checkpoint = current_.checkpointsCollected > previous_.checkpointsCollected;
    const bool finished = current_.raceCompleted && !previous_.raceCompleted;
    std::vector<const VisualNode *> triggered;
    for (const auto &[node, before] : edges)
      if (!before && boolean(*node,"condition")) triggered.push_back(node);
    dispatch("events/on-tick", VisualValue(emitted), emitted);
    if (checkpoint) dispatch("events/on-checkpoint", VisualValue(emitted), emitted);
    if (finished) dispatch("events/on-finish", VisualValue(emitted), emitted);
    for (const auto *node : triggered) handleEvent(*node, VisualValue(emitted), emitted);
  }

  VisualValue &variable(const std::string &name, bool create) {
    if (!frames_.empty()) {
      const auto found = frames_.back().locals.find(name);
      if (found != frames_.back().locals.end()) return found->second;
    }
    const auto found = result_.variables.find(name);
    if (found != result_.variables.end()) return found->second;
    if (!create) throw std::runtime_error("Variable '" + name + "' has not been set.");
    collectionSize(result_.variables.size()+1);
    return result_.variables[name];
  }

  VisualValue call(const VisualNode &node, bool reporter) {
    const auto found = procedures_.find(Field(node,"name"));
    if (found == procedures_.end()) throw std::runtime_error("Unknown procedure.");
    std::vector<VisualValue> arguments;
    for (std::size_t i=0; i<VisualProcedureParameters(*found->second).size(); ++i)
      arguments.push_back(value(node,"arg"+std::to_string(i)));
    return invoke(*found->second, arguments, reporter);
  }

  VisualValue invoke(const VisualNode &definition, const std::vector<VisualValue> &arguments, bool reporter) {
    if (frames_.size() >= 64) throw std::runtime_error("Procedure recursion exceeds 64 calls.");
    Frame frame;
    frame.name = Field(definition,"name");
    const auto parameters = VisualProcedureParameters(definition);
    if (parameters.size() != arguments.size()) throw std::runtime_error("Procedure argument count does not match.");
    for (std::size_t i=0; i<parameters.size(); ++i)
      frame.locals.emplace(parameters[i], arguments[i]);
    frames_.push_back(std::move(frame));
    struct Pop { std::vector<Frame> &frames; ~Pop() { frames.pop_back(); } } pop{frames_};
    const auto flow = stack(definition,"body");
    if (reporter && flow != Flow::Return) throw std::runtime_error("The procedure ended without returning a value.");
    return frames_.back().returned;
  }

  void handleEvent(const VisualNode &node, VisualValue payload, const VisualState &emitted) {
    if (frames_.size() >= 64) throw std::runtime_error("Event recursion exceeds 64 calls.");
    frames_.push_back(Frame{FindVisualBlock(node.definitionId)->label, {}, {}});
    events_.push_back({std::move(payload), emitted});
    struct Pop { Runtime &r; ~Pop() { r.frames_.pop_back(); r.events_.pop_back(); } } pop{*this};
    at(node, [&] { stack(node,"body"); });
  }

  void dispatch(const std::string &event, const VisualValue &payload, const VisualState &emitted,
                const std::string &message = {}) {
    for (auto id : program_.topLevel) {
      const auto &node = *program_.find(id);
      if (node.enabled && node.definitionId == event && (event != "events/on-message" || Field(node,"name") == message))
        handleEvent(node, payload, emitted);
    }
  }

  VisualValue map(const VisualNode &node) {
    const auto function = As<VisualProcedure>(value(node,"function"), "a procedure");
    const auto items = As<List>(value(node,"list"), "a list");
    const auto requestedWorkers = static_cast<std::uint32_t>(Integer(number(node,"workers"),1,256));
    const auto found = procedures_.find(function.name);
    if (found == procedures_.end()) throw std::runtime_error("Unknown mapped procedure.");
    if (VisualProcedureParameters(*found->second).size()!=1)
      throw std::runtime_error("A mapped procedure takes one item parameter.");
    const auto baseline = save();
    auto output = std::make_shared<VisualList>(items->size());
    if (items->empty()) return VisualValue(List(output));
    std::vector<std::uint64_t> seeds;
    seeds.reserve(items->size());
    for (std::size_t i=0; i<items->size(); ++i) { poll(); seeds.push_back(random_()); }
    // Nested maps reuse their worker; debugging follows input order. Neither
    // changes the isolated value semantics or deterministic random streams.
    const auto workerLimit = mapped_ || (control_.debugger && control_.debugger->enabled())
        ? std::size_t{1} : static_cast<std::size_t>(control_.workerCount);
    const auto workers = std::min({items->size(), static_cast<std::size_t>(requestedWorkers), workerLimit,
        static_cast<std::size_t>(host_.workerCapacity())});
    if (!workers) throw std::runtime_error("The simulation host has no branch worker capacity.");
    const ProgramBytecode *compiled=nullptr;
    if (control_.compilePrograms && !control_.operationLimit && control_.collectionLimit==1000000 &&
        (!control_.debugger || !control_.debugger->enabled())) {
      auto entry=bytecode_.find(function.name);
      if (entry==bytecode_.end()) entry=bytecode_.emplace(function.name,CompileMappedProgram(program_,function.name)).first;
      if (entry->second.program) compiled=&*entry->second.program;
      if (!compiled && control_.executionModeChanged) control_.executionModeChanged(entry->second.reason);
    }
    if (mapStorage_.size()<workers) mapStorage_.resize(workers);
    std::vector<unsigned char> pendingLanes;
    std::size_t fallbackLanes=0;
    auto compiledControl=control_;
    compiledControl.restartOrigin=initial_;
    if (compiled) {
      if (items->size()>1 && !nonUniform_.count(function.name)) {
        if (auto uniform=TryUniformProgram(*compiled,baseline,result_.variables,*items,control_,mapStorage_.front())) {
          result_.operations+=uniform->operations;
          if (uniform->stopped) throw Stop{};
          if (control_.executionModeChanged) control_.executionModeChanged("Uniform compiled block program");
          return VisualValue(List(std::make_shared<VisualList>(std::move(uniform->values))));
        }
        nonUniform_.insert(function.name);
      }
      if (auto executor=host_.batchExecutor()) {
        if (auto executed=executor->execute(*compiled,baseline,result_.variables,*items,seeds,compiledControl)) {
          result_.operations+=executed->operations;
          if (executed->stopped) throw Stop{};
          if (executed->fallbackLanes.empty())
            return VisualValue(List(std::make_shared<VisualList>(std::move(executed->values))));
          *output=std::move(executed->values);
          pendingLanes.resize(items->size(),0);
          for (const auto lane : executed->fallbackLanes) pendingLanes.at(lane)=1;
          fallbackLanes=executed->fallbackLanes.size();
        }
      } else if (control_.executionModeChanged) {
        control_.executionModeChanged("Compiled block program on CPU");
      }
    }
    if (workers > 1) while (mapHosts_.size() < workers) {
      poll(); mapHosts_.push_back(host_.fork());
    }
    std::atomic<std::size_t> next{0};
    std::atomic_bool abort{false};
    std::atomic_bool usedInterpreter{false};
    std::vector<std::exception_ptr> failures(workers);
    std::vector<std::uint64_t> operations(workers, 0);
    const auto run = [&](std::size_t worker) {
      try {
        VisualSimulationHost &host = workers > 1 ? *mapHosts_[worker] : host_;
        auto control = control_;
        control.restartOrigin = initial_;
        control.workerCount = 1; // Nested maps reuse this worker, not a new pool.
        control.executionModeChanged = {};
        control.published = {};
        control.candidateCountChanged = {};
        control.candidateLimit.reset();
        control.horizonMs = horizon_;
        control.stopRequested = [&] {
          return abort.load(std::memory_order_relaxed) ||
                 (control_.stopRequested && control_.stopRequested());
        };
        for (;;) {
          const auto i=next.fetch_add(1, std::memory_order_relaxed);
          if (i>=items->size() || abort.load(std::memory_order_relaxed)) break;
          if (!pendingLanes.empty() && !pendingLanes[i]) continue;
          bool interpreted=!compiled || !pendingLanes.empty();
          if (compiled && pendingLanes.empty()) {
            host.setHorizon(std::max(baseline->horizonMs,static_cast<std::uint32_t>(host.read().timeMs)));
            host.restore(*baseline->native);
            host.replaceInputs(*baseline->inputs);
            host.setHorizon(baseline->horizonMs);
            const auto executed=ExecuteHostBytecode(*compiled,host,baseline,result_.variables,(*items)[i],seeds[i],control,mapStorage_[worker]);
            if (executed.stopped) throw Stop{};
            operations[worker]+=executed.operations;
            interpreted=executed.needsInterpreter;
            if (!interpreted) (*output)[i]=executed.value;
          }
          if (interpreted) {
            usedInterpreter.store(true,std::memory_order_relaxed);
            Runtime job(program_, host, control);
            job.operationCounter_ = operationCounter_;
            job.restore(baseline);
            job.initial_ = initial_;
            job.result_.variables = result_.variables;
            job.random_.seed(seeds[i]);
            job.depth_ = depth_;
            job.mapped_ = true;
            (*output)[i] = job.invoke(*found->second, {(*items)[i]}, true);
            operations[worker] += job.result_.operations;
          }
        }
      } catch (...) {
        failures[worker] = std::current_exception();
        abort.store(true, std::memory_order_relaxed);
      }
    };
    executor_.run(workers, run);
    if (compiled && usedInterpreter.load(std::memory_order_relaxed) && control_.executionModeChanged)
      control_.executionModeChanged(fallbackLanes ? "CUDA block-program kernel with source fallback: "+
          std::to_string(fallbackLanes)+"/"+std::to_string(items->size())+" lanes" :
          "Source interpreter: dynamic values or compiled arena capacity");
    if (workers == 1) restore(baseline);
    for (auto count : operations) result_.operations += count;
    // Prefer the actual job failure over Stop exceptions in sibling workers.
    bool stopped = false;
    for (auto failure : failures) if (failure) {
      try { std::rethrow_exception(failure); }
      catch (const Stop &) { stopped = true; }
    }
    if (stopped) throw Stop{};
    return VisualValue(List(output));
  }

  VisualInputs editedInputs(const VisualNode &node, VisualInputs inputs) {
    const auto ms = time(number(node,"time"));
    const auto actionName = As<std::string>(value(node,"action"), "an input action");
    const auto action = std::find_if(Actions().begin(),Actions().end(),
        [&](const auto &entry) { return entry.first == actionName; });
    if (action == Actions().end() || action->second == SandboxInputAction::Unmapped ||
        action->second == SandboxInputAction::RaceRunning || action->second == SandboxInputAction::FinishLine)
      throw std::runtime_error("Choose a writable car input action.");
    const double inputValue = number(node,"value");
    SandboxInputEvent event;
    event.timeMs = static_cast<std::int32_t>(*SimulationTimelineTimeFromUserTime(ms,control_.tickMs));
    event.action = action->second;
    using namespace forevervalidator::experimental;
    if (event.action == SandboxInputAction::Steer || event.action == SandboxInputAction::Gas) {
      event.value.kind = PhysicsSandboxInputValueKind::Analog;
      event.value.analog = static_cast<AnalogInputState>(Integer(inputValue, kAnalogInputMinimum, kAnalogInputMaximum));
    } else {
      event.value.kind = PhysicsSandboxInputValueKind::Switch;
      event.value.switchState = Integer(inputValue,0,1) ? PhysicsSandboxSwitchState::Pressed : PhysicsSandboxSwitchState::Released;
    }
    // Upsert this time/action only. Do not normalize, clamp, or remove any
    // unrelated events as a side effect of editing a single input.
    const auto existing = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
      return input.timeMs == event.timeMs && input.action == event.action;
    });
    if (existing != inputs.end()) *existing = event;
    else inputs.insert(std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
      return input.timeMs > event.timeMs;
    }), event);
    collectionSize(inputs.size());
    return inputs;
  }
  void useInputs(VisualInputs inputs) {
    collectionSize(inputs.size());
    if (!std::is_sorted(inputs.begin(),inputs.end(),[](const auto &a,const auto &b) { return a.timeMs<b.timeMs; }))
      throw std::runtime_error("Sort inputs by time before applying the sequence.");
    std::set<std::pair<std::int32_t,SandboxInputAction>> keys;
    for (const auto &event : inputs) {
      poll();
      if (!keys.emplace(event.timeMs,event.action).second)
        throw std::runtime_error("Two inputs have the same time and action. Remove one explicitly before applying the sequence.");
    }
    // A state cannot claim to have been reached by a different input prefix.
    // Programs can freely edit input values, then restart before applying them.
    auto prefix = [&](const VisualInputs &events) {
      VisualInputs result;
      for (const auto &event : events)
        if (event.timeMs <= static_cast<std::int64_t>(current_.timeMs)) result.push_back(event);
      return result;
    };
    const auto before = prefix(*inputs_), after = prefix(inputs);
    if (before.size() != after.size() || !std::equal(before.begin(),before.end(),after.begin(),SameInputEvent))
      throw std::runtime_error("Past inputs changed. Restart or restore a state before the first edited input.");
    auto shared = std::make_shared<const VisualInputs>(std::move(inputs));
    host_.replaceInputs(*shared);
    inputs_ = std::move(shared);
  }

  void publish(double score, const VisualState &evaluationState, Snapshot snapshot = {}) {
    if (!std::isfinite(score)) throw std::runtime_error("Result score must be finite.");
    if (!snapshot) snapshot=save();
    const auto candidate=snapshot->candidate;
    result_.published = VisualPublishedRun{score, evaluationState, std::move(snapshot), candidate};
    if (control_.published) control_.published(*result_.published);
  }
  void count() {
    if (control_.candidateLimit && result_.candidates >= *control_.candidateLimit) throw Stop{};
    ++result_.candidates;
    if (control_.candidateCountChanged) control_.candidateCountChanged(result_.candidates);
  }

  VisualValue eval(const VisualNode &node);
  bool equal(const VisualValue &a, const VisualValue &b, std::size_t depth=0) {
    poll();
    if (depth>64) throw std::runtime_error("Value comparison exceeds 64 nested levels.");
    if (a.data.index()!=b.data.index()) return false;
    return std::visit([&](const auto &left) -> bool {
      using T=std::decay_t<decltype(left)>;
      const auto &right=std::get<T>(b.data);
      if constexpr (std::is_same_v<T,std::monostate>) return true;
      else if constexpr (std::is_same_v<T,VisualVector>) return left.x==right.x && left.y==right.y && left.z==right.z;
      else if constexpr (std::is_same_v<T,VisualRotation>) return left.x==right.x && left.y==right.y && left.z==right.z && left.w==right.w;
      else if constexpr (std::is_same_v<T,VisualRange>) return left.minimum==right.minimum && left.maximum==right.maximum;
      else if constexpr (std::is_same_v<T,VisualProcedure>) return left.name==right.name;
      else if constexpr (std::is_same_v<T,VisualVolume>) {
        return left.plane==right.plane && left.depth==right.depth && left.polygon==right.polygon &&
            equal(VisualValue(left.origin),VisualValue(right.origin),depth+1) &&
            equal(VisualValue(left.size),VisualValue(right.size),depth+1);
      } else if constexpr (std::is_same_v<T,VisualInputs>) {
        if (left.size()!=right.size()) return false;
        for (std::size_t i=0; i<left.size(); ++i) { poll(); if (!SameInputEvent(left[i],right[i])) return false; }
        return true;
      } else if constexpr (std::is_same_v<T,List>) {
        if (left==right) return true;
        if (!left || !right || left->size()!=right->size()) return false;
        for (std::size_t i=0; i<left->size(); ++i)
          if (!equal((*left)[i],(*right)[i],depth+1)) return false;
        return true;
      } else if constexpr (std::is_same_v<T,VisualState>) {
        for (const auto &[key,label] : VisualStateProperties()) {
          (void)label;
          if (!equal(ReadVisualStateProperty(left,key),ReadVisualStateProperty(right,key),depth+1)) return false;
        }
        return true;
      } else return left==right;
    },a.data);
  }
  Flow command(const VisualNode &node);
  Flow stack(const VisualNode &node, const std::string &key);
};

VisualValue Runtime::eval(const VisualNode &node) {
  return at(node,[&]() -> VisualValue {
    const auto &id = node.definitionId;
    if (id == "runtime/workers") return Scalar(control_.workerCount);
    if (id == "runtime/batch-size") return Scalar(control_.batchSize);
    if (id == "values/none") return VisualValue{};
    if (id == "procedures/reference") return VisualValue(VisualProcedure{Field(node,"name")});
    if (id == "procedures/apply") {
      const auto function=As<VisualProcedure>(value(node,"function"),"a procedure");
      const auto arguments=As<List>(value(node,"arguments"),"an argument list");
      const auto found=procedures_.find(function.name);
      if (found==procedures_.end()) throw std::runtime_error("Unknown procedure: "+function.name);
      return invoke(*found->second,*arguments,true);
    }
    if (id == "events/value") return events_.empty() ? VisualValue{} : events_.back().value;
    if (id == "events/state") return VisualValue(events_.empty() ? state() : events_.back().state);
    if (id == "procedures/map") return map(node);
    if (id == "data/numbers") {
      const auto from=number(node,"from"), to=number(node,"to"), step=number(node,"step");
      if (!step) throw std::runtime_error("Number-list step must not be zero.");
      const auto count=(step>0 ? from>to : from<to) ? 0 : std::floor((to-from)/step)+1;
      Integer(count,0,static_cast<double>(control_.collectionLimit));
      auto list=std::make_shared<VisualList>();
      for (std::size_t i=0; i<static_cast<std::size_t>(count); ++i) { poll(); list->push_back(Scalar(from+step*i)); }
      return VisualValue(List(list));
    }
    if (id == "values/boolean") return VisualValue(Field(node,"value") == "true");
    if (id == "values/text" || id == "inputs/action-name" || id == "targets/plane") return VisualValue(Field(node,"value"));
    if (id == "values/number-range" || id == "values/integer-range" ||
        id == "math/number-range" || id == "math/integer-range") {
      const bool literal = id.rfind("values/",0)==0;
      const double minimum = literal ? *ParseNumberValue(Field(node,"minimum")) : number(node,"minimum");
      const double maximum = literal ? *ParseNumberValue(Field(node,"maximum")) : number(node,"maximum");
      if (minimum > maximum) throw std::runtime_error("Range minimum exceeds maximum.");
      if (id.find("integer-range") != std::string::npos) { Integer(minimum); Integer(maximum); }
      return VisualValue(VisualRange{minimum,maximum});
    }
    if (id.rfind("values/",0)==0) {
      const auto parsed = ParseNumberValue(Field(node,"value"));
      if (!parsed) throw std::runtime_error("Invalid number.");
      return Scalar(*parsed);
    }
    if (id == "data/get") return variable(Field(node,"name"),false);
    if (id == "data/has-value") return VisualValue(!std::holds_alternative<std::monostate>(value(node,"value").data));
    if (id == "data/list") return VisualValue(List(std::make_shared<VisualList>()));
    if (id.rfind("data/",0)==0) {
      const auto list = As<List>(value(node,"list"),"a list");
      if (!list) throw std::runtime_error("Invalid list.");
      if (id == "data/contains") {
        const auto sought=value(node,"value");
        for (const auto &item : *list) if (equal(item,sought)) return VisualValue(true);
        return VisualValue(false);
      }
      if (id == "data/length") return Scalar(static_cast<double>(list->size()));
      if (id == "data/item") return list->at(index(node,list->size()));
      auto edited = std::make_shared<VisualList>(*list);
      if (id == "data/append") edited->push_back(value(node,"value"));
      else if (id == "data/replace-item") { const auto i=index(node,list->size()); (*edited)[i]=value(node,"value"); }
      else if (id == "data/delete-item") edited->erase(edited->begin()+static_cast<std::ptrdiff_t>(index(node,list->size())));
      else throw std::runtime_error("Not a list reporter.");
      collectionSize(edited->size());
      return VisualValue(List(edited));
    }
    if (id == "procedures/value") return call(node,true);
    if (id == "simulation/state") return VisualValue(state());
    if (id == "simulation/horizon") return Scalar(horizon_);
    if (id == "simulation/tick-duration") return Scalar(control_.tickMs);
    if (id == "simulation/previous-state") return VisualValue(previous_);
    if (id == "simulation/read") return ReadVisualStateProperty(As<VisualState>(value(node,"state"),"a simulation state"),Field(node,"property"));
    if (id == "simulation/snapshot") return VisualValue(save());
    if (id == "simulation/snapshot-state") return VisualValue(As<Snapshot>(value(node,"snapshot"),"a snapshot")->state);
    if (id == "simulation/snapshot-inputs") {
      const auto snapshot=As<Snapshot>(value(node,"snapshot"),"a snapshot");
      if (!snapshot || !snapshot->inputs) throw std::runtime_error("Invalid snapshot.");
      return VisualValue(*snapshot->inputs);
    }
    if (id == "simulation/inputs") return VisualValue(*inputs_);
    if (id == "simulation/history") {
      auto list = std::make_shared<VisualList>();
      for (const auto &sample : VisualHistoryStates(history_)) list->emplace_back(sample);
      return VisualValue(List(list));
    }
    static const std::map<std::string,std::string> stateReporters{
        {"car-position","position"},{"car-velocity","velocity"},{"car-local-velocity","local-velocity"},
        {"car-speed","speed"},{"car-rotation","rotation"},{"stunt-points","stunt-points"},
        {"finish-time","finish-time"},{"time","time"},{"checkpoint-count","checkpoints"},
        {"race-completed","finished"},{"sliding","sliding"},{"freewheeling","freewheeling"}};
    if (id.rfind("simulation/",0)==0) {
      const auto found = stateReporters.find(id.substr(11));
      if (found != stateReporters.end()) return ReadVisualStateProperty(state(),found->second);
    }
    if (id == "inputs/empty") return VisualValue(VisualInputs{});
    if (id.rfind("inputs/",0)==0) {
      auto inputs = As<VisualInputs>(value(node,"inputs"),"an input sequence");
      if (id == "inputs/count") return Scalar(static_cast<double>(inputs.size()));
      if (id == "inputs/set") return VisualValue(editedInputs(node,std::move(inputs)));
      if (id == "inputs/sort") {
        std::stable_sort(inputs.begin(), inputs.end(), [](const auto &a, const auto &b) { return a.timeMs < b.timeMs; });
        return VisualValue(std::move(inputs));
      }
      if (id == "inputs/value-at") {
        const auto atTime = *SimulationTimelineTimeFromUserTime(time(number(node,"time")), control_.tickMs);
        const auto actionName = As<std::string>(value(node,"action"), "an input action");
        const auto action = std::find_if(Actions().begin(), Actions().end(), [&](const auto &entry) { return entry.first == actionName; });
        if (action == Actions().end()) throw std::runtime_error("Unknown input action.");
        const SandboxInputEvent *last = nullptr;
        for (const auto &event : inputs) {
          poll();
          if (event.action == action->second && event.timeMs <= atTime && (!last || event.timeMs >= last->timeMs)) last = &event;
        }
        if (!last) return Scalar(0);
        using namespace forevervalidator::experimental;
        if (last->value.kind == PhysicsSandboxInputValueKind::Analog) return Scalar(last->value.analog);
        if (last->value.kind == PhysicsSandboxInputValueKind::Switch) return Scalar(last->value.switchState == PhysicsSandboxSwitchState::Released ? 0 : 1);
        return {};
      }
      const auto i = index(node,inputs.size());
      const auto event = inputs[i];
      if (id == "inputs/with-time") {
        inputs[i].timeMs = static_cast<std::int32_t>(*SimulationTimelineTimeFromUserTime(time(number(node,"time")), control_.tickMs));
        return VisualValue(std::move(inputs));
      }
      if (id == "inputs/with-value") {
        using namespace forevervalidator::experimental;
        const double assigned = number(node,"value");
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog)
          inputs[i].value.analog = static_cast<AnalogInputState>(Integer(assigned,kAnalogInputMinimum,kAnalogInputMaximum));
        else if (event.value.kind == PhysicsSandboxInputValueKind::Switch)
          inputs[i].value.switchState = Integer(assigned,0,1) ? PhysicsSandboxSwitchState::Pressed : PhysicsSandboxSwitchState::Released;
        else throw std::runtime_error("This event has no editable input value.");
        return VisualValue(std::move(inputs));
      }
      if (id == "inputs/time") return Scalar(static_cast<double>(UserTimelineTimeFromSimulationTime(event.timeMs,control_.tickMs)));
      if (id == "inputs/value") {
        using namespace forevervalidator::experimental;
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog) return Scalar(event.value.analog);
        if (event.value.kind == PhysicsSandboxInputValueKind::Switch)
          return Scalar(event.value.switchState == PhysicsSandboxSwitchState::Released ? 0 : 1);
        return {};
      }
      if (id == "inputs/action") {
        for (const auto &[name,action] : Actions()) if (event.action==action) return VisualValue(name);
      }
      if (id == "inputs/remove") { inputs.erase(inputs.begin()+static_cast<std::ptrdiff_t>(i)); return VisualValue(std::move(inputs)); }
    }
    if (id == "results/has-result") return VisualValue(result_.published.has_value());
    if (id == "results/snapshot") {
      if (!result_.published) throw std::runtime_error("No result has been kept yet.");
      return VisualValue(result_.published->snapshot);
    }
    if (id == "results/iterations") return Scalar(static_cast<double>(result_.candidates));
    if (id == "results/best-score") {
      if (!result_.published) throw std::runtime_error("No result has been kept yet.");
      return Scalar(result_.published->score);
    }
    if (id == "time/all") return VisualValue(VisualRange{0,static_cast<double>(horizon_)});
    if (id == "time/at") { const double t=number(node,"time"); time(t); return VisualValue(VisualRange{t,t}); }
    if (id == "time/range") {
      const double from=number(node,"from"), to=number(node,"to");
      time(from); time(to);
      if (to < from) throw std::runtime_error("Time range ends before it starts.");
      return VisualValue(VisualRange{from,to});
    }
    if (id == "targets/point" || id == "targets/direction" || id == "targets/size") {
      VisualVector v{number(node,"x"),number(node,"y"),number(node,"z")};
      if (id == "targets/direction") v=Normalize(v);
      if (id == "targets/size" && (v.x<=0 || v.y<=0 || v.z<=0)) throw std::runtime_error("Box dimensions must be positive.");
      return VisualValue(v);
    }
    if (id == "targets/polygon-from-points") {
      const auto points=As<List>(value(node,"points"),"a list of points");
      if (points->size()<3) throw std::runtime_error("A polygon needs at least three vertices.");
      VisualPolygon polygon;
      for (const auto &point : *points) {
        poll();
        const auto v=As<VisualVector>(point,"a point");
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) throw std::runtime_error("Invalid polygon vertex.");
        polygon.emplace_back(v.x,v.y);
      }
      return VisualValue(std::move(polygon));
    }
    if (id == "math/range-minimum" || id == "math/range-maximum") {
      const auto range=As<VisualRange>(value(node,"range"),"a range");
      return Scalar(id == "math/range-minimum" ? range.minimum : range.maximum);
    }
    if (id == "targets/rotation") {
      const auto yaw=number(node,"yaw"), pitch=number(node,"pitch"), roll=number(node,"roll");
      return VisualValue(Rotation(yaw,pitch,roll));
    }
    if (id == "targets/box") {
      const auto center=vector(node,"center"), size=vector(node,"size");
      if (size.x<=0 || size.y<=0 || size.z<=0) throw std::runtime_error("Box dimensions must be positive.");
      return VisualValue(VisualVolume{center,size,{},"",0});
    }
    if (id == "targets/prism") {
      const auto origin=vector(node,"origin");
      const double depth=number(node,"depth");
      if (depth<=0) throw std::runtime_error("Prism depth must be positive.");
      const auto plane = As<std::string>(value(node,"plane"),"a projection plane");
      if (plane != "xy" && plane != "xz" && plane != "yz") throw std::runtime_error("Projection plane must be xy, xz or yz.");
      return VisualValue(VisualVolume{origin,{},As<VisualPolygon>(value(node,"polygon"),"a polygon"),plane,depth});
    }
    if (id == "conditions/and") return VisualValue(boolean(node,"a") && boolean(node,"b"));
    if (id == "conditions/or") return VisualValue(boolean(node,"a") || boolean(node,"b"));
    if (id == "conditions/not") return VisualValue(!boolean(node,"value"));
    if (id == "conditions/inside") {
      const auto position=vector(node,"position");
      const auto volume=As<VisualVolume>(value(node,"volume"),"a volume");
      return VisualValue(Inside(position,volume));
    }
    if (id == "conditions/equal") {
      const auto a=value(node,"a"), b=value(node,"b");
      return VisualValue(equal(a,b));
    }
    if (id.rfind("conditions/",0)==0) {
      const double a=number(node,"a"), b=number(node,"b");
      if (id == "conditions/less") return VisualValue(a<b);
      if (id == "conditions/less-equal") return VisualValue(a<=b);
      if (id == "conditions/greater") return VisualValue(a>b);
      if (id == "conditions/greater-equal") return VisualValue(a>=b);
    }
    if (id == "math/random" || id == "math/random-integer") {
      const double a=number(node,"a"), b=number(node,"b");
      if (a>b) throw std::runtime_error("Random minimum exceeds maximum.");
      if (id == "math/random") return Scalar(a+(b-a)*static_cast<double>(random_()>>11)*0x1.0p-53);
      const auto low=Integer(a), high=Integer(b);
      const auto range=static_cast<std::uint64_t>(high-low)+1;
      const auto threshold=(std::uint64_t{0}-range)%range;
      std::uint64_t sample;
      do { poll(); sample=random_(); } while (sample<threshold);
      return Scalar(static_cast<double>(low+static_cast<std::int64_t>(sample%range)));
    }
    if (id == "math/magnitude") return Scalar(Length(vector(node,"value")));
    if (id == "math/normalize") return VisualValue(Normalize(vector(node,"value")));
    if (id == "math/component") {
      const auto v=vector(node,"value"); const auto axis=Field(node,"axis");
      return Scalar(axis=="x" ? v.x : axis=="y" ? v.y : v.z);
    }
    if (id == "math/vector-scale") {
      const auto v=vector(node,"value");
      const auto factor=number(node,"factor");
      return VisualValue(Scale(v,factor));
    }
    if (id == "math/distance" || id == "math/dot" || id == "math/vector-add" || id == "math/vector-subtract") {
      const auto a=vector(node,"a"), b=vector(node,"b");
      if (id == "math/distance") return Scalar(Length(Subtract(a,b)));
      if (id == "math/dot") return Scalar(a.x*b.x+a.y*b.y+a.z*b.z);
      return VisualValue(id=="math/vector-add" ? VisualVector{a.x+b.x,a.y+b.y,a.z+b.z} : Subtract(a,b));
    }
    if (id == "math/rotation-distance") {
      const auto a=As<VisualRotation>(value(node,"a"),"a rotation"), b=As<VisualRotation>(value(node,"b"),"a rotation");
      const double norm=std::sqrt((a.x*a.x+a.y*a.y+a.z*a.z+a.w*a.w)*(b.x*b.x+b.y*b.y+b.z*b.z+b.w*b.w));
      if (norm<=1e-12) throw std::runtime_error("Invalid rotation.");
      return Scalar(2*std::acos(std::clamp(std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w)/norm,0.0,1.0)));
    }
    if (id == "math/clamp") {
      const double v=number(node,"value"), lo=number(node,"minimum"), hi=number(node,"maximum");
      if (lo>hi) throw std::runtime_error("Clamp minimum exceeds maximum.");
      return Scalar(std::clamp(v,lo,hi));
    }
    if (node.inputs.count("value")) {
      const double v=number(node,"value");
      if (id=="math/abs") return Scalar(std::abs(v));
      if (id=="math/percent-ratio") return Scalar(v/100);
      if (id=="math/kmh") return Scalar(v*3.6);
      if (id=="math/floor") return Scalar(std::floor(v));
      if (id=="math/ceil") return Scalar(std::ceil(v));
      if (id=="math/round") return Scalar(std::round(v));
      if (id=="math/sqrt") return Scalar(std::sqrt(v));
      if (id=="math/sin") return Scalar(std::sin(v*kRadiansPerDegree));
      if (id=="math/cos") return Scalar(std::cos(v*kRadiansPerDegree));
    }
    if (node.inputs.count("a") && node.inputs.count("b")) {
      const double a=number(node,"a"), b=number(node,"b");
      if (id=="math/add") return Scalar(a+b);
      if (id=="math/subtract") return Scalar(a-b);
      if (id=="math/multiply") return Scalar(a*b);
      if (id=="math/divide" || id=="math/modulo") {
        if (!b) throw std::runtime_error("Division by zero.");
        return Scalar(id=="math/divide" ? a/b : std::fmod(a,b));
      }
      if (id=="math/min") return Scalar(std::min(a,b));
      if (id=="math/max") return Scalar(std::max(a,b));
      if (id=="math/weighted-blend") { const auto weight=number(node,"weight")/100; return Scalar(a*(1-weight)+b*weight); }
    }
    throw std::runtime_error("This block is not a runtime reporter.");
  });
}

// Statement execution is below the expression evaluator; both use the same
// live variables, call frames, host, and cancellation checkpoints.

Flow Runtime::stack(const VisualNode &node, const std::string &key) {
  const auto found=node.statements.find(key);
  if (found==node.statements.end()) return Flow::Next;
  for (auto id : found->second) {
    const auto &child=*program_.find(id);
    const auto flow=command(child);
    if (flow!=Flow::Next) return flow;
  }
  return Flow::Next;
}

Flow Runtime::command(const VisualNode &node) {
  if (!node.enabled) return Flow::Next;
  return at(node,[&]() -> Flow {
    const auto &id=node.definitionId;
    if (id=="flow/try") {
      try { return stack(node,"body"); }
      catch (const std::runtime_error &error) {
        variable(Field(node,"name"),true)=VisualValue(std::string(error.what()));
        return stack(node,"error");
      }
    }
    if (id=="procedures/do") {
      const auto function=As<VisualProcedure>(value(node,"function"),"a procedure");
      const auto arguments=As<List>(value(node,"arguments"),"an argument list");
      const auto found=procedures_.find(function.name);
      if (found==procedures_.end()) throw std::runtime_error("Unknown procedure: "+function.name);
      invoke(*found->second,*arguments,false);
      return Flow::Next;
    }
    if (id=="debug/pause") {
      if (control_.debugger) { control_.debugger->pause(); poll(true); }
      return Flow::Next;
    }
    if (id=="events/send") {
      const auto message=As<std::string>(value(node,"message"),"a message name");
      const auto payload=value(node,"value");
      dispatch("events/on-message",payload,state(),message);
      return Flow::Next;
    }
    if (id=="flow/for-each") {
      const auto list=As<List>(value(node,"list"),"a list");
      for (const auto &item : *list) {
        poll(true, false);
        variable(Field(node,"name"),true)=item;
        const auto flow=stack(node,"body");
        if (flow==Flow::Break) break;
        if (flow==Flow::Return) return flow;
      }
      return Flow::Next;
    }
    if (id=="flow/stop") throw Stop{};
    if (id=="flow/break") return Flow::Break;
    if (id=="flow/continue") return Flow::Continue;
    if (id=="flow/if") return stack(node,boolean(node,"condition") ? "body" : "else");
    if (id=="flow/repeat" || id=="flow/forever" || id=="flow/while" || id=="flow/until") {
      const auto count=id=="flow/repeat" ? static_cast<std::uint64_t>(Integer(number(node,"count"),0)) : std::uint64_t{0};
      for (std::uint64_t iteration=0;;++iteration) {
        // A loop back-edge is a cancellation/pause checkpoint, not entry to
        // another command. Step Over must finish the entire visible loop.
        poll(true, false);
        if (id=="flow/repeat" && iteration>=count) break;
        if (id=="flow/while" && !boolean(node,"condition")) break;
        if (id=="flow/until" && boolean(node,"condition")) break;
        const auto flow=stack(node,"body");
        if (flow==Flow::Break) break;
        if (flow==Flow::Return) return flow;
      }
      return Flow::Next;
    }
    if (id=="flow/section") return stack(node,"body");
    if (id=="procedures/call") { call(node,false); return Flow::Next; }
    if (id=="procedures/return") {
      const auto returned=value(node,"value");
      if (frames_.empty()) throw std::runtime_error("Return requires a procedure call.");
      frames_.back().returned=returned;
      return Flow::Return;
    }
    if (id=="data/set" || id=="data/local" || id=="data/change") {
      const auto name=Field(node,"name");
      auto assigned=value(node,"value");
      if (id=="data/local") {
        auto &locals=frames_.empty() ? result_.variables : frames_.back().locals;
        collectionSize(locals.size()+(locals.count(name) ? 0 : 1));
        locals[name]=std::move(assigned);
      } else {
        if (id=="data/change") assigned=Scalar(Number(variable(name,false))+Number(assigned));
        variable(name,true)=std::move(assigned);
      }
      return Flow::Next;
    }
    if (id=="math/seed") {
      random_.seed(static_cast<std::uint64_t>(Integer(number(node,"value"),0,kMaximumInteger)));
      return Flow::Next;
    }
    if (id=="simulation/restart") { restart(); return Flow::Next; }
    if (id=="simulation/restore") { restore(As<Snapshot>(value(node,"snapshot"),"a snapshot")); return Flow::Next; }
    if (id=="simulation/set-horizon") {
        const auto horizon=time(number(node,"time"),true);
      if (horizon<current_.timeMs) throw std::runtime_error("Horizon precedes the current simulation state.");
      current_=host_.setHorizon(horizon);
      horizon_=horizon;
      return Flow::Next;
    }
    if (id=="simulation/step") {
      tick();
      return Flow::Next;
    }
    if (id=="simulation/set-input") { useInputs(editedInputs(node,*inputs_)); return Flow::Next; }
    if (id=="simulation/replace-inputs") { useInputs(As<VisualInputs>(value(node,"inputs"),"an input sequence")); return Flow::Next; }
    if (id=="results/publish") { const auto chosen=number(node,"score"); publish(chosen,current_); return Flow::Next; }
    if (id=="results/publish-snapshot") {
      const auto snapshot=As<Snapshot>(value(node,"snapshot"),"a snapshot");
      if (!snapshot) throw std::runtime_error("Invalid result snapshot.");
      const auto chosen=number(node,"score");
      publish(chosen,snapshot->state,snapshot);
      return Flow::Next;
    }
    if (id=="results/clear") { result_.published.reset(); return Flow::Next; }
    if (id=="results/count") { count(); return Flow::Next; }
    if (id=="results/add-count") {
      const auto amount=static_cast<std::uint64_t>(Integer(number(node,"amount"),0));
      if (amount>std::numeric_limits<std::uint64_t>::max()-result_.candidates)
        throw std::runtime_error("The candidate counter would overflow.");
      if (control_.candidateLimit && amount>*control_.candidateLimit-std::min(result_.candidates,*control_.candidateLimit)) throw Stop{};
      result_.candidates+=amount;
      if (control_.candidateCountChanged) control_.candidateCountChanged(result_.candidates);
      return Flow::Next;
    }
    throw std::runtime_error("This block is not an executable command.");
  });
}

} // namespace

VisualExecutionResult ExecuteVisualProgram(const VisualProgram &program, VisualSimulationHost &host,
                                           const VisualRuntimeControl &control) {
  const auto validation=ValidateExecutableVisualProgram(program);
  if (!validation.ok) throw std::invalid_argument(validation.errors.front());
  return Runtime(program,host,control).run();
}
} // namespace forevertas::blocks

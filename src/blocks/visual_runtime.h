#ifndef FOREVERTAS_BLOCKS_VISUAL_RUNTIME_H
#define FOREVERTAS_BLOCKS_VISUAL_RUNTIME_H

#include "blocks/visual_program.h"
#include "mutations/input_event_utils.h"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace forevertas::blocks {

using VisualState = forevervalidator::experimental::PhysicsSandboxStateView;
using VisualInputs = std::vector<SandboxInputEvent>;

struct VisualVector { double x = 0, y = 0, z = 0; };
struct VisualProcedure { std::string name; };
struct VisualRotation { double x = 0, y = 0, z = 0, w = 1; };
struct VisualRange { double minimum = 0, maximum = 0; };
using VisualPolygon = std::vector<std::pair<double, double>>;
struct VisualVolume {
  VisualVector origin, size;
  VisualPolygon polygon;
  std::string plane;
  double depth = 0;
};
struct VisualSnapshot;

// The interpreter owns program flow, data, selection, and input generation.
// Physics operations are supplied by the host. Higher-level behavior is
// expressed by ordinary block sequences, including the bundled macroblocks.
struct VisualHostSnapshot { virtual ~VisualHostSnapshot() = default; };
class VisualSimulationHost {
public:
  virtual ~VisualSimulationHost() = default;
  virtual VisualState read() const = 0;
  virtual VisualState advance() = 0;
  virtual std::shared_ptr<const VisualHostSnapshot> capture() const = 0;
  virtual VisualState restore(const VisualHostSnapshot &snapshot) = 0;
  virtual VisualInputs inputs() const = 0;
  virtual void replaceInputs(VisualInputs inputs) = 0;
  virtual VisualState setHorizon(std::uint32_t milliseconds) = 0;
  virtual std::unique_ptr<VisualSimulationHost> fork() const {
    throw std::runtime_error("This simulation host cannot create an independent branch.");
  }
};

struct VisualSnapshot {
  std::shared_ptr<const VisualHostSnapshot> native;
  VisualState state, previous;
  VisualInputs inputs;
  std::shared_ptr<const std::vector<VisualState>> history;
  std::uint32_t horizonMs = 0;
  std::uint64_t candidate = 0;
};

struct VisualValue;
using VisualList = std::vector<VisualValue>;
struct VisualValue {
  using Storage = std::variant<std::monostate, double, bool, std::string,
      VisualVector, VisualRotation, VisualRange, VisualPolygon, VisualVolume,
      VisualState, VisualInputs, std::shared_ptr<const VisualList>,
      std::shared_ptr<const VisualSnapshot>, VisualProcedure>;
  Storage data;
  std::size_t collectionDepth = 0;
  VisualValue() = default;
  explicit VisualValue(std::shared_ptr<const VisualList> values);
  template<class T> explicit VisualValue(T value) : data(std::move(value)) {}
};

inline VisualValue::VisualValue(std::shared_ptr<const VisualList> values) : data(values) {
  if (!values) throw std::runtime_error("Invalid list.");
  for (const auto &item : *values)
    if (item.collectionDepth >= collectionDepth) collectionDepth = item.collectionDepth + 1;
  if (collectionDepth > 64) throw std::runtime_error("Lists may be nested at most 64 levels.");
}

struct VisualPublishedRun {
  double score = 0;
  VisualState evaluationState;
  std::shared_ptr<const VisualSnapshot> snapshot;
  std::uint64_t candidate = 0;
};

struct VisualCallFrame {
  std::string name;
  std::map<std::string, VisualValue> locals;
  VisualValue returned;
};

class VisualDebugger;

struct VisualRuntimeControl {
  std::function<bool()> stopRequested;
  std::function<void(const VisualPublishedRun &)> published;
  std::function<void(std::uint64_t)> candidateCountChanged;
  std::optional<std::uint64_t> candidateLimit;
  // Zero is unlimited; cancellation is checked at every command, expression,
  // loop back-edge, and physics tick, including empty infinite loops.
  std::uint64_t operationLimit = 0;
  std::size_t collectionLimit = 1000000;
  std::uint32_t horizonMs = 6000;
  std::uint32_t tickMs = 10;
  std::uint32_t workerCount = 1;
  std::shared_ptr<VisualDebugger> debugger;
};

struct VisualExecutionResult {
  std::map<std::string, VisualValue> variables;
  std::optional<VisualPublishedRun> published;
  std::shared_ptr<const VisualSnapshot> finalSnapshot;
  std::uint64_t candidates = 0, operations = 0;
  bool stopped = false;
};

const std::vector<std::pair<std::string, std::string>> &VisualStateProperties();
VisualValue ReadVisualStateProperty(const VisualState &state, const std::string &property);
VisualProgramValidation ValidateExecutableVisualProgram(const VisualProgram &program);
VisualExecutionResult ExecuteVisualProgram(const VisualProgram &program,
    VisualSimulationHost &host, const VisualRuntimeControl &control = {});

} // namespace forevertas::blocks
#endif

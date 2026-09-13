#ifndef FOREVERTAS_BLOCKS_PROGRAM_VALUE_CODEC_H
#define FOREVERTAS_BLOCKS_PROGRAM_VALUE_CODEC_H

#include "blocks/program_bytecode.h"
#include "blocks/visual_runtime.h"

#include <unordered_map>

namespace forevertas::blocks {
struct BytecodeNeedsInterpreter {};
std::optional<std::vector<bool>> ResolveDynamicInitialGlobals(const ProgramBytecode &program,
    const std::map<std::string,VisualValue> &globals,const VisualList &arguments);
class ProgramValueCodec {
public:
  ProgramValueCodec(const ProgramBytecode &program, vm::Memory &memory);
  vm::Value encode(const VisualValue &value);
  VisualValue decode(vm::Value value) const;
  std::shared_ptr<const VisualSnapshot> snapshot(vm::Value value) const;
  vm::Value encodeSnapshot(std::shared_ptr<const VisualSnapshot> snapshot, vm::Value inputs);
  const std::vector<std::string> &strings() const { return strings_; }
  void setStrings(std::vector<std::string> strings) { strings_=std::move(strings); }
  bool hasNativeObjects() const { return !snapshots_.empty(); }
  void setSnapshotEncoder(std::function<vm::Value(const std::shared_ptr<const VisualSnapshot> &,vm::Value)> encode) {
    snapshotEncoder_=std::move(encode);
  }
  void setSnapshotDecoder(std::function<std::shared_ptr<const VisualSnapshot>(vm::Value)> decode) {
    snapshotDecoder_=std::move(decode);
  }

private:
  const ProgramBytecode &program_;
  vm::Memory &memory_;
  std::vector<std::string> strings_;
  mutable std::unordered_map<std::uint32_t,std::shared_ptr<const VisualSnapshot>> snapshots_;
  std::unordered_map<const VisualSnapshot *,std::uint32_t> snapshotHandles_;
  std::unordered_map<std::shared_ptr<const VisualList>,vm::Value> encodedLists_;
  mutable std::unordered_map<std::uint32_t,std::shared_ptr<const VisualList>> decodedLists_;
  unsigned encodeDepth_=0;
  mutable unsigned decodeDepth_=0;
  std::function<std::shared_ptr<const VisualSnapshot>(vm::Value)> snapshotDecoder_;
  std::function<vm::Value(const std::shared_ptr<const VisualSnapshot> &,vm::Value)> snapshotEncoder_;
  std::uint32_t text(const std::string &value);
};

struct BytecodeExecution {
  VisualValue value;
  std::uint64_t operations = 0;
  bool stopped = false;
  bool needsInterpreter = false;
};

struct HostBytecodeStorage {
  std::unique_ptr<unsigned char[]> arena;
  std::vector<vm::Value> stack;
  std::uint32_t capacity = 4u*1024u*1024u;
  void prepare();
};

struct BytecodeBatchResult {
  std::vector<VisualValue> values;
  std::uint64_t operations=0;
  bool stopped=false;
  // Values at these indices are uncommitted; all other lanes remain valid.
  std::vector<std::size_t> fallbackLanes{};
};
class VisualBatchExecutor {
public:
  virtual ~VisualBatchExecutor() = default;
  virtual std::optional<BytecodeBatchResult> execute(const ProgramBytecode &program,
      const std::shared_ptr<const VisualSnapshot> &baseline,
      const std::map<std::string,VisualValue> &globals, const VisualList &arguments,
      const std::vector<std::uint64_t> &seeds, const VisualRuntimeControl &control) = 0;
};
BytecodeExecution ExecuteHostBytecode(const ProgramBytecode &program, VisualSimulationHost &host,
    const std::shared_ptr<const VisualSnapshot> &baseline,
    const std::map<std::string,VisualValue> &globals, const VisualValue &argument,
    std::uint64_t seed, const VisualRuntimeControl &control, HostBytecodeStorage &storage);
std::optional<BytecodeBatchResult> TryUniformProgram(const ProgramBytecode &program,
    const std::shared_ptr<const VisualSnapshot> &baseline,
    const std::map<std::string,VisualValue> &globals,const VisualList &arguments,
    const VisualRuntimeControl &control,HostBytecodeStorage &storage);

} // namespace forevertas::blocks
#endif

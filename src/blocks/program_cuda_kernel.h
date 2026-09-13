#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_KERNEL_H
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_KERNEL_H

#include "blocks/program_bytecode.h"
#include "validation/api/physics_sandbox_cuda_execution_access.h"

#include <functional>
#include <memory>

namespace forevertas::blocks {

vm::Result EvaluateCudaProgramHostMath(vm::Op op,vm::Value *arguments);

constexpr std::uint32_t CudaProgramStatePropertyCount=50;
struct CudaProgramCheckpoint {
  forevervalidator::simulation::CudaCandidateState state;
  vm::Value current[CudaProgramStatePropertyCount],previous[CudaProgramStatePropertyCount];
  std::uint32_t tick=0,inputOffset=0,inputCount=0,extensionGeneration=0;
};

struct CudaProgramRestoreContext {
  bool reuseBaseline=false;
  std::shared_ptr<const forevervalidator::experimental::PhysicsSandboxCudaExecutionContext> context;
  std::shared_ptr<const std::vector<CudaProgramCheckpoint>> checkpoints;
  std::shared_ptr<const std::vector<vm::Value>> checkpointInputs;
};

struct CudaProgramInput {
  const ProgramBytecode *program=nullptr;
  const forevervalidator::experimental::PhysicsSandboxCudaExecutionContext *context=nullptr;
  std::vector<unsigned char> arena;
  std::shared_ptr<const std::vector<unsigned char>> sharedArena;
  vm::Value globals, inputs, initialView, previousView, histories, restartSnapshot;
  std::vector<std::uint64_t> historySizes;
  std::vector<std::uint8_t> historyReachable;
  std::vector<std::uint8_t> restoreReachable;
  std::vector<vm::Value> arguments;
  std::vector<std::uint64_t> seeds;
  std::shared_ptr<const std::vector<CudaProgramCheckpoint>> checkpoints;
  std::shared_ptr<const std::vector<vm::Value>> checkpointInputs;
  std::uint32_t arenaCapacity=2u*1024u*1024u;
  std::uint32_t outputCapacity=128u*1024u;
  std::uint32_t outputCapacityLimit=2u*1024u*1024u;
  std::uint32_t tickMs=10;
  std::uint64_t collectionLimit=1000000;
  std::uint32_t batchSize=1;
  std::uint32_t importedSnapshotCount=0;
  std::function<std::shared_ptr<const CudaProgramRestoreContext>(std::uint32_t)> restoreContext;
  std::function<std::shared_ptr<const CudaProgramRestoreContext>(std::uint32_t,std::uint32_t)> horizonContext;
};

struct CudaProgramOutput {
  vm::Result result;
  std::vector<unsigned char> arena;
  bool outputCapacityExceeded=false;
  // Capacity learned by serialization-only retries.
  std::uint32_t outputCapacity=0;
};

struct CudaPhysicsCursorState {
  vm::Value current[CudaProgramStatePropertyCount],previous[CudaProgramStatePropertyCount];
};
constexpr std::uint32_t CudaPhysicsCursorMaximumLookahead=32;
struct CudaPhysicsCursorBatch {
  std::vector<CudaPhysicsCursorState> states;
  std::vector<unsigned char> arena;
  vm::Value native;
  vm::Error error=vm::Error::None;
};

class CudaProgramKernel {
public:
  CudaProgramKernel();
  ~CudaProgramKernel();
  CudaProgramKernel(const CudaProgramKernel &)=delete;
  CudaProgramKernel &operator=(const CudaProgramKernel &)=delete;
  std::size_t suggestedWaveSize(std::uint32_t arenaCapacity,std::uint32_t outputCapacity) const;
  std::vector<CudaProgramOutput> execute(const CudaProgramInput &input,
      const std::function<bool()> &cancelled);
  void startPhysicsCursor(const CudaProgramInput &input);
  void updatePhysicsCursor(const CudaProgramInput &input,bool preserveState);
  CudaPhysicsCursorBatch predictPhysicsCursor(std::uint32_t ticks);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
#endif

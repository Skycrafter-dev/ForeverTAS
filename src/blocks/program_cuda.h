#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_H
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_H

#include "blocks/program_value_codec.h"

namespace forevertas::blocks {
struct CudaProgramInput;
class CudaProgramPhysicsCursor;
class CudaProgramPrefixCache {
public:
  CudaProgramPrefixCache();
  ~CudaProgramPrefixCache();
  void start(const VisualPhysicsSnapshot &origin,std::uint32_t horizon,bool replace=false);
  void invalidate();
  bool needsNativeState(const VisualState &state) const;
  void observe(forevervalidator::experimental::PhysicsSandbox &sandbox,const VisualState &state,
      const VisualState *previous=nullptr);
  void observe(const CudaProgramPhysicsCursor &cursor);
  void append(CudaProgramInput &input,ProgramValueCodec &codec,vm::Memory &memory,const VisualSnapshot &origin) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class CudaProgramPhysicsCursor {
public:
  explicit CudaProgramPhysicsCursor(forevervalidator::experimental::PhysicsSandbox &sandbox);
  ~CudaProgramPhysicsCursor();
  void reset(forevervalidator::experimental::PhysicsSandbox &sandbox);
  void replaceInputs(forevervalidator::experimental::PhysicsSandbox &sandbox);
  const VisualState &read() const;
  std::optional<VisualAdvance> advance(std::uint32_t ticks);
  forevervalidator::experimental::PhysicsSandboxState capture() const;
private:
  friend class CudaProgramPrefixCache;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
using ProgramHistoryReplay=std::function<std::vector<VisualState>(const VisualSnapshot &,const VisualInputs &,std::uint32_t)>;
std::shared_ptr<VisualBatchExecutor> CreateCudaProgramExecutor(
    forevervalidator::experimental::PhysicsSandbox &sandbox,
    ProgramHistoryReplay replay,std::shared_ptr<CudaProgramPrefixCache> prefixes={});
}
#endif

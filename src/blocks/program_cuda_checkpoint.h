#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_CHECKPOINT_H
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_CHECKPOINT_H

#include "blocks/program_cuda_kernel.h"

#include <algorithm>
#include <stdexcept>

#ifdef __CUDACC__
#define FT_CHECKPOINT_HD __host__ __device__
#else
#define FT_CHECKPOINT_HD
#endif

namespace forevertas::blocks::cuda_program_detail {

FT_CHECKPOINT_HD inline bool SameCheckpointInput(const vm::Value &a,const vm::Value &b) {
  return a.kind==b.kind && a.handle==b.handle && a.x==b.x && a.y==b.y && a.z==b.z && a.w==b.w;
}

struct CheckpointInputValidation {
  std::uint32_t generation=0,matched=0;
  bool mismatch=false;

  // Actual inputs are immutable between calls. Reset after replacing them.
  FT_CHECKPOINT_HD bool matches(std::uint32_t next,const vm::Value *actual,const vm::Value *expected,
      std::uint32_t count) {
    if (generation!=next) { generation=next; matched=0; mismatch=false; }
    if (mismatch && count>matched) return false;
    for (;matched<count;++matched) {
      if (!SameCheckpointInput(actual[matched],expected[matched])) { mismatch=true; return false; }
    }
    return true;
  }
};

struct PackedCheckpoint {
  forevervalidator::simulation::CudaCandidatePhysicsState state;
  decltype(forevervalidator::simulation::CudaCandidateState::collisionReplacementOverflow) overflow;
  vm::Value current[CudaProgramStatePropertyCount],previous[CudaProgramStatePropertyCount];
  std::uint32_t tick,inputOffset,inputCount,extensionGeneration,inputGeneration;
};

struct CheckpointStaging {
  std::vector<PackedCheckpoint> checkpoints;
  std::vector<forevervalidator::simulation::CudaCandidateState> extensions;

  void assign(const std::vector<CudaProgramCheckpoint> *source,const std::vector<vm::Value> *inputs) {
    checkpoints.clear(); extensions.clear();
    if (!source) return;
    if (!source->empty() && !inputs) throw std::invalid_argument("Missing CUDA checkpoint inputs.");
    checkpoints.reserve(source->size());
    const CudaProgramCheckpoint *previous=nullptr;
    std::uint32_t inputGeneration=0;
    for (const auto &checkpoint : *source) {
      if (checkpoint.inputOffset>inputs->size() || checkpoint.inputCount>inputs->size()-checkpoint.inputOffset)
        throw std::invalid_argument("Invalid CUDA checkpoint input range.");
      if (previous && checkpoint.tick<previous->tick)
        throw std::invalid_argument("Unsorted CUDA checkpoints.");
      // Generation zero is unshared. Nonzero generations come from the source
      // cache's conservative comparison of every native extension byte.
      if (!previous || !checkpoint.extensionGeneration || checkpoint.extensionGeneration!=previous->extensionGeneration)
        extensions.push_back(checkpoint.state);
      bool sameInputPrefix=previous && previous->inputCount<=checkpoint.inputCount;
      for (std::uint32_t i=0;previous && i<previous->inputCount && sameInputPrefix;++i)
        sameInputPrefix=SameCheckpointInput((*inputs)[previous->inputOffset+i],(*inputs)[checkpoint.inputOffset+i]);
      if (!sameInputPrefix) ++inputGeneration;
      PackedCheckpoint packed;
      packed.state=checkpoint.state; packed.overflow=checkpoint.state.collisionReplacementOverflow;
      std::copy(std::begin(checkpoint.current),std::end(checkpoint.current),std::begin(packed.current));
      std::copy(std::begin(checkpoint.previous),std::end(checkpoint.previous),std::begin(packed.previous));
      packed.tick=checkpoint.tick; packed.inputOffset=checkpoint.inputOffset; packed.inputCount=checkpoint.inputCount;
      packed.extensionGeneration=static_cast<std::uint32_t>(extensions.size());
      packed.inputGeneration=inputGeneration;
      checkpoints.push_back(std::move(packed));
      previous=&checkpoint;
    }
  }
};

} // namespace forevertas::blocks::cuda_program_detail

#undef FT_CHECKPOINT_HD
#endif

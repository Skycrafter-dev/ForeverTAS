#include "blocks/program_cuda_checkpoint.h"

#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
using namespace forevertas::blocks;
using namespace forevertas::blocks::cuda_program_detail;
using namespace forevervalidator::simulation;

void Check(bool value,const char *message) {
  if (!value) throw std::runtime_error(message);
}

void Packing() {
  std::vector<CudaProgramCheckpoint> source(8);
  std::vector<vm::Value> inputs;
  const std::array<std::vector<unsigned>,8> sequences{{{0},{0,1},{0,1},{0,2},{0,2,3},{0},{0,1},{0,1}}};
  const std::array<unsigned,8> generations{1,1,1,2,2,0,0,2};
  const std::array<unsigned,8> inputGenerations{1,1,1,2,2,3,3,3};
  for (std::size_t i=0;i<source.size();++i) {
    auto &checkpoint=source[i];
    checkpoint.extensionGeneration=generations[i]; checkpoint.tick=static_cast<std::uint32_t>(i+1);
    checkpoint.state.candidateId=static_cast<std::uint32_t>(100+i);
    checkpoint.state.controlCursor=i*17;
    checkpoint.state.stunts.stuntsScore=i<3 ? 73 : (i<5 || i==7 ? 99 : static_cast<unsigned>(101+i));
    auto &overflow=checkpoint.state.collisionReplacementOverflow;
    overflow.count=i%3==0 ? CudaCollisionReplacementOverflowCapacity : static_cast<unsigned>(i%3-1);
    for (std::uint32_t j=0;j<overflow.count;++j)
      overflow.values[j]={static_cast<float>(i*1000+j),static_cast<float>(j+1),-static_cast<float>(j+2)};
    for (std::uint32_t j=0;j<CudaProgramStatePropertyCount;++j) {
      checkpoint.current[j]=vm::number(i*100+j);
      checkpoint.previous[j]=vm::number(i*100+j+0.5);
    }
    checkpoint.inputOffset=static_cast<std::uint32_t>(inputs.size());
    for (auto event : sequences[i]) inputs.push_back({vm::Kind::Event,0,static_cast<double>(event*10),0,1,static_cast<double>(event)});
    checkpoint.inputCount=static_cast<std::uint32_t>(sequences[i].size());
  }
  CheckpointStaging staging;
  staging.assign(&source,&inputs);
  Check(staging.checkpoints.size()==source.size() && staging.extensions.size()==5,"Checkpoint generations did not share conservatively.");
  for (std::size_t i=0;i<source.size();++i) {
    const auto &packed=staging.checkpoints[i];
    auto restored=staging.extensions[packed.extensionGeneration-1];
    static_cast<CudaCandidatePhysicsState &>(restored)=packed.state;
    restored.collisionReplacementOverflow=packed.overflow;
    Check(std::memcmp(&restored,&source[i].state,sizeof(restored))==0,"Checkpoint packing changed a native byte.");
    Check(std::memcmp(packed.current,source[i].current,sizeof(packed.current))==0 &&
        std::memcmp(packed.previous,source[i].previous,sizeof(packed.previous))==0,"Checkpoint packing changed public observations.");
    Check(packed.tick==source[i].tick && packed.inputOffset==source[i].inputOffset &&
        packed.inputCount==source[i].inputCount && packed.inputGeneration==inputGenerations[i],"Checkpoint input lineage changed.");
  }
  source[0].state.stunts.stuntsScore=999;
  source.resize(1);
  staging.assign(&source,&inputs);
  Check(staging.extensions.size()==1 && staging.extensions.front().stunts.stuntsScore==999,"Checkpoint staging retained an old generation.");
  const auto rejects=[&] {
    try { staging.assign(&source,&inputs); } catch (const std::invalid_argument &) { return true; }
    return false;
  };
  source[0].inputOffset=static_cast<std::uint32_t>(inputs.size()+1);
  Check(rejects(),"Checkpoint staging accepted an out-of-range offset.");
  source[0].inputOffset=0; source[0].inputCount=static_cast<std::uint32_t>(inputs.size()+1);
  Check(rejects(),"Checkpoint staging accepted an out-of-range input count.");
  source[0].inputCount=1; source.push_back(source.front()); source.back().tick=0;
  Check(rejects(),"Checkpoint staging accepted unsorted ticks.");
  staging.assign(nullptr,nullptr);
  Check(staging.checkpoints.empty() && staging.extensions.empty(),"Empty checkpoint staging retained data.");
}

void Validation() {
  std::array<vm::Value,4> actual{},expected{};
  for (unsigned i=0;i<actual.size();++i) actual[i]=expected[i]=vm::number(i);
  CheckpointInputValidation validation;
  Check(validation.matches(1,actual.data(),expected.data(),2) && validation.matched==2,"Input validation did not retain its proven prefix.");
  Check(validation.matches(1,actual.data(),expected.data(),4) && validation.matched==4,"Input validation did not extend its prefix.");
  Check(validation.matches(1,actual.data(),expected.data(),1),"A shorter checkpoint lost a proven prefix.");
  expected[2]=vm::number(99);
  Check(!validation.matches(2,actual.data(),expected.data(),4) && validation.matched==2 && validation.mismatch,
      "Input validation reused proof across different lineages.");
  Check(validation.matches(2,actual.data(),expected.data(),2),"A later mismatch rejected an earlier valid checkpoint.");
  Check(!validation.matches(2,actual.data(),expected.data(),3),"Input validation forgot a proven mismatch.");
  actual[2]=expected[2]; validation={};
  Check(validation.matches(2,actual.data(),expected.data(),4),"Replacing inputs did not invalidate a cached mismatch.");
  actual[0]=vm::number(-1); validation={};
  Check(!validation.matches(2,actual.data(),expected.data(),1),"Replacing inputs retained stale matching proof.");
  Check(validation.matches(3,nullptr,nullptr,0),"An empty input prefix was rejected.");
}

void ValidationParity() {
  std::mt19937 random(17091);
  std::vector<CudaProgramCheckpoint> source(64);
  std::vector<vm::Value> inputs,history;
  for (std::size_t i=0;i<source.size();++i) {
    if (!history.empty() && random()%5==0) history[random()%history.size()].y+=1;
    else if (!history.empty() && random()%3==0) history.pop_back();
    else if (history.size()<12) history.push_back({vm::Kind::Event,0,static_cast<double>(history.size()*10),
        static_cast<double>(random()%20),1,1});
    auto &checkpoint=source[i];
    checkpoint.state=source.front().state; checkpoint.extensionGeneration=1;
    checkpoint.tick=static_cast<std::uint32_t>(i+1);
    checkpoint.inputOffset=static_cast<std::uint32_t>(inputs.size());
    checkpoint.inputCount=static_cast<std::uint32_t>(history.size());
    inputs.insert(inputs.end(),history.begin(),history.end());
  }
  CheckpointStaging staging;
  staging.assign(&source,&inputs);
  CheckpointInputValidation validation;
  std::vector<vm::Value> actual;
  for (unsigned query=0;query<4096;++query) {
    const auto &checkpoint=staging.checkpoints[random()%staging.checkpoints.size()];
    const auto first=inputs.begin()+checkpoint.inputOffset;
    if (query%17==0) {
      actual.assign(first,first+checkpoint.inputCount);
      if (!actual.empty() && random()%2) actual[random()%actual.size()].y+=7;
      validation={};
    }
    const bool expected=checkpoint.inputCount<=actual.size() &&
        std::equal(first,first+checkpoint.inputCount,actual.begin(),SameCheckpointInput);
    const bool observed=checkpoint.inputCount<=actual.size() && validation.matches(checkpoint.inputGeneration,
        actual.data(),inputs.data()+checkpoint.inputOffset,checkpoint.inputCount);
    Check(expected==observed,"Incremental input validation differed from a complete comparison.");
  }
}
}

int main() {
  try {
    Packing(); Validation(); ValidationParity();
    std::cout << "PASS exact checkpoint packing, overflow retirement, generations and incremental input proof\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "blocks/program_vm.h"

#include <cuda_runtime.h>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace forevertas::blocks::vm;
struct Input { std::uint32_t length, itemBytes, capacity; };
struct Output { Error error; std::uint32_t handle, size, used, recycled, reused, remaining; };

__global__ void CheckAllocator(const Input *inputs,Output *outputs,std::uint32_t count) {
  const auto i=blockIdx.x*blockDim.x+threadIdx.x;
  if (i>=count) return;
  // Opaque allocation/release touches only the header. A small physical buffer
  // lets us test the full logical capacity range without reserving gigabytes.
  alignas(Value) unsigned char storage[64];
  Memory memory; memory.bytes=storage; memory.capacity=inputs[i].capacity;
  const auto value=memory.allocate(Kind::Opaque,inputs[i].length,inputs[i].itemBytes);
  Output result{memory.error,value.handle,0,memory.used,0,0,0};
  if (memory.error==Error::None) {
    result.size=memory.header(value.handle).bytes;
    std::uint32_t bucket=5,size=32;
    while (size<result.size) { size*=2; ++bucket; }
    memory.retain(value);
    memory.release(value);
    if (memory.free[bucket] || memory.header(value.handle).references!=1) result.size=0;
    memory.release(value);
    result.recycled=memory.free[bucket];
    const auto reused=memory.allocate(Kind::Opaque,inputs[i].length,inputs[i].itemBytes);
    result.error=memory.error; result.reused=reused.handle;
    result.remaining=memory.free[bucket];
    if (memory.used!=result.used) result.used=0;
  }
  outputs[i]=result;
}

void Check(cudaError_t result) {
  if (result!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
struct Buffer {
  void *data=nullptr;
  explicit Buffer(std::size_t bytes) { Check(cudaMalloc(&data,bytes)); }
  ~Buffer() { if (data) cudaFree(data); }
};

__global__ void CheckOverwrite(std::uint32_t *results) {
  const std::uint32_t lengths[]={0,1,4,50};
  const auto length=lengths[threadIdx.x];
  alignas(Value) unsigned char storage[4096];
  for (auto &byte : storage) byte=0xa5;
  Memory memory; memory.bytes=storage; memory.capacity=sizeof(storage);
  const auto value=memory.allocateForOverwrite(Kind::State,length);
  bool valid=memory.error==Error::None && memory.length(value)==length;
  for (std::uint32_t i=0;i<length*sizeof(Value);++i)
    valid=valid && storage[value.handle+i]==0xa5;
  for (std::uint32_t i=0;i<length;++i) memory.items(value)[i]=number(i);
  memory.release(value);
  const auto initialized=memory.allocate(Kind::List,length);
  valid=valid && initialized.handle==value.handle && memory.error==Error::None;
  for (std::uint32_t i=0;i<length;++i) {
    const auto item=memory.items(initialized)[i];
    valid=valid && item.kind==Kind::None && !item.handle && item.x==0 && item.y==0 && item.z==0 && item.w==0;
  }
  memory.release(initialized);
  const auto reused=memory.allocateForOverwrite(Kind::State,length);
  valid=valid && reused.handle==value.handle && memory.error==Error::None;
  for (std::uint32_t i=0;i<length;++i) memory.items(reused)[i]=boolean(i%2);
  memory.release(reused);
  Memory exhausted; exhausted.bytes=storage; exhausted.capacity=32;
  const auto failed=exhausted.allocateForOverwrite(Kind::State,50);
  valid=valid && !failed.handle && exhausted.error==Error::Capacity && exhausted.used==32;
  results[threadIdx.x]=valid;
}
}

int main() try {
  int devices=0;
  const auto status=cudaGetDeviceCount(&devices);
  if (status==cudaErrorNoDevice || status==cudaErrorInsufficientDriver || (status==cudaSuccess && devices==0)) {
    std::cout << "SKIP CUDA allocator: no usable CUDA device\n";
    return 77;
  }
  Check(status);
  std::vector<Input> cases;
  for (unsigned bit=5;bit<=30;++bit) {
    const auto size=std::uint32_t{1}<<bit;
    for (int delta : {-1,0,1}) {
      const auto length=size-static_cast<std::uint32_t>(sizeof(Header))+delta;
      for (const auto capacity : {size+31,size+32,std::numeric_limits<std::uint32_t>::max()})
        cases.push_back({length,1,capacity});
    }
  }
  for (const auto itemBytes : {0u,1u,40u,std::numeric_limits<std::uint32_t>::max()})
    for (const auto length : {0u,1u,26u,27u,std::numeric_limits<std::uint32_t>::max()})
      cases.push_back({length,itemBytes,std::numeric_limits<std::uint32_t>::max()});
  for (auto capacity : {0u,15u,16u,31u,32u,63u,64u}) cases.push_back({0,1,capacity});
  Buffer deviceInputs(cases.size()*sizeof(Input)),deviceOutputs(cases.size()*sizeof(Output));
  Check(cudaMemcpy(deviceInputs.data,cases.data(),cases.size()*sizeof(Input),cudaMemcpyHostToDevice));
  CheckAllocator<<<(cases.size()+31)/32,32>>>(static_cast<const Input *>(deviceInputs.data),
      static_cast<Output *>(deviceOutputs.data),static_cast<std::uint32_t>(cases.size()));
  Check(cudaGetLastError());
  std::vector<Output> outputs(cases.size());
  Check(cudaMemcpy(outputs.data(),deviceOutputs.data,outputs.size()*sizeof(Output),cudaMemcpyDeviceToHost));
  for (std::size_t i=0;i<cases.size();++i) {
    const auto &input=cases[i]; const auto &actual=outputs[i];
    const std::uint64_t required=sizeof(Header)+static_cast<std::uint64_t>(input.length)*input.itemBytes;
    std::uint32_t size=32,bucket=5;
    while (size<required && bucket<30) { size*=2; ++bucket; }
    const bool failed=size<required || size>input.capacity || 32u>input.capacity-size;
    const bool valid=failed
        ? actual.error==Error::Capacity && actual.handle==0 && actual.used==32
        : actual.error==Error::None && actual.handle==48 && actual.size==size && actual.used==32+size &&
          actual.recycled==32 && actual.reused==48 && actual.remaining==0;
    if (!valid) throw std::runtime_error("CUDA allocation/reuse mismatch at case "+std::to_string(i));
  }
  std::cout << "PASS CUDA allocator capacity, overflow, size-class boundaries and reuse: " << cases.size() << " cases\n";
  CheckOverwrite<<<1,4>>>(static_cast<std::uint32_t *>(deviceOutputs.data));
  Check(cudaGetLastError());
  std::uint32_t overwriteResults[4];
  Check(cudaMemcpy(overwriteResults,deviceOutputs.data,sizeof(overwriteResults),cudaMemcpyDeviceToHost));
  for (const auto valid : overwriteResults)
    if (!valid) throw std::runtime_error("CUDA overwrite allocation initialization/reuse mismatch");
  std::cout << "PASS CUDA overwrite allocation, ordinary initialization and capacity failure\n";
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}

#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_IMPORT_H
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_IMPORT_H

#include "blocks/program_vm.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace forevertas::blocks::cuda_program_detail {

// Only immutable payloads are shared. Every object keeps a private header, so
// reference counts, free lists and compaction forwarding links remain lane-local.
class SharedImportBuilder {
public:
  explicit SharedImportBuilder(const std::vector<unsigned char> &initial)
      : local(initial.size()) {
    source.bytes=const_cast<unsigned char *>(initial.data());
    source.capacity=source.used=static_cast<std::uint32_t>(initial.size());
    target.bytes=local.data(); target.capacity=static_cast<std::uint32_t>(local.size());
  }

  vm::Value copy(vm::Value value,bool writable=false) {
    if (!vm::reference(value)) return value;
    if (const auto found=handles.find(value.handle);found!=handles.end()) {
      value.handle=found->second; return value;
    }
    const auto &header=source.header(value.handle);
    const bool shared=!writable && value.kind!=vm::Kind::Opaque && value.kind!=vm::Kind::DeferredState &&
        static_cast<std::uint64_t>(header.length)*sizeof(vm::Value)>=4096;
    auto result=shared ? target.allocate(vm::Kind::Opaque,sizeof(std::uint32_t),1) :
        target.allocate(value.kind,header.length,value.kind==vm::Kind::Opaque ? 1 : sizeof(vm::Value));
    if (target.error!=vm::Error::None) throw std::runtime_error("Packing CUDA initial values exceeded their arena.");
    auto &next=target.header(result.handle);
    next.references=header.references;
    if (shared) {
      const auto offset=static_cast<std::uint32_t>(payloads.size());
      payloads.resize(payloads.size()+static_cast<std::size_t>(header.length)*sizeof(vm::Value));
      std::memcpy(target.bytes+result.handle,&offset,sizeof(offset));
      next.length=header.length;
      result.handle|=vm::Memory::sharedHandle;
    }
    handles.emplace(value.handle,result.handle);
    pending.push_back({value,result.handle});
    value.handle=result.handle;
    return value;
  }

  void finish() {
    for (std::size_t cursor=0;cursor<pending.size();++cursor) {
      const auto entry=pending[cursor];
      const auto length=source.length(entry.source);
      if (entry.source.kind==vm::Kind::Opaque) {
        std::memcpy(target.bytes+entry.destination,source.payload(entry.source.handle),length);
        continue;
      }
      std::uint32_t offset=0;
      if (entry.destination & vm::Memory::sharedHandle)
        std::memcpy(&offset,target.bytes+(entry.destination & ~vm::Memory::sharedHandle),sizeof(offset));
      for (std::uint32_t i=0;i<length;++i) {
        const auto value=copy(source.items(entry.source)[i]);
        // Copying a child can grow the shared buffer; reacquire its address.
        auto *items=entry.destination & vm::Memory::sharedHandle ?
            reinterpret_cast<vm::Value *>(payloads.data()+offset) :
            reinterpret_cast<vm::Value *>(target.bytes+entry.destination);
        items[i]=value;
      }
    }
    local.resize(target.used);
  }

  std::vector<unsigned char> local,payloads;

private:
  struct Entry { vm::Value source; std::uint32_t destination; };
  vm::Memory source,target;
  std::unordered_map<std::uint32_t,std::uint32_t> handles;
  std::vector<Entry> pending;
};
}
#endif

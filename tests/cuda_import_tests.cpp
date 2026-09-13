#include "blocks/program_cuda_import.h"

#include <iostream>

namespace {
using namespace forevertas::blocks;
using namespace vm;
void require(bool condition,const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Physics {
  bool cancelled() const { return false; }
  bool interpreterRequested() const { return false; }
};

void checkSharedImports() {
  std::vector<unsigned char> initial(8u*1024u*1024u);
  Memory source; source.bytes=initial.data(); source.capacity=static_cast<std::uint32_t>(initial.size());
  const auto globals=source.allocate(Kind::List,256);
  const auto large=source.allocate(Kind::List,70000);
  for (std::uint32_t i=0;i<70000;++i) source.items(large)[i]=number(i+1);
  source.items(globals)[0]=large;
  source.items(globals)[1]=source.copy(large);
  for (std::uint32_t root=2;root<4;++root) {
    const auto leaf=source.allocate(Kind::List,128);
    for (std::uint32_t i=0;i<128;++i) source.items(leaf)[i]=number(i);
    const auto parent=source.allocate(Kind::List,1024);
    for (std::uint32_t i=0;i<1024;++i) source.items(parent)[i]=source.copy(leaf);
    source.release(leaf);
    source.items(globals)[root]=parent;
  }
  const auto token=source.allocate(Kind::Opaque,4,1);
  const std::uint32_t tokenValue=123456;
  std::memcpy(source.bytes+token.handle,&tokenValue,sizeof(tokenValue));
  source.items(globals)[4]=token;
  const auto snapshot=source.allocate(Kind::Snapshot,2);
  source.items(snapshot)[0]=source.copy(token);
  source.items(snapshot)[1]=source.copy(large);
  source.items(globals)[5]=snapshot;
  source.items(globals)[6]=source.copy(snapshot);
  const Value variants[]={number(-0.0),boolean(true),{Kind::Text,0x80000017u},
      {Kind::Function,7},{Kind::Vector,0,1,2,3},{Kind::Rotation,0,4,5,6,7},
      {Kind::Range,0,-8,9},{Kind::Event,0,10,11,12,13},large,snapshot,token};
  const auto mixed=source.allocate(Kind::List,1024);
  for (std::uint32_t i=0;i<1024;++i) source.items(mixed)[i]=source.copy(variants[i%11]);
  source.items(globals)[7]=mixed;
  require(source.error==Error::None,"fixture allocation failed");
  initial.resize(source.used);

  cuda_program_detail::SharedImportBuilder packed(initial);
  const auto root=packed.copy(globals,true);
  packed.finish();
  require(packed.local.size()<32768,"large payloads were not removed from private storage");
  require(!(root.handle & Memory::sharedHandle),"mutable globals became shared");
  const auto immutable=packed.payloads;
  auto first=packed.local,second=packed.local;
  const auto used=static_cast<std::uint32_t>(first.size());
  first.resize(512u*1024u); second.resize(first.size());
  Memory lanes[2];
  for (unsigned lane=0;lane<2;++lane) {
    auto &memory=lanes[lane];
    memory.bytes=(lane ? second : first).data();
    memory.capacity=static_cast<std::uint32_t>(first.size()); memory.used=used;
    memory.sharedBytes=packed.payloads.data();
    const auto value=memory.items(root)[0];
    require(value.handle & Memory::sharedHandle,"large list is not shared");
    require(value.handle==memory.items(root)[1].handle,"shared list aliases changed");
    require(memory.length(value)==70000 && memory.items(value)[0].x==1 && memory.items(value)[69999].x==70000,
        "shared list contents changed");
    std::uint32_t observed=0;
    std::memcpy(&observed,memory.payload(memory.items(root)[4].handle),sizeof(observed));
    require(observed==tokenValue,"opaque token changed");
    const auto importedMixed=memory.items(root)[7];
    require(importedMixed.handle & Memory::sharedHandle,"mixed payload is not shared");
    for (std::uint32_t i=0;i<1024;++i) {
      const auto original=variants[i%11],actual=memory.items(importedMixed)[i];
      require(actual.kind==original.kind,"mixed payload type changed");
      if (!reference(original)) require(std::memcmp(&original,&actual,sizeof(Value))==0,"primitive bits changed");
      else {
        const unsigned slot=i%11==8 ? 0 : i%11==9 ? 5 : 4;
        require(actual.handle==memory.items(root)[slot].handle,"mixed payload alias changed");
      }
    }
    Physics physics;
    Machine<Physics> machine({},physics); machine.memory=memory;
    require(machine.equal(memory.items(root)[2],memory.items(root)[3]),"nested shared equality failed");
    require(machine.equal(memory.items(root)[5],memory.items(root)[6]),"snapshot identity changed");
    require(machine.error==Error::None,"shared equality raised an error");
  }
  auto &memory=lanes[0];
  const auto original=memory.items(root)[0];
  const auto references=lanes[1].header(original.handle).references;
  memory.assign(memory.items(root)[0],memory.allocate(Kind::List,0));
  require(memory.header(original.handle).references+1==references,"private reference count did not change");
  require(lanes[1].header(original.handle).references==references,"reference count leaked across lanes");
  const auto parent=memory.items(root)[2];
  const auto changed=memory.duplicate(parent,memory.length(parent));
  memory.assign(memory.items(changed)[0],number(-1));
  Physics physics;
  Machine<Physics> machine({},physics); machine.memory=memory;
  require(!machine.equal(parent,changed),"shared/local nested inequality failed");
  memory.release(changed);
  memory.release(root); lanes[1].release(root);
  require(memory.error==Error::None && lanes[1].error==Error::None,"shared release failed");
  require(packed.payloads==immutable,"shared payload was mutated");
  const auto reused=memory.allocate(Kind::List,0);
  require(reused.handle && memory.length(reused)==0,"shared header storage was not reusable");
  memory.release(reused);
}
}

int main() {
  try {
    checkSharedImports();
    std::cout << "PASS immutable shared payloads, mutable roots, nested equality, identity, lane-local ownership and reuse\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}

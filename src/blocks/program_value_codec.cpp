#include "blocks/program_value_codec.h"

#include <algorithm>
#include <type_traits>
#include <unordered_set>

namespace forevertas::blocks {
using namespace vm;

std::optional<std::vector<bool>> ResolveDynamicInitialGlobals(const ProgramBytecode &program,
    const std::map<std::string,VisualValue> &globals,const VisualList &arguments) {
  if (!program.usesDynamicCalls || program.procedureInitialGlobalReads.size()!=program.procedures.size() ||
      program.procedureNames.size()!=program.procedures.size() ||
      program.initialGlobalReads.size()!=program.symbols.size()) return {};
  std::unordered_map<std::string,std::uint32_t> names;
  for (std::size_t i=0;i<program.procedureNames.size();++i)
    if (!program.procedureNames[i].empty()) names.emplace(program.procedureNames[i],static_cast<std::uint32_t>(i));
  std::vector<bool> reads(program.symbols.size(),false), seen(program.procedures.size(),false);
  std::vector<std::uint32_t> pending;
  const auto enqueue=[&](std::uint32_t procedure) {
    if (procedure>=seen.size()) return false;
    if (!seen[procedure]) { seen[procedure]=true; pending.push_back(procedure); }
    return true;
  };
  if (!enqueue(program.entry)) return {};
  std::size_t budget=1000000;
  if (arguments.size()>budget) return {};
  std::vector<const VisualValue *> values;
  for (const auto &argument : arguments) values.push_back(&argument);
  std::unordered_set<const VisualList *> lists;
  while (!pending.empty() || !values.empty()) {
    // Procedure values originate in incoming values or Reference instructions;
    // calls and collection operations only propagate those existing values.
    while (!values.empty()) {
      if (!budget) return {};
      --budget;
      const auto &value=*values.back(); values.pop_back();
      if (const auto *function=std::get_if<VisualProcedure>(&value.data)) {
        const auto found=names.find(function->name);
        if (found==names.end() || !enqueue(found->second)) return {};
      } else if (const auto *list=std::get_if<std::shared_ptr<const VisualList>>(&value.data)) {
        if (!*list) return {};
        if (lists.insert(list->get()).second) {
          if (values.size()>budget || (*list)->size()>budget-values.size()) return {};
          for (const auto &item : **list) values.push_back(&item);
        }
      }
    }
    if (pending.empty()) break;
    const auto index=pending.back(); pending.pop_back();
    const auto begin=program.procedures[index].entry;
    const auto end=index+1<program.procedures.size() ? program.procedures[index+1].entry : program.code.size();
    if (begin>end || end>program.code.size()) return {};
    for (const auto symbol : program.procedureInitialGlobalReads[index]) {
      if (symbol>=reads.size()) return {};
      if (reads[symbol]) continue;
      reads[symbol]=true;
      const auto found=globals.find(program.symbols[symbol]);
      if (found!=globals.end()) {
        if (values.size()>=budget) return {};
        values.push_back(&found->second);
      }
    }
    for (auto pc=begin;pc<end;++pc) {
      const auto &instruction=program.code[pc];
      if (instruction.op==Op::Call || instruction.op==Op::CallValue || instruction.op==Op::Reference) {
        if (!enqueue(instruction.a)) return {};
      } else if (instruction.op==Op::Constant && instruction.literal.kind==Kind::Function && !enqueue(instruction.literal.handle))
        return {};
    }
  }
  return reads;
}

ProgramValueCodec::ProgramValueCodec(const ProgramBytecode &program, Memory &memory)
    : program_(program), memory_(memory), strings_(program.strings) {
  memory_.opaqueContext=this;
  memory_.releaseOpaque=[](void *context,std::uint32_t handle) {
    auto &codec=*static_cast<ProgramValueCodec *>(context);
    const auto found=codec.snapshots_.find(handle);
    if (found!=codec.snapshots_.end()) codec.snapshotHandles_.erase(found->second.get());
    codec.snapshots_.erase(handle);
  };
}

std::uint32_t ProgramValueCodec::text(const std::string &value) {
  const auto it=std::find(strings_.begin(),strings_.end(),value);
  if (it!=strings_.end()) return static_cast<std::uint32_t>(it-strings_.begin());
  strings_.push_back(value); return static_cast<std::uint32_t>(strings_.size()-1);
}

Value ProgramValueCodec::encodeSnapshot(std::shared_ptr<const VisualSnapshot> snapshot, Value inputs) {
  const auto found=snapshotHandles_.find(snapshot.get());
  if (found!=snapshotHandles_.end()) return memory_.copy(Value{Kind::Snapshot,found->second});
  if (snapshotEncoder_) {
    const auto value=snapshotEncoder_(snapshot,inputs);
    if (value.kind!=Kind::Snapshot || !value.handle || memory_.length(value)<2 ||
        memory_.items(value)[0].kind!=Kind::Opaque || !memory_.items(value)[0].handle)
      throw std::logic_error("Invalid compiled snapshot encoding.");
    snapshotHandles_[snapshot.get()]=value.handle;
    snapshots_[memory_.items(value)[0].handle]=std::move(snapshot);
    return value;
  }
  auto value=memory_.allocate(Kind::Snapshot,2);
  auto native=memory_.allocate(Kind::Opaque,0,1);
  if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
  snapshotHandles_[snapshot.get()]=value.handle;
  snapshots_[native.handle]=std::move(snapshot);
  memory_.items(value)[0]=native;
  memory_.items(value)[1]=memory_.copy(inputs);
  return value;
}

Value ProgramValueCodec::encode(const VisualValue &source) {
  ++encodeDepth_;
  struct Leave {
    ProgramValueCodec &codec;
    ~Leave() { if (!--codec.encodeDepth_) codec.encodedLists_.clear(); }
  } leave{*this};
  if (encodeDepth_>64) throw BytecodeNeedsInterpreter{};
  return std::visit([&](const auto &value) -> Value {
    using T=std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T,std::monostate>) return {};
    else if constexpr (std::is_same_v<T,double>) return number(value);
    else if constexpr (std::is_same_v<T,bool>) return boolean(value);
    else if constexpr (std::is_same_v<T,std::string>) return {Kind::Text,text(value)};
    else if constexpr (std::is_same_v<T,VisualVector>) return {Kind::Vector,0,value.x,value.y,value.z};
    else if constexpr (std::is_same_v<T,VisualRotation>) return {Kind::Rotation,0,value.x,value.y,value.z,value.w};
    else if constexpr (std::is_same_v<T,VisualRange>) return {Kind::Range,0,value.minimum,value.maximum};
    else if constexpr (std::is_same_v<T,VisualProcedure>) {
      const auto it=std::find(program_.procedureNames.begin(),program_.procedureNames.end(),value.name);
      if (it==program_.procedureNames.end()) throw BytecodeNeedsInterpreter{};
      return {Kind::Function,static_cast<std::uint32_t>(it-program_.procedureNames.begin())};
    } else if constexpr (std::is_same_v<T,VisualInputs>) {
      auto result=memory_.allocate(Kind::Inputs,static_cast<std::uint32_t>(value.size()));
      if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
      for (std::size_t i=0;i<value.size();++i) {
        const auto &e=value[i]; const auto kind=static_cast<std::uint32_t>(e.value.kind);
        memory_.items(result)[i]={Kind::Event,0,static_cast<double>(e.timeMs),
          static_cast<double>(kind==2 ? e.value.analog : static_cast<std::int32_t>(e.value.switchState)),
          static_cast<double>(static_cast<std::uint32_t>(e.action)),static_cast<double>(kind)};
      }
      return result;
    } else if constexpr (std::is_same_v<T,std::shared_ptr<const VisualList>>) {
      const auto found=encodedLists_.find(value);
      if (found!=encodedLists_.end()) return memory_.copy(found->second);
      auto result=memory_.allocate(Kind::List,static_cast<std::uint32_t>(value->size()));
      if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
      encodedLists_.emplace(value,result);
      for (std::size_t i=0;i<value->size();++i) memory_.items(result)[i]=encode((*value)[i]);
      return result;
    } else if constexpr (std::is_same_v<T,VisualState>) {
      auto result=memory_.allocate(Kind::State,static_cast<std::uint32_t>(VisualStateProperties().size()));
      if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
      for (std::size_t i=0;i<VisualStateProperties().size();++i) memory_.items(result)[i]=encode(ReadVisualStateProperty(value,i));
      return result;
    } else if constexpr (std::is_same_v<T,VisualPolygon>) {
      auto result=memory_.allocate(Kind::Polygon,static_cast<std::uint32_t>(value.size()));
      if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
      for (std::size_t i=0;i<value.size();++i) memory_.items(result)[i]={Kind::Vector,0,value[i].first,value[i].second};
      return result;
    } else if constexpr (std::is_same_v<T,VisualVolume>) {
      auto result=memory_.allocate(value.plane.empty() ? Kind::Box : Kind::Prism,value.plane.empty() ? 2 : 4);
      if (memory_.error!=Error::None) throw std::runtime_error(BytecodeError(memory_.error));
      memory_.items(result)[0]=encode(VisualValue(value.origin));
      if (value.plane.empty()) memory_.items(result)[1]=encode(VisualValue(value.size));
      else {
        memory_.items(result)[1]=encode(VisualValue(value.polygon));
        memory_.items(result)[2]={Kind::Text,text(value.plane)};
        memory_.items(result)[3]=number(value.depth);
      }
      return result;
    } else if constexpr (std::is_same_v<T,std::shared_ptr<const VisualSnapshot>>) {
      const auto found=snapshotHandles_.find(value.get());
      if (found!=snapshotHandles_.end()) return memory_.copy(Value{Kind::Snapshot,found->second});
      const auto inputs=encode(VisualValue(*value->inputs));
      const auto result=encodeSnapshot(value,inputs); memory_.release(inputs); return result;
    }
    throw std::runtime_error("Unsupported compiled value.");
  },source.data);
}

std::shared_ptr<const VisualSnapshot> ProgramValueCodec::snapshot(Value value) const {
  if (value.kind!=Kind::Snapshot || !value.handle) throw std::runtime_error("Expected a simulation snapshot.");
  const auto handle=memory_.items(value)[0].handle;
  auto found=snapshots_.find(handle);
  if (found==snapshots_.end() && snapshotDecoder_) found=snapshots_.emplace(handle,snapshotDecoder_(value)).first;
  if (found==snapshots_.end()) throw std::runtime_error("Unrecognized compiled snapshot.");
  return found->second;
}

VisualValue ProgramValueCodec::decode(Value value) const {
  ++decodeDepth_;
  struct Leave {
    const ProgramValueCodec &codec;
    ~Leave() { if (!--codec.decodeDepth_) codec.decodedLists_.clear(); }
  } leave{*this};
  if (decodeDepth_>64) throw BytecodeNeedsInterpreter{};
  switch (value.kind) {
  case Kind::None: return {};
  case Kind::Number: return VisualValue(value.x);
  case Kind::Boolean: return VisualValue(value.x!=0);
  case Kind::Text: return VisualValue(strings_.at(value.handle));
  case Kind::Vector: return VisualValue(VisualVector{value.x,value.y,value.z});
  case Kind::Rotation: return VisualValue(VisualRotation{value.x,value.y,value.z,value.w});
  case Kind::Range: return VisualValue(VisualRange{value.x,value.y});
  case Kind::Function: return VisualValue(VisualProcedure{program_.procedureNames.at(value.handle)});
  case Kind::Snapshot: return VisualValue(snapshot(value));
  case Kind::Inputs: {
    VisualInputs inputs(memory_.length(value));
    for (std::size_t i=0;i<inputs.size();++i) {
      const auto e=memory_.items(value)[i]; auto &out=inputs[i];
      out.timeMs=static_cast<std::int32_t>(e.x); out.action=static_cast<SandboxInputAction>(static_cast<unsigned>(e.z));
      using namespace forevervalidator::experimental;
      out.value.kind=static_cast<PhysicsSandboxInputValueKind>(static_cast<unsigned>(e.w));
      if (e.w==2) out.value.analog=static_cast<std::int32_t>(e.y);
      else out.value.switchState=static_cast<PhysicsSandboxSwitchState>(static_cast<unsigned>(e.y));
    }
    return VisualValue(std::move(inputs));
  }
  case Kind::List: {
    const auto found=decodedLists_.find(value.handle);
    if (found!=decodedLists_.end()) return VisualValue(found->second);
    auto list=std::make_shared<VisualList>(); list->reserve(memory_.length(value));
    decodedLists_.emplace(value.handle,list);
    for (std::uint32_t i=0;i<memory_.length(value);++i) list->push_back(decode(memory_.items(value)[i]));
    return VisualValue(std::shared_ptr<const VisualList>(std::move(list)));
  }
  case Kind::Polygon: {
    VisualPolygon polygon;
    for (std::uint32_t i=0;i<memory_.length(value);++i) { const auto p=memory_.items(value)[i]; polygon.emplace_back(p.x,p.y); }
    return VisualValue(std::move(polygon));
  }
  case Kind::Box: case Kind::Prism: {
    const auto *items=memory_.items(value); VisualVolume volume;
    volume.origin=std::get<VisualVector>(decode(items[0]).data);
    if (value.kind==Kind::Box) volume.size=std::get<VisualVector>(decode(items[1]).data);
    else { volume.polygon=std::get<VisualPolygon>(decode(items[1]).data); volume.plane=strings_.at(items[2].handle); volume.depth=items[3].x; }
    return VisualValue(std::move(volume));
  }
  case Kind::State: {
    VisualState state;
    const auto get=[&](const char *key) {
      const auto &p=VisualStateProperties();
      const auto it=std::find_if(p.begin(),p.end(),[&](const auto &entry) { return entry.first==key; });
      if (it==p.end() || static_cast<std::size_t>(it-p.begin())>=memory_.length(value)) throw std::runtime_error("Invalid compiled state.");
      return memory_.items(value)[it-p.begin()];
    };
#define NUMBER(key,field) state.field=static_cast<decltype(state.field)>(get(key).x)
#define VECTOR(key,field) { const auto v=get(key); state.field={static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z)}; }
    NUMBER("time",timeMs); NUMBER("tick",tick); NUMBER("duration",durationMs);
    VECTOR("position",car.position); VECTOR("velocity",car.linearSpeed); VECTOR("local-velocity",car.localSpeed);
    VECTOR("angular-velocity",car.angularSpeed); VECTOR("force",car.force); VECTOR("torque",car.torque); VECTOR("camera-up",car.cameraSupportUp);
    NUMBER("rotation-x",car.rotationX); NUMBER("rotation-y",car.rotationY); NUMBER("rotation-z",car.rotationZ); NUMBER("rotation-w",car.rotationW);
    NUMBER("signed-speed",car.signedSpeed); NUMBER("accelerate",accelerate); NUMBER("brake",brake); NUMBER("steering",steering);
    NUMBER("gear",car.gear); NUMBER("rpm",car.rpm); NUMBER("turning-rate",car.turningRate); NUMBER("sliding",car.sliding);
    NUMBER("freewheeling",car.freeWheeling); NUMBER("lateral-contact",car.lateralContact); NUMBER("turbo",car.turbo);
    NUMBER("turbo-type",car.turboType); NUMBER("turbo-boost",car.turboBoostFactor); NUMBER("burning",car.burning);
    NUMBER("gear-changed",car.gearChanged); NUMBER("camera-flight",car.cameraFlightTransition);
    NUMBER("checkpoints",checkpointsCollected); NUMBER("total-checkpoints",checkpointsTotal); NUMBER("laps",completedLaps);
    NUMBER("total-laps",totalLaps); NUMBER("finished",raceCompleted); NUMBER("respawns",respawnCount);
    NUMBER("environment",mapEnvironment); NUMBER("vehicle",vehicleModel);
#undef NUMBER
#undef VECTOR
    const auto array=[&](const char *key,auto &destination) {
      const auto v=get(key);
      if (v.kind!=Kind::List || memory_.length(v)!=destination.size()) throw std::runtime_error("Invalid compiled wheel list.");
      for (std::size_t i=0;i<destination.size();++i) destination[i]=static_cast<std::decay_t<decltype(destination[i])>>(memory_.items(v)[i].x);
    };
    array("wheel-contact",state.car.wheelContact); array("wheel-surface",state.car.wheelSurface);
    array("wheel-has-surface",state.car.wheelHasSurface); array("wheel-sliding",state.car.wheelSliding);
    if (get("finish-time").kind!=Kind::None) state.finishTimeMs=static_cast<std::uint32_t>(get("finish-time").x);
    if (get("finish-upper-bound").kind!=Kind::None) {
      state.finishTime=forevervalidator::FinishTimeEstimate{static_cast<std::uint64_t>(get("finish-lower-bound").x),
        static_cast<std::uint64_t>(get("finish-upper-bound").x),static_cast<std::uint64_t>(std::llround(get("precise-finish-time").x*1000000.0))};
    }
    if (get("stunt-points").kind!=Kind::None) state.stuntsScore=static_cast<std::uint32_t>(get("stunt-points").x);
    if (get("play-mode").kind!=Kind::None) state.playMode=static_cast<forevervalidator::PlayMode>(static_cast<std::uint32_t>(get("play-mode").x));
    return VisualValue(state);
  }
  default: throw std::runtime_error("Invalid compiled value.");
  }
}

} // namespace forevertas::blocks

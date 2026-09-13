#ifndef FOREVERTAS_BLOCKS_PROGRAM_VM_OPS_H
#define FOREVERTAS_BLOCKS_PROGRAM_VM_OPS_H

namespace forevertas::blocks::vm {

template<class Physics>
FT_VM Value Machine<Physics>::builtin(Op op, Value *a, std::uint32_t option) {
  if constexpr (ExactMathExecution<Physics>::value)
    if (requiresHostMath(op)) return physics.exactMath(*this);
  const auto scalar = [&](double value) { return number(finite(value)); };
  const auto composite = [&](Kind kind, std::uint32_t count) {
    auto value = memory.allocate(kind, count);
    if (memory.error == Error::None)
      for (std::uint32_t i = 0; i < count; ++i) memory.items(value)[i] = memory.copy(a[i]);
    return value;
  };
  switch (op) {
  case Op::MapPrepare: {
    integer(numeric(a[2]),1,256);
    if (error!=Error::None || !check(a[0].kind==Kind::Function,Error::Type) || !array(a[1],Kind::List) ||
        !check(a[0].handle<program.procedureCount,Error::Bounds) ||
        !check(program.procedures[a[0].handle].argumentCount==1,Error::ArgumentCount)) return {};
    const auto count=memory.length(a[1]);
    auto context=memory.allocate(Kind::List,8);
    if (memory.error!=Error::None) return {};
    const auto effects=program.procedures[a[0].handle].effects;
    context.x=effects;
    // Private scope fields: function, items, output, parent globals, selected
    // snapshot, event context, scalar/RNG state, and child seeds.
    auto *parts=memory.items(context);
    parts[0]=memory.copy(a[0]); parts[1]=memory.copy(a[1]);
    parts[2]=memory.allocate(Kind::List,count);
    if (memory.error!=Error::None || !count) return context;
    if (effects&Procedure::Globals) parts[3]=memory.copy(globals);
    if (effects&Procedure::Results) parts[4]=memory.copy(selected);
    if (effects&Procedure::Events) parts[5]=memory.copy(eventContext);
    if (effects&(Procedure::RandomState|Procedure::Results)) parts[6]=memory.allocate(Kind::Opaque,sizeof(MapState),1);
    if (count>UINT32_MAX/sizeof(std::uint64_t)) { memory.error=Error::Capacity; return context; }
    if (effects&Procedure::RandomState) parts[7]=memory.allocate(Kind::Opaque,count*sizeof(std::uint64_t),1);
    if (memory.error!=Error::None) return context;
    auto *seeds=parts[7].handle ? reinterpret_cast<std::uint64_t *>(memory.payload(parts[7].handle)) : nullptr;
    // Source draws all child seeds before running any child. Its subsequent
    // random stream must not include random draws made by isolated jobs.
    for (std::uint32_t i=0;i<count;++i) {
      if ((i&255u)==0 && !poll()) return context;
      const auto seed=random.next();
      if (seeds) seeds[i]=seed;
    }
    if (parts[6].handle) {
      auto &saved=*reinterpret_cast<MapState *>(memory.payload(parts[6].handle));
      if (seeds) saved.random=random;
      if (effects&Procedure::Results) { saved.candidates=candidates; saved.score=score; }
    }
    return context;
  }
  case Op::MapEnter: {
    if (!array(a[0],Kind::List) || !check(memory.length(a[0])==8,Error::Bounds)) return {};
    const auto *parts=memory.items(a[0]);
    const auto effects=static_cast<std::uint32_t>(a[0].x);
    const auto i=index(parts[1],a[1]);
    if (error!=Error::None) return {};
    if (effects&Procedure::Globals) {
      auto next=memory.allocate(Kind::List,memory.length(parts[3]));
      if (memory.error!=Error::None) return {};
      for (std::uint32_t j=0;j<memory.length(parts[3]);++j) {
        if ((j&255u)==0 && !poll()) { memory.release(next); return {}; }
        memory.items(next)[j]=memory.copy(memory.items(parts[3])[j]);
      }
      memory.assign(globals,next);
    }
    if (effects&Procedure::Results) { memory.assign(selected,{}); candidates=0; score=0; }
    if (effects&Procedure::Events) memory.assign(eventContext,{});
    if (effects&Procedure::RandomState)
      random.seed(reinterpret_cast<const std::uint64_t *>(memory.payload(parts[7].handle))[i]);
    return {};
  }
  case Op::MapCollect: {
    if (!array(a[0],Kind::List) || !check(memory.length(a[0])==8,Error::Bounds)) return {};
    const auto *parts=memory.items(a[0]);
    const auto effects=static_cast<std::uint32_t>(a[0].x);
    const auto i=index(parts[2],a[1]);
    if (error!=Error::None) return {};
    memory.assign(memory.items(parts[2])[i],memory.copy(a[2]));
    if (effects&Procedure::Globals) memory.assign(globals,memory.copy(parts[3]));
    if (effects&Procedure::Results) memory.assign(selected,memory.copy(parts[4]));
    if (effects&Procedure::Events) memory.assign(eventContext,memory.copy(parts[5]));
    if (parts[6].handle) {
      const auto &saved=*reinterpret_cast<const MapState *>(memory.payload(parts[6].handle));
      if (effects&Procedure::RandomState) random=saved.random;
      if (effects&Procedure::Results) { candidates=saved.candidates; score=saved.score; }
    }
    return {};
  }
  case Op::MapResult:
    if (!array(a[0],Kind::List) || !check(memory.length(a[0])==8,Error::Bounds)) return {};
    return memory.copy(memory.items(a[0])[2]);
  case Op::MapNeedsRestore: return boolean((static_cast<std::uint32_t>(a[0].x)&Procedure::Physics)!=0);
  case Op::RepeatCount: return scalar(static_cast<double>(integer(numeric(a[0]),0)));
  case Op::None: return {};
  case Op::HasValue: return boolean(a[0].kind != Kind::None);
  case Op::Not: return boolean(!truth(a[0]));
  case Op::Equal: return boolean(equal(a[0], a[1]));
  case Op::Less: return boolean(numeric(a[0]) < numeric(a[1]));
  case Op::LessEqual: return boolean(numeric(a[0]) <= numeric(a[1]));
  case Op::Greater: return boolean(numeric(a[0]) > numeric(a[1]));
  case Op::GreaterEqual: return boolean(numeric(a[0]) >= numeric(a[1]));
  case Op::Add: return scalar(numeric(a[0]) + numeric(a[1]));
  case Op::Subtract: return scalar(numeric(a[0]) - numeric(a[1]));
  case Op::Multiply: return scalar(numeric(a[0]) * numeric(a[1]));
  case Op::Divide: case Op::Modulo: {
    const auto left = numeric(a[0]), right = numeric(a[1]);
    if (!check(right != 0, Error::DivideByZero)) return {};
    return scalar(op == Op::Divide ? left/right : ::fmod(left,right));
  }
  case Op::Min: return scalar(minimum(numeric(a[0]),numeric(a[1])));
  case Op::Max: return scalar(maximum(numeric(a[0]),numeric(a[1])));
  case Op::Abs: return scalar(::fabs(numeric(a[0])));
  case Op::Floor: return scalar(::floor(numeric(a[0])));
  case Op::Ceil: return scalar(::ceil(numeric(a[0])));
  case Op::Round: return scalar(::round(numeric(a[0])));
  case Op::Sqrt: return scalar(::sqrt(numeric(a[0])));
  case Op::Sin: return scalar(::sin(numeric(a[0])*0.017453292519943295));
  case Op::Cos: return scalar(::cos(numeric(a[0])*0.017453292519943295));
  case Op::Percent: return scalar(numeric(a[0])/100.0);
  case Op::Kmh: return scalar(numeric(a[0])*3.6);
  case Op::Clamp: {
    const auto value=numeric(a[0]), lo=numeric(a[1]), hi=numeric(a[2]);
    check(lo<=hi,Error::Range); return scalar(value<lo ? lo : hi<value ? hi : value);
  }
  case Op::Blend: {
    const auto first=numeric(a[0]), second=numeric(a[1]), weight=numeric(a[2])/100;
    return scalar(first*(1-weight)+second*weight);
  }
  case Op::Seed: random.seed(static_cast<std::uint64_t>(integer(numeric(a[0]),0))); return {};
  case Op::Random: {
    const auto lo=numeric(a[0]), hi=numeric(a[1]); check(lo<=hi,Error::Range);
    return scalar(lo+(hi-lo)*static_cast<double>(random.next()>>11)*0x1.0p-53);
  }
  case Op::RandomInteger: {
    const auto lo=integer(numeric(a[0])), hi=integer(numeric(a[1]));
    if (!check(lo<=hi,Error::Range)) return {};
    const auto range=static_cast<std::uint64_t>(hi-lo)+1;
    const auto threshold=(std::uint64_t{0}-range)%range;
    auto value=random.next(); while (value<threshold) value=random.next();
    return scalar(static_cast<double>(lo+static_cast<std::int64_t>(value%range)));
  }
  case Op::Vector: case Op::Direction: case Op::Size: {
    Value value{Kind::Vector,0,numeric(a[0]),numeric(a[1]),numeric(a[2])};
    if (op==Op::Direction) value=normalize(value);
    if (op==Op::Size) check(value.x>0 && value.y>0 && value.z>0,Error::Geometry);
    return value;
  }
  case Op::Rotation: {
    constexpr double radians=0.017453292519943295;
    const auto cy=::cos(numeric(a[0])*radians/2), sy=::sin(numeric(a[0])*radians/2);
    const auto cp=::cos(numeric(a[1])*radians/2), sp=::sin(numeric(a[1])*radians/2);
    const auto cr=::cos(numeric(a[2])*radians/2), sr=::sin(numeric(a[2])*radians/2);
    return {Kind::Rotation,0,sp*cy*cr+cp*sy*sr,cp*sy*cr-sp*cy*sr,cp*cy*sr-sp*sy*cr,cp*cy*cr+sp*sy*sr};
  }
  case Op::RotationDistance: {
    check(a[0].kind==Kind::Rotation && a[1].kind==Kind::Rotation,Error::Type);
    const auto norm=::sqrt((a[0].x*a[0].x+a[0].y*a[0].y+a[0].z*a[0].z+a[0].w*a[0].w)*
                          (a[1].x*a[1].x+a[1].y*a[1].y+a[1].z*a[1].z+a[1].w*a[1].w));
    if (!check(norm>1e-12,Error::Geometry)) return {};
    const auto dot=::fabs(a[0].x*a[1].x+a[0].y*a[1].y+a[0].z*a[1].z+a[0].w*a[1].w)/norm;
    return scalar(2*::acos(minimum(1,maximum(0,dot))));
  }
  case Op::Range: case Op::IntegerRange: case Op::TimeRange: {
    auto lo=numeric(a[0]), hi=numeric(a[1]);
    if (op==Op::IntegerRange) { integer(lo); integer(hi); }
    if (op==Op::TimeRange) { time(lo); time(hi); }
    check(lo<=hi,Error::Range); return {Kind::Range,0,lo,hi};
  }
  case Op::RangeMinimum: case Op::RangeMaximum:
    check(a[0].kind==Kind::Range,Error::Type); return scalar(op==Op::RangeMinimum ? a[0].x : a[0].y);
  case Op::Magnitude: return scalar(length(a[0]));
  case Op::Normalize: return normalize(a[0]);
  case Op::Component:
    vector(a[0]); return scalar(option==0 ? a[0].x : option==1 ? a[0].y : a[0].z);
  case Op::Scale: {
    auto v=vector(a[0]); const auto scale=numeric(a[1]);
    v.x=finite(v.x*scale); v.y=finite(v.y*scale); v.z=finite(v.z*scale); return v;
  }
  case Op::VectorAdd: case Op::VectorSubtract: case Op::Distance: case Op::Dot: {
    const auto x=vector(a[0]), y=vector(a[1]);
    if (op==Op::Dot) return scalar(x.x*y.x+x.y*y.y+x.z*y.z);
    const auto sign=op==Op::VectorAdd ? 1 : -1;
    Value result{Kind::Vector,0,finite(x.x+sign*y.x),finite(x.y+sign*y.y),finite(x.z+sign*y.z)};
    return op==Op::Distance ? scalar(length(result)) : result;
  }
  case Op::List: return memory.allocate(Kind::List,0);
  case Op::Length: if (!array(a[0],Kind::List)) return {}; return scalar(memory.length(a[0]));
  case Op::Item: {
    if (!array(a[0],Kind::List)) return {};
    const auto i=index(a[0],a[1]);
    return error==Error::None ? memory.copy(memory.items(a[0])[i]) : Value{};
  }
  case Op::Contains: {
    if (!array(a[0],Kind::List)) return {};
    for (std::uint32_t i=0;i<memory.length(a[0]);++i) if (equal(memory.items(a[0])[i],a[1])) return boolean(true);
    return boolean(false);
  }
  case Op::Append: case Op::ReplaceItem: case Op::DeleteItem: {
    if (!array(a[0],Kind::List)) return {};
    const auto size=memory.length(a[0]);
    const auto i=op==Op::Append ? size : index(a[0],a[1]);
    if (error!=Error::None) return {};
    const auto count=op==Op::Append ? size+1 : op==Op::DeleteItem ? size-1 : size;
    check(count<=1000000,Error::Capacity);
    auto result=memory.allocate(Kind::List,count);
    if (memory.error!=Error::None) return {};
    for (std::uint32_t j=0;j<count;++j) {
      const auto value=(op==Op::Append || op==Op::ReplaceItem) && i==j ? a[op==Op::Append ? 1 : 2]
          : memory.items(a[0])[j+(op==Op::DeleteItem && j>=i)];
      memory.items(result)[j]=memory.copy(value);
    }
    return result;
  }
  case Op::Numbers: {
    const auto from=numeric(a[0]), to=numeric(a[1]), step=numeric(a[2]);
    if (!check(step!=0,Error::Range)) return {};
    const auto count=(step>0 ? from>to : from<to) ? 0 : ::floor((to-from)/step)+1;
    const auto size=static_cast<std::uint32_t>(integer(count,0,1000000));
    auto result=memory.allocate(Kind::List,size);
    if (memory.error==Error::None) for (std::uint32_t i=0;i<size;++i) memory.items(result)[i]=scalar(from+step*i);
    return result;
  }
  case Op::Box:
    vector(a[0]); vector(a[1]); check(a[1].x>0 && a[1].y>0 && a[1].z>0,Error::Geometry);
    return composite(Kind::Box,2);
  case Op::Polygon: {
    if (!array(a[0],Kind::List) || !check(memory.length(a[0])>=3,Error::Geometry)) return {};
    auto result=memory.duplicate(a[0],memory.length(a[0])); result.kind=Kind::Polygon;
    if (memory.error==Error::None) for (std::uint32_t i=0;i<memory.length(result);++i) {
      auto &point=memory.items(result)[i]; vector(point); point.z=0;
    }
    return result;
  }
  case Op::Prism: {
    vector(a[0]); check(numeric(a[1])>0,Error::Geometry);
    check(a[2].kind==Kind::Text && a[2].handle<3,Error::Geometry); array(a[3],Kind::Polygon);
    auto value=memory.allocate(Kind::Prism,4);
    if (memory.error==Error::None) {
      memory.items(value)[0]=memory.copy(a[0]); memory.items(value)[1]=memory.copy(a[3]);
      memory.items(value)[2]=memory.copy(a[2]); memory.items(value)[3]=memory.copy(a[1]);
    }
    return value;
  }
  case Op::Inside: {
    const auto p=vector(a[0]);
    if (!check(a[1].kind==Kind::Box || a[1].kind==Kind::Prism,Error::Type)) return {};
    const auto *v=memory.items(a[1]); const double x=p.x-v[0].x,y=p.y-v[0].y,z=p.z-v[0].z;
    if (a[1].kind==Kind::Box) return boolean(::fabs(x)<=v[1].x/2 && ::fabs(y)<=v[1].y/2 && ::fabs(z)<=v[1].z/2);
    // Projection identifiers are compiler-interned in fixed order: xy,xz,yz.
    const auto plane=v[2].handle;
    if (!check(plane<3,Error::Geometry)) return {};
    const double u=plane==2 ? y : x, w=plane==0 ? y : z, depth=plane==0 ? z : plane==1 ? y : x;
    if (::fabs(depth)>v[3].x/2) return boolean(false);
    const auto count=memory.length(v[1]); const auto *points=memory.items(v[1]); bool inside=false;
    for (std::uint32_t i=0,j=count-1;i<count;j=i++) {
      const auto ax=points[i].x,ay=points[i].y,bx=points[j].x,by=points[j].y;
      const auto cross=(u-ax)*(by-ay)-(w-ay)*(bx-ax);
      if (::fabs(cross)<1e-9 && u>=minimum(ax,bx) && u<=maximum(ax,bx) && w>=minimum(ay,by) && w<=maximum(ay,by)) return boolean(true);
      if ((ay>w)!=(by>w) && u<(bx-ax)*(w-ay)/(by-ay)+ax) inside=!inside;
    }
    return boolean(inside);
  }
  case Op::EmptyInputs: return memory.allocate(Kind::Inputs,0);
  case Op::InputCount:
    if (!array(a[0],Kind::Inputs)) return {};
    return scalar(memory.length(a[0]));
  case Op::InputTime: case Op::InputValue: case Op::InputAction: {
    if (!array(a[0],Kind::Inputs)) return {};
    const auto i=index(a[0],a[1]); if (error!=Error::None) return {};
    const auto event=memory.items(a[0])[i];
    if (op==Op::InputTime) return scalar(maximum(0,event.x-tickMs));
    if (op==Op::InputAction) return {Kind::Text,program.actionNames[static_cast<std::uint32_t>(event.z)]};
    return event.w==0 ? Value{} : scalar(event.w==1 ? (event.y!=0) : event.y);
  }
  case Op::HeldInput: {
    if (!array(a[0],Kind::Inputs)) return {};
    const auto at=time(numeric(a[1]))+tickMs;
    if (!check(a[2].kind==Kind::Text,Error::Type)) return {};
    std::uint32_t action=10;
    for (std::uint32_t i=0;i<10;++i) if (program.actionNames[i]==a[2].handle) action=i;
    if (!check(action<10,Error::UnknownAction)) return {};
    double latest=-2147483649.0; Value value=number(0);
    for (std::uint32_t i=0;i<memory.length(a[0]);++i) {
      const auto event=memory.items(a[0])[i];
      if (event.z==action && event.x<=at && event.x>=latest) {
        latest=event.x; value=event.w==0 ? Value{} : scalar(event.w==1 ? (event.y!=0) : event.y);
      }
    }
    return value;
  }
  case Op::RemoveInput: case Op::MoveInput: case Op::ChangeInput: case Op::SortInputs: case Op::SetInput: {
    if (!array(a[0],Kind::Inputs)) return {};
    const auto size=memory.length(a[0]); std::uint32_t i=0; bool insert=false;
    if (op==Op::SortInputs) {
      const auto *events=memory.items(a[0]);
      for (i=1;i<size && events[i-1].x<=events[i].x;++i)
        if ((i & 255u)==0 && !poll()) return {};
      if (i>=size) return memory.copy(a[0]);
    }
    Value event;
    if (op==Op::SetInput) {
      event.kind=Kind::Event; event.x=time(numeric(a[1]))+tickMs;
      if (!check(a[2].kind==Kind::Text,Error::Type)) return {};
      std::uint32_t action=10;
      for (std::uint32_t j=0;j<10;++j) if (program.actionNames[j]==a[2].handle) action=j;
      if (!check(action<10 && action!=0 && action!=7 && action!=8,Error::ReadOnlyInput)) return {};
      event.z=action; const bool analog=action==2 || action==4;
      event.w=analog ? 2 : 1; event.y=static_cast<double>(integer(numeric(a[3]),analog ? -65536 : 0,analog ? 65536 : 1));
      for (i=0;i<size;++i) if (memory.items(a[0])[i].x==event.x && memory.items(a[0])[i].z==event.z) break;
      insert=i==size;
      if (insert) for (i=0;i<size && memory.items(a[0])[i].x<=event.x;++i) {}
    } else if (op!=Op::SortInputs) i=index(a[0],a[1]);
    if (error!=Error::None) return {};
    const auto count=op==Op::RemoveInput ? size-1 : insert ? size+1 : size;
    if (!check(count<=1000000,Error::Capacity)) return {};
    auto result=memory.allocate(Kind::Inputs,count);
    if (memory.error!=Error::None) return {};
    for (std::uint32_t j=0;j<count;++j) {
      if (op==Op::SetInput && j==i) memory.items(result)[j]=event;
      else memory.items(result)[j]=memory.items(a[0])[j+(op==Op::RemoveInput && j>=i)-(insert && j>i)];
    }
    if (op==Op::MoveInput) memory.items(result)[i].x=time(numeric(a[2]))+tickMs;
    if (op==Op::ChangeInput) {
      auto &changed=memory.items(result)[i];
      check(changed.w==1 || changed.w==2,Error::ReadOnlyInput);
      changed.y=static_cast<double>(integer(numeric(a[2]),changed.w==2 ? -65536 : 0,changed.w==2 ? 65536 : 1));
    }
    if (op==Op::SortInputs) {
      // Stable merge passes bound both work and cancellation latency even for
      // a long reversed sequence. Already-sorted immutable values are shared.
      const auto scratch=memory.allocate(Kind::Inputs,count);
      if (memory.error!=Error::None) return result;
      auto *from=memory.items(result), *to=memory.items(scratch);
      for (std::uint32_t width=1;width<count;width*=2) {
        for (std::uint32_t first=0;first<count;first+=2*width) {
          const auto middle=first+width<count ? first+width : count;
          const auto end=first+2*width<count ? first+2*width : count;
          auto left=first, right=middle;
          for (std::uint32_t at=first;at<end;++at) {
            if ((at & 255u)==0 && !poll()) { memory.release(scratch); return result; }
            to[at]=(left<middle && (right>=end || from[left].x<=from[right].x)) ? from[left++] : from[right++];
          }
        }
        auto *swap=from; from=to; to=swap;
      }
      if (from!=memory.items(result))
        for (std::uint32_t j=0;j<count;++j) memory.items(result)[j]=from[j];
      memory.release(scratch);
    }
    return result;
  }
  case Op::Workers: return scalar(workers);
  case Op::BatchSize: return scalar(batchSize);
  case Op::Count: ++candidates; return {};
  case Op::AddCount: candidates+=static_cast<std::uint64_t>(integer(numeric(a[0]),0)); return {};
  case Op::Iterations: return scalar(static_cast<double>(candidates));
  case Op::ClearResult: memory.assign(selected,{}); return {};
  case Op::HasResult: return boolean(selected.kind==Kind::Snapshot);
  case Op::ResultScore: check(selected.kind==Kind::Snapshot,Error::NoResult); return scalar(score);
  case Op::ResultSnapshot: check(selected.kind==Kind::Snapshot,Error::NoResult); return memory.copy(selected);
  case Op::PublishSnapshot:
    if (array(a[0],Kind::Snapshot)) { score=numeric(a[1]); memory.assign(selected,memory.copy(a[0])); } return {};
  case Op::Publish:
    score=numeric(a[0]); memory.assign(selected,physics.apply(Op::Snapshot,*this,a,0)); return {};
  case Op::TickDuration: return scalar(tickMs);
  case Op::Read:
    return eventProperty(a[0],option);
  default: return physics.apply(op,*this,a,option);
  }
}

} // namespace forevertas::blocks::vm
#endif

#ifndef FOREVERTAS_BLOCKS_PROGRAM_VM_H
#define FOREVERTAS_BLOCKS_PROGRAM_VM_H

// Portable, bounded value/bytecode ABI. No Qt, STL containers, native search
// policies or template names enter this layer. The same instructions can be
// checked on the host and executed by a CUDA lane with a physics adapter.
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <cstring>

#ifdef __CUDACC__
#define FT_VM __device__
#else
#define FT_VM
#endif

namespace forevertas::blocks::vm {

enum class Kind : std::uint32_t {
  Unset, None, Number, Boolean, Text, Vector, Rotation, Range, Function, Event, NumberInterval,
  List, Inputs, State, Snapshot, Box, Prism, Polygon, Opaque, DeferredState
};
struct Value {
  Kind kind = Kind::None;
  std::uint32_t handle = 0;
  double x = 0, y = 0, z = 0, w = 0;
};
enum class Op : std::uint32_t {
  Constant, Load, Store, Local, Change, Drop, Jump, IfFalse, IfTrue, RepeatCount,
  Call, CallValue, Return, End, Stop, Reference, Apply, Do,
  None, Boolean, HasValue, List, Append, ReplaceItem, DeleteItem, Length, Item,
  Contains, Numbers, Add, Subtract, Multiply, Divide, Modulo, Min, Max, Abs,
  Floor, Ceil, Round, Sqrt, Sin, Cos, Clamp, Blend, Percent, Kmh,
  Equal, Less, LessEqual, Greater, GreaterEqual, Not,
  Random, RandomInteger, Seed, Vector, Direction, Size, Rotation, Range, IntegerRange,
  RangeMinimum, RangeMaximum, Magnitude, Normalize, Component, Scale, Distance,
  Dot, VectorAdd, VectorSubtract, RotationDistance, Box, Prism, Polygon, Inside,
  EmptyInputs, InputCount, InputTime, InputValue, InputAction, RemoveInput,
  SortInputs, MoveInput, ChangeInput, HeldInput, SetInput,
  Workers, BatchSize, Count, AddCount, Iterations, ClearResult, HasResult,
  ResultScore, ResultSnapshot, Publish, PublishSnapshot,
  Time, Horizon, TickDuration, AllTime, AtTime, TimeRange,
  Read, ReadCurrent, CurrentState, PreviousState, Snapshot, SnapshotState,
  SnapshotInputs, Inputs, UseInputs, Step, EventEnter, EventExit, EventValue, EventState, CheckText, CheckFunction, Interpret,
  EventCapture, EventRead, ReadPrevious, Restore, SetHorizon, History, Restart,
  MapPrepare, MapEnter, MapCollect, MapResult, MapNeedsRestore
};
struct Instruction {
  Op op = Op::None;
  std::uint32_t a = 0, b = 0;
  std::uint64_t source = 0;
  Value literal;
};
struct Procedure {
  enum Effect : std::uint32_t { Physics = 1, RandomState = 2, Globals = 4, Results = 8, Events = 16, All = 31 };
  std::uint32_t entry = 0, argumentOffset = 0, argumentCount = 0, localCount = 0;
  std::uint32_t effects = All;
};
struct Program {
  const Instruction *code = nullptr;
  const Procedure *procedures = nullptr;
  const std::uint32_t *arguments = nullptr;
  std::uint32_t codeSize = 0, procedureCount = 0, symbolCount = 0;
  std::uint32_t actionNames[10]{};
};
enum class Error : std::uint32_t {
  None, Capacity, Type, Unset, Integer, NonFinite, Bounds, Range, DivideByZero,
  Recursion, NoReturn, ArgumentCount, Time, Horizon, PastInputs, Unsorted,
  DuplicateInput, ReadOnlyInput, UnknownAction, NoResult, Geometry, Physics,
  Cancelled, InstructionLimit, Unsupported, Interpreter
};
struct Result { Value value; Error error = Error::None; std::uint64_t source = 0, operations = 0; };

template<class Physics,class=void> struct UniformExecution : std::false_type {};
template<class Physics> struct UniformExecution<Physics,std::void_t<decltype(Physics::uniformExecution)>>
    : std::bool_constant<Physics::uniformExecution> {};

template<class Physics,class=void> struct ExactMathExecution : std::false_type {};
template<class Physics> struct ExactMathExecution<Physics,std::void_t<decltype(Physics::exactMathExecution)>>
    : std::bool_constant<Physics::exactMathExecution> {};

#ifdef __CUDACC__
__host__ __device__
#endif
inline bool requiresHostMath(Op op) {
  return op==Op::Sin || op==Op::Cos || op==Op::Rotation || op==Op::RotationDistance;
}

FT_VM inline Value number(double value) { return {Kind::Number, 0, value}; }
FT_VM inline Value boolean(bool value) { return {Kind::Boolean, 0, value ? 1.0 : 0.0}; }
FT_VM inline bool reference(Value value) { return value.kind >= Kind::List && value.handle != 0; }
FT_VM inline double minimum(double a, double b) { return b < a ? b : a; }
FT_VM inline double maximum(double a, double b) { return a < b ? b : a; }

struct Header { std::uint32_t bytes, references, length, next; };
struct Memory {
  static constexpr std::uint32_t sharedHandle = 0x80000000u;
  unsigned char *bytes = nullptr;
  std::uint32_t capacity = 0, used = 32, free[32]{};
  Error error = Error::None;
  void *opaqueContext = nullptr;
  void (*releaseOpaque)(void *, std::uint32_t) = nullptr;
  const unsigned char *sharedBytes = nullptr;

  FT_VM std::uint32_t localHandle(std::uint32_t handle) {
    return sharedBytes ? handle & ~sharedHandle : handle;
  }
  FT_VM Header &header(std::uint32_t handle) {
    return *reinterpret_cast<Header *>(bytes + localHandle(handle) - sizeof(Header));
  }
  FT_VM unsigned char *payload(std::uint32_t handle) {
    if (sharedBytes && (handle & sharedHandle)) {
      const auto offset = *reinterpret_cast<const std::uint32_t *>(bytes + (handle & ~sharedHandle));
      return const_cast<unsigned char *>(sharedBytes + offset);
    }
    return bytes + handle;
  }
  FT_VM Value *items(Value value) { return reinterpret_cast<Value *>(payload(value.handle)); }
  FT_VM std::uint32_t length(Value value) { return header(value.handle).length; }
  FT_VM void retain(Value value) { if (reference(value)) ++header(value.handle).references; }
  FT_VM Value copy(Value value) { retain(value); return value; }
  FT_VM Value allocate(Kind kind, std::uint32_t length, std::uint32_t itemBytes = sizeof(Value)) {
    return allocateStorage(kind,length,itemBytes,true);
  }
  // The caller must assign every element before observing or releasing it.
  FT_VM Value allocateForOverwrite(Kind kind, std::uint32_t length) {
    return allocateStorage(kind,length,sizeof(Value),false);
  }
  FT_VM Value allocateStorage(Kind kind, std::uint32_t length, std::uint32_t itemBytes, bool initialize) {
    const std::uint64_t required = sizeof(Header) + static_cast<std::uint64_t>(length) * itemBytes;
    std::uint32_t size = 32, bucket = 5;
    while (size < required && bucket < 30) { size *= 2; ++bucket; }
    if (size < required || size > capacity) { error = Error::Capacity; return {}; }
    std::uint32_t offset;
    if (free[bucket]) {
      offset = free[bucket]; free[bucket] = *reinterpret_cast<std::uint32_t *>(bytes + offset);
    } else {
      if (used > capacity - size) { error = Error::Capacity; return {}; }
      offset = used; used += size;
    }
    const auto handle = offset + static_cast<std::uint32_t>(sizeof(Header));
    header(handle) = {size, 1, length, 0};
    if (initialize && kind != Kind::Opaque) {
      auto *values = reinterpret_cast<Value *>(bytes + handle);
      for (std::uint32_t i = 0; i < length; ++i) values[i] = {};
    }
    return {kind, handle};
  }
  FT_VM void enqueueRelease(Value value, std::uint32_t &pending) {
    if (!reference(value)) return;
    auto &h = header(value.handle);
    if (--h.references) return;
    if (value.kind == Kind::Opaque) {
#ifndef __CUDACC__
      if (releaseOpaque) releaseOpaque(opaqueContext,value.handle);
#endif
      h.length = 0;
    }
    h.next = pending;
    pending = value.handle;
  }
  FT_VM void release(Value value) {
    // Dead objects provide their own work list; releasing a nested value must
    // not recurse on the device call stack or allocate more arena storage.
    std::uint32_t pending = 0;
    enqueueRelease(value,pending);
    while (pending) {
      const auto handle = pending;
      auto &h = header(handle);
      pending = h.next;
      const auto *values = reinterpret_cast<const Value *>(payload(handle));
      for (std::uint32_t i = 0; i < h.length; ++i) enqueueRelease(values[i],pending);
      std::uint32_t bucket = 5, size = 32;
      while (size < h.bytes) { size *= 2; ++bucket; }
      const auto offset = localHandle(handle) - static_cast<std::uint32_t>(sizeof(Header));
      *reinterpret_cast<std::uint32_t *>(bytes + offset) = free[bucket]; free[bucket] = offset;
    }
  }
  FT_VM void assign(Value &destination, Value owned) { release(destination); destination = owned; }
  FT_VM Value duplicate(Value value, std::uint32_t newLength) {
    auto result = allocate(value.kind, newLength);
    if (error != Error::None) return {};
    const auto count = length(value) < newLength ? length(value) : newLength;
    for (std::uint32_t i = 0; i < count; ++i) items(result)[i] = copy(items(value)[i]);
    return result;
  }
};

// The C++ standard's mt19937_64 recurrence, not a different GPU random stream.
struct Random {
  std::uint64_t state[312];
  std::uint32_t index = 312;
  FT_VM void seed(std::uint64_t value) {
    state[0] = value;
    for (std::uint32_t i = 1; i < 312; ++i)
      state[i] = 6364136223846793005ull * (state[i-1] ^ (state[i-1] >> 62)) + i;
    index = 312;
  }
  FT_VM std::uint64_t next() {
    if (index == 312) {
      for (std::uint32_t i = 0; i < 312; ++i) {
        const auto x = (state[i] & 0xffffffff80000000ull) | (state[(i+1)%312] & 0x7fffffffull);
        state[i] = state[(i+156)%312] ^ (x >> 1) ^ ((x & 1) ? 0xb5026f5aa96619e9ull : 0);
      }
      index = 0;
    }
    auto value = state[index++];
    value ^= (value >> 29) & 0x5555555555555555ull;
    value ^= (value << 17) & 0x71d67fffeda60000ull;
    value ^= (value << 37) & 0xfff7eee000000000ull;
    return value ^ (value >> 43);
  }
};

template<class Physics> struct Machine {
  Program program;
  Memory memory;
  Physics &physics;
  Value *stack = nullptr;
  std::uint32_t stackSize = 0, stackCapacity = 0;
  Value globals, locals;
  Value eventContext;
  Value pendingEventState;
  struct Frame { Value locals; std::uint32_t pc, base; bool reporter; };
  Frame frames[64];
  std::uint32_t frameCount = 0, pc = 0;
  Random random;
  struct MapState { Random random; std::uint64_t candidates; double score; };
  Value selected;
  double score = 0;
  std::uint64_t candidates = 0, operations = 0, operationLimit = 0, source = 0;
  std::uint32_t workers = 1, batchSize = 1, tickMs = 10;
  Error error = Error::None;

  FT_VM Machine(Program code, Physics &adapter) : program(code), physics(adapter) {}

  FT_VM bool check(bool condition, Error code) { if (!condition && error == Error::None) error = code; return condition; }
  FT_VM bool poll() {
    if (physics.cancelled()) error = physics.interpreterRequested() ? Error::Interpreter : Error::Cancelled;
    return error == Error::None;
  }
  FT_VM double numeric(Value value) { return check(value.kind == Kind::Number, Error::Type) ? value.x : 0; }
  FT_VM bool truth(Value value) { return check(value.kind == Kind::Boolean, Error::Type) && value.x != 0; }
  FT_VM double finite(double value) { check(std::isfinite(value), Error::NonFinite); return value; }
  FT_VM std::int64_t integer(double value, double lo = -9007199254740991.0, double hi = 9007199254740991.0) {
    if (!check(std::isfinite(value) && value == ::floor(value) && value >= lo && value <= hi, Error::Integer)) return 0;
    return static_cast<std::int64_t>(value);
  }
  FT_VM std::uint32_t time(double value) {
    const auto result = integer(value, 0, 2147481040);
    check(result % tickMs == 0, Error::Time);
    return static_cast<std::uint32_t>(result);
  }
  FT_VM Value &cell(std::uint32_t symbol, std::uint32_t localSlot, bool local = false) {
    if (!check(symbol < program.symbolCount, Error::Bounds)) return stack[0];
    if (localSlot && locals.handle && (local || memory.items(locals)[localSlot-1].kind != Kind::Unset))
      return memory.items(locals)[localSlot-1];
    return memory.items(globals)[symbol];
  }
  FT_VM Value get(std::uint32_t symbol, std::uint32_t localSlot) {
    const auto value = cell(symbol,localSlot); check(value.kind != Kind::Unset, Error::Unset); return memory.copy(value);
  }
  FT_VM void push(Value owned) {
    if (check(stackSize < stackCapacity, Error::Capacity)) stack[stackSize++] = owned;
    else memory.release(owned);
  }
  FT_VM Value pop() {
    if (!check(stackSize != 0, Error::Bounds)) return {};
    return stack[--stackSize];
  }
  FT_VM bool array(Value value, Kind kind) { return check(value.kind == kind && value.handle != 0, Error::Type); }
  FT_VM std::uint32_t index(Value value, Value position) {
    return static_cast<std::uint32_t>(integer(numeric(position), 1, memory.length(value)) - 1);
  }
  FT_VM bool equal(Value a, Value b) {
    struct Comparison { std::uint32_t left, right, next, count; } parents[65];
    std::uint32_t depth = 0, comparisons = 0;
    for (;;) {
      if ((comparisons++ & 255u) == 0 && !poll()) return false;
      if (a.kind != b.kind) return false;
      if (a.kind == Kind::Snapshot) {
        if (a.handle != b.handle) return false;
      } else if (reference(a)) {
        if (memory.length(a) != memory.length(b)) return false;
        if (a.handle != b.handle && memory.length(a)) {
          if (!check(depth < 65,Error::Recursion)) return false;
          parents[depth++] = {a.handle,b.handle,1,memory.length(a)};
          a = memory.items(a)[0]; b = memory.items(b)[0];
          continue;
        }
      } else if (a.handle != b.handle || a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) return false;
      while (depth && parents[depth-1].next == parents[depth-1].count) --depth;
      if (!depth) return true;
      auto &parent = parents[depth-1];
      a = reinterpret_cast<const Value *>(memory.payload(parent.left))[parent.next];
      b = reinterpret_cast<const Value *>(memory.payload(parent.right))[parent.next++];
    }
  }
  FT_VM Value vector(Value value) {
    check(value.kind == Kind::Vector, Error::Type); return value;
  }
  FT_VM double length(Value value) {
    vector(value);
    const auto x=::fabs(value.x), y=::fabs(value.y), z=::fabs(value.z);
    const auto scale=maximum(maximum(x,y),z);
    return scale ? scale*::sqrt((x/scale)*(x/scale)+(y/scale)*(y/scale)+(z/scale)*(z/scale)) : 0;
  }
  FT_VM Value normalize(Value value) {
    const auto size = length(value);
    if (check(size > 1e-12, Error::Geometry)) { const auto factor=1.0/size; value.x *= factor; value.y *= factor; value.z *= factor; }
    return value;
  }
  FT_VM Value builtin(Op op, Value *a, std::uint32_t option);
  FT_VM bool needsEventState(Value value) {
    return value.kind==Kind::DeferredState && memory.items(value)[3].kind==Kind::None;
  }
  FT_VM bool pendingEventObserved() {
    return pendingEventState.handle && memory.header(pendingEventState.handle).references>1 && needsEventState(pendingEventState);
  }
  FT_VM Value eventState(Value value) {
    if (value.kind!=Kind::DeferredState) return memory.copy(value);
    auto &cached=memory.items(value)[3];
    if (cached.kind==Kind::None) {
      const auto time=builtin(Op::ReadCurrent,nullptr,0);
      if (!check(time.x==memory.items(value)[0].x,Error::Interpreter)) return {};
      cached=builtin(Op::CurrentState,nullptr,0);
    }
    return memory.copy(cached);
  }
  FT_VM Value eventProperty(Value value,std::uint32_t property) {
    if (value.kind==Kind::DeferredState) {
      if (property<3) return memory.copy(memory.items(value)[property]);
      if (needsEventState(value)) {
        const auto time=builtin(Op::ReadCurrent,nullptr,0);
        if (!check(time.x==memory.items(value)[0].x,Error::Interpreter)) return {};
        return builtin(Op::ReadCurrent,nullptr,property);
      }
      const auto state=eventState(value);
      Value result;
      if (array(state,Kind::State) && check(property<memory.length(state),Error::Bounds)) result=memory.copy(memory.items(state)[property]);
      memory.release(state); return result;
    }
    if (!array(value,Kind::State) || !check(property<memory.length(value),Error::Bounds)) return {};
    return memory.copy(memory.items(value)[property]);
  }
  FT_VM void beforeStateChange() {
    if (pendingEventObserved()) memory.release(eventState(pendingEventState));
    memory.assign(pendingEventState,{});
  }
  FT_VM bool call(std::uint32_t procedure, std::uint32_t count, bool reporter) {
    if (!check(procedure < program.procedureCount, Error::Bounds) ||
        !check(frameCount < 64, Error::Recursion)) return false;
    const auto &function = program.procedures[procedure];
    if (!check(count == function.argumentCount && count <= stackSize, Error::ArgumentCount)) return false;
    auto next = function.localCount ? memory.allocate(Kind::List, function.localCount) : Value{};
    if (memory.error != Error::None) return false;
    for (std::uint32_t i = 0; i < function.localCount; ++i) memory.items(next)[i].kind = Kind::Unset;
    frames[frameCount++] = {locals, pc, stackSize-count, reporter};
    for (std::uint32_t i = 0; i < count; ++i)
      memory.items(next)[program.arguments[function.argumentOffset+i]] = stack[stackSize-count+i];
    stackSize -= count; locals = next; pc = function.entry;
    return true;
  }
  FT_VM void begin(std::uint32_t entry, Value argument, std::uint64_t seed) {
    random.seed(seed); push(argument); pc = program.codeSize;
    call(entry, 1, true);
  }
  // A scheduler may suspend before an instruction, while its arguments and
  // ownership are still untouched. Resuming must not charge it twice.
  template<class Pause> FT_VM bool resume(Pause pause) {
    while (error == Error::None && memory.error == Error::None && pc < program.codeSize) {
      if (pause(program.code[pc])) return false;
      const auto &instruction = program.code[pc++]; source = instruction.source;
      ++operations;
      if (operationLimit && operations > operationLimit) { error = Error::InstructionLimit; break; }
      if ((operations & 255u) == 0 && !poll()) break;
      if (instruction.op==Op::Step || instruction.op==Op::UseInputs || instruction.op==Op::Restore || instruction.op==Op::SetHorizon || instruction.op==Op::Restart) {
        beforeStateChange();
        if (error!=Error::None || memory.error!=Error::None) break;
      }
      switch (instruction.op) {
      case Op::Constant: push(instruction.literal); break;
      case Op::Load: push(get(instruction.a,instruction.b)); break;
      case Op::Store: case Op::Local: case Op::Change: {
        auto value = pop(); auto &target = cell(instruction.a,instruction.b,instruction.op == Op::Local);
        if (instruction.op == Op::Change) value = number(finite(numeric(target)+numeric(value)));
        memory.assign(target, value); break;
      }
      case Op::Drop: memory.release(pop()); break;
      case Op::Jump: pc = instruction.a; break;
      case Op::IfFalse: case Op::IfTrue: {
        const auto value = pop(); const bool condition = truth(value); memory.release(value);
        if (condition == (instruction.op == Op::IfTrue)) pc = instruction.a;
        break;
      }
      case Op::Call: case Op::CallValue:
        call(instruction.a, instruction.b, instruction.op == Op::CallValue); break;
      case Op::Apply: case Op::Do: {
        const auto arguments = pop(), function = pop();
        if (instruction.b==1) {
          // Map already supplies one owned item, not an argument-list value.
          if (check(function.kind==Kind::Function,Error::Type)) {
            push(arguments); call(function.handle,1,instruction.op==Op::Apply);
          } else memory.release(arguments);
        } else {
          if (check(function.kind == Kind::Function, Error::Type) && array(arguments, Kind::List)) {
            const auto count = memory.length(arguments);
            if (check(count <= stackCapacity-stackSize, Error::Capacity)) {
              for (std::uint32_t i=0; i<count; ++i) push(memory.copy(memory.items(arguments)[i]));
              call(function.handle, count, instruction.op == Op::Apply);
            }
          }
          memory.release(arguments);
        }
        memory.release(function); break;
      }
      case Op::Reference: push({Kind::Function, instruction.a}); break;
      case Op::CheckText:
        if (check(stackSize!=0,Error::Bounds)) check(stack[stackSize-1].kind==Kind::Text,Error::Type);
        break;
      case Op::CheckFunction:
        if (check(stackSize!=0,Error::Bounds)) check(stack[stackSize-1].kind==Kind::Function,Error::Type);
        break;
      case Op::EventEnter: {
        const auto state=pop(), payload=pop();
        auto context=memory.allocate(Kind::List,3);
        if (memory.error==Error::None) {
          memory.items(context)[0]=eventContext;
          memory.items(context)[1]=payload;
          memory.items(context)[2]=state;
          eventContext=context;
        } else { memory.release(payload); memory.release(state); }
        break;
      }
      case Op::EventExit: {
        if (!array(eventContext,Kind::List)) break;
        const auto prior=memory.copy(memory.items(eventContext)[0]);
        memory.assign(eventContext,prior); break;
      }
      case Op::EventValue: push(eventContext.handle ? eventState(memory.items(eventContext)[1]) : Value{}); break;
      case Op::EventState:
        push(eventContext.handle ? eventState(memory.items(eventContext)[2]) : builtin(Op::CurrentState,nullptr,0)); break;
      case Op::EventRead:
        if (eventContext.handle) push(eventProperty(memory.items(eventContext)[2],instruction.a));
        else push(builtin(Op::ReadCurrent,nullptr,instruction.a));
        break;
      case Op::EventCapture: {
        // Only an observed state is materialized. A retained event context is
        // resolved before the next state change, including reentrant ticks.
        if (!pendingEventState.handle) {
          pendingEventState=memory.allocate(Kind::DeferredState,4);
          if (memory.error==Error::None)
            for (std::uint32_t i=0;i<3;++i) memory.items(pendingEventState)[i]=builtin(Op::ReadCurrent,nullptr,i);
        }
        push(memory.copy(pendingEventState)); break;
      }
      case Op::Return: case Op::End: {
        auto value = instruction.op == Op::Return ? pop() : Value{};
        if (!check(frameCount != 0, Error::NoReturn)) { memory.release(value); break; }
        const auto frame = frames[--frameCount];
        if (frame.reporter && instruction.op == Op::End) error = Error::NoReturn;
        while (stackSize > frame.base) memory.release(pop());
        memory.release(locals); locals = frame.locals; pc = frame.pc;
        if (frame.reporter) push(value); else memory.release(value);
        break;
      }
      case Op::Stop: error = Error::Cancelled; break;
      case Op::Interpret: error = Error::Interpreter; break;
      default: {
        if (!check(stackSize >= instruction.b, Error::Bounds)) break;
        auto *arguments = stack + stackSize - instruction.b;
        Value result;
        if constexpr (UniformExecution<Physics>::value) {
          if (!physics.uniformBuiltin(instruction.op,*this,arguments,instruction.b,result))
            result=builtin(instruction.op,arguments,instruction.a);
        } else result = builtin(instruction.op, arguments, instruction.a);
        for (std::uint32_t i = 0; i < instruction.b; ++i) memory.release(arguments[i]);
        stackSize -= instruction.b; push(result); break;
      }
      }
    }
    if (memory.error != Error::None) error = memory.error;
    return true;
  }
  FT_VM Result result() {
    return {stackSize ? pop() : Value{}, error, source, operations};
  }
  FT_VM Result execute(std::uint32_t entry, Value argument, std::uint64_t seed) {
    begin(entry,argument,seed);
    resume([](const Instruction &) { return false; });
    return result();
  }
};

} // namespace forevertas::blocks::vm
#include "blocks/program_vm_ops.h"
#undef FT_VM
#endif

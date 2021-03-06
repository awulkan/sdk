// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_H_
#define RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_H_

#include "vm/allocation.h"
#include "vm/bitfield.h"
#include "vm/bitmap.h"
#include "vm/compiler/assembler/assembler.h"
#include "vm/constants.h"
#include "vm/cpu.h"

namespace dart {

class BufferFormatter;
class ConstantInstr;
class Definition;
class Value;

template <class Location>
class TemplatePairLocation;

enum Representation {
  kNoRepresentation,
  kTagged,
  kUntagged,
  kUnboxedDouble,
  kUnboxedFloat,
  kUnboxedInt32,
  kUnboxedUint32,
  kUnboxedInt64,
  kUnboxedFloat32x4,
  kUnboxedInt32x4,
  kUnboxedFloat64x2,
  kPairOfTagged,
  kNumRepresentations
};

// The representation of 8 and 16 bit integers in 32 bit. SmallRepresentation
// tracks the real representation of these small integers.
enum SmallRepresentation {
  kNoSmallRepresentation,
  kSmallUnboxedInt8,
  kSmallUnboxedUint8,
  kSmallUnboxedInt16,
  kSmallUnboxedUint16,
};

// 'UnboxedFfiIntPtr' should be able to hold a pointer of the target word-size.
// On a 32-bit platform, it's an unsigned 32-bit int because it should be
// zero-extended to 64-bits, not sign-extended (pointers are inherently
// unsigned).
//
// Issue(36370): Use [kUnboxedIntPtr] instead.
static constexpr Representation kUnboxedFfiIntPtr =
    compiler::target::kWordSize == 4 ? kUnboxedUint32 : kUnboxedInt64;

// The representation which can be used for native pointers. We use signed 32/64
// bit representation to be able to do arithmetic on pointers.
static constexpr Representation kUnboxedIntPtr =
    compiler::target::kWordSize == 4 ? kUnboxedInt32 : kUnboxedInt64;

class FrameRebase;

// Location objects are used to connect register allocator and code generator.
// Instruction templates used by code generator have a corresponding
// LocationSummary object which specifies expected location for every input
// and output.
// Each location is encoded as a single word: for non-constant locations
// low 4 bits denote location kind, rest is kind specific location payload
// e.g. for REGISTER kind payload is register code (value of the Register
// enumeration), constant locations contain a tagged (low 2 bits are set to 01)
// Object handle.
//
// Locations must satisfy the following invariant: if two locations' encodings
// are bitwise unequal then these two locations are guaranteed to be disjoint.
// Properties like representation belong to the value that is stored in
// the location not to the location itself.
template <class Register, class FpuRegister>
class TemplateLocation : public ValueObject {
 private:
  enum {
    // Number of bits required to encode Kind value.
    kKindBitsPos = 0,
    kKindBitsSize = 5,

    kPayloadBitsPos = kKindBitsPos + kKindBitsSize,
    kPayloadBitsSize = kBitsPerWord - kPayloadBitsPos,
  };

  static const uword kInvalidLocation = 0;
  static const uword kLocationTagMask = 0x3;

 public:
  // Constant payload can overlap with kind field so Kind values
  // have to be chosen in a way that their last 2 bits are never
  // the same as kConstantTag or kPairLocationTag.
  // Note that two locations with different kinds should never point to
  // the same place. For example kQuadStackSlot location should never intersect
  // with kDoubleStackSlot location.
  enum Kind {
    // This location is invalid.  Payload must be zero.
    kInvalid = 0,

    // Constant value. This location contains a tagged Object handle.
    kConstantTag = 1,

    // This location contains a tagged pointer to a PairLocation.
    kPairLocationTag = 2,

    // Unallocated location represents a location that is not fixed and can be
    // allocated by a register allocator.  Each unallocated location has
    // a policy that specifies what kind of location is suitable. Payload
    // contains register allocation policy.
    kUnallocated = 3,

    // Spill slots allocated by the register allocator.  Payload contains
    // a spill index.
    kStackSlot = 4,        // Word size slot.
    kDoubleStackSlot = 7,  // 64bit stack slot.
    kQuadStackSlot = 11,   // 128bit stack slot.

    // Register location represents a fixed register.  Payload contains
    // register code.
    kRegister = 8,

    // FpuRegister location represents a fixed fpu register.  Payload contains
    // its code.
    kFpuRegister = 12,
  };

  TemplateLocation() : value_(kInvalidLocation) {
    // Verify that non-tagged location kinds do not interfere with location tags
    // (kConstantTag and kPairLocationTag).
    COMPILE_ASSERT((kInvalid & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kInvalid & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kUnallocated & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kUnallocated & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kStackSlot & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kStackSlot & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kDoubleStackSlot & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kDoubleStackSlot & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kQuadStackSlot & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kQuadStackSlot & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kRegister & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kRegister & kLocationTagMask) != kPairLocationTag);

    COMPILE_ASSERT((kFpuRegister & kLocationTagMask) != kConstantTag);
    COMPILE_ASSERT((kFpuRegister & kLocationTagMask) != kPairLocationTag);

    // Verify tags and tagmask.
    COMPILE_ASSERT((kConstantTag & kLocationTagMask) == kConstantTag);

    COMPILE_ASSERT((kPairLocationTag & kLocationTagMask) == kPairLocationTag);

    ASSERT(IsInvalid());
  }

  TemplateLocation(const TemplateLocation& other)
      : ValueObject(), value_(other.value_) {}

  TemplateLocation& operator=(const TemplateLocation& other) {
    value_ = other.value_;
    return *this;
  }

  bool IsInvalid() const { return value_ == kInvalidLocation; }

  // Constants.
  bool IsConstant() const {
    return (value_ & kLocationTagMask) == kConstantTag;
  }

  static TemplateLocation Constant(const ConstantInstr* obj) {
    TemplateLocation loc(reinterpret_cast<uword>(obj) | kConstantTag);
    ASSERT(obj == loc.constant_instruction());
    return loc;
  }

  ConstantInstr* constant_instruction() const {
    ASSERT(IsConstant());
    return reinterpret_cast<ConstantInstr*>(value_ & ~kLocationTagMask);
  }

  const Object& constant() const;

  bool IsPairLocation() const {
    return (value_ & kLocationTagMask) == kPairLocationTag;
  }

  static TemplateLocation Pair(TemplateLocation first, TemplateLocation second);

  TemplatePairLocation<TemplateLocation<Register, FpuRegister>>*
  AsPairLocation() const;

  // For pair locations, returns the ith component (for i in {0, 1}).
  TemplateLocation<Register, FpuRegister> Component(intptr_t i) const {
    return AsPairLocation()->At(i);
  }

  // Unallocated locations.
  enum Policy {
    kAny,
    kPrefersRegister,
    kRequiresRegister,
    kRequiresFpuRegister,
    kWritableRegister,
    kSameAsFirstInput,
  };

  bool IsUnallocated() const { return kind() == kUnallocated; }

  bool IsRegisterBeneficial() { return !Equals(Any()); }

  static TemplateLocation UnallocatedLocation(Policy policy) {
    return TemplateLocation(kUnallocated, PolicyField::encode(policy));
  }

  // Any free register is suitable to replace this unallocated location.
  static TemplateLocation Any() { return UnallocatedLocation(kAny); }

  static TemplateLocation PrefersRegister() {
    return UnallocatedLocation(kPrefersRegister);
  }

  static TemplateLocation RequiresRegister() {
    return UnallocatedLocation(kRequiresRegister);
  }

  static TemplateLocation RequiresFpuRegister() {
    return UnallocatedLocation(kRequiresFpuRegister);
  }

  static TemplateLocation WritableRegister() {
    return UnallocatedLocation(kWritableRegister);
  }

  // The location of the first input to the instruction will be
  // used to replace this unallocated location.
  static TemplateLocation SameAsFirstInput() {
    return UnallocatedLocation(kSameAsFirstInput);
  }

  // Empty location. Used if there the location should be ignored.
  static TemplateLocation NoLocation() { return TemplateLocation(); }

  Policy policy() const {
    ASSERT(IsUnallocated());
    return PolicyField::decode(payload());
  }

  // Register locations.
  static TemplateLocation RegisterLocation(Register reg) {
    return TemplateLocation(kRegister, reg);
  }

  bool IsRegister() const { return kind() == kRegister; }

  Register reg() const {
    ASSERT(IsRegister());
    return static_cast<Register>(payload());
  }

  // FpuRegister locations.
  static TemplateLocation FpuRegisterLocation(FpuRegister reg) {
    return TemplateLocation(kFpuRegister, reg);
  }

  bool IsFpuRegister() const { return kind() == kFpuRegister; }

  FpuRegister fpu_reg() const {
    ASSERT(IsFpuRegister());
    return static_cast<FpuRegister>(payload());
  }

  static bool IsMachineRegisterKind(Kind kind) {
    return (kind == kRegister) || (kind == kFpuRegister);
  }

  static TemplateLocation MachineRegisterLocation(Kind kind, intptr_t reg) {
    if (kind == kRegister) {
      return RegisterLocation(static_cast<Register>(reg));
    } else {
      ASSERT(kind == kFpuRegister);
      return FpuRegisterLocation(static_cast<FpuRegister>(reg));
    }
  }

  bool IsMachineRegister() const { return IsMachineRegisterKind(kind()); }

  intptr_t register_code() const {
    ASSERT(IsMachineRegister());
    return static_cast<intptr_t>(payload());
  }

  static uword EncodeStackIndex(intptr_t stack_index) {
    ASSERT((-kStackIndexBias <= stack_index) &&
           (stack_index < kStackIndexBias));
    return static_cast<uword>(kStackIndexBias + stack_index);
  }

  static TemplateLocation StackSlot(intptr_t stack_index, Register base) {
    uword payload = StackSlotBaseField::encode(base) |
                    StackIndexField::encode(EncodeStackIndex(stack_index));
    TemplateLocation loc(kStackSlot, payload);
    // Ensure that sign is preserved.
    ASSERT(loc.stack_index() == stack_index);
    return loc;
  }

  bool IsStackSlot() const { return kind() == kStackSlot; }

  static TemplateLocation DoubleStackSlot(intptr_t stack_index, Register base) {
    uword payload = StackSlotBaseField::encode(base) |
                    StackIndexField::encode(EncodeStackIndex(stack_index));
    TemplateLocation loc(kDoubleStackSlot, payload);
    // Ensure that sign is preserved.
    ASSERT(loc.stack_index() == stack_index);
    return loc;
  }

  bool IsDoubleStackSlot() const { return kind() == kDoubleStackSlot; }

  static TemplateLocation QuadStackSlot(intptr_t stack_index, Register base) {
    uword payload = StackSlotBaseField::encode(base) |
                    StackIndexField::encode(EncodeStackIndex(stack_index));
    TemplateLocation loc(kQuadStackSlot, payload);
    // Ensure that sign is preserved.
    ASSERT(loc.stack_index() == stack_index);
    return loc;
  }

  bool IsQuadStackSlot() const { return kind() == kQuadStackSlot; }

  Register base_reg() const {
    ASSERT(HasStackIndex());
    return StackSlotBaseField::decode(payload());
  }

  intptr_t stack_index() const {
    ASSERT(HasStackIndex());
    // Decode stack index manually to preserve sign.
    return StackIndexField::decode(payload()) - kStackIndexBias;
  }

  bool HasStackIndex() const {
    return IsStackSlot() || IsDoubleStackSlot() || IsQuadStackSlot();
  }

  // Returns the offset from the frame pointer for stack slot locations.
  intptr_t ToStackSlotOffset() const;

  const char* Name() const;
  void PrintTo(BufferFormatter* f) const;
  void Print() const;
  const char* ToCString() const;

  // Compare two locations.
  bool Equals(TemplateLocation other) const { return value_ == other.value_; }

  // If current location is constant might return something that
  // is not equal to any Kind.
  Kind kind() const { return KindField::decode(value_); }

  TemplateLocation Copy() const;

  static TemplateLocation read(uword value) { return TemplateLocation(value); }
  uword write() const { return value_; }

 private:
  explicit TemplateLocation(uword value) : value_(value) {}

  void set_stack_index(intptr_t index) {
    ASSERT(HasStackIndex());
    value_ = PayloadField::update(
        StackIndexField::update(EncodeStackIndex(index), payload()), value_);
  }

  void set_base_reg(Register reg) {
    ASSERT(HasStackIndex());
    value_ = PayloadField::update(StackSlotBaseField::update(reg, payload()),
                                  value_);
  }

  TemplateLocation(Kind kind, uword payload)
      : value_(KindField::encode(kind) | PayloadField::encode(payload)) {}

  uword payload() const { return PayloadField::decode(value_); }

  friend class FrameRebase;

  class KindField : public BitField<uword, Kind, kKindBitsPos, kKindBitsSize> {
  };
  class PayloadField
      : public BitField<uword, uword, kPayloadBitsPos, kPayloadBitsSize> {};

  // Layout for kUnallocated locations payload.
  typedef BitField<uword, Policy, 0, 3> PolicyField;

// Layout for stack slots.
#if defined(ARCH_IS_64_BIT)
  static const intptr_t kBitsForBaseReg = 6;
#else
  static const intptr_t kBitsForBaseReg = 5;
#endif
  static const intptr_t kBitsForStackIndex = kPayloadBitsSize - kBitsForBaseReg;
  class StackSlotBaseField
      : public BitField<uword, Register, 0, kBitsForBaseReg> {};
  class StackIndexField
      : public BitField<uword, intptr_t, kBitsForBaseReg, kBitsForStackIndex> {
  };
  COMPILE_ASSERT(1 << kBitsForBaseReg >= kNumberOfCpuRegisters);

  static const intptr_t kStackIndexBias = static_cast<intptr_t>(1)
                                          << (kBitsForStackIndex - 1);

  // Location either contains kind and payload fields or a tagged handle for
  // a constant locations. Values of enumeration Kind are selected in such a
  // way that none of them can be interpreted as a kConstant tag.
  uword value_;

  // The following functions are only defined for Location, not for
  // HostLocation, but they do need access to private fields or constructors.
  friend TemplateLocation<dart::Register, dart::FpuRegister>
  LocationArgumentsDescriptorLocation();
  friend TemplateLocation<dart::Register, dart::FpuRegister>
  LocationExceptionLocation();
  friend TemplateLocation<dart::Register, dart::FpuRegister>
  LocationStackTraceLocation();
};

using Location = TemplateLocation<dart::Register, dart::FpuRegister>;
using HostLocation =
    TemplateLocation<dart::host::Register, dart::host::FpuRegister>;

// The following functions are only defined for Location, not for HostLocation.
Location LocationArgumentsDescriptorLocation();
Location LocationExceptionLocation();
Location LocationStackTraceLocation();
// Constants.
Location LocationRegisterOrConstant(Value* value);
Location LocationRegisterOrSmiConstant(Value* value);
Location LocationWritableRegisterOrSmiConstant(Value* value);
Location LocationFixedRegisterOrConstant(Value* value, Register reg);
Location LocationFixedRegisterOrSmiConstant(Value* value, Register reg);
Location LocationAnyOrConstant(Value* value);

Location LocationRemapForSlowPath(Location loc,
                                  Definition* def,
                                  intptr_t* cpu_reg_slots,
                                  intptr_t* fpu_reg_slots);

// Return a memory operand for stack slot locations.
compiler::Address LocationToStackSlotAddress(Location loc);

template <class Location>
class TemplatePairLocation : public ZoneAllocated {
 public:
  TemplatePairLocation() {
    for (intptr_t i = 0; i < kPairLength; i++) {
      ASSERT(locations_[i].IsInvalid());
    }
  }

  intptr_t length() const { return kPairLength; }

  Location At(intptr_t i) const {
    ASSERT(i >= 0);
    ASSERT(i < kPairLength);
    return locations_[i];
  }

  void SetAt(intptr_t i, Location loc) {
    ASSERT(i >= 0);
    ASSERT(i < kPairLength);
    locations_[i] = loc;
  }

  Location* SlotAt(intptr_t i) {
    ASSERT(i >= 0);
    ASSERT(i < kPairLength);
    return &locations_[i];
  }

 private:
  static const intptr_t kPairLength = 2;
  Location locations_[kPairLength];
};

using PairLocation = TemplatePairLocation<Location>;
using HostPairLocation = TemplatePairLocation<HostLocation>;

template <typename T>
class SmallSet {
 public:
  SmallSet() : data_(0) {}

  explicit SmallSet(intptr_t data) : data_(data) {}

  bool Contains(T value) const { return (data_ & ToMask(value)) != 0; }

  void Add(T value) { data_ |= ToMask(value); }

  void Remove(T value) { data_ &= ~ToMask(value); }

  bool IsEmpty() const { return data_ == 0; }

  intptr_t data() const { return data_; }

 private:
  static intptr_t ToMask(T value) {
    ASSERT(static_cast<intptr_t>(value) < (kWordSize * kBitsPerByte));
    return 1 << static_cast<intptr_t>(value);
  }

  intptr_t data_;
};

class RegisterSet : public ValueObject {
 public:
  RegisterSet()
      : cpu_registers_(), untagged_cpu_registers_(), fpu_registers_() {
    ASSERT(kNumberOfCpuRegisters <= (kWordSize * kBitsPerByte));
    ASSERT(kNumberOfFpuRegisters <= (kWordSize * kBitsPerByte));
  }

  void AddAllNonReservedRegisters(bool include_fpu_registers) {
    for (intptr_t i = kNumberOfCpuRegisters - 1; i >= 0; --i) {
      if ((kReservedCpuRegisters & (1 << i)) != 0u) continue;
      Add(Location::RegisterLocation(static_cast<Register>(i)));
    }

    if (include_fpu_registers) {
      for (intptr_t i = kNumberOfFpuRegisters - 1; i >= 0; --i) {
        Add(Location::FpuRegisterLocation(static_cast<FpuRegister>(i)));
      }
    }
  }

  // Adds all registers which don't have a special purpose (e.g. FP, SP, PC,
  // CSP, etc.).
  void AddAllGeneralRegisters() {
    for (intptr_t i = kNumberOfCpuRegisters - 1; i >= 0; --i) {
      Register reg = static_cast<Register>(i);
      if (reg == FPREG || reg == SPREG) continue;
#if defined(TARGET_ARCH_ARM)
      if (reg == PC) continue;
#elif defined(TARGET_ARCH_ARM64)
      if (reg == R31) continue;
#endif
      Add(Location::RegisterLocation(reg));
    }

#if defined(TARGET_ARCH_ARM)
    if (TargetCPUFeatures::vfp_supported()) {
#endif
      for (intptr_t i = kNumberOfFpuRegisters - 1; i >= 0; --i) {
        Add(Location::FpuRegisterLocation(static_cast<FpuRegister>(i)));
      }
#if defined(TARGET_ARCH_ARM)
    }
#endif
  }

  void AddAllArgumentRegisters() {
    // All (native) arguments are passed on the stack in IA32.
#if !defined(TARGET_ARCH_IA32)
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; ++i) {
      const Register reg = static_cast<Register>(i);
      if (IsArgumentRegister(reg)) {
        Add(Location::RegisterLocation(reg));
      }
    }
    for (intptr_t i = 0; i < kNumberOfFpuRegisters; ++i) {
      const FpuRegister reg = static_cast<FpuRegister>(i);
      if (IsFpuArgumentRegister(reg)) {
        Add(Location::FpuRegisterLocation(reg));
      }
    }
#endif
  }

  void Add(Location loc, Representation rep = kTagged) {
    if (loc.IsRegister()) {
      cpu_registers_.Add(loc.reg());
      if (rep != kTagged) {
        // CPU register contains an untagged value.
        MarkUntagged(loc);
      }
    } else if (loc.IsFpuRegister()) {
      fpu_registers_.Add(loc.fpu_reg());
    }
  }

  void Remove(Location loc) {
    if (loc.IsRegister()) {
      cpu_registers_.Remove(loc.reg());
    } else if (loc.IsFpuRegister()) {
      fpu_registers_.Remove(loc.fpu_reg());
    }
  }

  bool Contains(Location loc) {
    if (loc.IsRegister()) {
      return ContainsRegister(loc.reg());
    } else if (loc.IsFpuRegister()) {
      return ContainsFpuRegister(loc.fpu_reg());
    } else {
      UNREACHABLE();
      return false;
    }
  }

  void DebugPrint();

  void MarkUntagged(Location loc) {
    ASSERT(loc.IsRegister());
    untagged_cpu_registers_.Add(loc.reg());
  }

  bool HasUntaggedValues() const {
    return !untagged_cpu_registers_.IsEmpty() || !fpu_registers_.IsEmpty();
  }

  bool IsTagged(Register reg) const {
    return !untagged_cpu_registers_.Contains(reg);
  }

  bool ContainsRegister(Register reg) const {
    return cpu_registers_.Contains(reg);
  }

  bool ContainsFpuRegister(FpuRegister fpu_reg) const {
    return fpu_registers_.Contains(fpu_reg);
  }

  intptr_t CpuRegisterCount() const { return RegisterCount(cpu_registers()); }
  intptr_t FpuRegisterCount() const { return RegisterCount(fpu_registers()); }

  static intptr_t RegisterCount(intptr_t registers);
  static bool Contains(intptr_t register_set, intptr_t reg) {
    return (register_set & (1 << reg)) != 0;
  }

  intptr_t cpu_registers() const { return cpu_registers_.data(); }
  intptr_t fpu_registers() const { return fpu_registers_.data(); }

 private:
  SmallSet<Register> cpu_registers_;
  SmallSet<Register> untagged_cpu_registers_;
  SmallSet<FpuRegister> fpu_registers_;

  DISALLOW_COPY_AND_ASSIGN(RegisterSet);
};

// Specification of locations for inputs and output.
class LocationSummary : public ZoneAllocated {
 public:
  enum ContainsCall {
    kNoCall,  // Used registers must be reserved as tmp.
    kCall,    // Registers have been saved and can be used without reservation.
    kCallCalleeSafe,       // Registers will be saved by the callee.
    kCallOnSlowPath,       // Used registers must be reserved as tmp.
    kCallOnSharedSlowPath  // Registers used to invoke shared stub must be
                           // reserved as tmp.
  };

  LocationSummary(Zone* zone,
                  intptr_t input_count,
                  intptr_t temp_count,
                  LocationSummary::ContainsCall contains_call);

  intptr_t input_count() const { return num_inputs_; }

  Location in(intptr_t index) const {
    ASSERT(index >= 0);
    ASSERT(index < num_inputs_);
    return input_locations_[index];
  }

  Location* in_slot(intptr_t index) {
    ASSERT(index >= 0);
    ASSERT(index < num_inputs_);
    return &input_locations_[index];
  }

  void set_in(intptr_t index, Location loc) {
    ASSERT(index >= 0);
    ASSERT(index < num_inputs_);
    // See FlowGraphAllocator::ProcessOneInstruction for explanation of this
    // restriction.
    if (always_calls()) {
      if (loc.IsUnallocated()) {
        ASSERT(loc.policy() == Location::kAny);
      } else if (loc.IsPairLocation()) {
        ASSERT(!loc.AsPairLocation()->At(0).IsUnallocated() ||
               loc.AsPairLocation()->At(0).policy() == Location::kAny);
        ASSERT(!loc.AsPairLocation()->At(0).IsUnallocated() ||
               loc.AsPairLocation()->At(0).policy() == Location::kAny);
      }
    }
    input_locations_[index] = loc;
  }

  intptr_t temp_count() const { return num_temps_; }

  Location temp(intptr_t index) const {
    ASSERT(index >= 0);
    ASSERT(index < num_temps_);
    return temp_locations_[index];
  }

  Location* temp_slot(intptr_t index) {
    ASSERT(index >= 0);
    ASSERT(index < num_temps_);
    return &temp_locations_[index];
  }

  void set_temp(intptr_t index, Location loc) {
    ASSERT(index >= 0);
    ASSERT(index < num_temps_);
    ASSERT(!always_calls() || loc.IsMachineRegister());
    temp_locations_[index] = loc;
  }

  intptr_t output_count() const { return 1; }

  Location out(intptr_t index) const {
    ASSERT(index == 0);
    return output_location_;
  }

  Location* out_slot(intptr_t index) {
    ASSERT(index == 0);
    return &output_location_;
  }

  void set_out(intptr_t index, Location loc) {
    ASSERT(index == 0);
    ASSERT(!always_calls() || (loc.IsMachineRegister() || loc.IsInvalid() ||
                               loc.IsPairLocation()));
    output_location_ = loc;
  }

  BitmapBuilder* stack_bitmap() {
    if (stack_bitmap_ == NULL) {
      stack_bitmap_ = new BitmapBuilder();
    }
    return stack_bitmap_;
  }
  void SetStackBit(intptr_t index) { stack_bitmap()->Set(index, true); }

  bool always_calls() const {
    return contains_call_ == kCall || contains_call_ == kCallCalleeSafe;
  }

  bool callee_safe_call() const { return contains_call_ == kCallCalleeSafe; }

  bool can_call() { return contains_call_ != kNoCall; }

  bool HasCallOnSlowPath() { return can_call() && !always_calls(); }

  bool call_on_shared_slow_path() const {
    return contains_call_ == kCallOnSharedSlowPath;
  }

  void PrintTo(BufferFormatter* f) const;

  static LocationSummary* Make(Zone* zone,
                               intptr_t input_count,
                               Location out,
                               ContainsCall contains_call);

  RegisterSet* live_registers() { return &live_registers_; }

#if defined(DEBUG)
  // Debug only verification that ensures that writable registers are correctly
  // preserved on the slow path.
  void DiscoverWritableInputs();
  void CheckWritableInputs();
#endif

 private:
  const intptr_t num_inputs_;
  Location* input_locations_;
  const intptr_t num_temps_;
  Location* temp_locations_;
  Location output_location_;

  BitmapBuilder* stack_bitmap_;

  const ContainsCall contains_call_;
  RegisterSet live_registers_;

#if defined(DEBUG)
  intptr_t writable_inputs_;
#endif
};

// Describes a change of stack frame where the stack or base register or stack
// offset may change. This class allows easily rebasing stack locations across
// frame manipulations.
//
// If the stack offset register matches 'old_base', it is changed to 'new_base'
// and 'stack_delta' (# of slots) is applied.
class FrameRebase : public ValueObject {
 public:
  FrameRebase(Register old_base, Register new_base, intptr_t stack_delta)
      : old_base_(old_base), new_base_(new_base), stack_delta_(stack_delta) {}

  Location Rebase(Location loc) const {
    if (loc.IsPairLocation()) {
      return Location::Pair(Rebase(loc.Component(0)), Rebase(loc.Component(1)));
    }
    if (!loc.HasStackIndex() || loc.base_reg() != old_base_) {
      return loc;
    }

    loc.set_base_reg(new_base_);
    loc.set_stack_index(loc.stack_index() + stack_delta_);
    return loc;
  }

 private:
  Register old_base_;
  Register new_base_;
  intptr_t stack_delta_;
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_H_

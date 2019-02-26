/*
 * Copyright (c) 2019, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

//===----------------------------------------------------------------------===//
//
/// GenXLiveness
/// ------------
///
/// GenXLiveness is an analysis that contains the liveness information for the
/// values in the code. Unlike the usual LLVM liveness analysis, the values
/// are in LLVM IR rather than machine IR.
///
/// This GenXLiveness pass is a container for the data structures required
/// for liveness analysis, plus methods to perform the analysis. The pass itself
/// does nothing; later passes manipulate it:
///
/// * GenXCategory creates a LiveRange and sets the category on it for each
///   value.
///
/// * GenXLiveRanges calls GenXLiveness to set up the LiveRange for each
///   value that needs one (a non-baled instruction or a function argument),
///   and erases the LiveRange for a value that does not need one (a baled
///   in instruction).
///
/// GenXLiveness is a FunctionGroupPass, because we want to share liveness
/// information between all the Functions in a FunctionGroup (i.e. between a
/// GenX kernel/function and its subroutines). Any pass that uses GenXLiveness,
/// which is almost all passes that run after it, must itself be a
/// FunctionGroupPass.
///
/// Here is what a LiveRange might look like if you dump() it in the debugger,
/// or see it as part of the liveness info in a -print-after-all:
///
/// ``add12.split48172:[145,199){general,align32}``
///
/// * ``add12.split48172`` is the Value attached to the LiveRange. As outlined below,
///   a LiveRange actually has SimpleValues rather than Values; if the attached
///   SimpleValue had been an element of a struct rather than a scalar value in
///   its own right, the name would have had # then the flattened index appended.
///
/// * A LiveRange can have more than one value attached after GenXCoalescing.
///   This would be shown by multiple comma-separated names.
///
/// * ``[145,199)`` is the segment in which the LiveRange is live. A LiveRange can
///   have multiple segments. This one is a normal (strong) segment; a weak one has
///   the start number prefixed with 'w' and a phicpy one has the start number
///   prefixed with 'ph'.
///
/// * ``general`` is the register category of the LiveRange.
///
/// * ``align32`` shows that the LiveRange has been marked as needing to be 32
///   byte (i.e. GRF) aligned.
///
/// * If the LiveRange was a kernel argument, its allocated offset would have
///   been shown with the word 'offset'.
///
/// SimpleValue
/// ^^^^^^^^^^^
///
/// Liveness information deals with SimpleValues rather than Values.
/// SimpleValue (a GenX backend specific class) is the entity that can have
/// a live range attached and a register allocated. A SimpleValue is either a
/// non-struct Value, or a non-struct element of a struct Value (where the
/// struct can contain nested structs).
///
/// A SimpleValue is represented by a pair:
///
/// - a Value *
/// - a flattened index for a non-struct element of a struct, otherwise 0
///
/// Having a flattened index (as generated by IndexFlattener::flatten()) allows
/// us to encode an element in multiply nested structs with a single index.
///
/// The idea of SimpleValue is that, where the LLVM IR contains a struct value,
/// which is unavoidable when a function has multiple return values, we want
/// to allocate a register to each non-struct element, not the whole struct.
///
/// Segments
/// ^^^^^^^^
///
/// A live range consists of one or more non-overlapping *segments*, where each
/// segment has a start (inclusive) and end (exclusive) instruction number, and a
/// strength, which is strong (normal), weak (see below) or phicpy (see below).
/// Two segments cannot be abutting if they have the same
/// strength. Later passes can interrogate this information to find out whether
/// two live ranges interfere, and can modify it by coalescing (merging) two
/// live ranges. After coalescing, multiple SimpleValues share the same live
/// range.
///
/// The numbering of instructions is handled in GenXNumbering.
///
/// Weak liveness
/// ^^^^^^^^^^^^^
/// 
/// A live range that extends over a call has the entire range of the called
/// subroutine, and any subroutines it can call, added to it. This makes that
/// live range interfere with any live range inside the subroutine, and thus
/// stops them using the same register.
///
/// However, because a subroutine has a single range in instruction numbering,
/// rather than one range per call site, this scheme means that two values A
/// and B that are live over two *different* call sites of the same subroutine
/// both include the subroutine's range, and thus look like they interfere.
/// This could stop A and B being coalesced, and thus add extra code and
/// register pressure.
///
/// To fix this, we have the concept of *weak liveness*. The values A and B
/// are only weakly live inside the subroutine. Two values are considered to
/// interfere only if there is some point where both are live, and at least
/// one of them is not weakly live at that point.
///
/// Thus, in our A and B example, A and B each interferes with any value inside
/// the subroutine, but not with each other.
///
/// Phicpy liveness
/// ^^^^^^^^^^^^^^^
///
/// A phi node has a short segment of liveness (a *phicpy segment*) at the end
/// of each of its incoming blocks, from the phi copy insertion point up to the
/// end of the block. The use of the incoming value in the phi node is counted
/// as being at that phi copy insertion point.
///
/// Normally, we split critical edges, so an incoming block to a phi node has
/// only the one successor, and the use of the incoming value at the phi copy
/// insertion point is a kill use. Often, the phi node and the incoming can be
/// coalesced, unless there is some interference elsewhere due to other values
/// previously coalesced into the two live ranges.
///
/// However, in one case (a goto/join branching to a join), we cannot split the
/// critical edge. Thus the phi copy insertion point is before the conditional
/// branch in a block with two successors, and the incoming value is likely to
/// be used in the other successor too. Then, there is interference between the
/// phi node and the incoming value, even though they could be coalesced.
///
/// To avoid this problem, each phicpy segment in a live range is marked as
/// such. A phicpy segment is valid only if there is no segment abutting it
/// before; if there is an abutting before segment, the coalescing code turns it
/// into a normal strong segment and merges the two together.
///
/// Then, interference between two live ranges LR1 and LR2 is ignored if:
///
/// 1. the interference arises between a phicpy segment in LR1 and a normal
///    (strong) segment in LR2; and
///
/// 2. the start of the phicpy segment is the phi copy insertion point where the
///    phi node is in LR1 and the incoming value is in LR2.
///
/// This then allows the incoming value and the phi node to be coalesced, even
/// if the incoming value is also used in the branch's other successor.
///
//===----------------------------------------------------------------------===//
#ifndef GENXLIVENESS_H
#define GENXLIVENESS_H

#include "FunctionGroup.h"
#include "IgnoreRAUWValueMap.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {

class BasicBlock;
class BitCastInst;
class CallInst;
class Function;
class FunctionPass;
class GenXBaling;
class GenXLiveness;
class GenXNumbering;
class Instruction;
class PHINode;
class raw_ostream;
class ReturnInst;
class Value;

FunctionGroupPass *createGenXGroupPrinterPass(raw_ostream &O, const std::string &Banner);

namespace genx {

class Bale;

/***********************************************************************
 * IndexFlattener : a class containing some (static) utility functions to
 * convert between struct indices (as found in an extractelement instruction)
 * and a flattened index, in which a struct containing further structs is
 * flattened as if it is a single struct containing just the non-struct
 * elements.
 *
 * SimpleValue uses this to encode and decode its flattened index.
 * Liveness and coalescing use flattenArg and getNumArgElements to calculate
 * live ranges for function args at the call sites.
 */
struct IndexFlattener {
  // flatten : convert struct indices into a flattened index
  static unsigned flatten(StructType *ST, ArrayRef<unsigned> Indices);
  // getNumElements : get the number of non-struct elements in the flattened
  // struct. Returns 1 if it is not a struct type, but 0 for void type.
  static unsigned getNumElements(Type *Ty) {
    if (auto ST = dyn_cast<StructType>(Ty))
      return flatten(ST, ST->getNumElements());
    return !Ty->isVoidTy();
  }
  // unflatten : convert a flattened index back into normal struct indices
  static unsigned unflatten(StructType *ST, unsigned Unflattened, SmallVectorImpl<unsigned> *Indices);
  // getElementType : get type of struct element from flattened index
  static Type *getElementType(Type *Ty, unsigned FlattenedIndex);
  // flattenArg : flatten an arg in a function or call, i.e. calculate the
  //    total number of flattened indices used up by previous args. If all
  //    previous args are not struct type, then this just returns the arg
  //    index
  static unsigned flattenArg(FunctionType *FT, unsigned ArgIndex);
  // getNumArgElements : get the number of non-struct elements in all args
  //    of the function
  static unsigned getNumArgElements(FunctionType *FT) {
    return flattenArg(FT, FT->getNumParams());
  }
};

class AssertingSV;

/***********************************************************************
 * SimpleValue : a non-struct value, possibly inside a struct
 * See comment at the top of the file.
 */
class SimpleValue {
  Value *V;
  unsigned Index; // flattened struct index
public:
  SimpleValue() : V(nullptr), Index(0) {}
  // Constructor from a non-struct value
  SimpleValue(Value *V) : V(V), Index(0) {}
  // Constructor from a struct value and an already flattened index
  SimpleValue(Value *V, unsigned Index) : V(V), Index(Index) {}
  // Constructor from a struct value and unflattened indices (as found in extractelement)
  SimpleValue(Value *V, ArrayRef<unsigned> Indices) : V(V),
    Index(IndexFlattener::flatten(cast<StructType>(V->getType()), Indices)) {}
  // Accessors
  Value *getValue() const { return V; }
  unsigned getIndex() const { return Index; }
  // getType : get the type of the (element) value
  Type *getType();
  // Comparisons
  bool operator==(SimpleValue Rhs) const { return V == Rhs.V && Index == Rhs.Index; }
  bool operator!=(SimpleValue Rhs) const { return !(*this == Rhs); }
  bool operator<(SimpleValue Rhs) const {
    if (V != Rhs.V)
      return V < Rhs.V;
    return Index < Rhs.Index;
  }
  // Debug dump/print
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const;
#endif
  void print(raw_ostream &OS) const;
  void printName(raw_ostream &OS) const;
};

inline raw_ostream &operator<<(raw_ostream &OS, SimpleValue V) {
  V.print(OS);
  return OS;
}

// AssertingSV : like a SimpleValue, but contains an AssertingVH
class AssertingSV {
  AssertingVH<Value> V;
  unsigned Index;
public:
  AssertingSV(SimpleValue SV) : V(SV.getValue()), Index(SV.getIndex()) {}
  SimpleValue get() const { return SimpleValue(V, Index); }
  Value *getValue() const { return V; }
  unsigned getIndex() const { return Index; }
  Type *getType() const { return get().getType(); }
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { get().dump(); }
#endif
  void print(raw_ostream &OS) const { get().print(OS); }
  void printName(raw_ostream &OS) const { get().printName(OS); }
};

// Segment : a single range of instruction numbers in which a value is
// live
struct Segment {
  enum { WEAK, PHICPY, STRONG };
  unsigned Start :30; // inclusive start of range
  unsigned End :30; // exclusive end of range
  unsigned Strength :2; // whether it is a weak or phicpy or strong segment
  Segment() : Start(0), End(0), Strength(STRONG) {}
  Segment(unsigned S, unsigned E) : Start(S), End(E), Strength(STRONG) {}
  Segment(unsigned S, unsigned E, unsigned Strength) : Start(S), End(E), Strength(Strength) {}
  bool operator<(Segment Rhs) const {
    if (Start != Rhs.Start)
      return Start < Rhs.Start;
    return End < Rhs.End;
  }
  bool isWeak() { return Strength == WEAK; }
};

// LiveRange : a collection of Segment structs, in order, describing
// all points in the program in which a value is live.
// Also contains a list of each SimpleValue that points to this LiveRange.
// Also a bitmap of register classes (general, surface, etc) that
// its def and uses need.
class LiveRange {
  friend class llvm::GenXLiveness;
  typedef SmallVector<Segment, 2> Segments_t;
  Segments_t Segments;
  typedef SmallVector<AssertingSV, 2> Values_t;
  Values_t Values;
public:
  unsigned Category :8;
  unsigned LogAlignment :7;
  bool DisallowCASC: 1; // disallow call arg special coalescing
  unsigned Offset :12; // kernel arg offset, else 0
  LiveRange() : Category(0), LogAlignment(0), DisallowCASC(false), Offset(0) {}
  // Iterator forwarders for Segments
  typedef Segments_t::iterator iterator;
  typedef Segments_t::const_iterator const_iterator;
  iterator begin() { return Segments.begin(); }
  iterator end() { return Segments.end(); }
  const_iterator begin() const { return Segments.begin(); }
  const_iterator end() const { return Segments.end(); }
  unsigned size() { return Segments.size(); }
  void resize(unsigned len) { Segments.resize(len); }
  // Iterator forwarders for Values.
  // This is complicated by the Values vector containing AssertingSV, but the
  // iterator wants to dereference to a Simplevalue.
  class value_iterator {
    Values_t::iterator i;
  public:
    value_iterator(Values_t::iterator i) : i(i) {}
    SimpleValue operator*() { return i->get(); }
    AssertingSV *operator->() { return i; }
    bool operator==(const value_iterator &Rhs) const { return i == Rhs.i; }
    bool operator!=(const value_iterator &Rhs) const { return !(*this == Rhs); }
    value_iterator &operator++() { ++i; return *this; }
  };
  value_iterator value_begin() { return Values.begin(); }
  value_iterator value_end() { return Values.end(); }
  unsigned value_size() { return Values.size(); }
  bool value_empty() { return Values.empty(); }
  // find : return iterator to segment containing Num (including the case
  // of being equal to the segment's End), or, if in a hole, the
  // iterator of the next segment, or, if at end, end().
  iterator find(unsigned Num);
  void clear() { Segments.clear(); Values.clear(); }
  void push_back(Segment Seg) { Segments.push_back(Seg); }
  void push_back(unsigned S, unsigned E) { Segments.push_back(Segment(S, E)); }
  SimpleValue addValue(SimpleValue V) { Values.push_back(V); return V; }
  // contains : test whether live range contains instruction number
  bool contains(unsigned Num) {
    iterator i = find(Num);
    return i != end() && i->End != Num && i->Start <= Num;
  }
  // getCategory : get the LR's register category
  unsigned getCategory() { return Category; }
  // setCategory : set the LR's register category
  void setCategory(unsigned Cat) { Category = Cat; }
  // getOrDefaultCategory : return category; if none, set default
  unsigned getOrDefaultCategory();
  // getLogAlignment : get log alignment
  unsigned getLogAlignment() { return LogAlignment; }
  // setAlignmentFromValue : increase alignment if necessary from a value
  void setAlignmentFromValue(SimpleValue V);
  // setLogAlignment : set log alignment to greater than implied by the LR's values
  void setLogAlignment(unsigned Align) { LogAlignment = std::max(LogAlignment, Align); }
  // addSegment : add a segment to a live range
  void addSegment(Segment Seg);
  // setSegmentsFrom : for this live range, clear out its segments
  //    and copy them from the other live range
  void setSegmentsFrom(LiveRange *Other);
  // addSegments : add segments from another LR to this one
  void addSegments(LiveRange *LR2);
  // sortAndMerge : after doing some push_backs, sort the segments
  //    and merge overlapping/adjacent ones
  void sortAndMerge();
  // getLength : add up the number of instructions covered by this LR
  unsigned getLength(bool WithWeak);
  // debug dump/print
  void dump() const;
  void print(raw_ostream &OS) const;
  void printSegments(raw_ostream &OS) const;
private:
  void value_clear() { Values.clear(); }
#ifndef NDEBUG
  // assertOk : assert that live range's segments are well formed
  void assertOk();
#else
  void assertOk() {}
#endif
};

inline raw_ostream &operator<<(raw_ostream &OS, const LiveRange &LR) {
  LR.print(OS);
  return OS;
}

// CallGraph : the call graph within a FunctionGroup
class CallGraph {
  FunctionGroup *FG;
public:
  class Node;
  struct Edge {
    unsigned Number;
    CallInst *Call;
    Node *Callee;
    bool operator==(Edge Rhs) const { return Number == Rhs.Number; }
    bool operator!=(Edge Rhs) const { return !(*this == Rhs); }
    bool operator<(Edge Rhs) const { return Number < Rhs.Number; }
    Edge() : Number(0), Call(0) {}
    Edge(unsigned Number, CallInst *Call) : Number(Number), Call(Call) {}
  };
  class Node {
    std::set<Edge> Edges;
  public:
    typedef std::set<Edge>::iterator iterator;
    iterator begin() { return Edges.begin(); }
    iterator end() { return Edges.end(); }
    void insert(Edge E) { Edges.insert(E); }
  };
private:
  std::map<Function *, Node> Nodes;
public:
  // constructor from FunctionGroup
  CallGraph(FunctionGroup *FG) : FG(FG) {}
  // build : build the call graph from the FunctionGroup
  void build(GenXLiveness *Liveness);

  // getRoot : get the root node
  Node *getRoot() { return &Nodes[FG->getHead()]; }
  // getNode : get the node for a Function
  Node *getNode(Function *F) { return &Nodes[F]; }
};

} // end namespace genx

class GenXLiveness : public FunctionGroupPass {
  FunctionGroup *FG;
  typedef std::map<genx::SimpleValue, genx::LiveRange *> LiveRangeMap_t;
  LiveRangeMap_t LiveRangeMap;
  genx::CallGraph *CG;
  GenXBaling *Baling;
  GenXNumbering *Numbering;
  std::map<Function *, Value *> UnifiedRets;
  std::map<Value *, Function *> UnifiedRetToFunc;
  std::map<AssertingVH<Value>, Value *> ArgAddressBaseMap;
public:
  static char ID;
  explicit GenXLiveness() : FunctionGroupPass(ID), CG(0), Baling(0), Numbering(0) { }
  ~GenXLiveness() { clear(); }
  virtual StringRef getPassName() const { return "GenX liveness analysis"; }
  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnFunctionGroup(FunctionGroup &FG);
  // setBaling : tell GenXLiveness where GenXBaling is
  void setBaling(GenXBaling *B) { Baling = B; }
  // Iterator forwarders.
  // This gives you an iterator of LiveRangeMap. The ->first field is the
  // value, and you only get each value once. The ->second field is the
  // LiveRange pointer, and you may get each one multiple times because
  // a live range may contain multiple values.
  typedef LiveRangeMap_t::iterator iterator;
  typedef LiveRangeMap_t::const_iterator const_iterator;
  iterator begin() { return LiveRangeMap.begin(); }
  iterator end() { return LiveRangeMap.end(); }
  const_iterator begin() const { return LiveRangeMap.begin(); }
  const_iterator end() const { return LiveRangeMap.end(); }
  // getLiveRange : get the live range for a Value of non-struct type
  genx::LiveRange *getLiveRange(Value *V) { return getLiveRange(genx::SimpleValue(V)); }
  // getLiveRange : get the live range for a genx::SimpleValue
  genx::LiveRange *getLiveRange(genx::SimpleValue V);
  // getLiveRangeOrNull : get the live range for a Value, or 0 if none
  genx::LiveRange *getLiveRangeOrNull(genx::SimpleValue V);
  // getOrCreateLiveRange : get the live range for a Value, or create
  // a new one if none
  genx::LiveRange *getOrCreateLiveRange(genx::SimpleValue V);
  // eraseLiveRange : get rid of live range for a Value, possibly multiple
  //  ones if it is a struct value
  void eraseLiveRange(Value *V);
  // eraseLiveRange : get rid of live range for a SimpleValue, if any.
  // It is assumed that the LiveRange (if any) has no other value atached.
  void eraseLiveRange(genx::SimpleValue V);
  // eraseLiveRange : get rid of the specified live range, and remove its
  // values from the map
  void eraseLiveRange(genx::LiveRange *LR);
  // twoAddrInterfere : check whether two live ranges interfere, allowing for single number interference sites at two address ops
  bool twoAddrInterfere(genx::LiveRange *LR1, genx::LiveRange *LR2);
  // interfere : test whether two live ranges interfere
  bool interfere(genx::LiveRange *LR1, genx::LiveRange *LR2);
  // getSingleInterferenceSites : check whether two live ranges interfere, returning single number interference sites
  bool getSingleInterferenceSites(genx::LiveRange *LR1, genx::LiveRange *LR2, SmallVectorImpl<unsigned> *Sites);
  // checkIfOverlappingSegmentsInterfere : given two segments that have been
  //    shown to overlap, check whether their strengths make them interfere
  bool checkIfOverlappingSegmentsInterfere(genx::LiveRange *LR1, genx::Segment *S1, genx::LiveRange *LR2, genx::Segment *S2);
  // coalesce : coalesce two live ranges
  genx::LiveRange *coalesce(genx::LiveRange *LR1, genx::LiveRange *LR2, bool DisallowCASC);
  // Set the GenXNumbering pointer for use by live range building
  void setNumbering(GenXNumbering *N) { Numbering = N; }
  GenXNumbering *getNumbering() { return Numbering; }
  // rebuildCallGraph : rebuild GenXLiveness's call graph
  void rebuildCallGraph();
  // buildSubroutineLRs : build an LR for each subroutine. Must be called
  //    before the first BuildLiveRange
  void buildSubroutineLRs();
  // buildLiveRange : build live range for given value if it is simple,
  // or one for each flattened index if it is struct type
  void buildLiveRange(Value *V);
  // buildLiveRange : build live range for given value
  genx::LiveRange *buildLiveRange(genx::SimpleValue V);
  // rebuildLiveRange : rebuild a live range that only has one value
  void rebuildLiveRange(genx::LiveRange *LR);
  // removeBale : remove the bale from its live range, and delete the range if
  // it now has no values.
  void removeBale(genx::Bale &B);
  // removeValue : remove the value from its live range, and delete the
  // range if it now has no values
  void removeValue(Value *V);
  void removeValue(genx::SimpleValue V);
  // removeValue : remove the value from its live range. Do not delete the
  // LR if it now has no values.
  genx::LiveRange *removeValueNoDelete(genx::SimpleValue V);
  // removeValuesNoDelete : remove all values from the live range, but do not
  // delete the LR
  void removeValuesNoDelete(genx::LiveRange *LR);
  // replaceValue : update liveness such that NewVal has OldVal's live range,
  // and OldVal does not have one at all.
  void replaceValue(Value *OldVal, Value *NewVal);
  void replaceValue(genx::SimpleValue OldVal, genx::SimpleValue(NewVal));
  // Set the LiveRange for a value in the map
  void setLiveRange(genx::SimpleValue V, genx::LiveRange *LR);
  // Get/create the unified return value for a function
  Value *getUnifiedRet(Function *F);
  // Test whether a value is a unified return value (and return its Function).
  Function *isUnifiedRet(Value *V);
  // Move unified return value from OldF to NewF.
  void moveUnifiedRet(Function *OldF, Function *NewF);
  // copyInterfere : test whether two live ranges copy-interfere
  bool copyInterfere(genx::LiveRange *LR1, genx::LiveRange *LR2);
  // See if V1 is a phi node and V2 wraps round to a phi use in the same BB after V1's def
  static bool wrapsAround(Value *V1, Value *V2);
  // Insert a copy of a non-struct value.
  Instruction *insertCopy(Value *InputVal, genx::LiveRange *LR, Instruction *InsertBefore, const Twine &Name, unsigned Number);
  // eraseUnusedTree : erase unused tree of instructions, and remove from GenXLiveness
  void eraseUnusedTree(Instruction *Inst);
  // setArgAddressBase : set the base value of an argument indirect address
  void setArgAddressBase(Value *Addr, Value *Base) { ArgAddressBaseMap[Addr] = Base; }
  // getAddressBase : get the base register of an address
  Value *getAddressBase(Value *Addr);
  // isBitCastCoalesced : see if the bitcast has been coalesced away
  bool isBitCastCoalesced(BitCastInst *BCI);
  // createPrinterPass : get a pass to print the IR, together with the GenX
  // specific analyses
  virtual Pass *createPrinterPass(raw_ostream &O, const std::string &Banner) const
  { return createGenXGroupPrinterPass(O, Banner); }
  // Debug dump
  void dump();
  using Pass::print; // Indicates we aren't replacing base class version of print
  virtual void print(raw_ostream &OS) const;
  virtual void releaseMemory() override { clear(); }

private:
  void clear();
  unsigned numberInstructionsInFunc(Function *Func, unsigned Num);
  unsigned getPhiOffset(PHINode *Phi) const;
  void rebuildLiveRangeForValue(genx::LiveRange *LR, genx::SimpleValue SV);
  genx::LiveRange *visitPropagateSLRs(Function *F);
  void merge(genx::LiveRange *LR1, genx::LiveRange *LR2);
};

void initializeGenXLivenessPass(PassRegistry &);

// Specialize DenseMapInfo for SimpleValue.
template <> struct DenseMapInfo<genx::SimpleValue> {
  static inline genx::SimpleValue getEmptyKey() {
    return genx::SimpleValue(DenseMapInfo<Value *>::getEmptyKey());
  }
  static inline genx::SimpleValue getTombstoneKey() {
    return genx::SimpleValue(DenseMapInfo<Value *>::getTombstoneKey());
  }
  static unsigned getHashValue(const genx::SimpleValue &SV) {
    return DenseMapInfo<Value *>::getHashValue(SV.getValue()) ^
           DenseMapInfo<unsigned>::getHashValue(SV.getIndex());
  }
  static bool isEqual(const genx::SimpleValue &LHS,
                      const genx::SimpleValue &RHS) {
    return LHS == RHS;
  }
};

} // end namespace llvm
#endif // GENXLIVENESS_H

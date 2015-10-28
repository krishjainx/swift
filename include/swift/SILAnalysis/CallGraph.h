//===------ CallGraph.h - The Call Graph Data Structure  -------*- C++ -*--===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_CALLGRAPH_H
#define SWIFT_SIL_CALLGRAPH_H

#include "swift/Basic/ArrayRefView.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILWitnessTable.h"
#include "llvm/Support/Allocator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <utility>

namespace swift {

static inline bool canHaveIndirectUses(SILFunction *F) {
  if (F->isPossiblyUsedExternally())
    return true;

  // ObjC functions are called through the runtime and are therefore alive
  // even if not referenced inside SIL.
  if (canBeCalledIndirectly(F->getLoweredFunctionType()->getRepresentation()))
    return true;

  return false;
}

class CallGraphNode;

class CallGraphEdge {
  // FIXME: this should be private
public:
  // TODO: Consider increasing SmallSize when we handle method calls.
  typedef llvm::SmallSetVector<CallGraphNode *, 2> CallGraphNodeSet;
  typedef llvm::PointerIntPair<CallGraphNodeSet *, 1> CalleeSet;

public:
  // Representation of the callees that this call site can call into.
  class Callees {
    friend class CallGraphEdge;

    static SILFunction *getFunctionFromNode(CallGraphNode *const &Node);

    typedef llvm::PointerUnion<CallGraphNode *, CalleeSet> CalleeStorage;
    typedef ArrayRefView<CallGraphNode *,
                         SILFunction *,
                         getFunctionFromNode> CalleeFunctionView;

    CalleeStorage TheCallees;

    explicit Callees(CallGraphNode *Node) : TheCallees(Node) {}
    explicit Callees(CalleeSet TheCallees) : TheCallees(TheCallees) {}

  public:
    CalleeFunctionView::iterator begin() const {
      return getFunctionView().begin();
    }

    CalleeFunctionView::iterator end() const {
      return getFunctionView().end();
    }

    bool canCallUnknownFunction() const {
      if (TheCallees.is<CalleeSet>())
        return TheCallees.get<CalleeSet>().getInt();

      if (TheCallees.get<CallGraphNode *>())
        return false;

      // We use nullptr to represent the case where we have no
      // information about the callees.
      return true;
    }

  private:
    CalleeFunctionView getFunctionView() const {
      // Make a view over the entire callee set.
      if (TheCallees.is<CalleeSet>()) {
        auto *Set = TheCallees.get<CalleeSet>().getPointer();
        return llvm::makeArrayRef(Set->begin(),
                                  Set->end());
      }

      auto *Node = TheCallees.get<CallGraphNode *>();
      // Create a singleton view.
      if (Node)
        return llvm::makeArrayRef(TheCallees.getAddrOfPtr1(), 1);

      // Create an empty view.
      return llvm::makeArrayRef((CallGraphNode **) nullptr, 0);
    }
  };


  // The call site represented by this call graph edge.
  FullApplySite TheApply;

  // The set of things we know this apply can call into.
  Callees CallsiteCallees;

  // A unique identifier for this edge based on the order in which it
  // was created relative to other edges.
  unsigned Ordinal;

public:
  /// Create a call graph edge for a call site with a single known
  /// callee.
  CallGraphEdge(FullApplySite TheApply, CallGraphNode *Node, unsigned Ordinal)
    : TheApply(TheApply), CallsiteCallees(Node), Ordinal(Ordinal) {
    assert(Node != nullptr && "Expected non-null callee node!");
  }

  /// Create a call graph edge for a call site for which we are not
  /// currently able to determine the callees.
  CallGraphEdge(FullApplySite TheApply, unsigned Ordinal)
    : TheApply(TheApply),
      CallsiteCallees((CallGraphNode *) nullptr),
      Ordinal(Ordinal) {
  }

  /// Create a call graph edge for a call site where we will fill in
  /// the set of potentially called functions later.
  CallGraphEdge(FullApplySite TheApply, CalleeSet KnownCallees,
                unsigned Ordinal)
    : TheApply(TheApply),
      CallsiteCallees(KnownCallees),
      Ordinal(Ordinal) {
  }

  ~CallGraphEdge() {
  }

  FullApplySite getApply() const { return TheApply; }

  /// Return the callee set.
  CallGraphNodeSet getCalleeSet() const {
    auto TheCallees = CallsiteCallees.TheCallees;
    if (TheCallees.is<CalleeSet>())
      return *TheCallees.get<CalleeSet>().getPointer();

    CallGraphNodeSet Result;
    if (auto *Node = TheCallees.get<CallGraphNode *>()) {
      assert(Node &&
             "Cannot get callee set for callsite with no known callees!");
      Result.insert(Node);
    }

    return Result;
  }

  Callees getCallees() const {
    return CallsiteCallees;
  }

  /// Return true if this edge represents a call that could reach an
  /// unknown function with a matching signature.
  bool canCallUnknownFunction() const {
    return getCallees().canCallUnknownFunction();
  }

  /// The apply has a complete callee set, and it's of size one. In
  /// other words we can replace its callee with a function_ref
  /// regardless of what kind of instruction the callee is now.
  bool hasSingleCallee() const {
    return getSingleCalleeOrNull() != nullptr;
  }

  /// Gets the single callee if the apply has one.
  CallGraphNode *getSingleCalleeOrNull() const {
    if (canCallUnknownFunction())
      return nullptr;

    if (CallsiteCallees.TheCallees.is<CallGraphNode *>())
      return CallsiteCallees.TheCallees.get<CallGraphNode *>();

    auto CS = CallsiteCallees.TheCallees.get<CalleeSet>();
    if (CS.getPointer()->size() != 1)
      return nullptr;

    return *CS.getPointer()->begin();
  }

  unsigned getOrdinal() const {
    return Ordinal;
  }

  void print(llvm::raw_ostream &OS, int Indent) const;
  void dump(int Indent) const;
  void dump() const;
};

class CallGraphNode {
  /// The function represented by this call graph node.
  SILFunction *Function;

  /// The call graph node ordinal within the SILModule.
  const unsigned Ordinal;

  /// Edges representing the known call sites that could call into
  /// this function.
  ///
  /// This is owned by the callgraph itself, not the callgraph node.
  llvm::SmallPtrSet<CallGraphEdge *, 4> CallerEdges;

  /// Edges representing the call sites within this function.
  ///
  /// This is owned by the callgraph itself, not the callgraph node.
  llvm::SmallPtrSet<CallGraphEdge *, 4> CalleeEdges;

  /// Do we know all the potential callers of this function?
  bool CallerEdgesComplete;

  /// May this function bind dynamic Self at one of its call sites? This is
  /// conservatively correct because it may stay on after edges are removed.
  bool MayBindDynamicSelf;

public:
  friend class CallGraph;

  CallGraphNode(SILFunction *Function, unsigned Ordinal)
    : Function(Function), Ordinal(Ordinal),
      CallerEdgesComplete(!canHaveIndirectUses(Function)),
      MayBindDynamicSelf(false) {
    assert(Function &&
           "Cannot build a call graph node with a null function pointer!");
  }

  ~CallGraphNode() = default;

  SILFunction *getFunction() const {
    return Function;
  }

  /// Get the known set of call graph edges that represent possible
  /// calls into this function.
  const llvm::SmallPtrSetImpl<CallGraphEdge *> &getCallerEdges() const {
    return CallerEdges;
  }

  /// Get the set of call sites in this function.
  const llvm::SmallPtrSetImpl<CallGraphEdge *> &getCalleeEdges() const {
    return CalleeEdges;
  }

  /// Do we know that the set of call sites is complete - i.e. that
  /// there is no other place that we can call from that can reach
  /// this function?
  bool isCallerEdgesComplete() const {
    return CallerEdgesComplete;
  }

  /// May this function bind dynamic Self at one of its call sites?
  bool mayBindDynamicSelf() const {
    return MayBindDynamicSelf;
  }

  /// Is this call graph node for a function that we can trivially
  /// know is dead?
  bool isTriviallyDead() const {
    return isCallerEdgesComplete() && getCallerEdges().empty();
  }

  unsigned getOrdinal() const {
    return Ordinal;
  }

  void print(llvm::raw_ostream &OS) const;
  void dump() const;

private:
  /// Mark a set of callers as known to not be complete.
  void markCallerEdgesIncomplete() {
    CallerEdgesComplete = false;
  }

  /// Add an edge representing a call site within this function.
  void addCalleeEdge(CallGraphEdge *Edge) {
    CalleeEdges.insert(Edge);
  }

  /// Remove an edge representing a call site within this function.
  void removeCalleeEdge(CallGraphEdge *Edge) {
    assert(CalleeEdges.count(Edge) && "Expected edge to be in set!");
    CalleeEdges.erase(Edge);
  }

  /// Add an edge representing a call site that calls into this function.
  void addCallerEdge(CallGraphEdge *Edge) {
    CallerEdges.insert(Edge);
  }

  /// Remove an edge representing a call site that calls into this function.
  void removeCallerEdge(CallGraphEdge *Edge) {
    assert(CallerEdges.count(Edge) && "Expected edge to be in set!");
    CallerEdges.erase(Edge);
  }
};

struct CallGraphSCC {
  llvm::TinyPtrVector<SILFunction *> SCCNodes;
};

class CallGraph {
  /// The module that this call graph belongs to.
  SILModule &M;

  /// A map from a function to the function's node in the call graph.
  llvm::DenseMap<SILFunction *, CallGraphNode *> FunctionToNodeMap;

  /// A map from an apply inst to its call edge in the call graph.
  llvm::DenseMap<FullApplySite , CallGraphEdge *> ApplyToEdgeMap;

  /// A vector of SCCs in bottom up SCC order.
  llvm::SmallVector<CallGraphSCC *, 16> BottomUpSCCOrder;

  /// A vector of functions in bottom up function order.
  llvm::SmallVector<SILFunction *, 32> BottomUpFunctionOrder;

  typedef llvm::DenseMap<AbstractFunctionDecl *,
                         CallGraphEdge::CalleeSet> CalleeSetMap;

  /// Map from function decls for methods to sets of CallGraphNodes
  /// representing functions that can be reached via that decl.
  CalleeSetMap CalleeSetCache;

  /// An allocator used by the callgraph.
  llvm::BumpPtrAllocator Allocator;

  /// Ordinal incremented for each node we add.
  unsigned NodeOrdinal;

  /// Ordinal incremented for each edge we add.
  unsigned EdgeOrdinal;

public:
#ifndef NDEBUG
  friend struct OrderedCallGraph;
#endif

  friend class CallGraphEditor;

  CallGraph(SILModule *M, bool completeModule);
  ~CallGraph();

  // Functions for getting bottom-up lists of SCCs or functions in the
  // call graph.
  ArrayRef<CallGraphSCC *> getBottomUpSCCOrder() {
    if (BottomUpSCCOrder.empty())
      computeBottomUpSCCOrder();

    return BottomUpSCCOrder;
  }

  /// Forces recomputation of the bottom-up function list.
  void invalidateBottomUpFunctionOrder() { BottomUpFunctionOrder.clear(); }

  ArrayRef<SILFunction *> getBottomUpFunctionOrder() {
    if (BottomUpFunctionOrder.empty())
      computeBottomUpFunctionOrder();

    return BottomUpFunctionOrder;
  }

  // Call graph queries on functions.

  /// May this function bind dynamic Self at one of its call sites?
  bool mayBindDynamicSelf(SILFunction *F) const {
    auto *Node = getCallGraphNode(F);
    assert(Node && "Expected call graph node for function!");
    return Node->mayBindDynamicSelf();
  }

  /// Get the known set of call graph edges that represent possible
  /// calls into a function.
  const llvm::SmallPtrSetImpl<CallGraphEdge *> &
  getCallerEdges(SILFunction *F) const {
    return getCallGraphNode(F)->getCallerEdges();
  }

  /// Do we know all the callers of this function?
  bool allCallersKnown(SILFunction *F) const {
    return getCallGraphNode(F)->isCallerEdgesComplete();
  }

  // Call graph queries on call sites.

  bool canCallUnknownFunction(FullApplySite FAS) const {
    return getCallGraphEdge(FAS)->canCallUnknownFunction();
  }

  /// Return the callee set for the given call site.
  CallGraphEdge::CallGraphNodeSet
  getCalleeSet(FullApplySite FAS) const {
    return getCallGraphEdge(FAS)->getCalleeSet();
  }

  /// Return the callees for the given call site.
  CallGraphEdge::Callees getCallees(FullApplySite FAS) const {
    return getCallGraphEdge(FAS)->getCallees();
  }

  /// Is this call site known to call exactly one single function?
  bool hasSingleCallee(FullApplySite FAS) const {
    return getCallGraphEdge(FAS)->hasSingleCallee();
  }

  // Printing/dumping functionality.

  void print(llvm::raw_ostream &OS);
  void printStats(llvm::raw_ostream &OS);
  void dump();
  void dumpStats();

  /// This function is meant for use from the debugger.  You can just say 'call
  /// CG->viewCG()' and a dot graph viewer window should pop up from the
  /// program, displaying the call graph. This depends on there being a dot
  /// graph viewer program, like 'graphviz', in your path.
  void viewCG();

  void verify() const;

  void verify(SILFunction *F) const;
private:
  // Functions for editing an existing call graph.

  void addEdgesForApply(FullApplySite AI) {
    addEdgesForApply(AI, getCallGraphNode(AI.getFunction()));
  }

  /// Removes a node from the graph. The node must not have any
  /// caller or callee edges.
  void removeNode(CallGraphNode *Node);

  void removeEdge(CallGraphEdge *Edge);
  void removeEdgesForApply(FullApplySite AI);

  // Query funtions for getting nodes and edges from the call graph.

  const CallGraphNode *getCallGraphNode(SILFunction *F) const {
    return const_cast<CallGraph *>(this)->getCallGraphNode(F);
  }

  CallGraphNode *getCallGraphNode(SILFunction *F) {
    auto *CGN = tryGetCallGraphNode(F);
    assert(CGN && "Expected call graph node for function!");
    return CGN;
  }

  CallGraphEdge *getCallGraphEdge(FullApplySite AI) {
    auto Found = ApplyToEdgeMap.find(AI);
    if (Found == ApplyToEdgeMap.end())
      return nullptr;

    assert(Found->second && "Unexpected null call graph edge in map!");
    return Found->second;
  }

  CallGraphEdge *getCallGraphEdge(FullApplySite AI) const {
    return const_cast<CallGraph *>(this)->getCallGraphEdge(AI);
  }

  CallGraphEdge::CalleeSet &getOrCreateCalleeSetForMethod(SILDeclRef Decl);

  CallGraphNode *tryGetCallGraphNode(SILFunction *F) const {
    return const_cast<CallGraph *>(this)->tryGetCallGraphNode(F);
  }

  CallGraphNode *tryGetCallGraphNode(SILFunction *F) {
    auto Found = FunctionToNodeMap.find(F);
    if (Found == FunctionToNodeMap.end())
      return nullptr;

    assert(Found->second && "Unexpected null call graph node in map!");
    return Found->second;
  }

  CallGraphNode *getOrAddCallGraphNode(SILFunction *F) {
    if (auto *CGN = tryGetCallGraphNode(F))
      return CGN;
    return addCallGraphNode(F);
  }

  void computeMethodCallees();
  void computeClassMethodCalleesForClass(ClassDecl *CD);
  void computeWitnessMethodCalleesForWitnessTable(SILWitnessTable &WTable);

  CallGraphNode *addCallGraphNode(SILFunction *F);
  void addEdges(SILFunction *F);
  CallGraphEdge *makeCallGraphEdgeForCallee(FullApplySite Apply,
                                            SILValue Callee);
  void addEdgesForApply(FullApplySite AI, CallGraphNode *CallerNode);
  void clearBottomUpSCCOrder();
  void computeBottomUpSCCOrder();
  void computeBottomUpFunctionOrder();
};

class CallGraphEditor {
  CallGraph *CG;
public:
  CallGraphEditor(CallGraph *CG) : CG(CG) {}

  void replaceApplyWithNew(FullApplySite Old, FullApplySite New);
  void replaceApplyWithNew(FullApplySite Old,
                           llvm::SmallVectorImpl<FullApplySite> &NewApplies);

  /// Detaches the call graph node from function \p Old and attaches it to
  /// function \a New.
  void moveNodeToNewFunction(SILFunction *Old, SILFunction *New);

  /// Removes all callee edges from function
  void removeAllCalleeEdgesFrom(SILFunction *F);

  /// Creates a new node for function \p F and adds callee edges for all
  /// full apply sites in the function.
  void addNewFunction(SILFunction *F) {
    if (CG && !CG->tryGetCallGraphNode(F)) {
      CG->addCallGraphNode(F);
      CG->addEdges(F);
    }
  }

  /// Removes the call graph node of function \p F. The node may have any
  /// adjacent caller or callee edges.
  void removeCallGraphNode(SILFunction *F) {
    if (CG)
      CG->removeNode(CG->getCallGraphNode(F));
  }

  /// Removes edges from the apply site \p AI.
  void removeEdgesForApply(FullApplySite AI) {
    if (CG)
      CG->removeEdgesForApply(AI);
  }

  /// Checks which function(s) are called by apply site \p AI and adds
  /// edges to \a AI.
  void addEdgesForApply(FullApplySite AI) {
    if (CG)
      CG->addEdgesForApply(AI);
  }

  /// Update uses of a changed apply site which is not a full apply site.
  /// If a use is a full apply site, its call graph edge is updated.
  void updatePartialApplyUses(ApplySite AI);

  void addEdgesForFunction(SILFunction *F) {
    if (CG)
      CG->addEdges(F);
  }

  void removeEdgeIfPresent(SILInstruction *I) {
    if (CG) {
      if (auto AI = FullApplySite::isa(I))
        if (auto *Edge = CG->getCallGraphEdge(AI))
          CG->removeEdge(Edge);
    }
  }

  /// Recomputes the callee sets. This should be done whenever a method is
  /// deleted.
  void updateCalleeSets() {
    if (CG)
      CG->computeMethodCallees();
  }

  /// Drops all references in function and removes the references to
  /// instructions in the function from the call graph.
  void dropAllReferences(SILFunction *F) {
    F->dropAllReferences();

    if (CG)
      CallGraphEditor(CG).removeAllCalleeEdgesFrom(F);
  }

  /// Erase the function from the module and any references to it from
  /// the call graph.
  void eraseFunction(SILFunction *F);
};

class CallGraphLinkerEditor {
  CallGraph *CG;

  void callback(SILFunction *F) {
    CallGraphEditor(CG).addEdgesForFunction(F);
  }

public:
  CallGraphLinkerEditor(CallGraph *CG) : CG(CG) {}

  std::function<void(SILFunction *)> getCallback() {
    return std::bind(&CallGraphLinkerEditor::callback, this,
                     std::placeholders::_1);
  }
};

} // end namespace swift

#endif

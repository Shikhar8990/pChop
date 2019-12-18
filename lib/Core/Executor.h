//===-- Executor.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Class to perform actual execution, hides implementation details from external
// interpreter.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTOR_H
#define KLEE_EXECUTOR_H

#include "klee/ExecutionState.h"
#include "klee/Interpreter.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/util/ArrayCache.h"
#include "llvm/Support/raw_ostream.h"
#include "PrefixTree.h"

#include "llvm/ADT/Twine.h"

#include "klee/Internal/Analysis/ReachabilityAnalysis.h"
#include "klee/Internal/Analysis/Inliner.h"
#include "klee/Internal/Analysis/AAPass.h"
#include "klee/Internal/Analysis/ModRefAnalysis.h"
#include "klee/Internal/Analysis/Cloner.h"
#include "klee/Internal/Analysis/SliceGenerator.h"
#include "klee/Internal/Analysis/Annotator.h"

#include <vector>
#include <string>
#include <map>
#include <set>
#include <fstream>
#include <ostream>
#include <mpi.h>

struct KTest;

namespace llvm {
  class BasicBlock;
  class BranchInst;
  class CallInst;
  class Constant;
  class ConstantExpr;
  class Function;
  class GlobalValue;
  class Instruction;
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
  class TargetData;
#else
  class DataLayout;
#endif
  class Twine;
  class Value;
}

namespace klee {  
  class Array;
  struct Cell;
  class ExecutionState;
  class ExternalDispatcher;
  class Expr;
  class InstructionInfoTable;
  struct KFunction;
  struct KInstruction;
  class KInstIterator;
  class KModule;
  class MemoryManager;
  class MemoryObject;
  class ObjectState;
  class PTree;
  class Searcher;
  class SeedInfo;
  class SpecialFunctionHandler;
  struct StackFrame;
  class StatsTracker;
  class TimingSolver;
  class TreeStreamWriter;
  template<class T> class ref;

  /// \todo Add a context object to keep track of data only live
  /// during an instruction step. Should contain addedStates,
  /// removedStates, and haltExecution, among others.

class Executor : public Interpreter {
  friend class BumpMergingSearcher;
  friend class MergingSearcher;
  friend class RandomPathSearcher;
  friend class OwningSearcher;
  friend class WeightedRandomSearcher;
  friend class RandomRecoveryPath;
  friend class SpecialFunctionHandler;
  friend class StatsTracker;

public:
  class Timer {
  public:
    Timer();
    virtual ~Timer();

    /// The event callback.
    virtual void run() = 0;
  };

  typedef std::pair<ExecutionState*,ExecutionState*> StatePair;

  enum TerminateReason {
    Abort,
    Assert,
    Exec,
    External,
    Free,
    Model,
    Overflow,
    Ptr,
    ReadOnly,
    ReportError,
    User,
    Unhandled
  };

private:
  static const char *TerminateReasonNames[];

  class TimerInfo;

  KModule *kmodule;
  InterpreterHandler *interpreterHandler;
  Searcher *searcher;

  ExternalDispatcher *externalDispatcher;
  TimingSolver *solver;
  MemoryManager *memory;
  std::set<ExecutionState*> states;
  StatsTracker *statsTracker;
  TreeStreamWriter *pathWriter, *symPathWriter;
  SpecialFunctionHandler *specialFunctionHandler;
  std::vector<TimerInfo*> timers;
  PTree *processTree;
  PrefixTree *prefixTree;

  /// Used to track states that have been added during the current
  /// instructions step. 
  /// \invariant \ref addedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::vector<ExecutionState *> addedStates;
  /// Used to track states that have been removed during the current
  /// instructions step. 
  /// \invariant \ref removedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::vector<ExecutionState *> removedStates;

  /// Suspended because of prefix ranging  
  std::vector<ExecutionState *> rangingSuspendedStates;
  
  /// map from the prefix to the suspended states
  std::map<std::string, ExecutionState*> prefixSuspendedStatesMap;

  /// When non-empty the Executor is running in "seed" mode. The
  /// states in this map will be executed in an arbitrary order
  /// (outside the normal search interface) until they terminate. When
  /// the states reach a symbolic branch then either direction that
  /// satisfies one or more seeds will be added to this map. What
  /// happens with other states (that don't satisfy the seeds) depends
  /// on as-yet-to-be-determined flags.
  std::map<ExecutionState*, std::vector<SeedInfo> > seedMap;
  
  /// Map of globals to their representative memory object.
  std::map<const llvm::GlobalValue*, MemoryObject*> globalObjects;

  /// Map of globals to their bound address. This also includes
  /// globals that have no representative object (i.e. functions).
  std::map<const llvm::GlobalValue*, ref<ConstantExpr> > globalAddresses;

  /// The set of legal function addresses, used to validate function
  /// pointers. We use the actual Function* address as the function address.
  std::set<uint64_t> legalFunctions;

  /// When non-null the bindings that will be used for calls to
  /// klee_make_symbolic in order replay.
  const struct KTest *replayKTest;
  /// When non-null a list of branch decisions to be used for replay.
  const std::vector<bool> *replayPath;
  /// The index into the current \ref replayKTest or \ref replayPath
  /// object.
  unsigned replayPosition;

  /// When non-null a list of "seed" inputs which will be used to
  /// drive execution.
  const std::vector<struct KTest *> *usingSeeds;  

  /// Disables forking, instead a random path is chosen. Enabled as
  /// needed to control memory usage. \see fork()
  bool atMemoryLimit;

  /// Disables forking, set by client. \see setInhibitForking()
  bool inhibitForking;

  /// Signals the executor to halt execution at the next instruction
  /// step.
  bool haltExecution;

  /// Signal from the master to stop executio
  bool haltFromMaster;

  /// Whether implied-value concretization is enabled. Currently
  /// false, it is buggy (it needs to validate its writes).
  bool ivcEnabled;

  /// The maximum time to allow for a single core solver query.
  /// (e.g. for a single STP query)
  double coreSolverTimeout;

  /// Assumes ownership of the created array objects
  ArrayCache arrayCache;

  /// File to print executed instructions to
  llvm::raw_ostream *debugInstFile;

  // @brief Buffer used by logBuffer
  std::string debugBufferString;

  /// path file
  std::string treepathFile;

  /// offloadStates
  unsigned int numOffloadStates;

  /// number of prefixes
  unsigned int numPrefixes;

  /// set for non recovery states
  std::set<ExecutionState*> nonRecoveryStates;

  int cntNumStates2Offload;

  /// set of states to offload
  std::set<ExecutionState*> state2Offload;

  // @brief buffer to store logs before flushing to file
  llvm::raw_string_ostream debugLogBuffer;

  /* TODO: ... */
  std::vector<ExecutionState *> suspendedStates;
  std::vector<ExecutionState *> resumedStates;
  ReachabilityAnalysis *ra;
  Inliner *inliner;
  AAPass *aa;
  ModRefAnalysis *mra;
  Cloner *cloner;
  SliceGenerator *sliceGenerator;

  unsigned int errorCount;

  llvm::raw_ostream *logFile;
 
  //PSE Vars 
  //prefixFiltering
  bool enablePathPrefixFilter;
  bool enableBranchHalt;
  unsigned int explorationDepth;
  unsigned int prefixDepth;
  unsigned int branchLevel2Halt;
  bool enableLB;
  bool ready2Offload;

  ///MPI_WorkerID
  int coreId;
  
  /// search mode
  std::string searchMode;

  /// offload request
  MPI_Request offloadReq;

  ///executor seeks an offload request
  bool waiting4OffloadReq;

  // branch history
  std::string brhistFileName;
  std::ofstream brhistFile;

  //flag to prevent
  bool coreInitialized; 

  char* pathPrefix;
  char* upperBound;
  char* lowerBound;

  //logFile
  std::string logFileName;
  std::ofstream mylogFile;

	//worklist of states which were halted cause they reached a certain depth
  //each element in the worklist is a vector which contains the halted branch
  //histories
  char** workList;
  std::vector<unsigned int> workListPathSize;
 
  llvm::Function* getTargetFunction(llvm::Value *calledVal,
                                    ExecutionState &state);
  
  void executeInstruction(ExecutionState &state, KInstruction *ki);

  void printFileLine(ExecutionState &state, KInstruction *ki,
                     llvm::raw_ostream &file);

	void run(ExecutionState &initialState,
            bool branchLevelHalt=false,
            bool pathPrefix=false);

  // Given a concrete object in our [klee's] address space, add it to 
  // objects checked code can reference.
  MemoryObject *addExternalObject(ExecutionState &state, void *addr, 
                                  unsigned size, bool isReadOnly);

  void initializeGlobalObject(ExecutionState &state, ObjectState *os, 
			      const llvm::Constant *c,
			      unsigned offset);
  void initializeGlobals(ExecutionState &state);

  void stepInstruction(ExecutionState &state);
  void updateStates(ExecutionState *current);
  void transferToBasicBlock(llvm::BasicBlock *dst, 
			    llvm::BasicBlock *src,
			    ExecutionState &state);

  void callExternalFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector< ref<Expr> > &arguments);

  ObjectState *bindObjectInState(ExecutionState &state, const MemoryObject *mo,
                                 bool isLocal, const Array *array = 0);

  /// Resolve a pointer to the memory objects it could point to the
  /// start of, forking execution when necessary and generating errors
  /// for pointers to invalid locations (either out of bounds or
  /// address inside the middle of objects).
  ///
  /// \param results[out] A list of ((MemoryObject,ObjectState),
  /// state) pairs for each object the given address can point to the
  /// beginning of.
  typedef std::vector< std::pair<std::pair<const MemoryObject*, const ObjectState*>, 
                                 ExecutionState*> > ExactResolutionList;
  void resolveExact(ExecutionState &state,
                    ref<Expr> p,
                    ExactResolutionList &results,
                    const std::string &name);

  /// Allocate and bind a new object in a particular state. NOTE: This
  /// function may fork.
  ///
  /// \param isLocal Flag to indicate if the object should be
  /// automatically deallocated on function return (this also makes it
  /// illegal to free directly).
  ///
  /// \param target Value at which to bind the base address of the new
  /// object.
  ///
  /// \param reallocFrom If non-zero and the allocation succeeds,
  /// initialize the new object from the given one and unbind it when
  /// done (realloc semantics). The initialized bytes will be the
  /// minimum of the size of the old and new objects, with remaining
  /// bytes initialized as specified by zeroMemory.
  void executeAlloc(ExecutionState &state,
                    ref<Expr> size,
                    bool isLocal,
                    KInstruction *target,
                    bool zeroMemory=false,
                    const ObjectState *reallocFrom=0);

  /// Free the given address with checking for errors. If target is
  /// given it will be bound to 0 in the resulting states (this is a
  /// convenience for realloc). Note that this function can cause the
  /// state to fork and that \ref state cannot be safely accessed
  /// afterwards.
  void executeFree(ExecutionState &state,
                   ref<Expr> address,
                   KInstruction *target = 0);
  
  void executeCall(ExecutionState &state, 
                   KInstruction *ki,
                   llvm::Function *f,
                   std::vector< ref<Expr> > &arguments);
                   
  // do address resolution / object binding / out of bounds checking
  // and perform the operation
  void executeMemoryOperation(ExecutionState &state,
                              bool isWrite,
                              ref<Expr> address,
                              ref<Expr> value /* undef if read */,
                              KInstruction *target /* undef if write */);

  void executeMakeSymbolic(ExecutionState &state, const MemoryObject *mo,
                           const std::string &name);

  /// Create a new state where each input condition has been added as
  /// a constraint and return the results. The input state is included
  /// as one of the results. Note that the output vector may included
  /// NULL pointers for states which were unable to be created.
  int branch(ExecutionState &state, 
              const std::vector< ref<Expr> > &conditions,
              std::vector<ExecutionState*> &result);

  // Fork current and return states in which condition holds / does
  // not hold, respectively. One of the states is necessarily the
  // current state, and one of the states may be null.
  StatePair fork(ExecutionState &current, ref<Expr> condition, bool isInternal);

  /// Add the given (boolean) condition as a constraint on state. This
  /// function is a wrapper around the state's addConstraint function
  /// which also manages propagation of implied values,
  /// validity checks, and seed patching.
  void addConstraint(ExecutionState &state, ref<Expr> condition);

  // Called on [for now] concrete reads, replaces constant with a symbolic
  // Used for testing.
  ref<Expr> replaceReadWithSymbolic(ExecutionState &state, ref<Expr> e);

  const Cell& eval(KInstruction *ki, unsigned index, 
                   ExecutionState &state) const;

  Cell& getArgumentCell(ExecutionState &state,
                        KFunction *kf,
                        unsigned index) {
    return state.stack.back().locals[kf->getArgRegister(index)];
  }

  Cell& getDestCell(ExecutionState &state,
                    KInstruction *target) {
    return state.stack.back().locals[target->dest];
  }

  void bindLocal(KInstruction *target, 
                 ExecutionState &state, 
                 ref<Expr> value);
  void bindArgument(KFunction *kf, 
                    unsigned index,
                    ExecutionState &state,
                    ref<Expr> value);

  ref<klee::ConstantExpr> evalConstantExpr(const llvm::ConstantExpr *ce);

  /// Return a unique constant value for the given expression in the
  /// given state, if it has one (i.e. it provably only has a single
  /// value). Otherwise return the original expression.
  ref<Expr> toUnique(const ExecutionState &state, ref<Expr> &e);

  /// Return a constant value for the given expression, forcing it to
  /// be constant in the given state by adding a constraint if
  /// necessary. Note that this function breaks completeness and
  /// should generally be avoided.
  ///
  /// \param purpose An identify string to printed in case of concretization.
  ref<klee::ConstantExpr> toConstant(ExecutionState &state, ref<Expr> e, 
                                     const char *purpose);

  /// Bind a constant value for e to the given target. NOTE: This
  /// function may fork state if the state has multiple seeds.
  void executeGetValue(ExecutionState &state, ref<Expr> e, KInstruction *target);

  /// Get textual information regarding a memory address.
  std::string getAddressInfo(ExecutionState &state, ref<Expr> address) const;

  // Determines the \param lastInstruction of the \param state which is not KLEE
  // internal and returns its InstructionInfo
  const InstructionInfo & getLastNonKleeInternalInstruction(const ExecutionState &state,
      llvm::Instruction** lastInstruction);

  bool shouldExitOn(enum TerminateReason termReason);

  // remove state from queue and delete
  void terminateState(ExecutionState &state);
  // call exit handler and terminate state
  void terminateStateEarly(ExecutionState &state, const llvm::Twine &message);
  // call exit handler and terminate state
  void terminateStateOnExit(ExecutionState &state);
  // call error handler and terminate state
  void terminateStateOnError(ExecutionState &state, const llvm::Twine &message,
                             enum TerminateReason termReason,
                             const char *suffix = NULL,
                             const llvm::Twine &longMessage = "");

  // call error handler and terminate state, for execution errors
  // (things that should not be possible, like illegal instruction or
  // unlowered instrinsic, or are unsupported, like inline assembly)
  void terminateStateOnExecError(ExecutionState &state, 
                                 const llvm::Twine &message,
                                 const llvm::Twine &info="") {
    terminateStateOnError(state, message, Exec, NULL, info);
  }

  /// bindModuleConstants - Initialize the module constant table.
  void bindModuleConstants();

  template <typename TypeIt>
  void computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie);

  /// bindInstructionConstants - Initialize any necessary per instruction
  /// constant values.
  void bindInstructionConstants(KInstruction *KI);

  void handlePointsToObj(ExecutionState &state, 
                         KInstruction *target, 
                         const std::vector<ref<Expr> > &arguments);

  void doImpliedValueConcretization(ExecutionState &state,
                                    ref<Expr> e,
                                    ref<ConstantExpr> value);

  /// Add a timer to be executed periodically.
  ///
  /// \param timer The timer object to run on firings.
  /// \param rate The approximate delay (in seconds) between firings.
  void addTimer(Timer *timer, double rate);

  void initTimers();
  void processTimers(ExecutionState *current,
                     double maxInstTime);
  void checkMemoryUsage();
  void printDebugInstructions(ExecutionState &state);
  void doDumpStates();

  bool isMayBlockingLoad(ExecutionState &state, KInstruction *ki);
  bool isRecoveryRequired(ExecutionState &state, KInstruction *ki);
  bool handleMayBlockingLoad(ExecutionState &state, KInstruction *ki,
                             bool &success);
  bool getAllRecoveryInfo(ExecutionState &state, KInstruction *kinst,
                          std::list<ref<RecoveryInfo> > &result);
  bool getLoadInfo(ExecutionState &state, KInstruction *kinst,
                   uint64_t &loadAddr, uint64_t &loadSize,
                   ModRefAnalysis::AllocSite &allocSite);
  void suspendState(ExecutionState &state);
  void resumeState(ExecutionState &state, bool implicitlyCreated, ExecutionState &recState);
  void notifyDependentState(ExecutionState &recoveryState);
  void onRecoveryStateExit(ExecutionState &state);
  void startRecoveryState(ExecutionState &state, ref<RecoveryInfo> recoveryInfo);
  void onRecoveryStateWrite(
    ExecutionState &state,
    ref<Expr> address,
    const MemoryObject *mo,
    ref<Expr> offset,
    ref<Expr> value
  );
  void onNormalStateWrite(
    ExecutionState &state,
    ref<Expr> address,
    ref<Expr> value
  );
  bool isOverridingStore(KInstruction *kinst);
  void onNormalStateRead(
    ExecutionState &state,
    ref<Expr> address,
    Expr::Width width
  );
  void dumpConstrains(ExecutionState &state);
  MemoryObject *onExecuteAlloc(ExecutionState &state, uint64_t size, bool isLocal, llvm::Instruction *allocInst, 
      bool zeroMemory, unsigned id);
  bool isDynamicAlloc(llvm::Instruction *allocInst);
  void onExecuteFree(ExecutionState *state, const MemoryObject *mo);
  void terminateStateRecursively(ExecutionState &state);
  void mergeConstraints(ExecutionState &dependedState, ref<Expr> condition);
  bool isFunctionToSkip(ExecutionState &state, llvm::Function *f);
  bool canSkipCallSite(ExecutionState &state, llvm::Function *f);
  void bindAll(ExecutionState *state, MemoryObject *mo, bool isLocal, bool zeroMemory);
  void unbindAll(ExecutionState *state, const MemoryObject *mo);
  void forkDependentStates(ExecutionState *trueState, ExecutionState *falseState);
  void mergeConstraintsForAll(ExecutionState &recoveryState, ref<Expr> condition);
  llvm::Function *getSlice(llvm::Function *target, uint32_t sliceId, ModRefAnalysis::SideEffectType type, uint32_t subId);
  ExecutionState *createSnapshotState(ExecutionState &state);

  //PSE Functions
  bool checkRange(std::vector<unsigned char> inPath);
  int convertPath2Number(std::vector<unsigned char> inPath, int upto=0);
  int convertPath2Number(char* inPath, int upto=0);
  void printPath(char* path, std::ostream& log, std::string message);
  void printStatePath(ExecutionState& state, std::ostream& log, std::string message);
  void replicateBranchHist(ExecutionState* state, ExecutionState* recState);
  bool addState2WorkList(ExecutionState &state,int count);
  ExecutionState* offLoad(bool &valid);
  ExecutionState* offloadFromStatesVector(bool &valid);
  int offloadFromStatesVector(std::vector<ExecutionState*>& offloadVec);
  ExecutionState* offloadOriginatingStates(bool &valid);
  void check2Offload();
  void newCheck2Offload();
  void printBranchHist(ExecutionState* state);

public:
  Executor(InterpreterOptions &opts, InterpreterHandler *ie);
  virtual ~Executor();

  const InterpreterHandler& getHandler() {
    return *interpreterHandler;
  }

  // XXX should just be moved out to utility module
  ref<klee::ConstantExpr> evalConstant(const llvm::Constant *c);

  virtual void setPathWriter(TreeStreamWriter *tsw) {
    pathWriter = tsw;
  }
  virtual void setSymbolicPathWriter(TreeStreamWriter *tsw) {
    symPathWriter = tsw;
  }

  virtual void setReplayKTest(const struct KTest *out) {
    assert(!replayPath && "cannot replay both buffer and path");
    replayKTest = out;
    replayPosition = 0;
  }

  virtual void setReplayPath(const std::vector<bool> *path) {
    assert(!replayKTest && "cannot replay both buffer and path");
    replayPath = path;
    replayPosition = 0;
  }

  //PSE Virtuals
  virtual void setUpperBound(char* path) {
    upperBound = path;
  }
  virtual void setLowerBound(char* path) {
    lowerBound = path;
  }
	virtual void setExplorationDepth(const int inExplorationDepth) {
      explorationDepth = inExplorationDepth;
  }

  virtual void setBrHistFile(std::string inBrHistFile) {
  	brhistFileName = inBrHistFile;
   	brhistFile.open(brhistFileName);
  }

  virtual void enableLoadBalancing(bool inLB) {
    enableLB = inLB;
  }

  virtual void setTestPrefixDepth(unsigned inPD) {
      prefixDepth = inPD;
  }

  typedef std::pair<unsigned, uint64_t> PSEAllocSite;
  typedef std::pair<std::string, PSEAllocSite> PSEModInfo;
  typedef std::map<PSEModInfo, uint32_t> PSEModInfoToIdMap;

  typedef std::pair<std::string, uint64_t> PSEAllocSiteG;
  typedef std::pair<std::string, PSEAllocSiteG> PSEModInfoG;
  typedef std::map<PSEModInfoG, uint32_t> PSEModInfoToIdMapG;

  typedef std::map<std::string, std::set<unsigned>> PSEModSetMap;
  typedef std::map<unsigned, std::pair<std::set<PSEModInfo>, std::set<PSEModInfoG>>> PSELoadToModInfoMap;

  PSEModSetMap pseModSetMap;
  PSEModInfoToIdMap pseModInfoToIdMap;
  PSEModInfoToIdMapG pseModInfoToIdMapG;
  std::set<unsigned> blockingLoads;
  std::set<unsigned> overrridingStores;
  PSELoadToModInfoMap pseLoadToModInfoMap;

  virtual const llvm::Module *
  setModule(llvm::Module *module, const ModuleOptions &opts);

  virtual void useSeeds(const std::vector<struct KTest *> *seeds) { 
    usingSeeds = seeds;
  }

  virtual void setPathFile(std::string inPathFile) {
    treepathFile = inPathFile;
    //pathWriter = new TreeStreamWriter(treepathFile);
  }

	virtual void enablePrefixChecking() {
		enablePathPrefixFilter = true;
	}

	virtual void setSearchMode(std::string inSearchMode) {
		searchMode = inSearchMode;
	}

  virtual void runFunctionAsMain(llvm::Function *f,
                                  int argc,
                                  char **argv,
                                  char **envp,
																  bool branchLevelHalt=false);

	virtual char** runFunctionAsMain2(llvm::Function *f,
   	      	                      int argc,
    	                            char **argv,
      	                          char **envp,
                                  //char **workList_main,
                                  std::vector<unsigned int> &workListPathSize_main);


  virtual void setLogFile(std::string inLogFile) {
    logFileName = inLogFile;
    mylogFile.open(logFileName);
  }

  /*** Runtime options ***/
  
  virtual void setHaltExecution(bool value) {
    haltExecution = value;
  }

  virtual void setInhibitForking(bool value) {
    inhibitForking = value;
  }

  virtual void setDataFlowAnalysisStructures(PSEModInfoToIdMap& inPseModInfoToIdMap,
                                            PSEModInfoToIdMapG& inPseModInfoToIdMapG,
                                            PSEModSetMap& inPseModSetMap,
                                            std::set<unsigned>& inBlockingLoads,
                                            std::set<unsigned>& inOverridingStores,
                                            PSELoadToModInfoMap& inPseLoadToModInfoMap) {
    pseModInfoToIdMap = inPseModInfoToIdMap;
    pseModInfoToIdMapG = inPseModInfoToIdMapG;
    pseModSetMap = inPseModSetMap;
    blockingLoads = inBlockingLoads;
    overrridingStores = inOverridingStores;
    pseLoadToModInfoMap = inPseLoadToModInfoMap;
  }

  /*** State accessor methods ***/

  virtual unsigned getPathStreamID(const ExecutionState &state);

  virtual unsigned getSymbolicPathStreamID(const ExecutionState &state);

  virtual void getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                Interpreter::LogType logFormat = Interpreter::STP);

  virtual bool getSymbolicSolution(const ExecutionState &state, 
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res);

  virtual void getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res);

  Expr::Width getWidthForLLVMType(LLVM_TYPE_Q llvm::Type *type) const;
};
  
} // End klee namespace

#endif

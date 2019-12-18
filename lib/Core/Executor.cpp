//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "ExecutorTimerInfo.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/CommandLine.h"
#include "klee/Common.h"
#include "klee/ASContext.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprSMTLIBPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/GetElementPtrTypeIterator.h"
#include "klee/Config/Version.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/System/MemoryUsage.h"
#include "klee/Internal/Support/Debug.h"
#include "klee/SolverStats.h"

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/IR/Function.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/User.h"
#else
#include "llvm/Attributes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
#include "llvm/Target/TargetData.h"
#else
#include "llvm/DataLayout.h"
#include "llvm/TypeBuilder.h"
#endif
#endif
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
#include "llvm/Support/CallSite.h"
#else
#include "llvm/IR/CallSite.h"
#endif

#include "llvm/PassManager.h"

#include "klee/Internal/Analysis/ReachabilityAnalysis.h"
#include "klee/Internal/Analysis/Inliner.h"
#include "klee/Internal/Analysis/AAPass.h"
#include "klee/Internal/Analysis/ModRefAnalysis.h"
#include "klee/Internal/Analysis/Cloner.h"
#include "klee/Internal/Analysis/SliceGenerator.h"

#ifdef HAVE_ZLIB_H
#include "klee/Internal/Support/CompressionStream.h"
#endif

#include <cassert>
#include <algorithm>
#include <iomanip>
#include <iosfwd>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <sys/mman.h>

#include <errno.h>
#include <cxxabi.h>

#define ENABLE_LOGGING false
#define ENABLE_OFFLOAD_LOGGING false

#define START_PREFIX_TASK 0
#define KILL 1
#define FINISH 2
#define OFFLOAD 3
#define OFFLOAD_RESP 4
#define BUG_FOUND 5
#define TIMEOUT 6
#define NORMAL_TASK 7
#define KILL_COMP 8
#define READY_TO_OFFLOAD 9
#define NOT_READY_TO_OFFLOAD 10

#define PREFIX_MODE 101
#define RANGE_MODE 102
#define NO_MODE 103

#define MASTER_NODE 0

#define OFFLOAD_READY_THRESH 8
#define OFFLOAD_NOT_READY_THRESH 4


using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  DumpStatesOnHalt("dump-states-on-halt",
                   cl::init(true),
		   cl::desc("Dump test cases for all active states on exit (default=on)"));

  cl::opt<bool>
  AllowExternalSymCalls("allow-external-sym-calls",
                        cl::init(false),
			cl::desc("Allow calls with symbolic arguments to external functions.  This concretizes the symbolic arguments.  (default=off)"));

  /// The different query logging solvers that can switched on/off
  enum PrintDebugInstructionsType {
    STDERR_ALL, ///
    STDERR_SRC,
    STDERR_COMPACT,
    FILE_ALL,    ///
    FILE_SRC,    ///
    FILE_COMPACT ///
  };

  llvm::cl::list<PrintDebugInstructionsType> DebugPrintInstructions(
      "debug-print-instructions",
      llvm::cl::desc("Log instructions during execution."),
      llvm::cl::values(
          clEnumValN(STDERR_ALL, "all:stderr", "Log all instructions to stderr "
                                               "in format [src, inst_id, "
                                               "llvm_inst]"),
          clEnumValN(STDERR_SRC, "src:stderr",
                     "Log all instructions to stderr in format [src, inst_id]"),
          clEnumValN(STDERR_COMPACT, "compact:stderr",
                     "Log all instructions to stderr in format [inst_id]"),
          clEnumValN(FILE_ALL, "all:file", "Log all instructions to file "
                                           "instructions.txt in format [src, "
                                           "inst_id, llvm_inst]"),
          clEnumValN(FILE_SRC, "src:file", "Log all instructions to file "
                                           "instructions.txt in format [src, "
                                           "inst_id]"),
          clEnumValN(FILE_COMPACT, "compact:file",
                     "Log all instructions to file instructions.txt in format "
                     "[inst_id]"),
          clEnumValEnd),
      llvm::cl::CommaSeparated);
#ifdef HAVE_ZLIB_H
  cl::opt<bool> DebugCompressInstructions(
      "debug-compress-instructions", cl::init(false),
      cl::desc("Compress the logged instructions in gzip format."));
#endif

  cl::opt<bool>
  DebugCheckForImpliedValues("debug-check-for-implied-values");


  cl::opt<bool>
  SimplifySymIndices("simplify-sym-indices",
                     cl::init(false),
		     cl::desc("Simplify symbolic accesses using equalities from other constraints (default=off)"));

  cl::opt<bool>
  EqualitySubstitution("equality-substitution",
		       cl::init(true),
		       cl::desc("Simplify equality expressions before querying the solver (default=on)."));

  cl::opt<unsigned>
  MaxSymArraySize("max-sym-array-size",
                  cl::init(0));

  cl::opt<bool>
  SuppressExternalWarnings("suppress-external-warnings",
			   cl::init(false),
			   cl::desc("Supress warnings about calling external functions."));

  cl::opt<bool>
  AllExternalWarnings("all-external-warnings",
		      cl::init(false),
		      cl::desc("Issue an warning everytime an external call is made," 
			       "as opposed to once per function (default=off)"));

  cl::opt<bool>
  OnlyOutputStatesCoveringNew("only-output-states-covering-new",
                              cl::init(false),
			      cl::desc("Only output test cases covering new code (default=off)."));

  cl::opt<bool>
  EmitAllErrors("emit-all-errors",
                cl::init(false),
                cl::desc("Generate tests cases for all errors "
                         "(default=off, i.e. one per (error,instruction) pair)"));
  
  cl::opt<bool>
  NoExternals("no-externals", 
           cl::desc("Do not allow external function calls (default=off)"));

  cl::opt<bool>
  AlwaysOutputSeeds("always-output-seeds",
		    cl::init(true));

  cl::opt<bool>
  OnlyReplaySeeds("only-replay-seeds",
		  cl::init(false),
                  cl::desc("Discard states that do not have a seed (default=off)."));
 
  cl::opt<bool>
  OnlySeed("only-seed",
	   cl::init(false),
           cl::desc("Stop execution after seeding is done without doing regular search (default=off)."));
 
  cl::opt<bool>
  AllowSeedExtension("allow-seed-extension",
		     cl::init(false),
                     cl::desc("Allow extra (unbound) values to become symbolic during seeding (default=false)."));
 
  cl::opt<bool>
  ZeroSeedExtension("zero-seed-extension",
		    cl::init(false),
		    cl::desc("(default=off)"));
 
  cl::opt<bool>
  AllowSeedTruncation("allow-seed-truncation",
		      cl::init(false),
                      cl::desc("Allow smaller buffers than in seeds (default=off)."));
 
  cl::opt<bool>
  NamedSeedMatching("named-seed-matching",
		    cl::init(false),
                    cl::desc("Use names to match symbolic objects to inputs (default=off)."));

  cl::opt<double>
  MaxStaticForkPct("max-static-fork-pct", 
		   cl::init(1.),
		   cl::desc("(default=1.0)"));

  cl::opt<double>
  MaxStaticSolvePct("max-static-solve-pct",
		    cl::init(1.),
		    cl::desc("(default=1.0)"));

  cl::opt<double>
  MaxStaticCPForkPct("max-static-cpfork-pct", 
		     cl::init(1.),
		     cl::desc("(default=1.0)"));

  cl::opt<double>
  MaxStaticCPSolvePct("max-static-cpsolve-pct",
		      cl::init(1.),
		      cl::desc("(default=1.0)"));

  cl::opt<double>
  MaxInstructionTime("max-instruction-time",
                     cl::desc("Only allow a single instruction to take this much time (default=0s (off)). Enables --use-forked-solver"),
                     cl::init(0));
  
  cl::opt<double>
  SeedTime("seed-time",
           cl::desc("Amount of time to dedicate to seeds, before normal search (default=0 (off))"),
           cl::init(0));
  
  cl::list<Executor::TerminateReason>
  ExitOnErrorType("exit-on-error-type",
		  cl::desc("Stop execution after reaching a specified condition.  (default=off)"),
		  cl::values(
		    clEnumValN(Executor::Abort, "Abort", "The program crashed"),
		    clEnumValN(Executor::Assert, "Assert", "An assertion was hit"),
		    clEnumValN(Executor::Exec, "Exec", "Trying to execute an unexpected instruction"),
		    clEnumValN(Executor::External, "External", "External objects referenced"),
		    clEnumValN(Executor::Free, "Free", "Freeing invalid memory"),
		    clEnumValN(Executor::Model, "Model", "Memory model limit hit"),
		    clEnumValN(Executor::Overflow, "Overflow", "An overflow occurred"),
		    clEnumValN(Executor::Ptr, "Ptr", "Pointer error"),
		    clEnumValN(Executor::ReadOnly, "ReadOnly", "Write to read-only memory"),
		    clEnumValN(Executor::ReportError, "ReportError", "klee_report_error called"),
		    clEnumValN(Executor::User, "User", "Wrong klee_* functions invocation"),
		    clEnumValN(Executor::Unhandled, "Unhandled", "Unhandled instruction hit"),
		    clEnumValEnd),
		  cl::ZeroOrMore);

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 0)
  cl::opt<unsigned int>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (default=0 (off))"),
                         cl::init(0));
#else
  cl::opt<unsigned long long>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (default=0 (off))"),
                         cl::init(0));
#endif
  
  cl::opt<unsigned>
  MaxForks("max-forks",
           cl::desc("Only fork this many times (default=-1 (off))"),
           cl::init(~0u));
  
  cl::opt<unsigned>
  MaxDepth("max-depth",
           cl::desc("Only allow this many symbolic branches (default=0 (off))"),
           cl::init(0));
  
  cl::opt<unsigned>
  MaxMemory("max-memory",
            cl::desc("Refuse to fork when above this amount of memory (in MB, default=2000)"),
            cl::init(2000));

  cl::opt<bool>
  MaxMemoryInhibit("max-memory-inhibit",
            cl::desc("Inhibit forking at memory cap (vs. random terminate) (default=on)"),
            cl::init(true));

  // CHASER options

  cl::opt<bool>
  PrintFunctionCalls("print-functions", cl::init(false),
                     cl::desc("Print function calls (default=off)"));

  cl::opt<bool>
  LazySlicing("lazy-slicing", cl::init(true),
              cl::desc("Lazy slicing of skipped functions (default=on)"));

  llvm::cl::opt<bool> UseSlicer("use-slicer",
                      llvm::cl::desc("Slice skipped functions"),
                      llvm::cl::init(false));
}


namespace klee {
  RNG theRNG;
}

const char *Executor::TerminateReasonNames[] = {
  [ Abort ] = "abort",
  [ Assert ] = "assert",
  [ Exec ] = "exec",
  [ External ] = "external",
  [ Free ] = "free",
  [ Model ] = "model",
  [ Overflow ] = "overflow",
  [ Ptr ] = "ptr",
  [ ReadOnly ] = "readonly",
  [ ReportError ] = "reporterror",
  [ User ] = "user",
  [ Unhandled ] = "xxx",
};

#define HUGE_ALLOC_SIZE (1U << 31)

Executor::Executor(InterpreterOptions &opts, InterpreterHandler *ih)
    : Interpreter(opts), kmodule(0), interpreterHandler(ih), searcher(0),
      externalDispatcher(new ExternalDispatcher()), statsTracker(0),
      pathWriter(0), symPathWriter(0), specialFunctionHandler(0),
      processTree(0), replayKTest(0), replayPath(0), usingSeeds(0),
      atMemoryLimit(false), inhibitForking(false), haltExecution(false),
      ivcEnabled(false), enableBranchHalt(false), haltFromMaster(false),
      ready2Offload(false),
      coreSolverTimeout(MaxCoreSolverTime != 0 && MaxInstructionTime != 0
                            ? std::min(MaxCoreSolverTime, MaxInstructionTime)
                            : std::max(MaxCoreSolverTime, MaxInstructionTime)),
      debugInstFile(0), debugLogBuffer(debugBufferString),
      errorCount(0),
      logFile(0) {

  if (coreSolverTimeout) UseForkedCoreSolver = true;
  Solver *coreSolver = klee::createCoreSolver(CoreSolverToUse);
  if (!coreSolver) {
    klee_error("Failed to create core solver\n");
  }

  Solver *solver = constructSolverChain(
      coreSolver,
      interpreterHandler->getOutputFilename(ALL_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(ALL_QUERIES_KQUERY_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_KQUERY_FILE_NAME));

  this->solver = new TimingSolver(solver, EqualitySubstitution);
  prefixTree =  new PrefixTree();
  memory = new MemoryManager(&arrayCache);

  if (optionIsSet(DebugPrintInstructions, FILE_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_COMPACT) ||
      optionIsSet(DebugPrintInstructions, FILE_SRC)) {
    std::string debug_file_name =
        interpreterHandler->getOutputFilename("instructions.txt");
    std::string ErrorInfo;
#ifdef HAVE_ZLIB_H
    if (!DebugCompressInstructions) {
#endif

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 5)
    debugInstFile = new llvm::raw_fd_ostream(debug_file_name.c_str(), ErrorInfo,
                                             llvm::sys::fs::OpenFlags::F_Text);
#else
    debugInstFile =
        new llvm::raw_fd_ostream(debug_file_name.c_str(), ErrorInfo);
#endif
#ifdef HAVE_ZLIB_H
    } else {
      debugInstFile = new compressed_fd_ostream(
          (debug_file_name + ".gz").c_str(), ErrorInfo);
    }
#endif
    if (ErrorInfo != "") {
      klee_error("Could not open file %s : %s", debug_file_name.c_str(),
                 ErrorInfo.c_str());
    }
  }

  ra = NULL;
  inliner = NULL;
  aa = NULL;
  mra = NULL;
  cloner = NULL;
  sliceGenerator = NULL;

  //PSE Vars
  enablePathPrefixFilter = 0;
	enableBranchHalt = false;
  explorationDepth = 0;
  branchLevel2Halt = 0;
  searchMode = "BFS";
  enableLB = false;
  numOffloadStates = 0;
  numPrefixes = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &coreId);
}

const Module *Executor::setModule(llvm::Module *module, const ModuleOptions &opts) {
  assert(!kmodule && module && "can only register one module"); // XXX gross
  
  kmodule = new KModule(module);
  
  // Initialize the context.
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
  TargetData *TD = kmodule->targetData;
#else
  DataLayout *TD = kmodule->targetData;
#endif
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width) TD->getPointerSizeInBits());

  specialFunctionHandler = new SpecialFunctionHandler(*this);
  specialFunctionHandler->prepare();

  if (!interpreterOpts.skippedFunctions.empty()) {
    /* build target functions */
    std::vector<std::string> targets;
    for (auto i = interpreterOpts.skippedFunctions.begin(), e = interpreterOpts.skippedFunctions.end(); i != e; i++) {
      targets.push_back(i->name);
    }

    logFile = interpreterHandler->openOutputFile("sa.log");
    ra = new ReachabilityAnalysis(module, opts.EntryPoint, targets, *logFile);
    inliner = new Inliner(module, ra, targets, interpreterOpts.inlinedFunctions, *logFile);
    aa = new AAPass();
    aa->setPAType(PointerAnalysis::Andersen_WPA);

    mra = new ModRefAnalysis(kmodule->module, ra, aa, opts.EntryPoint, targets, *logFile);
    cloner = new Cloner(module, ra, *logFile);
    if (UseSlicer) {
      sliceGenerator = new SliceGenerator(module, ra, aa, mra, cloner, *logFile, LazySlicing);
    }
  }

  kmodule->prepare(opts, interpreterOpts.skippedFunctions, interpreterHandler, ra, inliner, 
    aa, mra, cloner, sliceGenerator);

  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics() || userSearcherRequiresMD2U()) {
  //if(false) {
    statsTracker = new StatsTracker(*this,
                  interpreterHandler->getOutputFilename("assembly.ll"),
                  userSearcherRequiresMD2U());
  }
  return module;
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  if (processTree)
    delete processTree;
  if (specialFunctionHandler)
    delete specialFunctionHandler;
  if (statsTracker)
    delete statsTracker;
  delete solver;
  /* TODO: is it the right place? */
  if (sliceGenerator) delete sliceGenerator;
  if (cloner) delete cloner;
  if (mra) delete mra;
  if (inliner) delete inliner;
  if (ra) delete ra;
  //if (aa) delete aa;
  delete kmodule;
  while(!timers.empty()) {
    delete timers.back();
    timers.pop_back();
  }
  if (debugInstFile) {
    delete debugInstFile;
  }
  if (logFile) {
    delete logFile;
  }
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c, 
                                      unsigned offset) {
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
  TargetData *targetData = kmodule->targetData;
#else
  DataLayout *targetData = kmodule->targetData;
#endif
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i), 
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i), 
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i), 
			     offset + sl->getElementOffset(i));
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
  } else if (const ConstantDataSequential *cds =
               dyn_cast<ConstantDataSequential>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cds->getElementType());
    for (unsigned i=0, e=cds->getNumElements(); i != e; ++i)
      initializeGlobalObject(state, os, cds->getElementAsConstant(i),
                             offset + i*elementSize);
#endif
  } else if (!isa<UndefValue>(c)) {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state, 
                                           void *addr, unsigned size, 
                                           bool isReadOnly) {
  MemoryObject *mo = memory->allocateFixed((uint64_t) (unsigned long) addr, 
                                           size, 0);
  ObjectState *os = bindObjectInState(state, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i]);
  if(isReadOnly)
    os->setReadOnly(true);  
  return mo;
}


extern void *__dso_handle __attribute__ ((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module;

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 3)
  assert(m->lib_begin() == m->lib_end() &&
         "XXX do not support dependent libraries");
#endif
  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() && 
        !externalDispatcher->resolveSymbol(f->getName())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer((unsigned long) (void*) f);
      legalFunctions.insert((uint64_t) (unsigned long) (void*) f);
    }
    
    globalAddresses.insert(std::make_pair(f, addr));
  }

  // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  int *errno_addr = __errno_location();
  addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);

  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, const_cast<uint16_t*>(*addr-128),
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);
    
  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, const_cast<int32_t*>(*lower_addr-128),
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);
  
  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, const_cast<int32_t*>(*upper_addr-128),
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = 0;
      if (ty->isSized()) {
	      size = kmodule->targetData->getTypeStoreSize(ty);
      } else {
        klee_warning("Type for %.*s is not sized", (int)i->getName().size(),
			  i->getName().data());
      }

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        klee_warning("Unable to find size for global variable: %.*s (use will result in out of bounds access)",
			  (int)i->getName().size(), i->getName().data());
      }

      MemoryObject *mo = memory->allocate(size, false, true, i);
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.", 
                     i->getName().data());

        for (unsigned offset=0; offset<mo->size; offset++)
          os->write8(offset, ((unsigned char*)addr)[offset]);
      }
    } else {
      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = memory->allocate(size, false, true, &*i);
      if (!mo)
        llvm::report_fatal_error("out of memory");
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));
      //std::cout << "Allocating Memory for Global Var: "<<i->getName().str()<<"\n";
      //memory->updateIdToLLVMValueMap(i, 0);

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }
  
  // link aliases to their definitions (if bound)
  for (Module::alias_iterator i = m->alias_begin(), ie = m->alias_end(); 
       i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions. 
    globalAddresses.insert(std::make_pair(i, evalConstant(i->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      MemoryObject *mo = globalObjects.find(i)->second;
      const ObjectState *os = state.addressSpace.findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace.getWriteable(mo, os);
      
      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      // if(i->isConstant()) os->setReadOnly(true);
    }
  }
}

int Executor::branch(ExecutionState &state, 
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result) {
  TimerStatIncrementer timer(stats::forkTime);

  unsigned N = conditions.size();
  assert(N);

  if (MaxForks!=~0u && stats::forks >= MaxForks) {
    unsigned next = theRNG.getInt32() % N;
    for (unsigned i=0; i<N; ++i) {
      if (i == next) {
        result.push_back(&state);
      } else {
        result.push_back(NULL);
      }
    }
  } else {
#if false
    stats::forks += N-1;
		result.push_back(&state);
    for (unsigned i=1; i<N; ++i) {
      ExecutionState *es = result[theRNG.getInt32() % i];
      ExecutionState *ns = es->branch();
      if (ns->isRecoveryState()) {
        interpreterHandler->incRecoveryStatesCount();
      }
      addedStates.push_back(ns);
      result.push_back(ns);
      es->ptreeNode->data = 0;
      std::pair<PTree::Node*,PTree::Node*> res = 
        processTree->split(es->ptreeNode, ns, es);
      ns->ptreeNode = res.first;
      es->ptreeNode = res.second;
		}
#endif
#if true
    stats::forks += N-1; //incorrect fork counting in this case
    //generating a balanced tree instead of random
    result.push_back(&state);
    //if((coreId==0) || (state.depth >= prefixDepth) || state.isRecoveryState()) {
    //if((coreId==0) || (state.depth >= prefixDepth)) {// || state.isRecoveryState()) {
    if((coreId==0) || (!state.shallIRange())) {// || state.isRecoveryState()) {
    //if(true) {// || state.isRecoveryState()) {
      if(ENABLE_LOGGING) {
        mylogFile << "Switch: Not Using test ranging N \n";
        mylogFile.flush();
      }
      for (unsigned i=1; i<N; ++i) {
        ExecutionState *es = result[(result.size()-1)];
				ExecutionState *ns = es->branch();
        if(!ns->isRecoveryState()) {
          nonRecoveryStates.insert(ns);
        }
        addedStates.push_back(ns);
        result.push_back(ns);
        es->ptreeNode->data = 0;
        std::pair<PTree::Node*,PTree::Node*> res =
          processTree->split(es->ptreeNode, ns, es);
        ns->ptreeNode = res.first;
        es->ptreeNode = res.second;
        //ns->pathOS = pathWriter->open(ns->pathOS);
        //if(pathWriter) {
          //es->pathOS << "0";
          //ns->pathOS << "1";
          es->depth++;
          ns->depth++;
          es->branchHist.push_back('0');
          ns->branchHist.push_back('1');
        //}
      } 
    } else {
      if(ENABLE_LOGGING) {
        mylogFile << "Switch: Using test ranging N: "<<N
          <<" incoming state: "<<result[(result.size()-1)]<<"\n";
        mylogFile.flush();
      }
			for(unsigned i=1; i<N; ++i) {
				//check each condition one my one to see 
				//if test takes the case
				ExecutionState *es = result[(result.size()-1)];
				ExecutionState *ns = es->branch();
				result.push_back(ns);
        if(!ns->isRecoveryState()) {
          nonRecoveryStates.insert(ns);
        }
				if(ENABLE_LOGGING) {
					mylogFile<<"Switch: Creating new state: "<<ns<<"\n";
				}
				es->ptreeNode->data = 0;
				std::pair<PTree::Node*,PTree::Node*> res1 =
					 processTree->split(es->ptreeNode, ns, es);
				ns->ptreeNode = res1.first;
				es->ptreeNode = res1.second;
				//ns->pathOS = pathWriter->open(ns->pathOS);
				//if(pathWriter) {
					//es->pathOS << "0";
					//ns->pathOS << "1";
          es->depth++;
          ns->depth++;
          es->branchHist.push_back('0');
          ns->branchHist.push_back('1');
				//}
			}
			int satCase = 0;
			bool foundCase = false;
			for(unsigned i=0; i<N; ++i) {
				bool match = true;
				std::vector <unsigned char> casePath;
				//pathWriter->readStream(getPathStreamID(*result[i]), casePath);
        if(ENABLE_LOGGING) {
          mylogFile<<"Result[0] depth: "<<result[0]->depth<<"Case depth: "<<result[i]->depth<<"\n";
          printBranchHist(result[i]);
          //printPath(upperBound, mylogFile, "Upper bound: ");
        }  
				for(int xid=result[0]->depth-1; xid<result[i]->depth; xid++) {
					if(upperBound[xid] != result[i]->branchHist[xid]) {
						match = false;
						break;	
					}
				}	
				if(match) {
					satCase = i;
					foundCase = true;
					break;
				}
			}
			if(!foundCase) { // default case is taken
				//default case must be taken
				if(ENABLE_LOGGING) mylogFile << "Suspending all but default switch case states\n";
				for(int i=0; i<N-1; ++i) {
					rangingSuspendedStates.push_back(result[i]);
					if(ENABLE_LOGGING) {
						mylogFile<<"Suspending state: "<<result[i]<<" "<<result[i]->depth<<"\n";
						//printStatePath(*result[i], mylogFile, "Suspended State Path:");
						mylogFile.flush();
					}
				}
				addedStates.push_back(result[N-1]);
				// remove the state we came with from the searcher 
				//take care of the case when the state one comes in with has to be
				//removed
				if(ENABLE_LOGGING) {
					mylogFile << "Removing Case0 state "<<result[0]<<" "<<result[0]->depth<<"\n";
					mylogFile.flush();
				}
				std::vector<ExecutionState *> remStates;
				remStates.push_back(result[0]);
				//testSwitchRemovedState = result[0];
				searcher->update(nullptr, std::vector<ExecutionState *>(), remStates);
        //numOffloadStates--;
				auto ii = states.find(result[0]);
				assert(ii != states.end()); //can not be case as the state has to exist
				states.erase(ii); //remove the state from states vector	
			} else {
				if(ENABLE_LOGGING) mylogFile << "Suspending all but switch case state:"<<satCase<<"\n";
				for(int i=0; i<N; ++i) {
					if(i != satCase) {
						if(ENABLE_LOGGING) {
							mylogFile<<"Suspending state: "<<result[i]<<" "<<result[i]->depth<<"\n";
							//printStatePath(*result[i], mylogFile, "Suspended State Path:");
							mylogFile.flush();
						}
						rangingSuspendedStates.push_back(result[i]);
					} else {
						if(ENABLE_LOGGING) {
							mylogFile<<"Adding state: "<<result[i]<<" "<<result[i]->depth<<"\n";
							mylogFile.flush();
						}
						if(satCase != 0) {
							addedStates.push_back(result[i]);
						}
					}
				}
				if(satCase > 0) {
					//take care of the case when the state one comes in with has to be removed 
					if(ENABLE_LOGGING) {
						//gFile << "Removing Case0 state\n";
						mylogFile << "Removing Case0 state "<<result[0]<<" "<<result[0]->depth<<"\n";
						mylogFile.flush();
					}
					std::vector<ExecutionState *> remStates;
					remStates.push_back(result[0]);
					//testSwitchRemovedState = result[0];
					searcher->update(nullptr, std::vector<ExecutionState *>(), remStates);
					//auto ii = states.find(result[0]);
					//assert(ii != states.end()); //can not be case as the state has to exist
					//states.erase(ii); //remove the state from states vector
          //here I have to "recursively" remove the dependent states of the case0 state
          //from the states vector which might be suspended, this particular
          //prefix wont require it
          /*if(result[0] -> isRecoveryState()) {
            ExecutionState* depState = result[0]->getDependentState();
            assert(depState->isSuspended());
            auto ii = states.find(depState);
            assert(ii != states.end()); 
            states.erase(ii);
          }*/
          ExecutionState* curr = result[0];
          ExecutionState* next;
          while (curr) {
            if (curr->isRecoveryState()) {
              next = curr->getDependentState();
              assert(next);
            } else {
              next = NULL;
            }
            auto ii = states.find(curr);
            assert(ii != states.end());
            states.erase(ii);
            curr = next;
          }
				}
			}
    } 
#endif
  }

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).
  
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(state, siit->assignment.evaluate(conditions[i]), 
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }
      
      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      // Extra check in case we're replaying seeds with a max-fork
      if (result[i])
        seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (result[i] && !seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      } 
    }
  }

  /* handle the forks */
  for (unsigned i=0; i<result.size(); ++i) {
    ExecutionState *current = result[i];
    if (current) {
      if (current->isRecoveryState()) {
        if (i != 0) {
          /* here we must fork the dependent state */
          DEBUG_WITH_TYPE(
            DEBUG_BASIC,
            klee_message("forked recovery state (switch): %p", current)
          );
          ExecutionState *prev = result[i - 1];
          if(prev) {
            forkDependentStates(prev, current);
          }
        }
      }
    }
  }

  /* handle the constraints */
  for (unsigned i=0; i<result.size(); ++i) {
    ExecutionState *current = result[i];
    if (current) {
      ref<Expr> condition = conditions[i];
      addConstraint(*current, condition);
      if (current->isRecoveryState()) {
        mergeConstraintsForAll(*current, condition);
      }
    }
  }
  return result.size();
}

Executor::StatePair 
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal) {
  Solver::Validity res;
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  if (!isSeeding && !isa<ConstantExpr>(condition) && 
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack.back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) > 
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) > 
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) > 
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) > 
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value; 
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }
  }

  bool forkAndSuspend = false;
  double timeout = coreSolverTimeout;
  if (isSeeding)
    timeout *= it->second.size();

  //master does not do any test ranging
  if(coreId == 0) {
    if(ENABLE_LOGGING) {
      KInstruction *ki = current.pc;
      const InstructionInfo &ii = *ki->info;
      mylogFile << "Branch: Master Not ranging using tests at depth: "<<current.depth
        <<" Actual depth: "<<current.actDepth<<" isInternal: "<<isInternal<<" Id:"<<ii.line<<"\n";
      mylogFile.flush();
    }

    solver->setTimeout(timeout);
    bool success = solver->evaluate(current, condition, res);
    solver->setTimeout(0);
    if (!success) {
      current.pc = current.prevPC;
      terminateStateEarly(current, "Query timed out (fork).");
      return StatePair(0, 0);
    }
    if(!isInternal) {
      if(ENABLE_LOGGING) {
        if(res == Solver::True) {
          mylogFile<<"Taking true branch forkandSuspend: "<<forkAndSuspend<<" isInternal: "<<isInternal<<"\n";
        } 
        if(res == Solver::False) {
          mylogFile<<"Taking false branch forkandSuspend: "<<forkAndSuspend<<" isInternal: "<<isInternal<<"\n";
        }
      }
    }
  } else {
    //range if the depth less than prefixDepth
    //if((current.depth < prefixDepth) && (prefixDepth!=0) && ((current).isNormalState() && !(current).isRecoveryState())) {
    //if((current.depth < prefixDepth) && (prefixDepth!=0)) { 
    if(current.shallIRange()) {
      if(ENABLE_LOGGING) {
        KInstruction *ki = current.pc;
        const InstructionInfo &ii = *ki->info;
        mylogFile << "Branch: Ranging using tests at depth: "<<current.depth<<" Prefix Depth: "
          <<prefixDepth<<" Act Depth: "
          <<current.actDepth<<" IsInternal: "<<isInternal<<" Branch direction:"
          <<current.branchToTake(forkAndSuspend)<<" Id:"<<ii.line<<"\n";
      	mylogFile.flush();
      }
      if(isInternal) {
        //check to see if execution would have forked without the test  
        solver->setTimeout(timeout);
        bool success = solver->evaluate(current, condition, res);
        solver->setTimeout(0);
        if(!success) {
          current.pc = current.prevPC;
          terminateStateEarly(current, "Query timed out (fork).");
          return StatePair(0, 0);
        }
      } else {
        int solverRes = current.branchToTake(forkAndSuspend);
        if(solverRes == 0) { res = Solver::True; }
        else if(solverRes == 1) { res = Solver::False; }
        else { res = Solver::Unknown; }
        //else res = Solver::False;
      }
    } else {
      solver->setTimeout(timeout);
      bool success = solver->evaluate(current, condition, res);
      solver->setTimeout(0);
      if (!success) {
        current.pc = current.prevPC;
        terminateStateEarly(current, "Query timed out (fork).");
        return StatePair(0, 0);
      }
      if(ENABLE_LOGGING) {
        mylogFile << "Branch: Not ranging using tests at depth: "<<current.depth<<" "
          <<current.actDepth<<" isInternal: "<<isInternal<<"\n";
        mylogFile.flush();
      }
    }
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition<replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];
      
      if (res==Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res==Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if(branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else  {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res==Solver::Unknown) {
      assert(!replayKTest && "in replay mode, only one branch can be true.");
      
      if ((MaxMemoryInhibit && atMemoryLimit) || 
          current.forkDisabled ||
          inhibitForking || 
          (MaxForks!=~0u && stats::forks >= MaxForks)) {

	if (MaxMemoryInhibit && atMemoryLimit)
	  klee_warning_once(0, "skipping fork (memory cap exceeded)");
	else if (current.forkDisabled)
	  klee_warning_once(0, "skipping fork (fork disabled on current path)");
	else if (inhibitForking)
	  klee_warning_once(0, "skipping fork (fork disabled globally)");
	else 
	  klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;        
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && 
      (current.forkDisabled || OnlyReplaySeeds) && 
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success = 
        solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      
      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
    }
  }

  if (res==Solver::True) {
		if(forkAndSuspend) {
      if(ENABLE_LOGGING) {
        mylogFile << "Forking and suspending\n";
        mylogFile.flush();
      }
			ExecutionState *falseState = NULL, *trueState = &current;

			falseState = trueState->branch();
     
      if(!falseState->isRecoveryState()) {
        nonRecoveryStates.insert(falseState);
      }

			current.ptreeNode->data = 0;
			std::pair<PTree::Node*, PTree::Node*> res =
				processTree->split(current.ptreeNode, falseState, trueState);
			falseState->ptreeNode = res.first;
			trueState->ptreeNode = res.second;
			//if (pathWriter) {
				//falseState->pathOS = pathWriter->open(current.pathOS);
				if (!isInternal) {
					//trueState->pathOS << "0";
					//falseState->pathOS << "1";
          trueState->depth++;
          falseState->depth++;
          trueState->branchHist.push_back('0');
          falseState->branchHist.push_back('1');
				}
			//}
			if (symPathWriter) {
				falseState->symPathOS = symPathWriter->open(current.symPathOS);
				if (!isInternal) {
					trueState->symPathOS << "0";
					falseState->symPathOS << "1";
				}
			}

			addConstraint(*trueState, condition);
			addConstraint(*falseState, Expr::createIsZero(condition));
			
			if (trueState->isRecoveryState()) {
				forkDependentStates(trueState, falseState);

				/* propagate constraints if required */
				mergeConstraintsForAll(*trueState, condition);
				mergeConstraintsForAll(*falseState, Expr::createIsZero(condition));
			}
      //Just putting the false state in suspended queue
			rangingSuspendedStates.push_back(falseState);
      
      return StatePair(trueState, falseState);
		} else {
			//if (pathWriter) {
  			if (!isInternal) {
    			//current.pathOS << "0";
          current.depth++;
          current.branchHist.push_back('2');
  			}
			//}
      return StatePair(&current, 0);
		}
  } else if (res==Solver::False) {
		if(forkAndSuspend) {
      if(ENABLE_LOGGING) {
        mylogFile << "Forking and suspending\n";
        mylogFile.flush();
      }
			ExecutionState *trueState = NULL, *falseState = &current;

			++stats::forks;
			//falseState->depth++;
			trueState = falseState->branch();
      
      if(!trueState->isRecoveryState()) {
        nonRecoveryStates.insert(trueState);
      }

			current.ptreeNode->data = 0;
			std::pair<PTree::Node*, PTree::Node*> res =
				processTree->split(current.ptreeNode, falseState, trueState);
			falseState->ptreeNode = res.first;
			trueState->ptreeNode = res.second;
			//if (pathWriter) {
				//falseState->pathOS = pathWriter->open(current.pathOS);
				if (!isInternal) {
					//trueState->pathOS << "0";
					//falseState->pathOS << "1";
          trueState->depth++;
          falseState->depth++;
          trueState->branchHist.push_back('0');
          falseState->branchHist.push_back('1');
				}
			//}
			if (symPathWriter) {
				falseState->symPathOS = symPathWriter->open(current.symPathOS);
				if (!isInternal) {
					trueState->symPathOS << "0";
					falseState->symPathOS << "1";
				}
			}

			addConstraint(*trueState, condition);
			addConstraint(*falseState, Expr::createIsZero(condition));

			if (falseState->isRecoveryState()) {
				forkDependentStates(falseState, trueState);

				mergeConstraintsForAll(*trueState, condition);
				mergeConstraintsForAll(*falseState, Expr::createIsZero(condition));
			}
			//Just putting the true state in suspended mode
			rangingSuspendedStates.push_back(trueState);

      return StatePair(trueState, falseState);
		} else {
			//if (pathWriter) {
  			if (!isInternal) {
    			//current.pathOS << "1";
			    current.depth++;
          current.branchHist.push_back('3');
  			}
			//}
      return StatePair(0, &current);
		}
  } else {
    if(ENABLE_LOGGING) {
      mylogFile << "Forking\n";
      mylogFile.flush();
    }
		TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState = NULL, *trueState = &current;
    //ref<Expr> negatedCondition = Expr::createIsZero(condition);

    ++stats::forks;

    falseState = trueState->branch();
    

    if(!falseState->isRecoveryState()) {
      nonRecoveryStates.insert(falseState);
    }

    addedStates.push_back(falseState);
   
    if(coreId != 0) { 
      trueState->removeFalsePrefixes();
      falseState->removeTruePrefixes();
    }

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success =
          solver->getValue(current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }

      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node*, PTree::Node*> res =
      processTree->split(current.ptreeNode, falseState, trueState);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;
    //if (pathWriter) {
      //falseState->pathOS = pathWriter->open(current.pathOS);
      if (!isInternal) {
        //trueState->pathOS << "0";
        //falseState->pathOS << "1";
        trueState->depth++;
        falseState->depth++;
        trueState->branchHist.push_back('0');
        falseState->branchHist.push_back('1');
      //}
    }
    if (symPathWriter) {
      falseState->symPathOS = symPathWriter->open(current.symPathOS);
      if (!isInternal) {
        trueState->symPathOS << "0";
        falseState->symPathOS << "1";
      }
    }
    

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    if (trueState->isRecoveryState()) {
      forkDependentStates(trueState, falseState);

      /* propagate constraints if required */
      mergeConstraintsForAll(*trueState, condition);
      mergeConstraintsForAll(*falseState, Expr::createIsZero(condition));
    }

		return StatePair(trueState, falseState);
  }
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    if (!CE->isTrue())
      llvm::report_fatal_error("attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success = 
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint"); 
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition, 
                                 ConstantExpr::alloc(1, Expr::Bool));
}

ref<klee::ConstantExpr> Executor::evalConstant(const Constant *c) {
  if (const llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
    return evalConstantExpr(ce);
  } else {
    if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
      return ConstantExpr::alloc(ci->getValue());
    } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {      
      return ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt());
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      return globalAddresses.find(gv)->second;
    } else if (isa<ConstantPointerNull>(c)) {
      return Expr::createPointer(0);
    } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
      return ConstantExpr::create(0, getWidthForLLVMType(c->getType()));
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
    } else if (const ConstantDataSequential *cds =
                 dyn_cast<ConstantDataSequential>(c)) {
      std::vector<ref<Expr> > kids;
      for (unsigned i = 0, e = cds->getNumElements(); i != e; ++i) {
        ref<Expr> kid = evalConstant(cds->getElementAsConstant(i));
        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
#endif
    } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(cs->getType());
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = cs->getNumOperands(); i != 0; --i) {
        unsigned op = i-1;
        ref<Expr> kid = evalConstant(cs->getOperand(op));

        uint64_t thisOffset = sl->getElementOffsetInBits(op),
                 nextOffset = (op == cs->getNumOperands() - 1)
                              ? sl->getSizeInBits()
                              : sl->getElementOffsetInBits(op+1);
        if (nextOffset-thisOffset > kid->getWidth()) {
          uint64_t paddingWidth = nextOffset-thisOffset-kid->getWidth();
          kids.push_back(ConstantExpr::create(0, paddingWidth));
        }

        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)){
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = ca->getNumOperands(); i != 0; --i) {
        unsigned op = i-1;
        ref<Expr> kid = evalConstant(ca->getOperand(op));
        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else {
      // Constant{Vector}
      llvm::report_fatal_error("invalid argument to evalConstant()");
    }
  }
}

const Cell& Executor::eval(KInstruction *ki, unsigned index, 
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack.back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state, 
                         ref<Expr> value) {
  getDestCell(state, target).value = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index, 
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, 
                             ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;

    solver->setTimeout(coreSolverTimeout);      
    if (solver->getValue(state, e, value) &&
        solver->mustBeTrue(state, EqExpr::create(e, value), isTrue) &&
        isTrue)
      result = value;
    solver->setTimeout(0);
  }
  
  return result;
}


/* Concretize the given expression, and return a possible constant value. 
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr> 
Executor::toConstant(ExecutionState &state, 
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;

  std::string str;
  llvm::raw_string_ostream os(str);
  os << "silently concretizing (reason: " << reason << ") expression " << e
     << " to value " << value << " (" << (*(state.pc)).info->file << ":"
     << (*(state.pc)).info->line << ")";

  if (AllExternalWarnings)
    klee_warning(reason, os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));
    
  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints.simplifyExpr(e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, state, value);
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> value;
      bool success = 
        solver->getValue(state, siit->assignment.evaluate(e), value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }
    
    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches);
    
    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      ++bit;
    }
  }
}

void Executor::printDebugInstructions(ExecutionState &state) {
  // check do not print
  if (DebugPrintInstructions.size() == 0)
	  return;

  llvm::raw_ostream *stream = 0;
  if (optionIsSet(DebugPrintInstructions, STDERR_ALL) ||
      optionIsSet(DebugPrintInstructions, STDERR_SRC) ||
      optionIsSet(DebugPrintInstructions, STDERR_COMPACT))
    stream = &llvm::errs();
  else
    stream = &debugLogBuffer;

  if (!optionIsSet(DebugPrintInstructions, STDERR_COMPACT) &&
      !optionIsSet(DebugPrintInstructions, FILE_COMPACT))
    printFileLine(state, state.pc, *stream);

  (*stream) << state.pc->info->id;

  if (optionIsSet(DebugPrintInstructions, STDERR_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_ALL))
    (*stream) << ":" << *(state.pc->inst);
  (*stream) << "\n";

  if (optionIsSet(DebugPrintInstructions, FILE_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_COMPACT) ||
      optionIsSet(DebugPrintInstructions, FILE_SRC)) {
    debugLogBuffer.flush();
    (*debugInstFile) << debugLogBuffer.str();
    debugBufferString = "";
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  printDebugInstructions(state);
  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  state.prevPC = state.pc;
  ++state.pc;

  if (stats::instructions==StopAfterNInstructions)
    haltExecution = true;
}

void Executor::executeCall(ExecutionState &state, 
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
  Instruction *i = ki->inst;

  if (f && PrintFunctionCalls)
    klee_message("Function: %s", f->getName().str().c_str());

  if (f && f->isDeclaration()) {
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;
        
      // va_arg is handled by caller and intrinsic lowering, see comment for
      // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = state.stack.back();

      // varargs can be zero if no varargs were provided
      if (!sf.varargs)
        return;

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work fir x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, true, arguments[0], 
                               sf.varargs->getBaseExpr(), 0);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // X86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, true, arguments[0],
                               ConstantExpr::create(48, 32), 0); // gp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(4, 64)),
                               ConstantExpr::create(304, 32), 0); // fp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(8, 64)),
                               sf.varargs->getBaseExpr(), 0); // overflow_arg_area
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(16, 64)),
                               ConstantExpr::create(0, 64), 0); // reg_save_area
      }
      break;
    }
    // FIXME: terrible hack to fix an issue with inlining of memcpy
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    // FIXME: terrible hack end
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with vaeend, however (like call it twice).
      break;
        
    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
  } else {
    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).

    /* TODO: make it more readable... */
    if (state.isNormalState() && !state.isRecoveryState() && isFunctionToSkip(state, f)) {
      /* first, check if the skipped function has side effects */
      if (mra->hasSideEffects(f)) {
        /* create snapshot, recovery state will be created on demand... */
        unsigned int index = state.getSnapshots().size();
        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message("%p: adding snapshot (index = %u)", &state, index)
        );
        ref<ExecutionState> snapshotState(createSnapshotState(state));
        ref<Snapshot> snapshot(new Snapshot(snapshotState, f));
        state.addSnapshot(snapshot);
        interpreterHandler->incSnapshotsCount();

        /* TODO: will be replaced later... */
        state.clearRecoveredAddresses();

        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message("%p: skipping function call to %s", &state, f->getName().data())
        );

        if(ENABLE_LOGGING) {
          mylogFile<<"Skipping function call: "<<f->getName().data() <<"\n";
          mylogFile.flush();
        }
      } 
      return;
    }

    /* inject the sliced function if needed */
    if (state.isRecoveryState()) {
      ref<RecoveryInfo> recoveryInfo = state.getRecoveryInfo();
      if (UseSlicer) {
        f = getSlice(f, recoveryInfo->sliceId, ModRefAnalysis::Modifier, recoveryInfo->subId);
        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message("injecting slice: %s", f->getName().data())
        );

        /* handle fully sliced functions */
        if (f->isDeclaration()) {
          DEBUG_WITH_TYPE(
            DEBUG_BASIC,
            klee_message("ignoring fully sliced function: %s", f->getName().data())
          );
          return;
        }
      } else {
        /* we do it for consistent debugging... */
        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message("injecting: %s", f->getName().data())
        );
      }
    }

    KFunction *kf = kmodule->functionMap[f];
    state.pushFrame(state.prevPC, kf);
    state.pc = kf->instructions;

    if (statsTracker)
      statsTracker->framePushed(state, &state.stack[state.stack.size()-2]);

     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes

    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.", 
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }
    } else {
      Expr::Width WordSize = Context::get().getPointerWidth();

      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }

      StackFrame &sf = state.stack.back();
      unsigned size = 0;
      bool requires16ByteAlignment = false;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work for x86-32 and x86-64, however.
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          Expr::Width argWidth = arguments[i]->getWidth();
          // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a
          // 16 byte boundary if alignment needed by type exceeds 8 byte
          // boundary.
          //
          // Alignment requirements for scalar types is the same as their size
          if (argWidth > Expr::Int64) {
             size = llvm::RoundUpToAlignment(size, 16);
             requires16ByteAlignment = true;
          }
          size += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
        }
      }
      
      MemoryObject *mo = sf.varargs =
          memory->allocate(size, true, false, state.prevPC->inst,
                           (requires16ByteAlignment ? 16 : 8));
      if (!mo && size) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }

      if (mo) {
        //FIXME Shikhar updating map
        //memory->updateIdToLLVMValueMap(state.prevPC->inst, state.prevPC->id);
        if ((WordSize == Expr::Int64) && (mo->address & 15) &&
            requires16ByteAlignment) {
          // Both 64bit Linux/Glibc and 64bit MacOSX should align to 16 bytes.
          klee_warning_once(
              0, "While allocating varargs: malloc did not align to 16 bytes.");
        }

        ObjectState *os = bindObjectInState(state, mo, true);
        unsigned offset = 0;
        for (unsigned i = funcArgs; i < callingArgs; i++) {
          // FIXME: This is really specific to the architecture, not the pointer
          // size. This happens to work for x86-32 and x86-64, however.
          if (WordSize == Expr::Int32) {
            os->write(offset, arguments[i]);
            offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
          } else {
            assert(WordSize == Expr::Int64 && "Unknown word size!");

            Expr::Width argWidth = arguments[i]->getWidth();
            if (argWidth > Expr::Int64) {
              offset = llvm::RoundUpToAlignment(offset, 16);
            }
            os->write(offset, arguments[i]);
            offset += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
          }
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i) 
      bindArgument(kf, i, state, arguments[i]);
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src, 
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.
  
  // XXX this lookup has to go ?
  KFunction *kf = state.stack.back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc = &kf->instructions[entry];
  if (state.pc->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc->inst);
    state.incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki,
                             llvm::raw_ostream &debugFile) {
  const InstructionInfo &ii = *ki->info;
  if (ii.file != "")
    debugFile << "     " << ii.file << ":" << ii.line << ":";
  else
    debugFile << "     [no debug info]:";
}

/// Compute the true target of a function call, resolving LLVM and KLEE aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue*, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv))
        return 0;

      std::string alias = state.getFnAlias(gv->getName());
      if (alias != "") {
        llvm::Module* currModule = kmodule->module;
        GlobalValue *old_gv = gv;
        gv = currModule->getNamedValue(alias);
        if (!gv) {
          klee_error("Function %s(), alias for %s not found!\n", alias.c_str(),
                     old_gv->getName().str().c_str());
        }
      }
     
      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode()==Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

/// TODO remove?
static bool isDebugIntrinsic(const Function *f, KModule *KM) {
  return false;
}

static inline const llvm::fltSemantics * fpWidthToSemantics(unsigned width) {
  switch(width) {
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
  default:
    return 0;
  }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;
  /* TODO: replace with a better predicate (call stack counter?) */
  if (state.isRecoveryState() && state.getExitInst() == i) {
    onRecoveryStateExit(state);
    return;
  }

  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack.back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);
    
    if (!isVoidReturn) {
      result = eval(ki, 0, state).value;
    }
    
    if (state.stack.size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      terminateStateOnExit(state);
    } else {
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.pc = kcaller;
        ++state.pc;
      }

      if (!isVoidReturn) {
        LLVM_TYPE_Q Type *t = caller->getType();
        if (t != Type::getVoidTy(getGlobalContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(t);
            
          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) : 
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
      bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#elif LLVM_VERSION_CODE >= LLVM_VERSION(3, 2)
	    bool isSExt = cs.paramHasAttr(0, llvm::Attributes::SExt);
#else
	    bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#endif
            if (isSExt) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }
    }      
    break;
  }
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 1)
  case Instruction::Unwind: {
    for (;;) {
      KInstruction *kcaller = state.stack.back().caller;
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (state.stack.empty()) {
        terminateStateOnExecError(state, "unwind from initial stack frame");
        break;
      } else {
        Instruction *caller = kcaller->inst;
        if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
          transferToBasicBlock(ii->getUnwindDest(), caller->getParent(), state);
          break;
        }
      }
    }
    break;
  }
#endif
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;
      Executor::StatePair branches = fork(state, cond, false);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack.back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
    }
    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    BasicBlock *bb = si->getParent();

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      LLVM_TYPE_Q llvm::IntegerType *Ty = 
        cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
      unsigned index = si->findCaseValue(ci).getSuccessorIndex();
#else
      unsigned index = si->findCaseValue(ci);
#endif
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);
    } else {
      // Handle possible different branch targets

      // We have the following assumptions:
      // - each case value is mutual exclusive to all other values including the
      //   default value
      // - order of case branches is based on the order of the expressions of
      //   the scase values, still default is handled last
      std::vector<BasicBlock *> bbOrder;
      std::map<BasicBlock *, ref<Expr> > branchTargets;

      std::map<ref<Expr>, BasicBlock *> expressionOrder;

      // Iterate through all non-default cases and order them by expressions
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
      for (SwitchInst::CaseIt i = si->case_begin(), e = si->case_end(); i != e;
           ++i) {
        ref<Expr> value = evalConstant(i.getCaseValue());
#else
      for (unsigned i = 1, cases = si->getNumCases(); i < cases; ++i) {
        ref<Expr> value = evalConstant(si->getCaseValue(i));
#endif

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
        BasicBlock *caseSuccessor = i.getCaseSuccessor();
#else
        BasicBlock *caseSuccessor = si->getSuccessor(i);
#endif
        expressionOrder.insert(std::make_pair(value, caseSuccessor));
      }

      // Track default branch values
      ref<Expr> defaultValue = ConstantExpr::alloc(1, Expr::Bool);

      // iterate through all non-default cases but in order of the expressions
      for (std::map<ref<Expr>, BasicBlock *>::iterator
               it = expressionOrder.begin(),
               itE = expressionOrder.end();
           it != itE; ++it) {
        ref<Expr> match = EqExpr::create(cond, it->first);

        // Make sure that the default value does not contain this target's value
        defaultValue = AndExpr::create(defaultValue, Expr::createIsZero(match));

        // Check if control flow could take this case
        bool result;
        bool success = solver->mayBeTrue(state, match, result);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (result) {
          BasicBlock *caseSuccessor = it->second;

          // Handle the case that a basic block might be the target of multiple
          // switch cases.
          // Currently we generate an expression containing all switch-case
          // values for the same target basic block. We spare us forking too
          // many times but we generate more complex condition expressions
          // TODO Add option to allow to choose between those behaviors
          std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> res =
              branchTargets.insert(std::make_pair(
                  caseSuccessor, ConstantExpr::alloc(0, Expr::Bool)));

          res.first->second = OrExpr::create(match, res.first->second);

          // Only add basic blocks which have not been target of a branch yet
          if (res.second) {
            bbOrder.push_back(caseSuccessor);
          }
        }
      }

      // Check if control could take the default case
      bool res;
      bool success = solver->mayBeTrue(state, defaultValue, res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> ret =
            branchTargets.insert(
                std::make_pair(si->getDefaultDest(), defaultValue));
        if (ret.second) {
          bbOrder.push_back(si->getDefaultDest());
        }
      }

      // Fork the current state with each state having one of the possible
      // successors of this switch
      std::vector< ref<Expr> > conditions;
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        conditions.push_back(branchTargets[*it]);
      }
      std::vector<ExecutionState*> branches;
      int size = branch(state, conditions, branches);
      int x = 0;
      std::vector<ExecutionState*>::iterator bit = branches.begin();
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es && (x<size))
          transferToBasicBlock(*it, bb, *es);
        ++bit;
        ++x;
      }
    }
    break;
  }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();
    Function *f = getTargetFunction(fp, state);

    /* skip slicing annotations */
    if (f && f->getName().startswith(StringRef("__crit"))) {
        break;
    }

    // Skip debug intrinsics, we can't evaluate their metadata arguments.
    if (f && isDebugIntrinsic(f, kmodule))
      break;

    if (isa<InlineAsm>(fp)) {
      terminateStateOnExecError(state, "inline assembly is unsupported");
      break;
    }
    // evaluate arguments
    std::vector< ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j)
      arguments.push_back(eval(ki, j+1, state).value);

    if (f) {
      const FunctionType *fType = 
        dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType =
        dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector< ref<Expr> >::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();
            
          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
              bool isSExt = cs.paramHasAttr(i+1, llvm::Attribute::SExt);
#elif LLVM_VERSION_CODE >= LLVM_VERSION(3, 2)
	      bool isSExt = cs.paramHasAttr(i+1, llvm::Attributes::SExt);
#else
	      bool isSExt = cs.paramHasAttr(i+1, llvm::Attribute::SExt);
#endif
              if (isSExt) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }
            
          i++;
        }
      }

      executeCall(state, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, state).value;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        StatePair res = fork(*free, EqExpr::create(v, value), true);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function*) addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once((void*) (unsigned long) addr, 
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
    ref<Expr> result = eval(ki, state.incomingBBIndex, state).value;
#else
    ref<Expr> result = eval(ki, state.incomingBBIndex * 2, state).value;
#endif
    bindLocal(ki, state, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    ref<Expr> cond = eval(ki, 0, state).value;
    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, AddExpr::create(left, right));
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, SubExpr::create(left, right));
    break;
  }
 
  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, MulExpr::create(left, right));
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = UDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = URemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }
 
  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SRemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ShlExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = LShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);
 
    switch(ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = EqExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = NeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgtExpr::create(left, right);
      bindLocal(ki, state,result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgtExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }
 
    // Memory instructions...
  case Instruction::Alloca: {
    AllocaInst *ai = cast<AllocaInst>(i);
    unsigned elementSize = 
      kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, state).value;
      count = Expr::createZExtToPointerWidth(count);
      size = MulExpr::create(size, count);
    }
    executeAlloc(state, size, true, ki);
    break;
  }

  case Instruction::Load: {
    if (state.isNormalState() && state.isInDependentMode()) {
      if (state.isBlockingLoadRecovered() && isMayBlockingLoad(state, ki)) {
        /* TODO: rename variable */
        bool success;
        bool isBlocking = handleMayBlockingLoad(state, ki, success);
        if (!success) {
          return;
        }
        if (isBlocking) {
          /* TODO: break? */
          return;
        }
      }
    }
    ref<Expr> base = eval(ki, 0, state).value;
    executeMemoryOperation(state, false, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    executeMemoryOperation(state, true, base, value, 0);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> base = eval(ki, 0, state).value;

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator 
           it = kgepi->indices.begin(), ie = kgepi->indices.end(); 
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, state).value;
      base = AddExpr::create(base,
                             MulExpr::create(Expr::createSExtToPointerWidth(index),
                                             Expr::createPointer(elementSize)));
    }
    if (kgepi->offset)
      base = AddExpr::create(base,
                             Expr::createPointer(kgepi->offset));
    bindLocal(ki, state, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value,
                                           0,
                                           getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = SExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, pType));
    break;
  } 
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.add(APFloat(*fpWidthToSemantics(right->getWidth()),right->getAPValue()), APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.add(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.subtract(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.subtract(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }
 
  case Instruction::FMul: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.multiply(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.multiply(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.divide(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.divide(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.mod(APFloat(*fpWidthToSemantics(right->getWidth()),right->getAPValue()),
            APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.mod(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Res(arg->getAPValue());
#endif
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Res(arg->getAPValue());
#endif
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Arg(arg->getAPValue());
#endif
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Arg(arg->getAPValue());

#endif
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, true,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    APFloat LHS(*fpWidthToSemantics(left->getWidth()),left->getAPValue());
    APFloat RHS(*fpWidthToSemantics(right->getWidth()),right->getAPValue());
#else
    APFloat LHS(left->getAPValue());
    APFloat RHS(right->getAPValue());
#endif
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch( fi->getPredicate() ) {
      // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = CmpRes != APFloat::cmpUnordered;
      break;

    case FCmpInst::FCMP_UNO:
      Result = CmpRes == APFloat::cmpUnordered;
      break;

      // Ordered comparisons return false if either operand is NaN.  Unordered
      // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OEQ:
      Result = CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UGT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGT:
      Result = CmpRes == APFloat::cmpGreaterThan;
      break;

    case FCmpInst::FCMP_UGE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGE:
      Result = CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_ULT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLT:
      Result = CmpRes == APFloat::cmpLessThan;
      break;

    case FCmpInst::FCMP_ULE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLE:
      Result = CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UNE:
      Result = CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual;
      break;
    case FCmpInst::FCMP_ONE:
      Result = CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual;
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    bindLocal(ki, state, ConstantExpr::alloc(Result, Expr::Bool));
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(i->getType()));

    bindLocal(ki, state, result);
    break;
  }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
  case Instruction::Fence: {
    // Ignore for now
    break;
  }
#endif

  // Other instructions...
  // Unhandled
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    terminateStateOnError(state, "XXX vector instructions unhandled",
                          Unhandled);
    break;
 
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::newCheck2Offload() {
	int flag;
	MPI_Status status;
	MPI_Iprobe(MASTER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
	waiting4OffloadReq = true;
	if(flag) {
		if(status.MPI_TAG == OFFLOAD) {
			int count;
			char buffer;
			MPI_Get_count(&status, MPI_CHAR, &count);
			MPI_Recv(&buffer, count, MPI_CHAR, MASTER_NODE, OFFLOAD, MPI_COMM_WORLD, &status);
			if(ENABLE_OFFLOAD_LOGGING) {
				mylogFile << "Offload Request\n";
				mylogFile.flush();
			}
			bool valid;
			std::vector<ExecutionState*> states2Offload;
			int minSize = offloadFromStatesVector(states2Offload);
				
			//found some states
			if(states2Offload.size() > 0) {
				std::vector<unsigned char> commonPref;
				for(int x=0; x<minSize; x++) {
					int val = ((states2Offload[0])->branchHist)[x];
					bool match = true;
					for(int y=1; y<states2Offload.size(); y++) {
						if(val != ((states2Offload[y])->branchHist)[x]) {
							match = false;
							break;
						}
					}
					if(match) {
						commonPref.push_back(val);
					} else {
						break;
					}
				}
				if(ENABLE_OFFLOAD_LOGGING) {
					mylogFile<<"Common Prefix Length: "<<commonPref.size()<<"\n";
				}

				//now combine the prefixes
				//commonPref.push_back('-');
				int start = commonPref.size();
				for(int x=0; x<states2Offload.size(); x++) {
					commonPref.push_back('-');
					for(int y=start; y<states2Offload[x]->branchHist.size(); y++) {
						commonPref.push_back(states2Offload[x]->branchHist[y]);
					}
				}
	
				if(ENABLE_OFFLOAD_LOGGING) {
					mylogFile<<"Combined Prefix Length: "<<commonPref.size()<<"\n";
					mylogFile<<"Prefix: ";
					for(int x=0; x<commonPref.size(); x++) {
						mylogFile<<commonPref[x];
					}
					mylogFile<<"\n";
					mylogFile.flush();
				}
	
				char* pkt2Send = (char*)malloc(commonPref.size()*sizeof(char));
				for(int x=0; x<commonPref.size(); x++) {
					pkt2Send[x] = commonPref[x];
				}
				//if(ENABLE_LOGGING) printPath(pkt2Send, mylogFile, "Packet to Send: ");
				MPI_Send(pkt2Send, commonPref.size(), MPI_CHAR, 0, OFFLOAD_RESP, MPI_COMM_WORLD);

				searcher->update(nullptr, std::vector<ExecutionState *>(), states2Offload);
				for(auto it=states2Offload.begin(); it!=states2Offload.end(); ++it) {
					auto ii = states.find(*it);
					assert(ii != states.end()); //can not be case as the state has to exist
					states.erase(ii); //remove the state from states vector
				
					rangingSuspendedStates.push_back(*it);
					auto hit = std::find(removedStates.begin(), removedStates.end(), *it);
					if(hit != removedStates.end()) {
						removedStates.erase(hit);
					}
				}
			} else {
				char offloadFailed = 'x';
				MPI_Send(&offloadFailed, 1, MPI_CHAR, 0, OFFLOAD_RESP, MPI_COMM_WORLD);
			}
			waiting4OffloadReq = false;
		} else if(status.MPI_TAG == KILL) {
			char dummyRecv;
			MPI_Recv(&dummyRecv, 1, MPI_CHAR, MASTER_NODE, KILL, MPI_COMM_WORLD, &status);
			haltExecution = true;
			haltFromMaster = true;
		}
	}
}

void Executor::check2Offload() {
  int flag;
  MPI_Status status;
  MPI_Iprobe(MASTER_NODE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
  waiting4OffloadReq = true;
	if(flag) {
  	if(status.MPI_TAG == OFFLOAD) {
    	int count;
      char buffer;
    	MPI_Get_count(&status, MPI_CHAR, &count);
    	MPI_Recv(&buffer, count, MPI_CHAR, MASTER_NODE, OFFLOAD, MPI_COMM_WORLD, &status);
    	if(ENABLE_LOGGING) {
        mylogFile << "Offload Request\n";
        mylogFile.flush();
      }
    	bool valid;
    	ExecutionState* state2Remove = offloadFromStatesVector(valid);

    	if(valid) {
        assert(state2Remove);
        //pathWriter->readStream(getPathStreamID(*state2Remove), packet2send);
        char* pkt2Send = (char*)malloc(state2Remove->branchHist.size()*sizeof(char));
        for(int x=0; x<state2Remove->branchHist.size();  x++) {
          pkt2Send[x] = state2Remove->branchHist[x];
        }
        //if(ENABLE_LOGGING) printPath(pkt2Send, mylogFile, "Packet to Send: ");
        MPI_Send(pkt2Send, state2Remove->branchHist.size(), MPI_CHAR, 0, OFFLOAD_RESP, MPI_COMM_WORLD);
        if(ENABLE_LOGGING) {
          mylogFile << "Offloading State Act Depth"<<state2Remove->actDepth<<" Prefix Depth: "<<state2Remove->depth<<"\n";
          mylogFile.flush();
        }
        std::vector<ExecutionState *> remStates;
        remStates.push_back(state2Remove);
        searcher->update(nullptr, std::vector<ExecutionState *>(), remStates);
        auto ii = states.find(state2Remove);
        assert(ii != states.end()); //can not be case as the state has to exist
        states.erase(ii); //remove the state from states vector
        rangingSuspendedStates.push_back(state2Remove);
        auto hit = std::find(removedStates.begin(), removedStates.end(), state2Remove);
        if(hit != removedStates.end()) {
          removedStates.erase(hit);
        }
   		} else {
        char offloadFailed = 'x';
        MPI_Send(&offloadFailed, 1, MPI_CHAR, 0, OFFLOAD_RESP, MPI_COMM_WORLD);
      }
    	waiting4OffloadReq = false;
  	} else if(status.MPI_TAG == KILL) {
      char dummyRecv;
      MPI_Recv(&dummyRecv, 1, MPI_CHAR, MASTER_NODE, KILL, MPI_COMM_WORLD, &status);
      haltExecution = true;
      haltFromMaster = true;
    }
	}
}

void Executor::updateStates(ExecutionState *current) {
  
  if (searcher) {
    if (!removedStates.empty()) {
      /* we don't want to pass suspended states to the searcher */
      std::vector<ExecutionState *> filteredStates;
      for (std::vector<ExecutionState *>::iterator i = removedStates.begin(); i != removedStates.end(); i++) {
        ExecutionState *removedState = *i;
        if (removedState->isNormalState() && removedState->isSuspended()) {
            continue;
        }
        filteredStates.push_back(removedState);
        //numOffloadStates--;
      }
      searcher->update(current, addedStates, filteredStates);
    } else {
      searcher->update(current, addedStates, removedStates);
    }

    /* handle suspended states */
    for (std::vector<ExecutionState *>::iterator i = suspendedStates.begin(); i != suspendedStates.end(); i++) {
      searcher->removeState(*i);
      //numOffloadStates--;
    }
    suspendedStates.clear();

    /* handle resumed states */
    for (std::vector<ExecutionState *>::iterator i = resumedStates.begin(); i != resumedStates.end(); i++) {
      searcher->addState(*i);
      //numOffloadStates++;
    }
    resumedStates.clear();
  }
  
  states.insert(addedStates.begin(), addedStates.end());
  //numOffloadStates = numOffloadStates + addedStates.size();

	//adding states to the suspended states prefix map
  for(auto it = rangingSuspendedStates.begin(); it != rangingSuspendedStates.end(); ++it) {
   	std::vector<unsigned char> recvP;
		for(unsigned int x=0;x<((*it)->branchHist).size();x++) {
  		//std::cout<<recv_prefix[x];
			unsigned char dd = ((*it)->branchHist)[x];
  		if(dd == '2') {
    		recvP.push_back('0');
  		} else if(dd == '3') {
    		recvP.push_back('1');
  		} else if(dd == '-') {
    		continue;
  		} else {
    		recvP.push_back(dd);
  		}
		}

    (*it)->clearPrefixes();
 
		//pathWriter->readStream(getPathStreamID(**it), suspendedStatePath);
    std::string path(recvP.begin(), recvP.end());
    prefixSuspendedStatesMap.insert(std::make_pair(path, *it));
    prefixTree->addToTree(recvP);
  }
	
  addedStates.clear();
	rangingSuspendedStates.clear();

  for (std::vector<ExecutionState *>::iterator it = removedStates.begin(),
                                               ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    if (it2 == states.end()) {
      /* TODO: trying to handle removal of suspended states. Find a better solution... */
      assert(es->isNormalState() && es->isSuspended());
      continue;
    } else {
      //numOffloadStates--;
      states.erase(it2);
    }
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    delete es;
  }
  removedStates.clear();
  
  if(enableLB) newCheck2Offload();
}

ExecutionState* Executor::offloadOriginatingStates(bool &valid) {
  valid = false;
  if(haltExecution || haltFromMaster) return NULL;
  if(states.size() > 1) {
    //look for the first non recovery and non suspended state
    for(auto it=states.begin(); it!=states.end(); ++it) {
      if((*it)->isNormalState() && (!((*it)->isRecoveryState())) &&
        ((!(*it)->isSuspended()))) {
        //continue;
        bool isRemoved=false;
        for(auto it2=removedStates.begin(); it2!=removedStates.end(); ++it2) {
          if(*it == *it2) {
            isRemoved=true;
            break;
          }
        }
        if(isRemoved)
          continue;
        valid = true;
        return *it;
			} else if((*it)->isRecoveryState() && (!(*it)->isSuspended())) {
				bool isRemoved=false;
				for(auto it2=removedStates.begin(); it2!=removedStates.end(); ++it2) {
  				if(*it == *it2) {
    				isRemoved=true;
   					break;
  				}
				}
				if(isRemoved)
  				continue;
				//assert(origState->isNormalState() && !origState->isRecoveryState());
        valid = true;
        return *it;
        //get the originating state that is not a recovery state itself
			}
    } 
  } 
  return NULL;
}

int Executor::offloadFromStatesVector(std::vector<ExecutionState*>& offloadVec) {
	int minSize;	
  if(!haltExecution && !haltFromMaster && ready2Offload) {
		//first calculate the number of states that can be offloaded
		//right now offloading 1/4 of the states
		//for(auto it=states.begin(); it!=states.end(); ++it) {
		//	if(!(*it)->isSuspended()) {
		//		numStates2Offload++;
		//	}
		//}
		//numStates2Offload = numStates2Offload/4;
		//if(numStates2Offload > 16) {
		//	numStates2Offload = 16;
		//}

    //if(numStates2Offload < 1) {
    //  return 0;
    //}
    assert(removedStates.size() == 0);
		for(auto it=states.begin(); it!=states.end(); ++it) {
			if(!(*it)->isSuspended()) {
      	//bool isRemoved=false;
				//for(auto it2=removedStates.begin(); it2!=removedStates.end(); ++it2) {
  			//	if(*it == *it2) {
    	  //			isRemoved=true;
    		//		break;
  			//	}
				//}
				//if(isRemoved)
  			//	continue;
				if(offloadVec.size() == 0) {
					minSize = (*it)->branchHist.size();
				}	else {
					if((*it)->branchHist.size() < minSize) {
						minSize = (*it)->branchHist.size();
					}
				}
				offloadVec.push_back(*it);
			}
			//if(offloadVec.size() == numStates2Offload) {
			//	break;
			//}
		}		
		int numStates2Offload = 0;
    if(offloadVec.size() < 4) {
      offloadVec.clear();
      return 0;
    } else if(offloadVec.size() > 64) {
      //numStates2Offload = offloadVec.size()/4;
      numStates2Offload = 16;
      offloadVec.erase(offloadVec.begin()+16, offloadVec.end());
      minSize = (offloadVec[0]->branchHist).size();
      for(int x=1; x < offloadVec.size(); x++) {
        if((offloadVec[x]->branchHist).size() < minSize) {
          minSize = (offloadVec[x]->branchHist).size();
        }
      }
    } else {
      numStates2Offload = offloadVec.size()/4;
      offloadVec.erase(offloadVec.begin()+numStates2Offload, offloadVec.end());
      minSize = (offloadVec[0]->branchHist).size();
      for(int x=1; x < offloadVec.size(); x++) {
        if((offloadVec[x]->branchHist).size() < minSize) {
          minSize = (offloadVec[x]->branchHist).size();
        }
      }
    }
		if(ENABLE_OFFLOAD_LOGGING) {
			mylogFile << "Number of states that are to be offloaded: "<<" "<<numStates2Offload
        <<" "<<offloadVec.size()<<" "<<states.size()<<"\n";
		}
	}
	if(ENABLE_OFFLOAD_LOGGING) {
		mylogFile << "Found states to offload: "<<offloadVec.size()<<" minSize: "<<minSize<<"\n";
	}
	return minSize;
}

ExecutionState* Executor::offloadFromStatesVector(bool &valid) {
  valid = false;
  if(!ready2Offload) { return NULL; }
  if(haltExecution || haltFromMaster) return NULL;
  //look for the first non recovery and non suspended state
  for(auto it=states.begin(); it!=states.end(); ++it) {
    //if((*it)->isNormalState() && (!((*it)->isRecoveryState())) && 
    if(!(*it)->isSuspended()) {
      bool isRemoved=false;
      for(auto it2=removedStates.begin(); it2!=removedStates.end(); ++it2) {
        if(*it == *it2) {
          isRemoved=true;
          break;
        }
      }
      if(isRemoved)
        continue;
      valid = true;
      return *it;
    }
  }
  valid=false;
  return NULL; 
}

ExecutionState* Executor::offLoad(bool &valid) {

  if(ENABLE_LOGGING) mylogFile << "Offloading\n";
  valid = false;
  if(haltExecution || haltFromMaster) return NULL;
  if(searchMode == "DFS" || searchMode == "BFS" || searchMode == "RAND" ||
      searchMode == "COVNEW") {
    if(searcher->atleast2states()) {
      ExecutionState* resp = searcher->getState2Offload();
      assert(!resp->isRecoveryState());
      if(resp->isSuspended()) {
      //if(resp->isSuspended() || !resp->getSnapshots().empty()) {
        valid = false;
        return NULL;
      }
      valid=true;
      return resp;
    } else {
      valid = false;
      return NULL;
    }
  }
  return NULL;
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (LLVM_TYPE_Q StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else {
      const SequentialType *set = cast<SequentialType>(*ii);
      uint64_t elementSize = 
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index = 
          evalConstant(c)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = 
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
    }
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, klee::gep_type_begin(gepi), klee::gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (std::vector<KFunction*>::iterator it = kmodule->functions.begin(), 
         ie = kmodule->functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell c = {
        .value = evalConstant(kmodule->constants[i])
    };
    kmodule->constantTable.push_back(c);
  }
}

void Executor::checkMemoryUsage() {
  if (!MaxMemory)
    return;
  if ((stats::instructions & 0xFFFF) == 0) {
    // We need to avoid calling GetTotalMallocUsage() often because it
    // is O(elts on freelist). This is really bad since we start
    // to pummel the freelist once we hit the memory cap.
    unsigned mbs = (util::GetTotalMallocUsage() >> 20) +
                   (memory->getUsedDeterministicSize() >> 20);

    if (mbs > MaxMemory) {
      if (mbs > MaxMemory + 100) {
        // just guess at how many to kill
        unsigned numStates = states.size();
        unsigned toKill = std::max(1U, numStates - numStates * MaxMemory / mbs);
        klee_warning("killing %d states (over memory cap)", toKill);
        std::vector<ExecutionState *> arr;
        for (std::set<ExecutionState *>::iterator i = states.begin(); i != states.end(); i++) {
          ExecutionState *toremove = *i;
          if ((toremove->isNormalState() && toremove->isSuspended()) || toremove->isRecoveryState())  {
            continue;
          }
          arr.push_back(toremove);
        }
        for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
          unsigned idx = rand() % N;
          // Make two pulls to try and not hit a state that
          // covered new code.
          if (arr[idx]->coveredNew)
            idx = rand() % N;

          std::swap(arr[idx], arr[N - 1]);
          terminateStateEarly(*arr[N - 1], "Memory limit exceeded.");
        }
      }
      atMemoryLimit = true;
    } else {
      atMemoryLimit = false;
    }
  }
}

void Executor::doDumpStates() {
  if (!DumpStatesOnHalt || states.empty())
    return;
  klee_message("halting execution, dumping remaining states");
  for (std::set<ExecutionState *>::iterator it = states.begin(),
                                            ie = states.end();
       it != ie; ++it) {
    ExecutionState &state = **it;
    stepInstruction(state); // keep stats rolling
    terminateStateEarly(state, "Execution halting.");
  }
  updateStates(0);
}

void Executor::run(ExecutionState &initialState, bool branchLevelHalt, bool pathPrefix) {
  bindModuleConstants();

  // Delay init till now so that ticks don't accrue during
  // optimization and such.
  initTimers();

  enableBranchHalt = branchLevelHalt;

  states.insert(&initialState);
  nonRecoveryStates.insert(&initialState);
  initialState.setPrefix(upperBound);
  initialState.setPrefixDepth(prefixDepth);
  initialState.addPrefix(upperBound, prefixDepth);
  numOffloadStates = 1;

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    
    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(), 
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    double lastTime, startTime = lastTime = util::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) {
        doDumpStates();
        return;
      }

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it = 
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      unsigned numSeeds = it->second.size();
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc;
      stepInstruction(state);

      executeInstruction(state, ki);
      processTimers(&state, MaxInstructionTime * numSeeds);
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        double time = util::getWallTime();
        if (SeedTime>0. && time > startTime + SeedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time >= lastTime+10) {
          lastTime = time;
          lastNumSeeds = numSeeds;          
          klee_message("%d seeds remaining over: %d states", 
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed) {
      doDumpStates();
      return;
    }
  }

  searcher = constructUserSearcher(*this, searchMode);

  std::vector<ExecutionState *> newStates(states.begin(), states.end());
  searcher->update(0, newStates, std::vector<ExecutionState *>());
  
  branchLevel2Halt = explorationDepth;
  haltExecution = false;
  while (!haltFromMaster) {
    int prev_statedepth = 0;
    int prev_recStatedepth = 0;
    while (!states.empty() && !haltExecution) {
      //std::cout << "States Size: "<< states.size() << std::endl;
      assert(!searcher->empty());
      ExecutionState &state = searcher->selectState();
      if(false) mylogFile<<"Selected State Addr: "<<&state<<" NormalState: "
                                  <<state.isNormalState()<<" Recovery State: "
                                  <<state.isRecoveryState()<<" CoreId: "
                                  <<coreId<<" State size: "<<states.size()<<std::endl;
  
      if(false) {
        if(prev_statedepth!=state.actDepth/* && ENABLE_LOGGING*/ && !state.isRecoveryState()) {
          //if(prev_statedepth!=state.actDepth) {
          mylogFile<<"Sd:"<<state.actDepth<<"\n";
          mylogFile.flush();
          prev_statedepth = state.actDepth;
        }
      }
    
      //just do states equal to the numner of workers,
      //phase1depth in this case is the number of workers
      if(enableBranchHalt) {
        if(coreId == 0) {
          cntNumStates2Offload = 0;
          for(auto it = states.begin(); it != states.end(); ++it) {
            if(!(*it)->isSuspended()) {
              cntNumStates2Offload++;
            }
          }
          if(cntNumStates2Offload >= branchLevel2Halt) {
            haltExecution = true;
            haltFromMaster = true;
            break;
          }
        } 
				else {
					//removing states that have reached the termination depth but only do
          //it with normal non-recovery states cause thats how we define
          //a bounded space, is this true??, maybe, likely
          if(!state.isRecoveryState()) {
            if(state.actDepth > branchLevel2Halt) {
              if(ENABLE_LOGGING) {
                mylogFile<<"Removing State: "<<&state<<" "<<states.size()<<" "<<state.depth<<"\n";
                mylogFile.flush();
              }

              std::vector<ExecutionState *> remStates;
              remStates.push_back(&state);
              searcher->update(nullptr, std::vector<ExecutionState *>(), remStates);
              //find the state that we came in with in the states vector
              auto ii = states.find(&state);
              assert(ii != states.end()); //can not be case as the state has to exist
              states.erase(ii); //remove the state from states vector
              //nonRecoveryStates.erase(ii);
              continue;
            }
          }
        }
      }
      KInstruction *ki = state.pc;
      //printStatePath(state, std::cout, "Selected State Path: ");
      stepInstruction(state);
      executeInstruction(state, ki);
      processTimers(&state, MaxInstructionTime);
      checkMemoryUsage();
      updateStates(&state);

			//Look at the states size, and see if anything changes regards to 
			//offload situation of this worker
			if((coreId!=0) && enableLB && (prefixDepth!=0)) {
  			char dummy;
        numOffloadStates = searcher->getSize();
  			if(ready2Offload && (numOffloadStates<OFFLOAD_NOT_READY_THRESH)) {
    			//can not offload now
    			MPI_Send(&dummy, 1, MPI_CHAR, 0, NOT_READY_TO_OFFLOAD, MPI_COMM_WORLD);
    			ready2Offload=false;
    			if(ENABLE_LOGGING) {
      			mylogFile<<"NOT READY2OFF\n";
      			mylogFile.flush();
    			}
  			} else if(!ready2Offload && (numOffloadStates>=OFFLOAD_READY_THRESH)) {
    			//can offload now
    			MPI_Send(&dummy, 1, MPI_CHAR, 0, READY_TO_OFFLOAD, MPI_COMM_WORLD);
    			ready2Offload=true;
    			if(ENABLE_LOGGING) {
     				mylogFile<<"READY2OFF\n";
      			mylogFile.flush();
    			}
  			}
			}
    }

    if((coreId != 0) && (!haltFromMaster)) {
      //tell the master the you have finished working on your prefix
      char result;
      if(ENABLE_LOGGING) {
        mylogFile << "Finish:  "<<coreId<<"\n";
        mylogFile.flush();
      }
      MPI_Send(&result, 1, MPI_CHAR, 0, FINISH, MPI_COMM_WORLD);
      //receive some message from the master
      MPI_Status status;
      MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      int count;
      MPI_Get_count(&status, MPI_CHAR, &count);
      if(status.MPI_TAG == KILL) {
        char dummy2;
        MPI_Recv(&dummy2, 1, MPI_CHAR, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        //std::cout << "Killing Process: "<<coreId<<"\n";
        haltFromMaster = true;
        haltExecution = true;
      } else if (status.MPI_TAG == START_PREFIX_TASK) {
        char* recv_prefix;
        recv_prefix = (char*)malloc(count*sizeof(char));
        MPI_Recv(recv_prefix, count, MPI_CHAR, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        std::cout << "Process: "<<coreId<<" Prefix Task: Length:"<<count<<"\n";
        if(ENABLE_LOGGING) {
          mylogFile << "Process: "<<coreId<<" Prefix Task: Length:"<<count<<"\n";

          if(ENABLE_OFFLOAD_LOGGING) {
            for(int c=0; c<count; c++) {
              mylogFile<<recv_prefix[c];  
            }
          }
          mylogFile<<"\n";
        }

        setLowerBound(recv_prefix);
        setUpperBound(recv_prefix);
        enablePrefixChecking();
				setTestPrefixDepth(count);

        //first, conunt the hyphens
        std::vector<int> dashLoc;
        for(int x=0; x<count; x++) {
          if(recv_prefix[x] == '-') {
            if(ENABLE_OFFLOAD_LOGGING) {
              mylogFile << "Found dash at : "<<x<<"\n";
              mylogFile.flush();
            }
            dashLoc.push_back(x);
          }
        }
        
        std::vector<unsigned char> recvP;
        std::vector<ExecutionState*> rangingResumedStates;
        std::vector<std::string> resumePaths;

        for(int pref=0; pref<dashLoc.size(); pref++) {
          for(int loc=0; loc<dashLoc[0]; loc++) {
            recvP.push_back(recv_prefix[loc]);
          }
          if(ENABLE_OFFLOAD_LOGGING) {
            mylogFile << "Initial Size: "<<recvP.size()<<"\n";
            mylogFile << "Doing offload at loc: "<<dashLoc[pref]<<"\n";
          }
          for(int pl=dashLoc[pref]+1; pl<count; pl++) {
            if(recv_prefix[pl] == '-') {
              if(ENABLE_OFFLOAD_LOGGING) {
                mylogFile<<"Found terminating - at: "<<pl<<"\n";
                mylogFile.flush();
              }
              break;
            }
            recvP.push_back(recv_prefix[pl]);
          }

          if(ENABLE_OFFLOAD_LOGGING) {
            mylogFile<<"PPrefix: "<<recvP.size()<<"\n";
            for(int c=0; c<recvP.size(); c++) {
              mylogFile<<recvP[c];  
            }
            mylogFile.flush();
            mylogFile<<"\n";
          }

          //coverting it to 101010... format
          std::vector<unsigned char> resP;
          for(unsigned int x=0;x<recvP.size();x++) {
  					//std::cout<<recv_prefix[x];
  					unsigned char dd = recvP[x];
  					if(dd == '2') {
    					resP.push_back('0');
  					} else if(dd == '3') {
    					resP.push_back('1');
  					} else if(dd == '-') {
    					continue;
  					} else {
              resP.push_back(dd);
            }
					}

          std::vector<unsigned char> prefixToResume;
          prefixTree->getPathToResume(resP, prefixToResume, mylogFile);
          if(ENABLE_OFFLOAD_LOGGING) {
            mylogFile << "Path to Resume: ";
            for(unsigned int x=0;x<prefixToResume.size();x++) {
              mylogFile<<prefixToResume[x];
            }
            mylogFile<<"\n";
          }

          std::string resumePath(prefixToResume.begin(), prefixToResume.end());
          assert(prefixSuspendedStatesMap.find(resumePath) != prefixSuspendedStatesMap.end());
          ExecutionState* resumedState = prefixSuspendedStatesMap[resumePath];
          if(ENABLE_OFFLOAD_LOGGING) {
            mylogFile << "Resume states prefix lists size: "<<resumedState->getPrefixesSize()<<"\n";
            mylogFile.flush();
          }

          char* stPref = (char*)malloc(recvP.size()*sizeof(char));
          for(int x=0; x<recvP.size(); x++) {
            stPref[x] = recvP[x];
          }
          resumedState->addPrefix(stPref, recvP.size());
          if(ENABLE_OFFLOAD_LOGGING) {
            mylogFile<<"Adding prefix: "<<recvP.size()<<"\n";
            mylogFile.flush();
          }
          
          auto iu = std::find(rangingResumedStates.begin(), rangingResumedStates.end(),
              resumedState);
          if(iu == rangingResumedStates.end()) {
            rangingResumedStates.push_back(resumedState);
            resumePaths.push_back(resumePath);
          }

          recvP.clear();
        }

        if(ENABLE_OFFLOAD_LOGGING) {
          mylogFile << "Number of states ot resume: "<<rangingResumedStates.size()<<"\n";
          mylogFile.flush();
          for(int jj=0; jj<rangingResumedStates.size(); ++jj) {
            mylogFile<<"resume state prefix list: "<<rangingResumedStates[jj]->getPrefixesSize()<<" State depth: "
              <<rangingResumedStates[jj]->depth<<"\n";
            mylogFile.flush();
          }
        }
        
        states.insert(rangingResumedStates.begin(), rangingResumedStates.end());
        std::vector<ExecutionState *> resumedStates(states.begin(), states.end());
        searcher->update(0, resumedStates, std::vector<ExecutionState *>());
        for(auto hh = resumePaths.begin(); hh != resumePaths.end(); ++hh) {
          prefixSuspendedStatesMap.erase(*hh);
        }

        rangingResumedStates.clear();
        resumePaths.clear();
      }
    }
  }
	
	//here empty out all the states into the worklist
	if(enableBranchHalt && (coreId==0)) {
    workList = (char **)malloc(cntNumStates2Offload*sizeof(char*));
    unsigned int stateNum=0;
    for(auto it=states.begin(); it!=states.end(); ++it) {
      if(!(*it)->isSuspended()) {
        addState2WorkList(**it,stateNum);
        stateNum++;
      }
    }
	}
	
  delete searcher;
  searcher = 0;
  
  //doDumpStates();
  
}

bool Executor::addState2WorkList(ExecutionState &state, int count) {

  char* newPath = (char*)malloc(state.branchHist.size()*sizeof(char));
  for (int I = 0; I < state.branchHist.size(); ++I) {
    newPath[I] = state.branchHist[I];
  }
  workList[count] = newPath;
  workListPathSize.push_back(state.branchHist.size());
  return true;
}

std::string Executor::getAddressInfo(ExecutionState &state, 
                                     ref<Expr> address) const{
  std::string Str;
  llvm::raw_string_ostream info(Str);
  info << "\taddress: " << address << "\n";
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second <<"]\n";
  }
  
  MemoryObject hack((unsigned) example);    
  MemoryMap::iterator lower = state.addressSpace.objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower==state.addressSpace.objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address
         << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower!=state.addressSpace.objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower==state.addressSpace.objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address 
           << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}

void Executor::terminateState(ExecutionState &state) {
  if (replayKTest && replayPosition!=replayKTest->numObjects) {
    klee_warning_once(replayKTest,
                      "replay did not consume all objects in test input.");
  }

  if (!state.isRecoveryState()) {
    interpreterHandler->incPathsExplored();
  }

  auto fit = nonRecoveryStates.find(&state);
  if(fit != nonRecoveryStates.end()) { 
    nonRecoveryStates.erase(fit);
  }

  std::vector<ExecutionState *>::iterator it =
      std::find(addedStates.begin(), addedStates.end(), &state);
  if (it==addedStates.end()) {
    state.pc = state.prevPC;
    removedStates.push_back(&state);
  } else {
    // never reached searcher, just delete immediately
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(&state);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    addedStates.erase(it);
    processTree->remove(state.ptreeNode);
    delete &state;
  }
  if(ENABLE_LOGGING) {
    mylogFile << "Terminating state: "<<&state<<"\n";
    mylogFile.flush();
  }
}

void Executor::terminateStateEarly(ExecutionState &state, 
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  if (state.isRecoveryState()) {
    terminateStateRecursively(state);
  } else {
    terminateState(state);
  }
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew || 
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, 0, 0);
  

  if (state.isRecoveryState()) {
    terminateStateRecursively(state);
  } else {
    //if(ENABLE_LOGGING) printStatePath(state, std::cout, "Terminated State Path: ");
    //if(ENABLE_LOGGING) //printStatePath(state, brhistFile, "");
    //if(ENABLE_LOGGING) //brhistFile.flush();
    
    if(ENABLE_LOGGING) {
      for(int x=0; x<(state.branchHist).size(); x++) {
        brhistFile<<state.branchHist[x];
      }
      brhistFile<<"\n";
      brhistFile.flush();
    }
    
    terminateState(state);

  }
}

const InstructionInfo & Executor::getLastNonKleeInternalInstruction(const ExecutionState &state,
    Instruction ** lastInstruction) {
  // unroll the stack of the applications state and find
  // the last instruction which is not inside a KLEE internal function
  ExecutionState::stack_ty::const_reverse_iterator it = state.stack.rbegin(),
      itE = state.stack.rend();

  // don't check beyond the outermost function (i.e. main())
  itE--;

  const InstructionInfo * ii = 0;
  if (kmodule->internalFunctions.count(it->kf->function) == 0){
    ii =  state.prevPC->info;
    *lastInstruction = state.prevPC->inst;
    //  Cannot return yet because even though
    //  it->function is not an internal function it might of
    //  been called from an internal function.
  }

  // Wind up the stack and check if we are in a KLEE internal function.
  // We visit the entire stack because we want to return a CallInstruction
  // that was not reached via any KLEE internal functions.
  for (;it != itE; ++it) {
    // check calling instruction and if it is contained in a KLEE internal function
    const Function * f = (*it->caller).inst->getParent()->getParent();
    if (kmodule->internalFunctions.count(f)){
      ii = 0;
      continue;
    }
    if (!ii){
      ii = (*it->caller).info;
      *lastInstruction = (*it->caller).inst;
    }
  }

  if (!ii) {
    // something went wrong, play safe and return the current instruction info
    *lastInstruction = state.prevPC->inst;
    return *state.prevPC->info;
  }
  return *ii;
}

bool Executor::shouldExitOn(enum TerminateReason termReason) {
  std::vector<TerminateReason>::iterator s = ExitOnErrorType.begin();
  std::vector<TerminateReason>::iterator e = ExitOnErrorType.end();

  for (; s != e; ++s)
    if (termReason == *s)
      return true;

  return false;
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     enum TerminateReason termReason,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;
  Instruction * lastInst;
  const InstructionInfo &ii = getLastNonKleeInternalInstruction(state, &lastInst);
  
  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(lastInst, message)).second) {
    if (shouldExitOn(termReason)) {
      errorCount++;
    }
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: (location information missing) %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");

    std::string MsgString;
    llvm::raw_string_ostream msg(MsgString);
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
      msg << "assembly.ll line: " << ii.assemblyLine << "\n";
    }
    msg << "Stack: \n";
    state.dumpStack(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;

    std::string suffix_buf;
    if (!suffix) {
      suffix_buf = TerminateReasonNames[termReason];
      suffix_buf += ".err";
      suffix = suffix_buf.c_str();
    }

    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }

  if (state.isRecoveryState()) {
    terminateStateRecursively(state);
  } else {
    terminateState(state);
  }

  if (shouldExitOn(termReason)) {
    unsigned int maxCount = interpreterOpts.maxErrorCount;

    if (interpreterOpts.errorLocations.empty()) {
      if (maxCount == 0 || maxCount == errorCount) {
        haltExecution = true;
      }
    } else if (ii.file != "") {
      InterpreterOptions::ErrorLocations &errorLocations = interpreterOpts.errorLocations;
      for (std::vector<ErrorLocationOption>::size_type i = 0; i < errorLocations.size(); ++i) {
        std::string basename = ii.file.substr(ii.file.find_last_of("/\\") + 1);
        InterpreterOptions::ErrorLocations::iterator entry = errorLocations.find(basename);
        if (entry != errorLocations.end()) {
          entry->second.erase(std::remove(entry->second.begin(), entry->second.end(), ii.line), entry->second.end());
          if (entry->second.empty()) {
            errorLocations.erase(entry);
          }
          break;
        }
      }
      if (errorLocations.empty()) {
        haltExecution = true;
        //haltFromMaster = true;
        if(coreId == 0) {
          haltFromMaster = true;
        } else {
          char dummySend;
          MPI_Send(&dummySend, 1, MPI_CHAR, 0, BUG_FOUND, MPI_COMM_WORLD);
        }
      }
    }
  }
}

// XXX shoot me
static const char *okExternalsList[] = { "printf", 
                                         "fprintf", 
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList + 
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
  //std::cout <<"External Function: " << function->getName().str() <<"\n";
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;
  
  if (NoExternals && !okExternals.count(function->getName())) {
    klee_warning("Calling not-OK external function : %s\n",
               function->getName().str().c_str());
    terminateStateOnError(state, "externals disallowed", User);
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(), 
       ae = arguments.end(); ai!=ae; ++ai) {
    if (AllowExternalSymCalls) { // don't bother checking uniqueness
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      ce->toMemory(&args[wordIndex]);
      wordIndex += (ce->getWidth()+63)/64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth()+63)/64;
      } else {
        terminateStateOnExecError(state, 
                                  "external call with symbolic argument: " + 
                                  function->getName());
        return;
      }
    }
  }

  state.addressSpace.copyOutConcretes();

  if (!SuppressExternalWarnings) {

    std::string TmpStr;
    llvm::raw_string_ostream os(TmpStr);
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i=0; i<arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size()-1)
	os << ", ";
    }
    os << ")";
    
    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }
  
  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          External);
    return;
  }

  if (!state.addressSpace.copyInConcretes()) {
    terminateStateOnError(state, "external modified read-only object",
                          External);
    return;
  }

  LLVM_TYPE_Q Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(getGlobalContext())) {
    ref<Expr> e = ConstantExpr::fromMemory((void*) args, 
                                           getWidthForLLVMType(resultType));
    bindLocal(target, state, e);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state, 
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayKTest || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason to?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() % n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.
  
  static unsigned id;
  const Array *array =
      arrayCache.CreateArray("rrws_arr" + llvm::utostr(++id),
                             Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  llvm::errs() << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state, 
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack.back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom) {
  size = toUnique(state, size);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    MemoryObject *mo = NULL;
    if (state.isRecoveryState() && isDynamicAlloc(state.prevPC->inst)) {
      mo = onExecuteAlloc(state, CE->getZExtValue(), isLocal, state.prevPC->inst, zeroMemory, state.prevPC->id);
    } else {
      if (CE->getZExtValue() < HUGE_ALLOC_SIZE) {
        mo = memory->allocate(CE->getZExtValue(), isLocal, false, state.prevPC->inst);
        //FIXME Shikhar updating map
        //memory->updateIdToLLVMValueMap(state.prevPC->inst, state.prevPC->id);
      } else {
        klee_message("NOTE: found huge concrete malloc (size = %ld), returning 0",
                     CE->getZExtValue());
      }
    }
    if (!mo) {
      bindLocal(target, state, 
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      ObjectState *os = bindObjectInState(state, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
      bindLocal(target, state, mo->getBaseExpr());
      
      if (reallocFrom) {
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i=0; i<count; i++)
          os->write(i, reallocFrom->read8(i));
        state.addressSpace.unbindObject(reallocFrom->getObject());
      }
    }
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    // 
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;

    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (!res)
        break;
      example = tmp;
    }

    StatePair fixedSize = fork(state, EqExpr::create(example, size), true);
    
    if (fixedSize.second) { 
      // Check for exactly two values
      ref<ConstantExpr> tmp;
      bool success = solver->getValue(*fixedSize.second, size, tmp);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      bool res;
      success = solver->mustBeTrue(*fixedSize.second, 
                                   EqExpr::create(tmp, size),
                                   res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (res) {
        executeAlloc(*fixedSize.second, tmp, isLocal,
                     target, zeroMemory, reallocFrom);
      } else {
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize = 
          fork(*fixedSize.second, 
               UltExpr::create(ConstantExpr::alloc(HUGE_ALLOC_SIZE, W), size),
               true);
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returning 0");
          bindLocal(target, *hugeSize.first, 
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
        }
        
        if (hugeSize.second) {

          std::string Str;
          llvm::raw_string_ostream info(Str);
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << tmp << "\n";
          terminateStateOnError(*hugeSize.second, "concretized symbolic size",
                                Model, NULL, info.str());
        }
      }
    }

    if (fixedSize.first) // can be zero when fork fails
      executeAlloc(*fixedSize.first, example, isLocal, 
                   target, zeroMemory, reallocFrom);
  }
}

void Executor::executeFree(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "free");
    
    for (Executor::ExactResolutionList::iterator it = rl.begin(), 
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, "free of alloca", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, "free of global", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else {
        it->second->addressSpace.unbindObject(mo);
        if (it->second->isRecoveryState()) {
            onExecuteFree(it->second, mo);
        }
        if (target)
          bindLocal(target, *it->second, Expr::createPointer(0));
      }
    }
  }
}

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results, 
                            const std::string &name) {
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace.resolve(state, solver, p, rl);
  
  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); 
       it != ie; ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());
    
    StatePair branches = fork(*unbound, inBounds, true);
    
    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    terminateStateOnError(*unbound, "memory error: invalid pointer: " + name,
                          Ptr, NULL, getAddressInfo(*unbound, p));
  }
}

void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
  Expr::Width type = (isWrite ? value->getWidth() : 
                     getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints.simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints.simplifyExpr(value);
  }

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(coreSolverTimeout);
  if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize && mo->size>=MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }
    
    ref<Expr> offset = mo->getOffsetExpr(address);

    bool inBounds;
    solver->setTimeout(coreSolverTimeout);
    bool success = solver->mustBeTrue(state, 
                                      mo->getBoundsCheckOffset(offset, bytes),
                                      inBounds);
    solver->setTimeout(0);
    if (!success) {
      state.pc = state.prevPC;
      terminateStateEarly(state, "Query timed out (bounds check).");
      return;
    }

    if (inBounds) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = state.addressSpace.getWriteable(mo, os);
          wos->write(offset, value);
          if (state.isRecoveryState()) {
            onRecoveryStateWrite(state, address, mo, offset, value);
          }
          if (state.isNormalState()) {
            onNormalStateWrite(state, address, value);
          }
        }
      } else {
        ref<Expr> result = os->read(offset, type);
        if (state.isNormalState()) {
          onNormalStateRead(state, address, type);
        }
        
        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);
        
        bindLocal(target, state, result);
      }

      return;
    }
  } 

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)
  
  ResolutionList rl;  
  solver->setTimeout(coreSolverTimeout);
  bool incomplete = state.addressSpace.resolve(state, solver, address, rl,
                                               0, coreSolverTimeout);
  solver->setTimeout(0);
  
  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;
  
  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);
    
    StatePair branches = fork(*unbound, inBounds, true);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped 
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
          wos->write(mo->getOffsetExpr(address), value);
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
        bindLocal(target, *bound, result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }
  
  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "Query timed out (resolve).");
    } else {
      terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr,
                            NULL, getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state, 
                                   const MemoryObject *mo,
                                   const std::string &name) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayKTest) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
    const Array *array = arrayCache.CreateArray(uniqueName, mo->size);
    bindObjectInState(state, mo, false, array);
    state.addSymbolic(mo, array);
    
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, "ran out of inputs during seeding",
                                  User);
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
	    std::stringstream msg;
	    msg << "replace size mismatch: "
		<< mo->name << "[" << mo->size << "]"
		<< " vs " << obj->name << "[" << obj->numBytes << "]"
		<< " in test\n";

            terminateStateOnError(state, msg.str(), User);
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes, 
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, mo, false);
    if (replayPosition >= replayKTest->numObjects) {
      terminateStateOnError(state, "replay count mismatch", User);
    } else {
      KTestObject *obj = &replayKTest->objects[replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", User);
      } else {
        for (unsigned i=0; i<mo->size; i++)
          os->write8(i, obj->bytes[i]);
      }
    }
  }
}

/***/
char** Executor::runFunctionAsMain2(Function *f,
         int argc,
         char **argv,
         char **envp,
         //char** workList_main,
         std::vector<unsigned int> &workListPathSize_main) {
  if(explorationDepth > 0) {
    runFunctionAsMain(f, argc, argv, envp, true);
    states.clear();
    workListPathSize_main = workListPathSize;
    //workList_main = workList;
    return workList;
  }
  else {
    runFunctionAsMain(f, argc, argv, envp);
  }

  if (statsTracker)
    statsTracker->done();
	enablePathPrefixFilter=false;
  return NULL;
}


void Executor::runFunctionAsMain(Function *f,
				int argc,
				char **argv,
				char **envp,
				bool branchLevelHalt) {
  std::vector<ref<Expr> > arguments;

  // force deterministic initialization of memory objects
  srand(1);
  srandom(1);
  
  MemoryObject *argvMO = 0;

	std::string mode="";
	if (branchLevelHalt)
		mode = "Branch Level Halt";
	if(enablePathPrefixFilter)
		mode = "Path Prefix";
	if(branchLevelHalt&&enablePathPrefixFilter)
		mode = "Branch Level Halt with Path Prefix";
	if(ENABLE_LOGGING) mylogFile<<"Search Strategy: "<<searchMode<<"\n";
	if(ENABLE_LOGGING) mylogFile << "Execution Mode: "<<mode<<"\n";
	if(enablePathPrefixFilter) {
		if(ENABLE_LOGGING) mylogFile<<"Executing Prefix: ...";
		for(int it = prefixDepth-10; it < prefixDepth; ++it) {
			if(ENABLE_LOGGING) mylogFile<<upperBound[it];
		}
		if(ENABLE_LOGGING) mylogFile<<"\n";
	}
	if(branchLevelHalt)
		if(ENABLE_LOGGING) mylogFile<<"Branch Level to Halt: "<<explorationDepth<<"\n";

  // In order to make uclibc happy and be closer to what the system is
  // doing we lay out the environments at the end of the argv array
  // (both are terminated by a null). There is also a final terminating
  // null that uclibc seems to expect, possibly the ELF header?
  
  int envc;
  for (envc=0; envp[envc]; ++envc) ;

  unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
  KFunction *kf = kmodule->functionMap[f];
  assert(kf);
  Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
  if (ai!=ae) {
    arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));

    if (++ai!=ae) {
      argvMO = memory->allocate((argc+1+envc+1+1) * NumPtrBytes, false, true,
                                f->begin()->begin());

      if (!argvMO)
        klee_error("Could not allocate memory for function arguments");

      arguments.push_back(argvMO->getBaseExpr());

      if (++ai!=ae) {
        uint64_t envp_start = argvMO->address + (argc+1)*NumPtrBytes;
        arguments.push_back(Expr::createPointer(envp_start));

        if (++ai!=ae)
          klee_error("invalid main function (expect 0-3 arguments)");
      }
    }
  }

  ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);
  
  /*if (pathWriter) 
    state->pathOS = pathWriter->open();
  if (symPathWriter) 
    state->symPathOS = symPathWriter->open();*/


  if (statsTracker)
    statsTracker->framePushed(*state, 0);

  assert(arguments.size() == f->arg_size() && "wrong number of arguments");
  for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
    bindArgument(kf, i, *state, arguments[i]);

  if (argvMO) {
    ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

    for (int i=0; i<argc+1+envc+1+1; i++) {
      if (i==argc || i>=argc+1+envc) {
        // Write NULL pointer
        argvOS->write(i * NumPtrBytes, Expr::createPointer(0));
      } else {
        char *s = i<argc ? argv[i] : envp[i-(argc+1)];
        int j, len = strlen(s);

        MemoryObject *arg = memory->allocate(len+1, false, true, state->pc->inst);
        if (!arg)
          klee_error("Could not allocate memory for function arguments");
        ObjectState *os = bindObjectInState(*state, arg, false);
        for (j=0; j<len+1; j++)
          os->write8(j, s[j]);

        // Write pointer to newly allocated and initialised argv/envp c-string
        argvOS->write(i * NumPtrBytes, arg->getBaseExpr());
      }
    }
  }
  initializeGlobals(*state);

  processTree = new PTree(state);
  state->ptreeNode = processTree->root;
	run(*state, branchLevelHalt, enablePathPrefixFilter);
  delete processTree;
  processTree = 0;
  
  // hack to clear memory objects
  delete memory;
  memory = NULL;
  memory = new MemoryManager(NULL);
 
  if(pathWriter) { 
    delete pathWriter;
  }
  
  globalObjects.clear();
  globalAddresses.clear();

}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state, std::string &res,
                                Interpreter::LogType logFormat) {

  std::ostringstream info;

  switch (logFormat) {
  case STP: {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } break;

  case KQUERY: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprPPrinter::printConstraints(info, state.constraints);
    res = info.str();
  } break;

  case SMTLIB2: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprSMTLIBPrinter printer;
    printer.setOutput(info);
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    printer.setQuery(query);
    printer.generateOutput();
    res = info.str();
  } break;

  default:
    klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
  solver->setTimeout(coreSolverTimeout);

  ExecutionState tmp(state);

  // Go through each byte in every test case and attempt to restrict
  // it to the constraints contained in cexPreferences.  (Note:
  // usually this means trying to make it an ASCII character (0-127)
  // and therefore human readable. It is also possible to customize
  // the preferred constraints.  See test/Features/PreferCex.c for
  // an example) While this process can be very expensive, it can
  // also make understanding individual test cases much easier.
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    std::vector< ref<Expr> >::const_iterator pi = 
      mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
    for (; pi != pie; ++pi) {
      bool mustBeTrue;
      // Attempt to bound byte to constraints held in cexPreferences
      bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), 
					mustBeTrue);
      // If it isn't possible to constrain this particular byte in the desired
      // way (normally this would mean that the byte can't be constrained to
      // be between 0 and 127 without making the entire constraint list UNSAT)
      // then just continue on to the next byte.
      if (!success) break;
      // If the particular constraint operated on in this iteration through
      // the loop isn't implied then add it to the list of constraints.
      if (!mustBeTrue) tmp.addConstraint(*pi);
    }
    if (pi!=pie) break;
  }

  std::vector< std::vector<unsigned char> > values;
  std::vector<const Array*> objects;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values);
  solver->setTimeout(0);
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(llvm::errs(), state.constraints,
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }
  
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly && 
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, it->second);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(LLVM_TYPE_Q llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

///

Interpreter *Interpreter::create(InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(opts, ih);
}
    
bool Executor::isMayBlockingLoad(ExecutionState &state, KInstruction *ki) {
  /* basic check based on static analysis */
  if (!ki->mayBlock) {
    return false;
  }

  /* there is no need for recovery, if the value is not used... */
  if (ki->inst->hasNUses(0)) {
    return false;
  }

  if (!isRecoveryRequired(state, ki)) {
    return false;
  }
  return true;
}

bool Executor::isRecoveryRequired(ExecutionState &state, KInstruction *ki) {
  /* resolve address expression */
  ref<Expr> addressExpr = eval(ki, 0, state).value;
  if (!isa<ConstantExpr>(addressExpr)) {
    addressExpr = state.constraints.simplifyExpr(addressExpr);
    addressExpr = toConstant(state, addressExpr, "resolveOne failure");
  }

  uint64_t address = dyn_cast<ConstantExpr>(addressExpr)->getZExtValue();
  Expr::Width width = getWidthForLLVMType(ki->inst->getType());
  size_t size = Expr::getMinBytesForWidth(width);

  /* check if already recovered */
  if (state.isAddressRecovered(address)) {
    DEBUG_WITH_TYPE(
      DEBUG_BASIC,
      klee_message("%p: load from %#lx is already recovered", &state, address)
    );
    return false;
  }

  /* check if someone has written to this location */
  WrittenAddressInfo info;
  if (!state.getWrittenAddressInfo(address, size, info)) {
    /* this address was not overriden */
    return true;
  }

  /* TODO: handle recovered loads... */
  if (state.getCurrentSnapshotIndex() == info.snapshotIndex) {
    /* TODO: hack... */
    state.markLoadAsUnrecovered();
    DEBUG_WITH_TYPE(
      DEBUG_BASIC,
      klee_message("location (%lx, %zu) was written, recovery is not required", address, size);
    );
    return false;
  }

  return true;
}

bool Executor::handleMayBlockingLoad(ExecutionState &state, KInstruction *ki,
                                     bool &success) {
  success = true;
  /* find which slices should be executed... */
  std::list< ref<RecoveryInfo> > &recoveryInfos = state.getPendingRecoveryInfos();
  if (!getAllRecoveryInfo(state, ki, recoveryInfos)) {
    success = false;
    return false;
  }
  if (recoveryInfos.empty()) {
    /* we are not dependent on previously skipped functions */
    return false;
  }

  /* TODO: move to another place? */
  state.pc = state.prevPC;

  ref<RecoveryInfo> ri = state.getPendingRecoveryInfo();
  if(ENABLE_LOGGING) {
    mylogFile<<"This state saw a blocking load: "<<&state<<" isRec?: "<<state.isRecoveryState()<<" Depth: "<<state.depth<<"\n";
    mylogFile.flush();
  }
  startRecoveryState(state, ri);

  if (!state.isSuspended()) {
    suspendState(state);
  }

  return true;
}

bool Executor::getAllRecoveryInfo(ExecutionState &state, KInstruction *ki,
                                  std::list<ref<RecoveryInfo> > &result) {
  Instruction *loadInst;
  uint64_t loadAddr;
  uint64_t loadSize;
  ModRefAnalysis::AllocSite preciseAllocSite;

  /* TODO: decide which value to pass (original, cloned) */
  loadInst = ki->getOrigInst();
  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("%p: may-blocking load:", &state));
  DEBUG_WITH_TYPE(DEBUG_BASIC, errs() << "- instruction:" << *loadInst << "\n");
  DEBUG_WITH_TYPE(DEBUG_BASIC, errs() << "- stack trace:\n");
  DEBUG_WITH_TYPE(DEBUG_BASIC, state.dumpStack(errs()));

  if (!getLoadInfo(state, ki, loadAddr, loadSize, preciseAllocSite))
    return false;
  
	/* get the allocation site computed by static analysis */
  std::set<ModRefAnalysis::ModInfo> approximateModInfos;
  mra->getApproximateModInfos(ki->getOrigInst(), preciseAllocSite, approximateModInfos);

  /* all the recovery information which may be required  */
  std::list< ref<RecoveryInfo> > required;
  /* the snapshots of the state */
  std::vector< ref<Snapshot> > &snapshots = state.getSnapshots();
  /* we start from the last snapshot which is not affected by an overwrite */
  unsigned int startIndex = state.getStartingIndex(loadAddr, loadSize);

  /* collect recovery information */
  for (unsigned int index = startIndex; index < snapshots.size(); index++) {
    if (state.isRecoveryState()) {
      if (state.getRecoveryInfo()->snapshotIndex == index) {
        break;
      }
    }

    ref<Snapshot> snapshot = snapshots[index];
    Function *snapshotFunction = snapshot->f;
    for (std::set<ModRefAnalysis::ModInfo>::iterator j = approximateModInfos.begin(); 
        j != approximateModInfos.end(); j++) {
      ModRefAnalysis::ModInfo modInfo = *j;
      if (modInfo.first != snapshotFunction) {
        /* the function of the snapshot must match the modifier */
        continue;
      }

      /* get the corresponding slice id */
      ModRefAnalysis::ModInfoToIdMap &modInfoToIdMap = mra->getModInfoToIdMap();
      ModRefAnalysis::ModInfoToIdMap::iterator entry = modInfoToIdMap.find(modInfo);
      if (entry == modInfoToIdMap.end()) {
        llvm_unreachable("ModInfoToIdMap is empty");
      }

      uint32_t sliceId = entry->second;

      /* initialize... */
      ref<RecoveryInfo> recoveryInfo(new RecoveryInfo());
      recoveryInfo->loadInst = loadInst;
      recoveryInfo->loadAddr = loadAddr;
      recoveryInfo->loadSize = loadSize;
      recoveryInfo->f = modInfo.first;
      recoveryInfo->sliceId = sliceId;
      recoveryInfo->snapshot = snapshot;
      recoveryInfo->snapshotIndex = index;
      required.push_back(recoveryInfo);

      /* TODO: validate that each snapshot corresponds to at most one modifier */
      break;
    }
  }

  /* do some filtering... */
  for (std::list< ref<RecoveryInfo> >::reverse_iterator i = required.rbegin(); i != required.rend(); i++) {
    ref<RecoveryInfo> recoveryInfo = *i;
    unsigned int index = recoveryInfo->snapshotIndex;
    unsigned int sliceId = recoveryInfo->sliceId;

    DEBUG_WITH_TYPE(
      DEBUG_BASIC,
      klee_message(
        "recovery info: addr = %#lx, size = %lx, function: %s, slice id = %u, snapshot index = %u",
        recoveryInfo->loadAddr,
        recoveryInfo->loadSize,
        recoveryInfo->f->getName().data(),
        recoveryInfo->sliceId,
        recoveryInfo->snapshotIndex
      )
    );

    ref<Expr> expr;
    if (state.getRecoveredValue(index, sliceId, loadAddr, expr)) {
      /* this slice was already executed from this snapshot,
         and we know which value was written (or not) */
      state.addRecoveredAddress(loadAddr);

      if (!expr.isNull()) {
        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message(
            "%p: cached recovered value (index = %u, slice id = %u, addr = %lx)",
            &state,
            index,
            sliceId,
            loadAddr
          )
        );
        /* execute write without recovering */
        ref<Expr> base = eval(ki, 0, state).value;
        executeMemoryOperation(state, true, base, expr, 0);

        /* TODO: add docs */
        break;

      } else {
        DEBUG_WITH_TYPE(
          DEBUG_BASIC,
          klee_message(
            "%p: ignoring non-modifying slice (index = %u, slice id = %u, addr = %lx)",
            &state,
            index,
            sliceId,
            loadAddr
          )
        );
      }
    } else {
      /* the slice was never executed, so we must add it */
      DEBUG_WITH_TYPE(
        DEBUG_BASIC,
        klee_message(
          "%p: adding recovery info for a non-executed slice (index = %u, slice id = %u)",
          &state,
          index,
          sliceId
        )
      );
      /* TODO: add docs */
      state.updateRecoveredValue(index, sliceId, loadAddr, NULL);
      result.push_front(recoveryInfo);
    }
  }
  return true;
}

bool Executor::getLoadInfo(ExecutionState &state, KInstruction *ki,
                           uint64_t &loadAddr, uint64_t &loadSize,
                           ModRefAnalysis::AllocSite &allocSite) {
  ObjectPair op;
  bool success;
  ConstantExpr *ce;

  ref<Expr> address = eval(ki, 0, state).value;

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address)) {
      address = state.constraints.simplifyExpr(address);
    }
  }

  /* execute solver query */
  solver->setTimeout(coreSolverTimeout);
  if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure (getLoadInfo)");
    success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    /* get load address */
    ce = dyn_cast<ConstantExpr>(address);
    if (!ce) {
      /* TODO: use the resolve() API in order to support symbolic addresses */
      state.dumpStack(llvm::errs());
      llvm_unreachable("getLoadInfo() does not support symbolic addresses");
    }

    loadAddr = ce->getZExtValue();

    /* get load size */
    Expr::Width width = getWidthForLLVMType(ki->inst->getType());
    loadSize = Expr::getMinBytesForWidth(width);

    /* get allocation site value and offset */
    const MemoryObject *mo = op.first;
    /* TODO: we don't actually need the offset... */
    ref<Expr> offsetExpr = mo->getOffsetExpr(address);
    offsetExpr = toConstant(state, offsetExpr, "...");
    ce = dyn_cast<ConstantExpr>(offsetExpr);
    assert(ce);

    /* translate value... */
    /*TODO SLICER FIXES*/
    const Value *translatedValue = cloner->translateValue((Value *)(mo->allocSite));
    //const Value *translatedValue = ((Value *)(mo->allocSite));
    uint64_t offset = ce->getZExtValue();

    /* get the precise allocation site */
    allocSite = std::make_pair(translatedValue, offset);
  } else {
    DEBUG_WITH_TYPE(
      DEBUG_BASIC,
      klee_message("Unable to resolve blocking load address to one memory object")
    );
    ResolutionList rl;
    solver->setTimeout(coreSolverTimeout);
    bool incomplete = state.addressSpace.resolve(state, solver, address, rl, 0,
                                                 coreSolverTimeout);
    solver->setTimeout(0);

    if (rl.empty()) {
      if (!incomplete) {
        klee_warning(
            "Unable to resolve blocking load to any address. Terminating state");
        terminateStateOnError(
            state, "Unable to resolve blocking load to any address", Unhandled);
      } else {
        klee_warning("Unable to resolve blocking load address: Solver timeout");
        terminateStateEarly(
            state, "Unable to resolve blocking load address: solver timeout");
      }
    } else {
      klee_warning("Resolving blocking load address: multiple resolutions");
      terminateStateEarly(
          state, "Resolving blocking load address: multiple resolutions");
    }
    return false;
  }
  return true;
}

void Executor::suspendState(ExecutionState &state) {
  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("suspending: %p", &state));
  state.setSuspended();
  suspendedStates.push_back(&state);

  auto fit = nonRecoveryStates.find(&state);
  if(fit != nonRecoveryStates.end()) { 
    nonRecoveryStates.erase(fit);
  }
  //numOffloadStates--;

}

void Executor::resumeState(ExecutionState &state, bool implicitlyCreated, ExecutionState &recState) {

  if(!state.isRecoveryState()) {
    nonRecoveryStates.insert(&state);
  }

  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("resuming: %p", &state));
  state.setResumed();
  state.setRecoveryState(0);
  state.markLoadAsUnrecovered();
  if (implicitlyCreated) {
    DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("adding an implicitly created state: %p", &state));
    addedStates.push_back(&state);
    if(ENABLE_LOGGING) {
      mylogFile << "Implicitly creating\n";
      mylogFile.flush();
    }
  } else {
    resumedStates.push_back(&state);
  }

  /* debug... */
  state.getAllocationRecord().dump();
   
  replicateBranchHist(&recState, &state);
  if(ENABLE_LOGGING) {
    mylogFile<<"Resuming State: "<<&state<<" depth: "<<state.depth<<" "<<state.getPrefixesSize()<<"\n";
    //printBranchHist(&state);
  }
  //numOffloadStates++;
  //state.depth++;
}

void Executor::onRecoveryStateExit(ExecutionState &state) {
  if(ENABLE_LOGGING) {
    mylogFile<<"Exiting recovery state " << &state<<" "<<state.depth<<"\n";
    mylogFile.flush();
  }
  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("%p: recovery state reached exit instruction", &state));
  ExecutionState *dependentState = state.getDependentState();
  //dumpConstrains(*dependentState);

  /* check if we need to run another recovery state */
  if (dependentState->hasPendingRecoveryInfo()) {
    ref<RecoveryInfo> ri = dependentState->getPendingRecoveryInfo();
    replicateBranchHist(&state, dependentState);
    //dependentState->depth = (dependentState->branchHist).size();
    startRecoveryState(*dependentState, ri);
  } else {
    notifyDependentState(state);
  }
  terminateState(state);
}

void Executor::notifyDependentState(ExecutionState &recoveryState) {
  ExecutionState *dependentState = recoveryState.getDependentState();
  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("%p: notifying dependent state %p", &recoveryState, dependentState));

  if(ENABLE_LOGGING) {
    mylogFile<<"Notifying state for recovery: "<<&recoveryState<<" dependent: "<<dependentState<<"\n";
  }

  if (recoveryState.isNormalState()) {
    /* the allocation record of the recovery states contains the allocation record of the dependent state */
    dependentState->setAllocationRecord(recoveryState.getAllocationRecord());
  }

  if (states.find(dependentState) == states.end()) {
    resumeState(*dependentState, true, recoveryState);
  } else {
    resumeState(*dependentState, false, recoveryState);
  }
}

void Executor::startRecoveryState(ExecutionState &state, ref<RecoveryInfo> recoveryInfo) {
  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message(
      "starting recovery for function %s, load address %#lx",
      recoveryInfo->f->getName().str().c_str(),
      recoveryInfo->loadAddr
    )
  );

  ref<ExecutionState> snapshotState = recoveryInfo->snapshot->state;

  /* TODO: non-first snapshots hold normal state properties! */

  /* initialize recovery state */
  ExecutionState *recoveryState = new ExecutionState(*snapshotState);
  //recoveryState->actDepth = state.actDepth+1;
  
  if (recoveryInfo->snapshotIndex == 0) {
    /* a recovery state which is created from the first snapshot has no dependencies */
    recoveryState->setType(RECOVERY_STATE);
  } else {
    /* in this case, a recovery state may depend on previous skipped functions */
    recoveryState->setType(NORMAL_STATE | RECOVERY_STATE);

    /* initialize... */
    recoveryState->setResumed();
    /* not linked to any recovery state at this point */
    recoveryState->setRecoveryState(0);
    /* TODO: we need only a prefix of the snapshots... */
    recoveryState->markLoadAsRecovered();
    recoveryState->clearRecoveredAddresses();
    /* TODO: we actually need only a prefix of that */
    recoveryState->setRecoveryCache(state.getRecoveryCache());
    /* this state may create another recovery state, so it must hold the allocation record */
    recoveryState->setAllocationRecord(state.getAllocationRecord());
    /* make sure it is empty... */
    assert(recoveryState->getGuidingConstraints().empty());
    /* TODO: handle writtenAddresses */

    assert(recoveryState->getPendingRecoveryInfos().empty());
  }

  /* set exit instruction */
  recoveryState->setExitInst(snapshotState->pc->inst);

  /* set dependent state */
  recoveryState->setDependentState(&state);

  /* set originating state */
  ExecutionState *originatingState;
  if (state.isRecoveryState()) {
    originatingState = state.getOriginatingState();
  } else {
    /* this must be the originating state */
    originatingState = &state;
  }
  recoveryState->setOriginatingState(originatingState);

  /* set recovery information */
  recoveryState->setRecoveryInfo(recoveryInfo);

  /* pass allocation record to recovery state */
  recoveryState->setGuidingAllocationRecord(state.getAllocationRecord());

  /* recursion level */
  unsigned int level = state.isRecoveryState() ? state.getLevel() + 1 : 0;
  recoveryState->setLevel(level);

  /* add the guiding constraints to the recovery state */
  std::set< ref<Expr> > &constraints = originatingState->getGuidingConstraints();
  for (std::set< ref<Expr> >::iterator i = constraints.begin(); i != constraints.end(); i++) {
    addConstraint(*recoveryState, *i);
  }
  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message("adding %lu guiding constraints", constraints.size())
  );

  /* TODO: update prevPC? */
  recoveryState->pc = recoveryState->prevPC;

  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message(
      "adding recovery state: %p (snapshot index = %u, level = %u)",
      recoveryState,
      recoveryInfo->snapshotIndex,
      recoveryState->getLevel()
    )
  );

  /* link the current state to it's recovery state */
  state.setRecoveryState(recoveryState);

  /* update process tree */
  state.ptreeNode->data = 0;
  std::pair<PTree::Node*, PTree::Node*> res = processTree->split(state.ptreeNode, recoveryState, &state);
  recoveryState->ptreeNode = res.first;
  state.ptreeNode = res.second;

  /* add the recovery state to the searcher */
  recoveryState->setPriority(PRIORITY_HIGH);
  addedStates.push_back(recoveryState);

  /* update statistics */
  interpreterHandler->incRecoveryStatesCount();
 

  /* create new branch hist */
  //std::cout<<"Starting rec\n";
  //std::cout.flush();
  replicateBranchHist(&state, recoveryState);
  //recoveryState->depth  = (recoveryState->branchHist).size();
  if(ENABLE_LOGGING) {
    mylogFile << "Starting recovery state and suspending state: "<<&state<<" "<<state.depth<<" "<<recoveryState
      <<" "<<recoveryState->depth<<"\n";
    mylogFile.flush();
    //printBranchHist(recoveryState);
  }
     
  //now that you have verfied the existence of -, move the depth ahead
  //recoveryState->depth++;
}

/* TODO: handle vastart calls */
void Executor::onRecoveryStateWrite(
  ExecutionState &state,
  ref<Expr> address,
  const MemoryObject *mo,
  ref<Expr> offset,
  ref<Expr> value) {
  if(!isa<ConstantExpr>(address)) { return;}
  if(!isa<ConstantExpr>(offset)) { return;}
  assert(isa<ConstantExpr>(address));
  assert(isa<ConstantExpr>(offset));

  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message(
      "write in state %p: mo = %p, address = %lx, size = %x, offset = %lx",
      &state,
      mo,
      mo->address,
      mo->size,
      dyn_cast<ConstantExpr>(offset)->getZExtValue()
    )
  );

  uint64_t storeAddr = dyn_cast<ConstantExpr>(address)->getZExtValue();
  ref<RecoveryInfo> recoveryInfo = state.getRecoveryInfo();
  if (storeAddr != recoveryInfo->loadAddr) {
    return;
  }

  /* copy data to dependent state... */
  ExecutionState *dependentState = state.getDependentState();
  const ObjectState *os = dependentState->addressSpace.findObject(mo);
  ObjectState *wos = dependentState->addressSpace.getWriteable(mo, os);
  wos->write(offset, value);
  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message("copying from %p to %p", &state, dependentState)
  );

  /* TODO: ... */
  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message(
      "%p: updating recovered value for %p (index = %u, slice id = %u)",
      &state,
      dependentState,
      recoveryInfo->snapshotIndex,
      recoveryInfo->sliceId
    )
  );
  dependentState->updateRecoveredValue(
    recoveryInfo->snapshotIndex,
    recoveryInfo->sliceId,
    storeAddr,
    value
  );
}

void Executor::onNormalStateWrite(
  ExecutionState &state,
  ref<Expr> address,
  ref<Expr> value
) {
  if (!state.isInDependentMode()) {
    return;
  }

  if (state.prevPC->inst->getOpcode() != Instruction::Store) {
    /* TODO: this must be a vastart call, check! */
    return;
  }

  if (!isOverridingStore(state.prevPC)) {
    return;
  }

  assert(isa<ConstantExpr>(address));

  uint64_t concreteAddress = dyn_cast<ConstantExpr>(address)->getZExtValue();
  size_t sizeInBytes = value->getWidth() / 8;
  if (value->getWidth() == Expr::Bool) {
    /* in this case, the width of the written value is extended to Int8 */
    sizeInBytes = 1;
  } else {
    sizeInBytes = value->getWidth() / 8;
    assert(sizeInBytes * 8 == value->getWidth());
  }

  /* TODO: don't add if already recovered */
  state.addWrittenAddress(concreteAddress, sizeInBytes, state.getCurrentSnapshotIndex());
  DEBUG_WITH_TYPE(
    DEBUG_BASIC,
    klee_message("%p: adding written address: (%lx, %zu)",
      &state,
      concreteAddress,
      sizeInBytes
    )
  );
}

/* checking if a store may override a skipped function stores ... */
bool Executor::isOverridingStore(KInstruction *ki) {
  assert(ki->inst->getOpcode() == Instruction::Store);
  return ki->mayOverride;
}

void Executor::onNormalStateRead(
  ExecutionState &state,
  ref<Expr> address,
  Expr::Width width
) {
  if (!state.isInDependentMode()) {
    return;
  }

  if (state.isBlockingLoadRecovered()) {
    return;
  }

  assert(isa<ConstantExpr>(address));

  ConstantExpr *ce = dyn_cast<ConstantExpr>(address);
  uint64_t addr = ce->getZExtValue();

  /* update recovered loads */
  state.addRecoveredAddress(addr);
  state.markLoadAsRecovered();
}

void Executor::dumpConstrains(ExecutionState &state) {
  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("constraints (state = %p):", &state));
  for (ConstraintManager::constraint_iterator i = state.constraints.begin(); i != state.constraints.end(); i++) {
      ref<Expr> e = *i;
      DEBUG_WITH_TYPE(DEBUG_BASIC, errs() << "  -- "; e->dump());
  }
}

MemoryObject *Executor::onExecuteAlloc(ExecutionState &state, uint64_t size, bool isLocal, Instruction *allocInst, bool zeroMemory, unsigned id) {
    MemoryObject *mo = NULL;

    /* get the context of the allocation instruction */
    std::vector<Instruction *> callTrace;
    state.getCallTrace(callTrace);
    ASContext context(cloner, callTrace, allocInst);

    ExecutionState *dependentState = state.getDependentState();
    AllocationRecord &guidingAllocationRecord = state.getGuidingAllocationRecord();
    AllocationRecord &allocationRecord = dependentState->getAllocationRecord();

    if (guidingAllocationRecord.exists(context)) {
        /* the address should be already bound */
        mo = guidingAllocationRecord.getAddr(context);
        if (mo) {
            DEBUG_WITH_TYPE(
                DEBUG_BASIC,
                klee_message("%p: reusing allocated address: %lx, size: %lu", &state, mo->address, size)
            );
        } else {
            DEBUG_WITH_TYPE(
                DEBUG_BASIC,
                klee_message("%p: reusing null address", &state)
            );
        }
    } else {
        if (size < HUGE_ALLOC_SIZE) {
            mo = memory->allocate(size, isLocal, false, allocInst);
            //FIXME Shikhar updating map
            //memory->updateIdToLLVMValueMap(allocInst, id);
            DEBUG_WITH_TYPE(
                DEBUG_BASIC,
                klee_message("%p: allocating new address: %lx, size: %lu", &state, mo->address, size)
            );
        } else {
            mo = NULL;
            DEBUG_WITH_TYPE(
                DEBUG_BASIC,
                klee_message("%p: allocating null address", &state)
            );
        }

        /* TODO: do we need to add the MemoryObject here? */
        allocationRecord.addAddr(context, mo);
        if (state.isNormalState()) {
          state.getAllocationRecord().addAddr(context, mo);
        }
    }

    if (mo) {
        /* bind the address to the dependent states */
        bindAll(dependentState, mo, isLocal, zeroMemory);
    }

    return mo;
}

bool Executor::isDynamicAlloc(Instruction *allocInst) {
    CallInst *callInst = dyn_cast<CallInst>(allocInst);
    if (!callInst) {
        return false;
    }

    Value *calledValue = callInst->getCalledValue();
    const char *functions[] = {
        "malloc",
        "calloc",
        "realloc",
    };

    for (unsigned int i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
        if (calledValue->getName() == StringRef(functions[i])) {
            return true;
        }
    }

    return false;
}

void Executor::onExecuteFree(ExecutionState *state, const MemoryObject *mo) {
    ExecutionState *dependentState = state->getDependentState();
    unbindAll(dependentState, mo);
}

void Executor::terminateStateRecursively(ExecutionState &state) {
  ExecutionState *current = &state;
  ExecutionState *next = NULL;

  DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("recursively terminating..."));
  while (current) {
    if (current->isRecoveryState()) {
      next = current->getDependentState();
      assert(next);
    } else {
      next = NULL;
    }

    DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("terminating state %p", current));
    terminateState(*current);
    current = next;
  }
}

void Executor::mergeConstraints(ExecutionState &dependentState, ref<Expr> condition) {
    assert(dependentState.isNormalState());
    addConstraint(dependentState, condition);
}

bool Executor::isFunctionToSkip(ExecutionState &state, Function *f) {
    for (auto i = interpreterOpts.skippedFunctions.begin(), e = interpreterOpts.skippedFunctions.end(); i != e; i++) {
        const SkippedFunctionOption &option = *i;
        if ((option.name == f->getName().str())) {
            Instruction *callInst = state.prevPC->inst;
            const InstructionInfo &info = kmodule->infos->getInfo(callInst);
            const std::vector<unsigned int> &lines = option.lines;

            /* skip any call site */
            if (lines.empty()) {
                return true;
            }

            /* check if we have debug information */
            if (info.line == 0) {
                klee_warning_once(0, "call filter for %s: debug info not found...", option.name.data());
                return true;
            }

            return std::find(lines.begin(), lines.end(), info.line) != lines.end();
        }
    }

    return false;
}

void Executor::bindAll(ExecutionState *state, MemoryObject *mo, bool isLocal, bool zeroMemory) {
    ExecutionState *next;
    do {
        /* this state is a normal state (and might be a recovery state as well) */
        next = NULL;
        if (state->isRecoveryState()) {
            next = state->getDependentState();
        }

        DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("%p: binding address: %lx", state, mo->address));
        if (!state->addressSpace.findObject(mo)) {
            ObjectState *os = bindObjectInState(*state, mo, isLocal);
            /* initialize allocated object */
            if (zeroMemory) {
                os->initializeToZero();
            } else {
                os->initializeToRandom();
            }
        }

        state = next;
    } while (next);
}

void Executor::unbindAll(ExecutionState *state, const MemoryObject *mo) {
    ExecutionState *next;
    do {
        /* this state is a normal state (and might be a recovery state as well) */
        next = NULL;
        if (state->isRecoveryState()) {
            next = state->getDependentState();
        }

        DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("%p: unbinding address %lx", state, mo->address));
        state->addressSpace.unbindObject(mo);

        state = next;
    } while (next);
}

void Executor::forkDependentStates(ExecutionState *trueState, ExecutionState *falseState) {
    ExecutionState *current = trueState->getDependentState();
    ExecutionState *forked = NULL;
    ExecutionState *prevForked = falseState;
    ExecutionState *forkedOriginatingState = NULL;

    /* fork the chain of dependent states */
    do {
        forked = new ExecutionState(*current);
        assert(forked->isSuspended());
        DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("forked dependent state: %p (from %p)", forked, current));

        if (forked->isRecoveryState()) {
            interpreterHandler->incRecoveryStatesCount();
        }

        forked->setRecoveryState(prevForked);
        prevForked->setDependentState(forked);

        current->ptreeNode->data = 0;
        std::pair<PTree::Node*, PTree::Node*> res = processTree->split(current->ptreeNode, forked, current);
        forked->ptreeNode = res.first;
        current->ptreeNode = res.second;

        if (current->isRecoveryState()) {
            prevForked = forked;
            current = current->getDependentState();
        } else {
            forkedOriginatingState = forked;
            current = NULL;
        }
    } while (current);

    /* update originating state */
    current = falseState;
    do {
        if (current->isRecoveryState()) {
            DEBUG_WITH_TYPE(
              DEBUG_BASIC,
              klee_message("%p: updating originating state %p", current, forkedOriginatingState)
            );
            current->setOriginatingState(forkedOriginatingState);
            current = current->getDependentState();
        } else {
            /* TODO: initialize originating state to NULL? */
            current = NULL;
        }
    } while (current);
}

void Executor::mergeConstraintsForAll(ExecutionState &recoveryState, ref<Expr> condition) {
    ExecutionState *next = recoveryState.getDependentState();
    do {
        mergeConstraints(*next, condition);

        if (next->isRecoveryState()) {
            next = next->getDependentState();
        } else {
            next = NULL;
        }
    } while (next);

    /* add the guiding constraints only to the originating state */
    ExecutionState *originatingState = recoveryState.getOriginatingState();
    originatingState->addGuidingConstraint(condition);
}

/* on demand slicing... */
Function *Executor::getSlice(Function *target, uint32_t sliceId, ModRefAnalysis::SideEffectType type,
    uint32_t subId) {
    Cloner::SliceInfo *sliceInfo = NULL;

    sliceInfo = cloner->getSliceInfo(target, sliceId);
    if (!sliceInfo || !sliceInfo->isSliced) {
        DEBUG_WITH_TYPE(DEBUG_BASIC,
            klee_message("generating slice for: %s (id = %u)", target->getName().data(), sliceId)
        );
        sliceGenerator->generateSlice(target, sliceId, type);
        sliceGenerator->dumpSlice(target, sliceId, true);

        /* update statistics */
        interpreterHandler->incGeneratedSlicesCount();

        if (!sliceInfo) {
            sliceInfo = cloner->getSliceInfo(target, sliceId);
            assert(sliceInfo);
        }

        std::set<Function *> &reachable = ra->getReachableFunctions(target);
        for (std::set<Function *>::iterator i = reachable.begin(); i != reachable.end(); i++) {
            /* original function */
            Function *f = *i;
            if (f->isDeclaration()) {
                continue;
            }

            /* get the cloned function (using the slice id) */
            Function *cloned = cloner->getSliceInfo(f, sliceId)->f;
            if (cloned->isDeclaration()) {
                /* a sliced function can become empty (a decleration) */
                continue;
            }

            /* initialize KFunction */
            KFunction *kcloned = new KFunction(cloned, kmodule);
            kcloned->isCloned = true;

            DEBUG_WITH_TYPE(DEBUG_BASIC, klee_message("adding function: %s", cloned->getName().data()));
            /* update debug info */
            kmodule->infos->addClonedInfo(cloner, cloned);
            /* update function map */
            kmodule->addFunction(kcloned, true, cloner, mra);
            /* update the instruction constants of the new KFunction */
            for (unsigned i = 0; i < kcloned->numInstructions; ++i) {
                bindInstructionConstants(kcloned->instructions[i]);
            }
            /* when we add a KFunction, additional constants might be added */
            for (unsigned i = kmodule->constantTable.size(); i < kmodule->constants.size(); ++i) {
                Cell c = {
                    .value = evalConstant(kmodule->constants[i])
                };
                kmodule->constantTable.push_back(c);
            }
        }
    }

    return sliceInfo->f;
}

ExecutionState *Executor::createSnapshotState(ExecutionState &state) {
    ExecutionState *snapshotState = new ExecutionState(state);

    /* remove guiding constraints */
    snapshotState->clearGuidingConstraints();

    return snapshotState;
}

bool Executor::checkRange(std::vector<unsigned char> inPath) {
  bool violatePrefix = false;
  if(ENABLE_LOGGING) {
    mylogFile << "Checking feasibility of Path: ";
    for (auto I = inPath.begin(); I != inPath.end(); ++I) {
      mylogFile << *I;
    }
    mylogFile << "\n";
    mylogFile.flush();
  } 

  int minLen = (inPath.size()<prefixDepth)?inPath.size():prefixDepth;
  if (convertPath2Number(inPath, minLen) != convertPath2Number(upperBound, minLen)) {
    violatePrefix = true;
  }
  return violatePrefix;
}

int Executor::convertPath2Number(std::vector<unsigned char> inPath, int upto) {
  int limit = (upto==0)?(inPath.size()-1):(upto-1);
  int sum = 0, index=0;
  for(int it=limit; it >= 0; it--) {
    if(inPath[it]=='1') {
      sum = sum+pow(2,index);
    }
    index++;
  }
  return sum;
}

int Executor::convertPath2Number(char* inPath, int upto) {
  int limit = (upto==0)?(prefixDepth-1):(upto-1);
  int sum = 0, index=0;
  for(int it=limit; it >= 0; it--) {
    if(inPath[it]=='1') {
      sum = sum+pow(2,index);
    }
    index++;
  }
  return sum;
}

//adding a utility to print paths
void Executor::printPath(char* path, std::ostream& log, std::string message) {
  log<<message;
  for (int I = 0; I != prefixDepth; ++I) {
  	log << path[I];
	}
  log	<< std::endl;
}

void Executor::printStatePath(ExecutionState& state, std::ostream& log, std::string message) {
  std::vector<unsigned char> lastTestPath;
  pathWriter->readStream(getPathStreamID(state), lastTestPath);
  for(int x=0; x<lastTestPath.size(); x++) {
    log<<lastTestPath[x];
  }
  log<<"\n";
}

void Executor::replicateBranchHist(ExecutionState* state, ExecutionState* recState) {
  assert(recState->depth <= state->depth);
  for(int x=(recState->branchHist).size(); x<(state->branchHist).size(); x++) {
  //for(int x=0; x<(state->branchHist).size(); x++) {
    (recState->branchHist).push_back((state->branchHist)[x]);
  }
  recState->depth = state->depth;
  recState->prefixes = state->prefixes;
}

void Executor::printBranchHist(ExecutionState* state) {
  mylogFile<<"Branch History: ";
  for(int x=0; x<(state->branchHist).size(); x++) {
    mylogFile<<state->branchHist[x];
  }
  mylogFile<<"\n";
  mylogFile.flush();
}

//===-- Searcher.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_SEARCHER_H
#define KLEE_SEARCHER_H

#include "PTree.h"

#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <set>
#include <map>
#include <queue>
#include <unordered_map>

namespace llvm {
  class BasicBlock;
  class Function;
  class Instruction;
  class raw_ostream;
}

namespace klee {
  template<class T> class DiscretePDF;
  class ExecutionState;
  class Executor;

  class Searcher {
  public:
    virtual ~Searcher();

    virtual ExecutionState &selectState() = 0;
    virtual ExecutionState* getState2Offload() = 0;
    virtual bool atleast2states() = 0;


    virtual void update(ExecutionState *current,
                        const std::vector<ExecutionState *> &addedStates,
                        const std::vector<ExecutionState *> &removedStates) = 0;

    virtual bool empty() = 0;

    virtual unsigned int getSize() = 0;

    // prints name of searcher as a klee_message()
    // TODO: could probably make prettier or more flexible
    virtual void printName(llvm::raw_ostream &os) {
      os << "<unnamed searcher>\n";
    }

    // pgbovine - to be called when a searcher gets activated and
    // deactivated, say, by a higher-level searcher; most searchers
    // don't need this functionality, so don't have to override.
    virtual void activate() {}
    virtual void deactivate() {}

    // utility functions

    void addState(ExecutionState *es, ExecutionState *current = 0) {
      std::vector<ExecutionState *> tmp;
      tmp.push_back(es);
      update(current, tmp, std::vector<ExecutionState *>());
    }

    void removeState(ExecutionState *es, ExecutionState *current = 0) {
      std::vector<ExecutionState *> tmp;
      tmp.push_back(es);
      update(current, std::vector<ExecutionState *>(), tmp);
    }

    enum CoreSearchType {
      DFS,
      BFS,
      RandomState,
      RandomPath,
      NURS_CovNew,
      NURS_MD2U,
      NURS_Depth,
      NURS_ICnt,
      NURS_CPICnt,
      NURS_QC
    };

    enum RecoverySearchType {
      RS_DFS,
      RS_RandomPath,
    };
  };

  class DFSSearcher : public Searcher {
    std::vector<ExecutionState*> states;

    public:
    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return (states.size()>1?true:false); }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return states.empty(); }
    unsigned int getSize() { return states.size(); }
    void printName(llvm::raw_ostream &os) {
      os << "DFSSearcher\n";
    }
  };

  class BFSSearcher : public Searcher {
    std::deque<ExecutionState*> states;
    std::unordered_map<ExecutionState*,int> depthMap;
    std::unordered_map<unsigned int, std::deque<ExecutionState*>> depthStatesMap;
    unsigned int currentMinDepth;

  public:
    BFSSearcher();
    ~BFSSearcher();
    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return ((depthStatesMap[currentMinDepth]).size()>1?true:false);}
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    void insertIntoDepthStateMap(ExecutionState * current);
    void removeFromDepthStateMap(ExecutionState* current);
    void updateDepthStateMap(ExecutionState* current, int oldDepth);
    bool empty() { return states.empty(); }
    unsigned int getSize() { return states.size(); }
    void printName(llvm::raw_ostream &os) {
      os << "BFSSearcher\n";
    }
  };

  class RandomSearcher : public Searcher {
    std::vector<ExecutionState*> states;

  public:
    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return (states.size()>1?true:false); }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return states.empty(); }
    unsigned int getSize() { return states.size(); }
    void printName(llvm::raw_ostream &os) {
      os << "RandomSearcher\n";
    }
  };

  class WeightedRandomSearcher : public Searcher {
  public:
    enum WeightType {
      Depth,
      QueryCost,
      InstCount,
      CPInstCount,
      MinDistToUncovered,
      CoveringNew
    };

  private:
    DiscretePDF<ExecutionState*> *states;
    WeightType type;
    bool updateWeights;
    
    double getWeight(ExecutionState*);

  public:
    WeightedRandomSearcher(WeightType type);
    ~WeightedRandomSearcher();
    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    //bool atleast2states() { return false; }
    bool atleast2states();
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty();
    unsigned int getSize() {return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "WeightedRandomSearcher::";
      switch(type) {
      case Depth              : os << "Depth\n"; return;
      case QueryCost          : os << "QueryCost\n"; return;
      case InstCount          : os << "InstCount\n"; return;
      case CPInstCount        : os << "CPInstCount\n"; return;
      case MinDistToUncovered : os << "MinDistToUncovered\n"; return;
      case CoveringNew        : os << "CoveringNew\n"; return;
      default                 : os << "<unknown type>\n"; return;
      }
    }
  };

  class RandomPathSearcher : public Searcher {
    Executor &executor;

  public:
    RandomPathSearcher(Executor &_executor);
    ~RandomPathSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty();
    unsigned int getSize() {return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "RandomPathSearcher\n";
    }
  };

  class MergingSearcher : public Searcher {
    Executor &executor;
    std::set<ExecutionState*> statesAtMerge;
    Searcher *baseSearcher;
    llvm::Function *mergeFunction;

  private:
    llvm::Instruction *getMergePoint(ExecutionState &es);

  public:
    MergingSearcher(Executor &executor, Searcher *baseSearcher);
    ~MergingSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return baseSearcher->empty() && statesAtMerge.empty(); }
    unsigned int getSize() {return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "MergingSearcher\n";
    }
  };

  class BumpMergingSearcher : public Searcher {
    Executor &executor;
    std::map<llvm::Instruction*, ExecutionState*> statesAtMerge;
    Searcher *baseSearcher;
    llvm::Function *mergeFunction;

  private:
    llvm::Instruction *getMergePoint(ExecutionState &es);

  public:
    BumpMergingSearcher(Executor &executor, Searcher *baseSearcher);
    ~BumpMergingSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return baseSearcher->empty() && statesAtMerge.empty(); }
    unsigned int getSize() {return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "BumpMergingSearcher\n";
    }
  };

  class BatchingSearcher : public Searcher {
    Searcher *baseSearcher;
    double timeBudget;
    unsigned instructionBudget;

    ExecutionState *lastState;
    double lastStartTime;
    unsigned lastStartInstructions;

  public:
    BatchingSearcher(Searcher *baseSearcher, 
                     double _timeBudget,
                     unsigned _instructionBudget);
    ~BatchingSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return baseSearcher->empty(); }
    unsigned int getSize() {return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "<BatchingSearcher> timeBudget: " << timeBudget
         << ", instructionBudget: " << instructionBudget
         << ", baseSearcher:\n";
      baseSearcher->printName(os);
      os << "</BatchingSearcher>\n";
    }
  };

  class IterativeDeepeningTimeSearcher : public Searcher {
    Searcher *baseSearcher;
    double time, startTime;
    std::set<ExecutionState*> pausedStates;

  public:
    IterativeDeepeningTimeSearcher(Searcher *baseSearcher);
    ~IterativeDeepeningTimeSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return baseSearcher->empty() && pausedStates.empty(); }
    unsigned int getSize() { return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "IterativeDeepeningTimeSearcher\n";
    }
  };

  class InterleavedSearcher : public Searcher {
    typedef std::vector<Searcher*> searchers_ty;

    searchers_ty searchers;
    unsigned index;

  public:
    explicit InterleavedSearcher(const searchers_ty &_searchers);
    ~InterleavedSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty() { return searchers[0]->empty(); }
    unsigned int getSize() { return 0; }
    
    void printName(llvm::raw_ostream &os) {
      os << "<InterleavedSearcher> containing "
         << searchers.size() << " searchers:\n";
      for (searchers_ty::iterator it = searchers.begin(), ie = searchers.end();
           it != ie; ++it)
        (*it)->printName(os);
      os << "</InterleavedSearcher>\n";
    }
  };

  class SplittedSearcher : public Searcher {
    Searcher *baseSearcher;
    Searcher *recoverySearcher;
    unsigned int ratio;

  public:
    SplittedSearcher(Searcher *baseSearcher, Searcher *recoverySearcher, unsigned int ratio);
    ~SplittedSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return (baseSearcher->atleast2states()); }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty();
    unsigned int getSize() { return (baseSearcher->getSize() + recoverySearcher->getSize()); } 
    void printName(llvm::raw_ostream &os) {
      os << "SplittedSearcher\n";
      os << "- base searcher: "; baseSearcher->printName(os);
      os << "- recovery searcher: "; recoverySearcher->printName(os);
      os << "- ratio = " << ratio << "\n";
    }
  };

  class RandomRecoveryPath : public Searcher {
    Executor &executor;
    /* a stack of recovery states,
     * where each state is the root of a recovery tree
     */
    std::stack<PTree::Node *> treeStack;
    /* this is a simple way to keep track of the states of the recovery trees */
    std::vector<ExecutionState *> states;

  public:
    RandomRecoveryPath(Executor &executor);

    ~RandomRecoveryPath();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }
    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);

    bool empty();
    unsigned int getSize() { return 0; }

    void printName(llvm::raw_ostream &os) {
      os << "RandomRecoveryPath\n";
    }

  };

  class OptimizedSplittedSearcher : public Searcher {
    Searcher *baseSearcher;
    Searcher *recoverySearcher;
    Searcher *highPrioritySearcher;
    unsigned int ratio;

  public:
    OptimizedSplittedSearcher(
      Searcher *baseSearcher,
      Searcher *recoverySearcher,
      Searcher *highPrioritySearcher,
      unsigned int ratio
    );
    ~OptimizedSplittedSearcher();

    ExecutionState &selectState();
    ExecutionState* getState2Offload();
    bool atleast2states() { return false; }

    void update(ExecutionState *current,
                const std::vector<ExecutionState *> &addedStates,
                const std::vector<ExecutionState *> &removedStates);
    bool empty();
    unsigned int getSize() { return 0; }
    void printName(llvm::raw_ostream &os) {
      os << "OptimizedSplittedSearcher\n";
      os << "- base searcher: "; baseSearcher->printName(os);
      os << "- low priority searcher: "; recoverySearcher->printName(os);
      os << "- high priority searcher: "; highPrioritySearcher->printName(os);
      os << "- ratio = " << ratio << "\n";
    }
  };
}

#endif

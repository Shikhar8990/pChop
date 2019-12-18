//===-- UserSearcher.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "UserSearcher.h"

#include "Searcher.h"
#include "Executor.h"

#include "klee/Internal/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace klee;

namespace {
  cl::list<Searcher::CoreSearchType>
  CoreSearch("search", cl::desc("Specify the search heuristic (default=random-path interleaved with nurs:covnew)"),
	     cl::values(clEnumValN(Searcher::DFS, "dfs", "use Depth First Search (DFS)"),
			clEnumValN(Searcher::BFS, "bfs", "use Breadth First Search (BFS), where scheduling decisions are taken at the level of (2-way) forks"),
			clEnumValN(Searcher::RandomState, "random-state", "randomly select a state to explore"),
			clEnumValN(Searcher::RandomPath, "random-path", "use Random Path Selection (see OSDI'08 paper)"),
			clEnumValN(Searcher::NURS_CovNew, "nurs:covnew", "use Non Uniform Random Search (NURS) with Coverage-New"),
			clEnumValN(Searcher::NURS_MD2U, "nurs:md2u", "use NURS with Min-Dist-to-Uncovered"),
			clEnumValN(Searcher::NURS_Depth, "nurs:depth", "use NURS with 2^depth"),
			clEnumValN(Searcher::NURS_ICnt, "nurs:icnt", "use NURS with Instr-Count"),
			clEnumValN(Searcher::NURS_CPICnt, "nurs:cpicnt", "use NURS with CallPath-Instr-Count"),
			clEnumValN(Searcher::NURS_QC, "nurs:qc", "use NURS with Query-Cost"),
			clEnumValEnd));

  cl::opt<bool>
  UseIterativeDeepeningTimeSearch("use-iterative-deepening-time-search", 
                                    cl::desc("(experimental)"));

  cl::opt<bool>
  UseBatchingSearch("use-batching-search", 
		    cl::desc("Use batching searcher (keep running selected state for N instructions/time, see --batch-instructions and --batch-time)"),
		    cl::init(false));

  cl::opt<unsigned>
  BatchInstructions("batch-instructions",
                    cl::desc("Number of instructions to batch when using --use-batching-search"),
                    cl::init(10000));
  
  cl::opt<double>
  BatchTime("batch-time",
            cl::desc("Amount of time to batch when using --use-batching-search"),
            cl::init(5.0));


  cl::opt<bool>
  UseMerge("use-merge", 
           cl::desc("Enable support for klee_merge() (experimental)"));
 
  cl::opt<bool>
  UseBumpMerge("use-bump-merge", 
           cl::desc("Enable support for klee_merge() (extra experimental)"));

  cl::opt<bool> UseSplittedSearcher("split-search", cl::desc("..."));

  cl::list<Searcher::RecoverySearchType> RecoverySearch(
    "recovery-search",
    cl::desc("Specify the recovery search heuristic (disabled by default)"),
	  cl::values(
      clEnumValN(Searcher::RS_DFS, "dfs", "use depth first search"),
      clEnumValN(Searcher::RS_RandomPath, "random-path", "use random path selection"),
      clEnumValEnd
    )
  );

  cl::opt<unsigned int>
  SplitRatio("split-ratio",
            cl::desc("ratio for choosing recovery states (default = 20)"),
            cl::init(20));
}


bool klee::userSearcherRequiresMD2U() {
  return (std::find(CoreSearch.begin(), CoreSearch.end(), Searcher::NURS_MD2U) != CoreSearch.end() ||
	  std::find(CoreSearch.begin(), CoreSearch.end(), Searcher::NURS_CovNew) != CoreSearch.end() ||
	  std::find(CoreSearch.begin(), CoreSearch.end(), Searcher::NURS_ICnt) != CoreSearch.end() ||
	  std::find(CoreSearch.begin(), CoreSearch.end(), Searcher::NURS_CPICnt) != CoreSearch.end() ||
	  std::find(CoreSearch.begin(), CoreSearch.end(), Searcher::NURS_QC) != CoreSearch.end());
}


Searcher *getNewSearcher(Searcher::CoreSearchType type, Executor &executor) {
  Searcher *searcher = NULL;
  switch (type) {
  case Searcher::DFS: searcher = new DFSSearcher(); break;
  case Searcher::BFS: searcher = new BFSSearcher(); break;
  case Searcher::RandomState: searcher = new RandomSearcher(); break;
  case Searcher::RandomPath: searcher = new RandomPathSearcher(executor); break;
  case Searcher::NURS_CovNew: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::CoveringNew); break;
  case Searcher::NURS_MD2U: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::MinDistToUncovered); break;
  case Searcher::NURS_Depth: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::Depth); break;
  case Searcher::NURS_ICnt: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::InstCount); break;
  case Searcher::NURS_CPICnt: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::CPInstCount); break;
  case Searcher::NURS_QC: searcher = new WeightedRandomSearcher(WeightedRandomSearcher::QueryCost); break;
  }

  return searcher;
}

Searcher *klee::constructUserSearcher(Executor &executor, std::string searchMode) {

  std::cout << "User Searcher Search Strategy:" << searchMode << "\n";
  std::cout.flush();
  Searcher *searcher;// = getNewSearcher(Searcher::BFS, executor);
  if(searchMode=="DFS") {
    searcher = getNewSearcher(Searcher::DFS, executor);
  } else if(searchMode=="RAND") {
    searcher = getNewSearcher(Searcher::RandomState, executor);
  } else if(searchMode=="COVNEW") {
    searcher = getNewSearcher(Searcher::NURS_CovNew, executor);
  } else {
    searcher = getNewSearcher(Searcher::DFS, executor); 
  }

  if (UseSplittedSearcher) {
    std::cout<<"Using splitted searcher \n";
    std::cout.flush();
    Searcher *searcher1;    
    /* TODO: Should both of the searchers be of the same type? */
    if(searchMode=="DFS") {
      searcher1 = getNewSearcher(Searcher::DFS, executor);
    } else if(searchMode=="RAND") {
      searcher1 = getNewSearcher(Searcher::RandomState, executor);
    } else if(searchMode=="COVNEW") {
      searcher1 = getNewSearcher(Searcher::NURS_CovNew, executor);
    } else {
      searcher1 = getNewSearcher(Searcher::DFS, executor);
    }

    searcher = new SplittedSearcher(searcher, searcher1, SplitRatio);
    return searcher;
  }                                                                          

  return searcher;
}

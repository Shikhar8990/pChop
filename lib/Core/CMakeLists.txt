#===------------------------------------------------------------------------===#
#
#                     The KLEE Symbolic Virtual Machine
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#
klee_add_component(kleeCore
  AddressSpace.cpp
  CallPathManager.cpp
  Context.cpp
  CoreStats.cpp
  ExecutionState.cpp
  Executor.cpp
  ExecutorTimers.cpp
  ExecutorUtil.cpp
  ExternalDispatcher.cpp
  ImpliedValue.cpp
  Memory.cpp
  MemoryManager.cpp
  PTree.cpp
  Searcher.cpp
  SeedInfo.cpp
  SpecialFunctionHandler.cpp
  StatsTracker.cpp
  TimingSolver.cpp
  UserSearcher.cpp
  ASContext.cpp
  AllocationRecord.cpp
  PrefixTree.cpp
)

# TODO: Work out what the correct LLVM components are for
# kleeCore.
set(LLVM_COMPONENTS
  core
  support
)

if ("${LLVM_PACKAGE_VERSION}" VERSION_EQUAL "3.6" OR
    "${LLVM_PACKAGE_VERSION}" VERSION_GREATER "3.6")
  list(APPEND LLVM_COMPONENTS mcjit executionengine native)
else()
  list(APPEND LLVM_COMPONENTS jit engine)
endif()


klee_get_llvm_libs(LLVM_LIBS ${LLVM_COMPONENTS})
target_link_libraries(kleeCore PUBLIC ${LLVM_LIBS})
target_include_directories(kleeCore PUBLIC
    ${SVF_ROOT_DIR}/include
    ${DG_ROOT_DIR}/src
    ${DG_ROOT_DIR}/tools
)
target_link_libraries(kleeCore PRIVATE
  kleeAnalysis
  kleeBasic
  kleeModule
  kleaverSolver
  kleaverExpr
  kleeSupport
)

################################################################################
# MPI
################################################################################
find_package(MPI)
if (MPI_FOUND)
  include_directories(SYSTEM ${MPI_INCLUDE_PATH})
endif()

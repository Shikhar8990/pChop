# pChop

A parallel implementation of Chopper(https://github.com/davidtr1037/chopper).

## Build

  Process similar to Chopper 

* Build LLVM 3.4 using CMake
  ```
  mkdir build
  cd build
  cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON <LLVM_SRC>
  make
  ```
* Build SVF (Pointer Analysis)
  https://github.com/davidtr1037/SVF/tree/master
* Build DG (Static Slicing)
  https://github.com/davidtr1037/dg/tree/master
* Build klee-uclibc
  https://github.com/davidtr1037/klee-uclibc
* STP Solver
  https://github.com/stp/stp
* Set up Open MPI 
  https://github.com/open-mpi/ompi 
* Build pChop
  ```
  git checkout master
  mkdir pchop_build
  cd pchop_build
  CXXFLAGS="-fno-rtti" cmake \ 
      -DENABLE_SOLVER_STP=ON \
      -DENABLE_POSIX_RUNTIME=ON \
      -DENABLE_KLEE_UCLIBC=ON \
      -DKLEE_UCLIBC_PATH=<KLEE_UCLIBC_DIR> \
      -DLLVM_CONFIG_BINARY=<LLVM_CONFIG_PATH> \
      -DENABLE_UNIT_TESTS=OFF \
      -DKLEE_RUNTIME_BUILD_TYPE=Release+Asserts \
      -DENABLE_SYSTEM_TESTS=ON \
      -DSVF_ROOT_DIR=<SVF_PROJECT_DIR> \
      -DDG_ROOT_DIR=<DG_PROJECT_DIR> \
      <PCHOP_ROOT_DIR>
  make
  ```
  
## Running pChop

One can run pChop on LLVM bitcode (.bc) files similar to KLEE/Chopper. 
pChop specific command line arguments.
*timeOut : serch terminates after timeOut seconds
*output-dir : Name of the output directory prefix storing the tests. If there are four workers
              the name of the directories would be output-dir1, output-dir2, outpit-dir3 and
              output-dir4; output-dir0 belongs to the coordinator.
*lb : flag to enable load balancing (off by default)
*phase1Depth : number of states to generate for initial distribution (best to have value same as the number of workers)
               should be 0 if using only 1 worker
*phase2Depth : depth at which to terminate execution (should be 0 if doing time bound exploration)
*searchPolicy : search strategy (BFS, DFS or RAND)

### Sample Command
```
mpirun -n 6 /path/to/pchop/bin/klee --libc=uclibc --posix-runtime --timeOut=1800 --inline=memcpy,strlen --skip-functions=_asn1_set_value:1043,asn1_der_decoding_bb --lb -max-memory=4096 --output-dir=DFS_4_137 --phase1Depth=4 --phase2Depth=0 --searchPolicy=DFS test.bc 32
```

## Framework-wide configuration
##
## NOTE:  this file will be included by the specific makefiles
##
## The only thing that we assume here is that the specific makefile
## defines the ROOT directory of the experimentation framework, which
## is the directory where this file resides.


##
## ROOT PATHS
##
TOOLS=$(ROOT)/tools


##
## CONFIGURATIONS
##
default: all


##
## OTHER COMMANDS
##
UNAME:=$(shell uname)
SHELL:=/bin/bash
GIT:=git


##
## LLVM
##
LLVM_BUILD_DIR:=
LLVM_CONFIG:=$(LLVM_BUILD_DIR)/llvm-config
CLANG:=$(LLVM_BUILD_DIR)/clang
CLANGXX:=$(LLVM_BUILD_DIR)/clang++
LLVM_COMPILER:=$(CLANG)
OPT:=$(LLVM_BUILD_DIR)/opt
LINK=$(LLVM_BUILD_DIR)/llvm-link
DIS=$(LLVM_BUILD_DIR)/llvm-dis


##
## WLLVM
##
WLLVM=wllvm
EXTRACTBC=extract-bc


##
## KLEE
##
KLEE_UCLIBC_DIR=$(TOOLS)/klee-uclibc

STP_DIR=$(TOOLS)/stp
STP=$(STP_DIR)/build/stp
MINISAT_DIR=$(TOOLS)/minisat
MINISAT=$(MINISAT_DIR)/build/libminisat.so

KLEE_SRC_DIR=
KLEE_DIR=
KLEE_BIN=$(KLEE_DIR)/bin
KLEE=$(KLEE_BIN)/klee

$(STP): $(STP_DIR) $(MINISAT)
	-test -d $(STP_DIR)/build || mkdir -p $(STP_DIR)/build; \
	cd $(STP_DIR)/build; \
	cmake -DMINISAT_LIBDIR=$(dir $(MINISAT)) -DBUILD_SHARED_LIBS:BOOL=OFF -DENABLE_PYTHON_INTERFACE:BOOL=OFF ..; \
	make

$(STP_DIR):
	-test -d $@ || git clone https://github.com/stp/stp.git $@
	cd $@; \
	git checkout tags/2.1.2

$(MINISAT): $(MINISAT_DIR)
	-test -d $(MINISAT_DIR)/build || mkdir -p $(MINISAT_DIR)/build; \
	cd $(MINISAT_DIR)/build; \
	cmake ..; \
	make

$(MINISAT_DIR):
	-test -d $@ || git clone https://github.com/stp/minisat.git $@


###
### EXPERIMENT CONFIGURATION
###
KTIMEOUT:=86400.
KINSTRTIMEOUT:=200.
KSLICE=--split-search
KFLAGS=--simplify-sym-indices \
--output-module \
--max-memory=8095 \
--allocate-determ=true \
--allocate-determ-size=8095 \
--allocate-determ-start-address=0x7ffef66f3000 \
--max-sym-array-size=2048 \
--disable-inlining \
--use-forked-solver \
--use-cex-cache \
--libc=uclibc \
--posix-runtime \
--allow-external-sym-calls \
--only-output-states-covering-new \
--watchdog \
--max-memory-inhibit=false \
--max-static-fork-pct=1 \
--max-static-solve-pct=1 \
--max-static-cpfork-pct=1 \
--switch-type=internal \
--dump-states-on-halt=false \
--environ=/tmp/test.env \
--run-in=/tmp/sandbox \
--max-instruction-time=$(KINSTRTIMEOUT) \
--max-solver-time=$(KINSTRTIMEOUT) \
--max-time=$(KTIMEOUT) \
--search=$(KSEARCH) \
$(KSLICE)


### COMMON TARGETS
###
.PHONY: prepare
## Create both the enviroment file and the sandbox directory
prepare: $(ROOT)/testing-env.sh $(ROOT)/sandbox.tgz
	-rm -rf /tmp/* > /dev/null 2>&1
	cd $(ROOT); \
	env -i /bin/bash -c '(source testing-env.sh; env >test.env)'; \
	mv test.env /tmp/test.env; \
	tar xzf sandbox.tgz; \
	mv sandbox /tmp

$(ROOT)/sandbox.tgz:
	cd $(ROOT); wget -q http://www.doc.ic.ac.uk/~cristic/klee/sandbox.tgz

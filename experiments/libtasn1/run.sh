#!/bin/sh

run_experiment()
{
echo "* Running experiment $1"
cd $1
# Compiling case study
make

# Running KLEE
make all-klee
terminate_klee klee

# Running CSE without CSE-crafted searcher
make all-cse
terminate_cse cse-no-searcher

# Running CSE with CSE-crafted searcher
make KSLICE="-split-search -split-ratio=10" all-cse
terminate_cse cse-split-searcher-10

make KSLICE="-split-search -split-ratio=20" all-cse
terminate_cse cse-split-searcher-20

make KSLICE="-split-search -split-ratio=30" all-cse
terminate_cse cse-split-searcher-30

make KSLICE="-split-search -split-ratio=40" all-cse
terminate_cse cse-split-searcher-40

make KSLICE="-split-search -split-ratio=50" all-cse
terminate_cse cse-split-searcher-50

# Running CSE with CSE-optimized searcher
make KSLICE="-recovery-search=random-path -split-ratio=10" all-cse
terminate_cse cse-recovery-searcher-rp-10

make KSLICE="-recovery-search=random-path -split-ratio=20" all-cse
terminate_cse cse-recovery-searcher-rp-20

make KSLICE="-recovery-search=random-path -split-ratio=30" all-cse
terminate_cse cse-recovery-searcher-rp-30

make KSLICE="-recovery-search=random-path -split-ratio=40" all-cse
terminate_cse cse-recovery-searcher-rp-40

make KSLICE="-recovery-search=random-path -split-ratio=50" all-cse
terminate_cse cse-recovery-searcher-rp-50

make KSLICE="-recovery-search=dfs -split-ratio=10" all-cse
terminate_cse cse-recovery-searcher-dfs-10

make KSLICE="-recovery-search=dfs -split-ratio=20" all-cse
terminate_cse cse-recovery-searcher-dfs-20

make KSLICE="-recovery-search=dfs -split-ratio=30" all-cse
terminate_cse cse-recovery-searcher-dfs-30

make KSLICE="-recovery-search=dfs -split-ratio=40" all-cse
terminate_cse cse-recovery-searcher-dfs-40

make KSLICE="-recovery-search=dfs -split-ratio=50" all-cse
terminate_cse cse-recovery-searcher-dfs-50

cd ..
}

terminate_klee()
{
mkdir $1
mv klee-run-* $1
mv run-klee-* $1
}

terminate_cse()
{
mkdir $1
mv cse-run-* $1
mv run-cse-* $1
}


## MAIN
run_experiment CVE-2012-1569
run_experiment CVE-2014-3467
run_experiment CVE-2015-2806
run_experiment CVE-2015-3622

#!/bin/bash

# Parameters: 1- directory, 2- label
# Returns on STDOUT: label,coverage,dfs,random
print_single_result()
{
    paste -d ',' \
    <( echo "$2" ) \
    <( grep Elapsed $1*/*-coverage/info | cut -d ' ' -f2 ) \
    <( grep Elapsed $1*/*-dfs/info | cut -d ' ' -f2 ) \
    <( grep Elapsed $1*/*-random/info | cut -d ' ' -f2 )
}

# Parameters: 1- directory, 2- label
# Returns on STDOUT for each subdirectory 'directory-[10,50]' found: label,coverage,dfs,random
print_multiple_result()
{
    paste -d ',' \
	  <( for num in `seq 10 10 50`; do echo "$2 $num"; done ) \
	  <( grep Elapsed $1-*/*-coverage/info | cut -d ' ' -f2 ) \
	  <( grep Elapsed $1-*/*-dfs/info | cut -d ' ' -f2 ) \
	  <( grep Elapsed $1-*/*-random/info | cut -d ' ' -f2 )
}

print_header()
{
    echo "EXPERIMENT,COVERAGE,DFS,RANDOM"
}

print_results()
{
    cd $1
    print_header
    print_single_result   "klee" "KLEE"
    print_single_result   "cse-no-searcher" "STANDARD"
    print_multiple_result "cse-split-searcher" "SPLIT-SEARCH"
    print_multiple_result "cse-recovery-searcher-rp" "RECOVERY-SEARCH-RP"
    print_multiple_result "cse-recovery-searcher-dfs" "RECOVERY-SEARCH-DFS"
    cd ..
}


## MAIN
print_results CVE-2012-1569 > CVE-2012-1569.csv
print_results CVE-2014-3467 > CVE-2014-3467.csv
print_results CVE-2015-2806 > CVE-2015-2806.csv
print_results CVE-2015-3622 > CVE-2015-3622.csv

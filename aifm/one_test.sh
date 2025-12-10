#!/bin/bash

source shared.sh

function run_single_test {
    echo "Running test $1..."
    rerun_local_iokerneld
    if [[ $1 == *"tcp"* ]]; then
    	rerun_mem_server
    fi
    if run_program ./bin/$1 2>/dev/null | grep -q "Passed"; then
        say_passed
    else
        say_failed
    fi
}

function cleanup {
    kill_local_iokerneld
    kill_mem_server
}

if [ -z "$1" ]; then
    echo "Usage: $0 <test_name>"
    echo "Available tests:"
    # List tests from bin directory, replacing newlines with ' | '
    ls bin | grep test_ | tr '\n' ' ' | sed 's/ / | /g'
    echo ""
    exit 1
fi

run_single_test $1
cleanup

#!/bin/bash

EXTRA_ARGS="--reporter Automake --reporter console::out=-::colour-mode=ansi"
TEST_FAILED=0

# Find all .elf files in the current directory and its subdirectories
find -type f -name "host_test*.elf" | while read -r test_file; do
    echo "Running $test_file..."
    "$test_file" $EXTRA_ARGS # Execute the .elf file
    if [ $? -ne 0 ]; then
        TEST_FAILED=1
    fi
done

# Return exit value 1 or 0, to ensure that the CI job passes/fails
if [ $TEST_FAILED -ne 1 ]; then
    echo "Some host tests failed."
    exit 1
else
    echo "All host tests passed."
    exit 0
fi
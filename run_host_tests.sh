#!/bin/bash

# Extra arguments for the .elf files, to make the host test results and failures more informative
# (https://github.com/catchorg/Catch2/tree/devel/docs)
EXTRA_ARGS="--reporter Automake --reporter console::out=-::colour-mode=ansi"

# Indicator for the CI job, whether a host test has failed or not
TEST_FAILED=0

## Find all host_test*.elf files
for test_file in $(find . -type f -name "host_test*.elf"); do
    echo "Running $test_file..."
    "$test_file" $EXTRA_ARGS    # Call the host_test*.elf file with the extra arguments
    if [ $? -ne 0 ]; then       # Check whether the host test failed
        TEST_FAILED=1
    fi
done

# Return exit value 1 (FAIL) 0 (PASS) to ensure that the CI job passes/fails
if [ $TEST_FAILED -ne 0 ]; then
    echo "Some host tests failed."
    exit 1
else
    echo "All host tests passed."
    exit 0
fi

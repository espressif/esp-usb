#!/bin/bash

EXTRA_ARGS="--reporter Automake --reporter console::out=-::colour-mode=ansi"

# Find all .elf files in the current directory and its subdirectories
find -type f -name "host_test*.elf" | while read -r test_file; do
    echo "Running $test_file..."
    "$test_file" $EXTRA_ARGS # Execute the .elf file
    echo "---------------------------------"
done

echo "All tests executed."
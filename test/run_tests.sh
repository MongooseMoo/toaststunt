#!/bin/bash

# Test runner for ToastStunt tests
# Usage: ./run_tests.sh [test_name]
# Examples:
#   ./run_tests.sh                    # Run all tests
#   ./run_tests.sh test_ast          # Run AST tests only
#   ./run_tests.sh test_ast_roundtrip # Run round-trip tests only

echo "=== ToastStunt Test Runner ==="

# Ensure we're in the test directory
cd "$(dirname "$0")" || exit 1

# Start server if not running - check if test_server.sh exists
if [ -f "./test_server.sh" ]; then
    if ! ./test_server.sh status | grep -q "Server is running"; then
        echo "Starting test server..."
        ./test_server.sh start
        sleep 2
    fi
else
    echo "Note: test_server.sh not found, assuming server is already running"
fi

# Default to all tests if no argument provided
TEST_TARGET="${1:-}"

echo "Running tests..."
if [ -n "$TEST_TARGET" ]; then
    echo "Target: $TEST_TARGET"
    PATH="$HOME/.rbenv/bin:$PATH" make "$TEST_TARGET" > test_output.log 2>&1
else
    echo "Running all tests"
    PATH="$HOME/.rbenv/bin:$PATH" make > test_output.log 2>&1
fi

# Show results
echo ""
echo "=== TEST RESULTS ==="
cat test_output.log

# Show validation summary for AST tests
if [[ "$TEST_TARGET" == *"ast"* ]] || [ -z "$TEST_TARGET" ]; then
    echo ""
    echo "=== AST VALIDATION SUMMARY ==="
    grep -E "(✅|❌|===)" test_output.log | head -20 || echo "No validation markers found"
fi

echo ""
echo "Full output saved to: test_output.log"
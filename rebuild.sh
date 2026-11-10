#!/bin/bash
# ToastStunt rebuild and test update script
# Kills any running moo processes, rebuilds, and updates test executable

set -e

echo "=== ToastStunt Rebuild Script ==="

# Kill any running moo processes
echo "Killing any running moo processes..."
pkill -f "moo.*Test.db" 2>/dev/null || true
sleep 3

# Navigate to build directory and build
echo "Building ToastStunt..."
cd build
make -j32

# Wait a moment for any file locks to clear
sleep 1

# Copy executable to test directory
echo "Updating test executable..."
cp moo ../test/

echo "=== Rebuild complete! ==="
echo "New executable ready at test/moo"
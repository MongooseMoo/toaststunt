# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System and Commands

ToastStunt uses CMake for building. The main workflow is:

```bash
# Initial setup (from project root)
mkdir build && cd build
cmake ../
make -j2

# Or use the generated Makefile in root:
make
```

### Build Types
- **Release**: Optimizations enabled, warnings disabled (default)
- **Debug**: Optimizations disabled, debug enabled
- **Warn**: Optimizations enabled, warnings enabled
- **LeakCheck**: Minimal optimizations, debug enabled, address sanitizer

Change build type: `cmake -DCMAKE_BUILD_TYPE=BuildNameHere ../`

### Windows Build (MSVC)

The `windows` branch has full Windows/MSVC support.

**Prerequisites:**
- Visual Studio 2022 with C++ workload
- vcpkg with packages: `nettle:x64-windows`, `argon2:x64-windows`
- Ruby 3.x (via scoop: `scoop install ruby`) for running tests

**Build commands:**
```powershell
# From project root - create build directory
mkdir build-win
cd build-win

# Configure with vcpkg toolchain
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build Release
cmake --build . --config Release

# Output: build-win/Release/moo.exe
```

**Key Windows files:**
- `src/include/platform.h` - Cross-platform abstractions (sockets, file ops)
- `src/dependencies/thpool.cc` - C++ thread pool (std::thread instead of pthreads)
- `src/exec.cc` - Windows CreateProcess implementation with PATHEXT support
- `test/executables/*.bat` - Windows batch file test executables

**Windows-specific behavior:**
- Uses `SetConsoleCtrlHandler` instead of Unix signals
- Uses `BCryptGenRandom` for random number generation
- Uses socket pairs instead of pipes for background task notification (Windows select() limitation)
- `exec()` searches PATHEXT extensions (.COM, .EXE, .BAT, .CMD)
- Files always opened in binary mode to ensure cross-platform compatibility

### Testing

Tests are written in Ruby using Test::Unit framework, located in `test/` directory.

**Unix/Linux:**
```bash
# Setup (in test/ directory)
bundle install
# Create symlinks in executables/: echo, sleep, true
ln -s `which echo` executables/echo
ln -s `which sleep` executables/sleep
ln -s `which true` executables/true

# Copy built executable to test directory, then:
./moo Test.db /dev/null 9898  # In one terminal
make                          # In another terminal to run tests
```

**Windows:**
```powershell
# Setup (in test/ directory)
# Ruby must be in PATH (e.g., C:\Users\Q\scoop\apps\ruby\current\bin)
bundle install

# Copy moo.exe to test directory
copy ..\build-win\Release\moo.exe .

# Batch file test executables are already in test/executables/*.bat

# Start server (in one terminal)
.\moo.exe Test.db NUL 9898

# Run individual test (in another terminal)
ruby -Itests/lib tests/test_primitives.rb

# Run all tests
# Note: The Makefile uses bash, so on Windows run tests individually:
ruby -Itests/lib tests/test_algorithms.rb
ruby -Itests/lib tests/test_objects.rb
# etc.
```

**Test configuration:** `test/test.yml` - set host/port (default: localhost:9898)

**Current Windows test status (~95% passing):**
- Most core tests pass (objects, verbs, properties, JSON, algorithms, etc.)
- Minor failures in: queued_tasks structure, $USER vs $USERNAME, process timing

## Architecture Overview

ToastStunt is a MOO (MUD Object Oriented) server - a network-accessible, multi-user, programmable interactive system. It's a fork of Stunt, which forked from LambdaMOO.

### Core Components

**Database Layer** (`src/db*.cc`, `src/objects.cc`, `src/property.cc`, `src/verbs.cc`):
- Object-oriented database with objects, properties, and verbs
- Persistent storage with checkpoint/flush mechanisms
- Supports SQLite integration for extended storage

**Virtual Machine** (`src/execute.cc`, `src/eval_vm.cc`, `src/program.cc`):
- Bytecode interpreter for MOO language
- Task scheduling and execution
- Exception handling and security

**Language Processing** (`src/parser.y`, `src/ast.cc`, `src/code_gen.cc`):
- Bison-based parser for MOO language
- AST generation and bytecode compilation
- Decompilation support

**Networking** (`src/server.cc`, `src/network.cc`, `src/net_mplex.cc`):
- Multi-connection handling with select/poll
- IPv6 support and TLS connections
- HAProxy source IP rewriting

**Extensions**:
- **Threading** (`src/background.cc`): Thread pool for background operations
- **FileIO** (`src/fileio.cc`): Enhanced file operations
- **Waifs** (`src/waif.cc`): Lightweight objects without persistence
- **Maps** (`src/map.cc`): Associative arrays as first-class type
- **JSON** (`src/json.cc`): Native JSON parsing/generation
- **SQLite** (`src/sqlite.cc`): Database integration
- **Crypto** (`src/crypto.cc`, `src/argon2.cc`): Hashing and encryption

### Key Data Structures

The server operates on a type system defined in `src/include/structures.h`:
- `Var`: Universal value type (numbers, strings, lists, maps, objects, waifs)
- `Objid`: Object identifiers
- `Program`: Compiled MOO code (bytecode)
- Object database with properties and verbs

### Configuration

Key configuration files:
- `src/include/options.h`: Compile-time server options
- `src/include/config.h.cmake`: Generated configuration header
- `version_options.h`: Auto-generated version/feature flags

### Notable Features

- **64-bit integers** (with 32-bit fallback)
- **Threaded operations**: Background tasks for expensive operations
- **Enhanced networking**: IPv6, TLS, threading for DNS lookups
- **Advanced debugging**: Task profiling, lag detection, improved tracebacks
- **Waif improvements**: Enhanced lightweight objects with recycling
- **Modern C++**: Uses C++14 standard with careful memory management

### Development Patterns

- Heavy use of reference counting for memory management
- Optional garbage collection for cyclic references (`ENABLE_GC`)
- Extensive use of variant types (`Var`) with type checking
- Fork-based checkpointing for database persistence
- Modular builtin function registration system
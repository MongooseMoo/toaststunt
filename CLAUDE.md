# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System and Commands

ToastStunt uses CMake for building. The main workflow is:

```bash
# Initial setup (from project root)
mkdir build && cd build
cmake ../
make -j32

# RECOMMENDED: Use the rebuild script to avoid "text file busy" issues:
./rebuild.sh

# Manual build process (from project root):
cd build && make -j32 && killall moo 2>/dev/null; cp moo ../test/
```

### Build Types
- **Release**: Optimizations enabled, warnings disabled (default)
- **Debug**: Optimizations disabled, debug enabled
- **Warn**: Optimizations enabled, warnings enabled
- **LeakCheck**: Minimal optimizations, debug enabled, address sanitizer

Change build type: `cmake -DCMAKE_BUILD_TYPE=BuildNameHere ../`

### Testing
Tests are written in Ruby using Test::Unit framework, located in `test/` directory:

```bash
# Ruby Setup (first time only)
# Install rbenv and add to PATH
export PATH="$HOME/.rbenv/bin:$PATH"
eval "$(rbenv init -)"
# Add to ~/.bashrc permanently:
echo 'export PATH="$HOME/.rbenv/bin:$PATH"' >> ~/.bashrc
echo 'eval "$(rbenv init -)"' >> ~/.bashrc

# Setup (in test/ directory)
bundle install
# Create symlinks in executables/: echo, sleep, true
ln -s `which echo` executables/echo
ln -s `which sleep` executables/sleep  
ln -s `which true` executables/true

# RECOMMENDED: Use the test server script for easier management:
./test_server.sh start    # Start server
./test_server.sh test     # Run tests (auto-starts server)
./test_server.sh stop     # Stop server
./test_server.sh status   # Check server status
./test_server.sh logs     # Show server logs

# Manual approach:
./moo Test.db /tmp/toast-test-out.db 9898 &  # Start server in background. Never give /dev/null as the output when running as root: a server without the guard in dump_database() replaces the device with a regular file.
PATH="$HOME/.rbenv/bin:$PATH" make  # Run all tests with rbenv in PATH
```

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

## AST Implementation Notes

The MOO-visible behaviour and the node reference are in `docs/Features/ast.md`.

- `src/ast_map.cc` holds both conversions and `parse_ast()`, `unparse_ast()` and `validate_ast()`. `verb_ast()` is in `src/verbs.cc` beside `verb_code()`.
- Code to tree: `parse_list_as_program()` then `decompile_program()`, the same tree `verb_code()` prints. There is no hook in the parser.
- Tree to code: build `Stmt`/`Expr` nodes between `begin_code_allocation()` and `end_code_allocation()`, then call `unparse_stmts()` in `src/unparse.cc`. Do not write a second unparser.
- A name that is not a built-in function is a compile error, so `parse_ast({"foo(42);"})` raises `E_INVARG`.
- `test/tests/test_ast.rb` compares inside the server with `equal()`, which is exact about case and map contents.

**Error Debugging**: Use errlog() for ToastStunt logging, not printf().

### Ruby Unit Test Framework

**Test Structure**: Tests go in `test/tests/test_*.rb`, inherit from `Test::Unit::TestCase`, require `'test_helper'`

**MOO Integration**: Use `run_test_as("wizard") do ... end` to wrap MOO interactions. The `command()` function sends MOO code and returns string results - MOO formatted like `{1, [MAP]}` are strings, not Ruby arrays.

**Test Runner**: Use `./run_tests.sh test_name` - it handles server start/stop automatically. Don't write separate scripts - use the existing test infrastructure.
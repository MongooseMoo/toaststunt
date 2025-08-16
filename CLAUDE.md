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
./moo Test.db /dev/null 9898 &  # Start server in background
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

### Key Debugging Discovery (August 2025)

**Critical Finding**: AST parse_ast() requires valid MOO code with recognized builtin functions.

During AST implementation debugging, discovered that:
- ✅ Parser callback mechanism works perfectly for valid MOO code
- ❌ Unknown function names (like `foo()`) cause complete parse failure
- ❌ Parse failures return NULL, so AST callback never triggers
- ✅ Valid builtins (like `length()`, `typeof()`) parse successfully and trigger AST capture

**Test Requirements**: Always use valid ToastStunt builtin functions in AST tests:
- ✅ Good: `parse_ast({"length({1,2,3});"})` 
- ❌ Bad: `parse_ast({"foo(42);"})` - fails because `foo` unknown
- ✅ Good: `parse_ast({"return 42;"})` - statements work fine
- ✅ Good: `parse_ast({"x = 1;"})` - assignments work fine

**Error Debugging**: Use errlog() for ToastStunt logging, not printf(). Parser errors appear in server logs when parsing fails with detailed error messages.

### Ruby Unit Test Framework

**Test Structure**: Tests go in `test/tests/test_*.rb`, inherit from `Test::Unit::TestCase`, require `'test_helper'`

**MOO Integration**: Use `run_test_as("wizard") do ... end` to wrap MOO interactions. The `command()` function sends MOO code and returns string results - MOO formatted like `{1, [MAP]}` are strings, not Ruby arrays.

**Test Runner**: Use `./run_tests.sh test_name` - it handles server start/stop automatically. Don't write separate scripts - use the existing test infrastructure.

## AST Unparsing Architecture Rules

**CRITICAL: NEVER IMPLEMENT CUSTOM UNPARSING**

- **Only implement**: MAP↔Expr/Stmt conversion functions (`map_to_stmt`, `stmt_to_map`, etc.)
- **Use existing**: MOO's `unparse_program()` infrastructure for Stmt→code conversion
- **Never write**: Custom statement-to-string functions, stream capture, or parallel unparsing logic
- **Why**: MOO already has complete, working unparsing. Reimplementing creates broken duplicates.

**The correct bf_unparse_ast() pattern:**
1. `map_to_stmt()` - convert MAP to Stmt (custom code)
2. `list_prg(stmt, 0, 0)` - convert Stmt to code lines (existing MOO infrastructure)
3. Return the code lines

**KEY FUNCTION: `list_prg(stmt, fully_parenthesize, indent_lines)`**
- Located in unparse.cc line 862
- Takes Stmt directly (no need for unparse_program)
- Uses global receiver pattern to output lines
- This is THE function to call for Stmt→code conversion

**NEVER create custom unparsing functions again. Use list_prg().**
cmake_minimum_required(VERSION 3.10)
set(VERSION_MAJOR 2)
set(VERSION_MINOR 7)
set(VERSION_RELEASE 3)
set(VERSION_EXT "_2")
project(ToastStunt VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_RELEASE} LANGUAGES CXX C)

# Windows platform detection
if(WIN32)
    set(IS_WINDOWS TRUE)
    message(STATUS "Building for Windows")
    # Force unforked checkpoints on Windows (no fork() available)
    set(FORCE_UNFORKED_CHECKPOINTS TRUE)
    add_definitions(-DUNFORKED_CHECKPOINTS)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DNOMINMAX)
endif()

# Compile Options
option(ONLY_32_BITS "Use 32-bit integers instead of 64-bit." OFF)
option(USE_JEMALLOC "Use jemalloc (if available) for memory allocation." OFF)

# Fall back to 32-bit mode if the target doesn't support 64-bit
if( CMAKE_SIZEOF_VOID_P EQUAL 4 )
    set(ONLY_32_BITS true)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/CMakeModules/")

# Don't bother if we don't have these
find_package(BISON REQUIRED)
find_package(Perl REQUIRED)
find_package(gperf REQUIRED)

# Architecture detection (cross-platform)
if(NOT WIN32)
    # Unix: use uname for architecture detection
    EXECUTE_PROCESS( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE )
else()
    # Windows: use CMake's processor detection
    set(ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()

if(APPLE AND ${ARCHITECTURE} STREQUAL "arm64")
    set(APPLE_SILICON_FOUND 1)
endif()

# Check for a virtualized container (Unix only)
if(NOT WIN32 AND EXISTS "/proc/1/cgroup")
    file(READ "/proc/1/cgroup" CGROUPS)
    string(REGEX MATCH ".*(docker|lxc).*" IN_A_CONTAINER ${CGROUPS})
else()
    set(IN_A_CONTAINER 0)
endif()

# Where to find header files
include_directories(${CMAKE_SOURCE_DIR}/src/include)
include_directories(${CMAKE_SOURCE_DIR}/src/dependencies)
include_directories(${CMAKE_BINARY_DIR})

# Build types
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Warn)
    list(APPEND CMAKE_CONFIGURATION_TYPES LeakCheck)
else()
    set(allowableBuildTypes Release Debug Warn LeakCheck)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${allowableBuildTypes}")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()
endif()

# Compiler Flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(MSVC)
    # MSVC compiler flags
    # /Zc:preprocessor enables standards-conformant preprocessor (required for complex macros)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Zc:preprocessor")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:preprocessor")
    set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} /O2 /W0")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /W0")
    set(CMAKE_C_FLAGS_DEBUG     "${CMAKE_C_FLAGS_DEBUG} /Od /Zi")
    set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} /Od /Zi")
    # Warn build type for MSVC
    set(CMAKE_C_FLAGS_WARN      "/O2 /W3")
    set(CMAKE_CXX_FLAGS_WARN    "/O2 /W3")
else()
    # GCC/Clang compiler flags
    # These flags are shared by the RELEASE and WARN build types.
    set(SHARED_COMPILE_FLAGS "-O3")
    if(NOT APPLE_SILICON_FOUND AND NOT IN_A_CONTAINER)
        set(SHARED_COMPILE_FLAGS "${SHARED_COMPILE_FLAGS} -march=native")
    elseif(APPLE_SILICON_FOUND)
        set(SHARED_COMPILE_FLAGS "${SHARED_COMPILE_FLAGS} -mcpu=apple-m1")
    endif()

    # The default for non-developers: Optimization enabled and all warnings silenced.
    set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} ${SHARED_COMPILE_FLAGS} -w")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${SHARED_COMPILE_FLAGS} -w")

    # The normal LambdaMOO way: Optimizations and warnings enabled
    set(CMAKE_C_FLAGS_WARN   "${SHARED_COMPILE_FLAGS} -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-switch")
    set(CMAKE_CXX_FLAGS_WARN "${CMAKE_C_FLAGS_WARN}")

    # Check for memory leaks: Minimal optimizations, debug enabled, and sanitize address
    set(CMAKE_C_FLAGS_LEAKCHECK  "-O1 -g -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")
    set(CMAKE_CXX_FLAGS_LEAKCHECK "${CMAKE_C_FLAGS_LEAKCHECK}")
    if(CMAKE_BUILD_TYPE MATCHES "LeakCheck")
        set(EXTRA_LINKER_FLAGS "-fsanitize=address")
    endif()
endif()

# For single-config generators (Make, Ninja), apply build-type-specific flags
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")
endif()

# Compatibility Checks Galore
include(CheckStructHasMember)
include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckTypeSize)
include(CheckLibraryExists)

check_type_size("long" SIZEOF_LONG)
check_type_size("float" SIZEOF_FLOAT)
check_function_exists(waitpid HAVE_WAITPID)
check_function_exists(wait2 HAVE_WAIT2)
check_function_exists(wait3 HAVE_WAIT3)
check_function_exists(strftime HAVE_STRFTIME)
check_function_exists(sigemptyset HAVE_SIGEMPTYset)
check_function_exists(sigrelse HAVE_SIGRELSE)
check_function_exists(lrand48 HAVE_LRAND48)
check_function_exists(random HAVE_RANDOM)
check_function_exists(select HAVE_SELECT)
check_function_exists(poll HAVE_POLL)
check_function_exists(strtoimax HAVE_STRTOIMAX)
check_function_exists(accept4 HAVE_ACCEPT4)

check_symbol_exists(tzname time.h HAVE_TZNAME)

check_struct_has_member("struct tm"  tm_zone  time.h  HAVE_TM_ZONE)

# Check for a Microsoft operating system (for virtual timers)
string(FIND ${CMAKE_SYSTEM} "Microsoft" IS_WSL)
if (IS_WSL GREATER_EQUAL 0)
    message(STATUS "Found Windows Subsystem for Linux.")
else()
    unset(IS_WSL)
endif()

# Yacc (-d is a default flag on my bison)
bison_target(MOOParser src/parser.y ${CMAKE_BINARY_DIR}/parser.cc COMPILE_FLAGS "-y")

# Keywords.cc
set(KEYWORDS ${CMAKE_BINARY_DIR}/keywords.cc)
# Note: --key-positions=1,3,$ means positions 1, 3, and end-of-word
# We use OUTPUT_FILE instead of shell redirection for Windows compatibility
# The $ needs to be escaped differently on Windows vs Unix
if(WIN32)
    # Use VERBATIM to pass $ literally to gperf
    add_custom_command(
        OUTPUT ${KEYWORDS}
        COMMAND ${gperf_EXECUTABLE} --language=C++ --ignore-case --readonly-tables --struct-type --omit-struct-type "--key-positions=1,3,$" ${CMAKE_SOURCE_DIR}/src/keywords.gperf --output-file=${CMAKE_BINARY_DIR}/keywords.cc
        COMMENT "Generating keywords.cc"
        VERBATIM)
else()
    add_custom_command(
        OUTPUT ${KEYWORDS}
        COMMAND ${gperf_EXECUTABLE} --language=C++ --ignore-case --readonly-tables --struct-type --omit-struct-type --key-positions=1,3,$$ ${CMAKE_SOURCE_DIR}/src/keywords.gperf > ${CMAKE_BINARY_DIR}/keywords.cc
        COMMENT "Generating keywords.cc")
endif()

# version_options.h
execute_process(COMMAND ${PERL_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/include/version_opt_gen.pl)

# Add current version control situation to version_src.h
set(VERSION_SRC ${CMAKE_BINARY_DIR}/version_src.h)
set(VERSION_DUMMY ${CMAKE_BINARY_DIR}/version_src_dummy.h)
add_custom_command(
    OUTPUT  ${VERSION_DUMMY}
            ${VERSION_SRC}
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/CMakeModules/version.cmake
    COMMENT "Generating version_src.h")

# Find packages. We don't do anything with the information yet because we need to know if things exist
#                to modify the source files, so we have to do it before moo is defined. But we can't
#                do the package setup UNTIL moo is defined.
if(WIN32)
    # Windows uses native threads (std::thread backed by Windows threads)
    set(CMAKE_USE_WIN32_THREADS_INIT 1)
    set(CMAKE_USE_PTHREADS_INIT 0)
    set(CMAKE_HAVE_THREADS_LIBRARY 1)
elseif(APPLE_SILICON_FOUND)
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    set(CMAKE_HAVE_THREADS_LIBRARY 1)
    set(CMAKE_USE_WIN32_THREADS_INIT 0)
    set(CMAKE_USE_PTHREADS_INIT 1)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
else()
    find_package(Threads REQUIRED)
endif()
find_package(Nettle REQUIRED)
find_package(Argon2 REQUIRED)
find_package(PCRE)
find_package(CURL)
find_package(ASPELL)
find_package(SQLite3)
find_package(OpenSSL)

if(USE_JEMALLOC)
    find_library(JEMALLOC_LIBRARY NAMES jemalloc)
    if(NOT JEMALLOC_LIBRARY)
        message(WARNING "jemalloc library not found. Not using.")
    else()
        set(JEMALLOC_FOUND 1)
    endif()
endif()

# Millions of source files
set(src_CSRCS
    src/dependencies/sosemanuk.c
    src/dependencies/strnatcmp.c
    src/http_parser.c
    src/regexpr.c)

# thpool and linenoise: platform-specific builds
if(NOT WIN32)
    list(APPEND src_CSRCS src/dependencies/thpool.c)
    # linenoise requires Unix terminal APIs (termios.h)
    list(APPEND src_CSRCS src/dependencies/linenoise.c)
endif()

if(PCRE_FOUND)
    list(APPEND src_CSRCS src/dependencies/pcrs.c)
endif()

set(src_YAJL
    src/dependencies/yajl/yajl.c
    src/dependencies/yajl/yajl_alloc.c
    src/dependencies/yajl/yajl_buf.c
    src/dependencies/yajl/yajl_encode.c
    src/dependencies/yajl/yajl_gen.c
    src/dependencies/yajl/yajl_lex.c
    src/dependencies/yajl/yajl_parser.c
    src/dependencies/yajl/yajl_version.c)

set(src_CRYPT
    src/dependencies/crypt/crypt_blowfish.c
    src/dependencies/crypt/crypt_gensalt.c
    src/dependencies/crypt/crypt_sha.c
    src/dependencies/crypt/crypt_des.c)

set(src_CXXSRCS
    src/ast.cc
    src/base64.cc
    src/code_gen.cc
    src/collection.cc
    src/crypto.cc
    src/db_file.cc
    src/db_io.cc
    src/db_objects.cc
    src/db_properties.cc
    src/db_verbs.cc
    src/decompile.cc
    src/disassemble.cc
    src/eval_env.cc
    src/eval_vm.cc
    src/exec.cc
    src/execute.cc
    src/extensions.cc
    src/fileio.cc
    src/functions.cc
    src/garbage.cc
    src/json.cc
    ${KEYWORDS}
    src/log.cc
    src/list.cc
    src/map.cc
    src/match.cc
    src/network.cc
    src/net_mplex.cc
    src/numbers.cc
    src/objects.cc
    src/parse_cmd.cc
    src/pattern.cc
    src/program.cc
    src/property.cc
    src/quota.cc
    src/server.cc
    src/storage.cc
    src/streams.cc
    src/str_intern.cc
    src/sym_table.cc
    src/system.cc
    src/tasks.cc
    src/timers.cc
    src/unparse.cc
    src/utils.cc
    src/verbs.cc
    src/version.cc
    src/sqlite.cc
    src/pcre_moo.cc
    src/background.cc
    src/waif.cc
    src/simplexnoise.cc
    src/argon2.cc
    src/spellcheck.cc
    src/curl.cc)

# thpool: Use C++ version on Windows (std::thread instead of pthreads)
if(WIN32)
    list(APPEND src_CXXSRCS src/dependencies/thpool.cc)
endif()

set(src_GRAMMAR
    ${BISON_MOOParser_OUTPUTS})

set(src_SERVER
    ${VERSION_SRC}
    ${src_GRAMMAR}
    ${src_YAJL}
    ${src_CRYPT}
    ${src_CSRCS}
    ${src_CXXSRCS})

add_executable(moo ${src_SERVER})

# Make sure we have the packages we need
include_directories(${NETTLE_INCLUDE_DIR})
target_link_libraries(moo ${NETTLE_LIBRARIES})
include_directories(${ARGON2_INCLUDE_DIR})
target_link_libraries(moo ${ARGON2_LIBRARIES})

if(NOT WIN32)
    # Unix threading
    if(CMAKE_USE_PTHREADS_INIT)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
    else()
        target_link_libraries(moo Threads::Threads)
    endif()
endif()
# Windows uses std::thread backed by native Windows threads - no additional libs needed

if(PCRE_FOUND)
    include_directories(${PCRE_INCLUDE_DIRS})
    target_link_libraries(moo ${PCRE_LIBRARIES})
endif()

if(CURL_FOUND)
    include_directories(${CURL_INCLUDE_DIRS})
    target_link_libraries(moo ${CURL_LIBRARIES})
endif()

if(ASPELL_FOUND)
    include_directories(${ASPELL_INCLUDE_DIRS})
    target_link_libraries(moo ${ASPELL_LIBRARIES})
endif()

if(SQLITE3_FOUND)
    include_directories(${SQLITE3_INCLUDE_DIRS})
    target_link_libraries(moo ${SQLITE3_LIBRARIES})
endif()

if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIR})
    target_link_libraries(moo ${OPENSSL_LIBRARIES})
endif()

if(JEMALLOC_FOUND)
    include_directories(${JEMALLOC_INCLUDE_DIRS})
    target_link_libraries(moo ${JEMALLOC_LIBRARY})
endif()

# Setup #defines as needed
CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/src/include/config.h.cmake
    ${CMAKE_BINARY_DIR}/config.h)

# Configure linker flags
if(WIN32)
    # Windows libraries
    target_link_libraries(moo ws2_32 bcrypt ${EXTRA_LINKER_FLAGS})
else()
    # Unix libraries
    target_link_libraries(moo m ${EXTRA_LINKER_FLAGS})
    if(NOT APPLE)
        target_link_libraries(moo crypt)
    endif()
endif()

if(CMAKE_BUILD_TYPE)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
else()
    message(STATUS "Build type: Multi-config (use --config at build time)")
endif()
message(STATUS "C flags: ${CMAKE_C_FLAGS}")
message(STATUS "CXX flags: ${CMAKE_CXX_FLAGS}")
if(JEMALLOC_FOUND)
    message(STATUS "Using jemalloc")
endif()

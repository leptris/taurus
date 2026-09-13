# Leptris - Fast XML Parser and XPath Evaluator
# Pure C library with Ruby bindings and CLI
cmake_minimum_required(VERSION 3.20)

project(leptris
    VERSION 1.9.150
    DESCRIPTION "Fast XML parser and XPath evaluator in pure C"
    LANGUAGES C CXX
)

# C11 standard required (was C99; bumped for _Static_assert, which
# MSVC's C99 mode doesn't recognize as a keyword. C11 makes it
# standard. VS2019 16.8+ supports C11; older MSVC fails the
# CMAKE_C_STANDARD_REQUIRED check rather than silently building.)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C++11 standard for benchmarks
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)  # Default to static
option(BUILD_TESTING "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" OFF)
option(LEPTRIS_BUILD_CLI "Build CLI tool" ON)
option(LEPTRIS_BUILD_MAN_PAGES "Generate man pages with asciidoctor" OFF)
option(LEPTRIS_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(LEPTRIS_ENABLE_UTF8PROC "Enable Unicode support via utf8proc" ON)
option(LEPTRIS_ENABLE_ICONV "Enable encoding conversion via iconv" ON)
option(LEPTRIS_ENABLE_ASAN "Enable AddressSanitizer (catches heap bugs at runtime)" OFF)
option(LEPTRIS_ENABLE_FUZZING "Build libFuzzer harness" OFF)
option(LEPTRIS_BUILD_DOCS "Build Doxygen API docs" OFF)

# LTO default: ON for Release/RelWithDebInfo, OFF for Debug/empty.
# The user can override with -DLEPTRIS_ENABLE_LTO=OFF.
# LTO delivers 1.3-3.5x speedups on parse/SAX/DOM benchmarks — see
# PR #74 for the benchmark data.  Downside: 5-30s longer link time.
# CheckIPOSupported guards against unsupported toolchains.
if(NOT DEFINED LEPTRIS_ENABLE_LTO)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(LEPTRIS_ENABLE_LTO_DEFAULT ON)
    else()
        set(LEPTRIS_ENABLE_LTO_DEFAULT OFF)
    endif()
else()
    set(LEPTRIS_ENABLE_LTO_DEFAULT ${LEPTRIS_ENABLE_LTO})
endif()
option(LEPTRIS_ENABLE_LTO "Enable link-time optimization (default ON for Release)" ${LEPTRIS_ENABLE_LTO_DEFAULT})

# Optimization level (TODO 167 Phase A). CMake's default for Release is
# -O2; -O3 enables more aggressive inlining, loop transforms, and
# auto-vectorization. Trade-off: larger binary, longer compile, occasionally
# exposes latent UB.
#
#   default      — use CMake's choice (-O2 for Release)
#   aggressive   — force -O3 for Release/RelWithDebInfo
set(LEPTRIS_OPT_LEVEL "default" CACHE STRING "Optimization level: default (-O2) or aggressive (-O3)")
set_property(CACHE LEPTRIS_OPT_LEVEL PROPERTY STRINGS default aggressive)

# Target architecture (TODO 167 Phase B). -march=native lets the compiler use
# the host's full ISA (AVX2, AVX-512, BMI2, etc). The resulting binary is NOT
# portable to older CPUs — keep opt-in.
#
#   default   — baseline (runs on any x86-64 / ARM64)
#   native    — -march=native (gcc/clang) or /arch:AVX2 (MSVC)
set(LEPTRIS_TARGET_ARCH "default" CACHE STRING "Target CPU arch: default (portable) or native (host ISA)")
set_property(CACHE LEPTRIS_TARGET_ARCH PROPERTY STRINGS default native)

# AddressSanitizer — see TODO 35.
if(LEPTRIS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer: ENABLED")
endif()

# Link-time optimization (TODO 110).
#
# Lets the compiler inline across translation-unit boundaries.  Without
# LTO, every "static inline" helper in element_modify.c / element_query.c
# that crosses a TU boundary from a caller in another .c file is a real
# function call — including leptris_pool_alloc, leptris_element_set_attribute,
# the chartype table lookups, etc.  With LTO, the compiler sees the whole
# program at link time and inlines aggressively.
#
# Off by default because:
#   - LTO significantly increases link time (5-30s for leptris)
#   - Some older toolchains have LTO bugs
#   - Debug builds don't benefit
#
# Recommended for release builds where every cycle counts.
if(LEPTRIS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "Link-time optimization: ENABLED")
    else()
        message(WARNING "LEPTRIS_ENABLE_LTO requested but not supported: ${lto_error}")
    endif()
endif()

# Optimization level (TODO 167 Phase A).
if(LEPTRIS_OPT_LEVEL STREQUAL "aggressive")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang"
       OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        # CMake sets -O2 by default for Release; -O3 replaces it.
        # Apply to both C and CXX so benchmarks get the same flag.
        foreach(_lang C CXX)
            foreach(_cfg RELEASE RELWITHDEBINFO)
                string(REPLACE "-O2" "-O3"
                    CMAKE_${_lang}_FLAGS_${_cfg}
                    "${CMAKE_${_lang}_FLAGS_${_cfg}}")
                # If -O2 wasn't there (rare), append -O3 explicitly.
                if(NOT CMAKE_${_lang}_FLAGS_${_cfg} MATCHES "-O3")
                    set(CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}} -O3")
                endif()
            endforeach()
        endforeach()
        message(STATUS "Optimization level: -O3 (aggressive)")
    elseif(MSVC)
        # /O2 is the Release default; /O3 doesn't exist as a separate flag in
        # MSVC (the equivalent aggressiveness is /O2 + /GL + /Gw + /Gy). LTO
        # already covers /GL. Add the others.
        add_compile_options(/Gw /Gy)
        message(STATUS "Optimization level: /O2 + /Gw + /Gy (MSVC aggressive)")
    endif()
else()
    message(STATUS "Optimization level: default (-O2)")
endif()

# Target architecture (TODO 167 Phase B).
if(LEPTRIS_TARGET_ARCH STREQUAL "native")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang"
       OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        add_compile_options(-march=native)
        message(STATUS "Target architecture: native (-march=native)")
    elseif(MSVC)
        add_compile_options(/arch:AVX2)
        message(STATUS "Target architecture: AVX2 (/arch:AVX2)")
    endif()
else()
    message(STATUS "Target architecture: default (portable)")
endif()

# -fno-semantic-interposition for shared libs (TODO 167 Phase C).
# Allows GCC/Clang to inline exported symbols into callers within the same
# shared object, and lets LLVM's optimizer treat the public API as
# non-interposable. ~5% on shared builds. No effect on static builds.
if(BUILD_SHARED_LIBS)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-fno-semantic-interposition)
        message(STATUS "Semantic interposition: disabled (shared-lib build)")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        # Clang defaults to -fno-semantic-interposition behavior (it doesn't
        # allow interposition of defined symbols anyway), but explicitly
        # setting -fvisibility-no-protects the inlining decision.
        # Apple Clang ignores the flag; suppress the warning.
        check_c_compiler_flag("-fno-semantic-interposition" _leptris_fsi_supported)
        if(_leptris_fsi_supported)
            add_compile_options(-fno-semantic-interposition)
            message(STATUS "Semantic interposition: disabled (shared-lib build)")
        endif()
    endif()
endif()

# libFuzzer — see TODO 40.  Implies ASAN.
if(LEPTRIS_ENABLE_FUZZING)
    add_compile_options(-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=fuzzer,address)
    message(STATUS "libFuzzer: ENABLED (implies ASAN)")
endif()

# Profile-guided optimization (TODO 159 Phase F).
#
# Three-step build process:
#   1. cmake -B build-pgo -DLEPTRIS_ENABLE_PGO=GENERATE ...
#      cmake --build build-pgo
#   2. Run a representative workload against the instrumented binary
#      (e.g. `ctest --test-dir build-pgo` or your app's test suite).
#      For clang: `llvm-profdata merge build-pgo/pgo-data/*.profraw
#      -o build-pgo/pgo-data/default.profdata` between steps 2 and 3.
#      GCC and MSVC generate the profile directly.
#   3. cmake -B build-pgo -DLEPTRIS_ENABLE_PGO=USE ...
#      cmake --build build-pgo
#
# Closes the switch-dispatch gap vs computed-goto without GCC-isms.
# Clang, GCC, and MSVC all support PGO.
#
# Defaults to OFF because it's a two-pass build and most users won't
# bother. Distributions / packagers who want the last ~10-15% on the
# XPath VM dispatch loop should enable it; on the DOM parse path the
# measured gain is smaller (~1-2%, TODO 185 round 3).
set(LEPTRIS_ENABLE_PGO "OFF" CACHE STRING "Profile-guided optimization phase: OFF, GENERATE, or USE")
set_property(CACHE LEPTRIS_ENABLE_PGO PROPERTY STRINGS OFF GENERATE USE)
set(LEPTRIS_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "PGO profile directory")

if(NOT LEPTRIS_ENABLE_PGO STREQUAL "OFF")
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        if(LEPTRIS_ENABLE_PGO STREQUAL "GENERATE")
            add_compile_options(-fprofile-instr-generate=${LEPTRIS_PGO_DIR}/default.profraw)
            add_link_options(-fprofile-instr-generate=${LEPTRIS_PGO_DIR}/default.profraw)
            file(MAKE_DIRECTORY ${LEPTRIS_PGO_DIR})
            message(STATUS "PGO (clang): GENERATE → ${LEPTRIS_PGO_DIR}")
        elseif(LEPTRIS_ENABLE_PGO STREQUAL "USE")
            add_compile_options(-fprofile-instr-use=${LEPTRIS_PGO_DIR}/default.profdata)
            message(STATUS "PGO (clang): USE ← ${LEPTRIS_PGO_DIR}/default.profdata")
        endif()
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        if(LEPTRIS_ENABLE_PGO STREQUAL "GENERATE")
            add_compile_options(-fprofile-generate=${LEPTRIS_PGO_DIR})
            add_link_options(-fprofile-generate=${LEPTRIS_PGO_DIR})
            file(MAKE_DIRECTORY ${LEPTRIS_PGO_DIR})
            message(STATUS "PGO (gcc): GENERATE → ${LEPTRIS_PGO_DIR}")
        elseif(LEPTRIS_ENABLE_PGO STREQUAL "USE")
            add_compile_options(-fprofile-use=${LEPTRIS_PGO_DIR} -Wno-coverage-mismatch)
            message(STATUS "PGO (gcc): USE ← ${LEPTRIS_PGO_DIR}")
        endif()
    elseif(MSVC)
        if(LEPTRIS_ENABLE_PGO STREQUAL "GENERATE")
            add_compile_options(/GL)
            add_link_options(/GENPROFILE:${LEPTRIS_PGO_DIR} /LTCG)
            file(MAKE_DIRECTORY ${LEPTRIS_PGO_DIR})
            message(STATUS "PGO (MSVC): GENERATE → ${LEPTRIS_PGO_DIR}")
        elseif(LEPTRIS_ENABLE_PGO STREQUAL "USE")
            add_compile_options(/GL)
            add_link_options(/USEPROFILE:${LEPTRIS_PGO_DIR} /LTCG)
            message(STATUS "PGO (MSVC): USE ← ${LEPTRIS_PGO_DIR}")
        endif()
    else()
        message(WARNING "LEPTRIS_ENABLE_PGO=${LEPTRIS_ENABLE_PGO} not supported for ${CMAKE_C_COMPILER_ID}; ignoring")
    endif()
endif()

# Doxygen API docs — see TODO 63.
if(LEPTRIS_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/docs/Doxyfile
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs
            COMMENT "Generating Doxygen API documentation")
        message(STATUS "Doxygen: ENABLED (target: docs)")
    else()
        message(WARNING "LEPTRIS_BUILD_DOCS=ON but Doxygen not found; skipping")
    endif()
endif()

# Position independent code for static libraries
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide all symbols by default; the public surface is exactly the
# functions marked LEPTRIS_API. Cuts the shared-library export table
# from ~445 entries down to the ~295 leptris_* symbols. See TODO 80.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Configuration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Check for compile flag support (TODO 167 Phase C uses check_c_compiler_flag).
include(CheckCCompilerFlag)

# Add cmake modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

# Include AdocMan for man page generation
if(LEPTRIS_BUILD_MAN_PAGES)
    include(AdocMan)
endif()

# Add subdirectories
add_subdirectory(src)

if(LEPTRIS_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples/c)
endif()

if(LEPTRIS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Zero-copy test executable (disabled - not needed for benchmarks)
# add_executable(test_zero_copy test_zero_copy.c)
# target_link_libraries(test_zero_copy leptris)

# Install CMake config files
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/leptris-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/leptris-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/leptris-config.cmake"
    INSTALL_DESTINATION lib/cmake/leptris
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/leptris-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/leptris-config-version.cmake"
    DESTINATION lib/cmake/leptris
)


# pkg-config file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/leptris.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/leptris.pc"
    @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/leptris.pc"
    DESTINATION lib/pkgconfig
)

# Generate and install man pages (when building CLI)
if(LEPTRIS_BUILD_CLI AND LEPTRIS_BUILD_MAN_PAGES)
    # Use AdocMan.cmake to generate man pages from AsciiDoc sources
    # CLI man pages (section 1): cli/man/leptris.1.adoc, etc.
    set(ADOC_MAN_SOURCES
        cli/man/leptris.1.adoc
        cli/man/leptris-parse.1.adoc
        cli/man/leptris-xpath.1.adoc
        cli/man/leptris-format.1.adoc
    )

    foreach(ADOC_SOURCE ${ADOC_MAN_SOURCES})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ADOC_SOURCE}")
            add_adoc_man("${CMAKE_CURRENT_SOURCE_DIR}/${ADOC_SOURCE}")
        endif()
    endforeach()
endif()

# Generate library API man page (section 5)
if(LEPTRIS_BUILD_MAN_PAGES)
    # Library man page (section 5): src/man/libleptris.5.adoc
    set(LIB_MAN_SOURCE src/man/libleptris.5.adoc)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${LIB_MAN_SOURCE}")
        add_adoc_man("${CMAKE_CURRENT_SOURCE_DIR}/${LIB_MAN_SOURCE}")
    endif()
endif()

# Summary
message(STATUS "")
message(STATUS "Leptris ${PROJECT_VERSION} Configuration:")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  C Standard: C${CMAKE_C_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Optimization Level: ${LEPTRIS_OPT_LEVEL}")
message(STATUS "  Target Architecture: ${LEPTRIS_TARGET_ARCH}")
message(STATUS "  Shared Libraries: ${BUILD_SHARED_LIBS}")
message(STATUS "  Build Tests: ${BUILD_TESTING}")
message(STATUS "  Build Examples: ${BUILD_EXAMPLES}")
message(STATUS "  Build CLI: ${LEPTRIS_BUILD_CLI}")
message(STATUS "  Generate Man Pages: ${LEPTRIS_BUILD_MAN_PAGES}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
# Update the summary to include benchmarks
message(STATUS "  Build Benchmarks: ${LEPTRIS_BUILD_BENCHMARKS}")

# test/CMakeLists.txt — Google Test integration for Leptris.
#
# Each subdirectory under test/ maps to one module under src/leptris/.
# Adding a new test executable = one call to leptris_add_test().

# Fetch Google Test if the consumer hasn't provided one.  We prefer a
# system/vcpkg install when available; FetchContent is the fallback so
# local builds work without a package manager.
if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_rt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# Convenience aggregate target — `cmake --build build --target check_all`.
add_custom_target(check_all
    COMMENT "Running all Leptris specs")

# leptris_add_test(<name> <sources...>)
#
# Creates an executable, links it to libleptris + gtest_main, registers
# it with ctest via gtest_discover_tests, and adds it to check_all.
function(leptris_add_test name)
    add_executable(${name} ${ARGN})
    # Specs exercise internal symbols (pool internals, vtables,
    # compact pointers) that the shared library does not export —
    # always link the static objects.
    if(TARGET leptris_static)
        target_link_libraries(${name} PRIVATE leptris_static GTest::gtest_main)
    else()
        target_link_libraries(${name} PRIVATE leptris GTest::gtest_main)
    endif()
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        # Pool / StringView internals — used by memory specs.
        ${PROJECT_SOURCE_DIR}/src/leptris/memory
        ${PROJECT_SOURCE_DIR}/src/leptris/common
        # DOM internals — used by vtable specs.
        ${PROJECT_SOURCE_DIR}/src/leptris/dom
        # RNG internals — pattern-IR specs (#878).
        ${PROJECT_SOURCE_DIR}/src/leptris/rng
        # Flat doc internals — used by flat parse specs (TODO 139).
        ${PROJECT_SOURCE_DIR}/src/leptris/flat)
    target_compile_features(${name} PRIVATE cxx_std_11)
    # Timing specs measure ratios; concurrent processes steal cache
    # and skew short measurements — run test_perf's cases serially.
    if(name STREQUAL "test_perf")
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST
                             PROPERTIES RUN_SERIAL TRUE)
    else()
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
    endif()
    add_dependencies(check_all ${name})
endfunction()

# Specialization for CLI specs, which invoke the built binary via
# subprocess and need to know its path.
function(leptris_add_cli_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest_main)
    target_compile_features(${name} PRIVATE cxx_std_11)
    target_compile_definitions(${name} PRIVATE
        LEPTRIS_CLI_BIN="${CMAKE_BINARY_DIR}/cli/leptris"
        LEPTRIS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        # CLI cases spawn subprocesses; fork/exec contention under
        # parallel ctest flakes them — run serially.
        RUN_SERIAL TRUE
    )
    add_dependencies(check_all ${name} leptris-cli)
endfunction()

leptris_add_test(test_smoke      smoke/test_smoke.cpp)
leptris_add_test(test_diff       diff/test_diff.cpp)
leptris_add_test(test_sch        sch/test_schematron.cpp)
leptris_add_test(test_sch_corpus  sch/test_schematron_corpus.cpp)
leptris_add_test(test_qt3          xquery/test_qt3.cpp)
target_compile_definitions(test_qt3 PRIVATE
    LEPTRIS_QT3_DIR="${CMAKE_CURRENT_SOURCE_DIR}/xquery/qt3")
target_compile_definitions(test_sch_corpus PRIVATE
    LEPTRIS_SCH_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sch/conformance-cases")
leptris_add_test(test_parser     parser/test_parser.cpp)
leptris_add_test(test_large_docs parser/test_large_documents.cpp)
leptris_add_test(test_encoding   parser/test_encoding.cpp)
leptris_add_test(test_dom        dom/test_dom.cpp)
leptris_add_test(test_map_lifecycle dom/test_map_lifecycle.cpp)
leptris_add_test(test_document_children dom/test_document_children.cpp)
leptris_add_test(test_vtable     dom/test_vtable.cpp)
leptris_add_test(test_text_borrowed dom/test_text_borrowed.cpp)
leptris_add_test(test_compact    dom/test_compact.cpp)
leptris_add_test(test_digest     dom/test_digest.cpp)
leptris_add_test(test_rng        rng/test_rng.cpp)
target_compile_definitions(test_rng PRIVATE
    LEPTRIS_RNG_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rng/include-cases")
leptris_add_test(test_rng_corpus rng/test_rng_corpus.cpp)
target_compile_definitions(test_rng_corpus PRIVATE
    LEPTRIS_RNG_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rng/jing-cases")
leptris_add_test(test_simd_text  common/test_simd_text.cpp)
leptris_add_test(test_simd_spans common/test_simd_spans.cpp)
leptris_add_test(test_pool       memory/test_pool.cpp)
leptris_add_test(test_oom        memory/test_oom_injection.cpp)
leptris_add_test(test_arena      memory/test_arena.cpp)
leptris_add_test(test_pool_arena memory/test_pool_arena.cpp)
leptris_add_test(test_xpath      xpath/test_xpath.cpp)
leptris_add_test(test_exslt       xpath/test_exslt.cpp)
leptris_add_test(test_compiled   xpath/test_compiled.cpp)
leptris_add_test(test_xpath_conf xpath/test_xpath_conformance.cpp)
leptris_add_test(test_bytecode_vm xpath/test_bytecode_vm.cpp)
leptris_add_test(test_xslt        xpath/test_xslt.cpp)
leptris_add_test(test_xquery      xquery/test_xquery.cpp)
leptris_add_test(test_html        html/test_html.cpp)
leptris_add_test(test_html5lib     html/test_html5lib.cpp)
file(GLOB html5lib_dats
     "${CMAKE_CURRENT_SOURCE_DIR}/html/html5lib-tests/tree-construction/*.dat")
list(SORT html5lib_dats)
set(HTML5LIB_DAT_LIST "")
foreach(f ${html5lib_dats})
    get_filename_component(basename "${f}" NAME)
    list(APPEND HTML5LIB_DAT_LIST "\"${basename}\"")
endforeach()
string(REPLACE ";" ",\n    " HTML5LIB_DAT_INITIALIZER "${HTML5LIB_DAT_LIST}")
configure_file(html/html5lib_cases.h.in html/html5lib_cases.h @ONLY)
target_compile_definitions(test_html5lib PRIVATE
    LEPTRIS_HTML5LIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/html/html5lib-tests/tree-construction")
target_include_directories(test_html5lib PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/html)
target_compile_definitions(test_xquery PRIVATE
    LEPTRIS_XQUERY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/test/xquery")

# The libxslt general suite (test/xslt/libxslt_suite_general) as
# behavioral specs — data files stay in the source tree.
leptris_add_test(test_libxslt_suite xslt/test_libxslt_suite.cpp)
target_compile_definitions(test_libxslt_suite PRIVATE
    LEPTRIS_XSLT_SUITE_DIR="${PROJECT_SOURCE_DIR}/test/xslt/libxslt_suite_general"
    LEPTRIS_XSLT_OPEN_LIST="${PROJECT_SOURCE_DIR}/test/xslt/open_cases.txt")
leptris_add_test(test_serialize  serializer/test_serialize.cpp)
leptris_add_test(test_c14n       serializer/test_c14n.cpp)
leptris_add_test(test_perf       perf/test_perf.cpp)
leptris_add_test(test_sax        sax/test_sax.cpp)
leptris_add_test(test_pull       sax/test_pull.cpp)
leptris_add_test(test_pull_batch sax/test_pull_batch.cpp)
leptris_add_test(test_recorder   sax/test_recorder.cpp)
leptris_add_test(test_iterparse  sax/test_iterparse.cpp)
leptris_add_test(test_streaming  sax/test_streaming.cpp)
leptris_add_test(test_abi        abi/test_header_hygiene.cpp)
leptris_add_test(test_public_surface abi/test_public_surface.cpp)
leptris_add_test(test_serialize_abi abi/test_serialize_abi.cpp)
leptris_add_test(test_nokogiri_api abi/test_nokogiri_api.cpp)
leptris_add_test(test_prev_sib_c14n abi/test_previous_sibling_and_c14n_ex.cpp)
leptris_add_test(test_ns_mut_create abi/test_namespace_mutation_and_create.cpp)
leptris_add_test(test_dom_mut_bugs abi/test_dom_mutation_bugs.cpp)
leptris_add_test(test_v0_5_14_bugs abi/test_v0_5_14_bugs.cpp)
leptris_add_test(test_dom_deep_copy abi/test_dom_deep_copy.cpp)
leptris_add_test(test_node_get_xpath abi/test_node_get_xpath.cpp)
leptris_add_test(test_parse_fragment abi/test_parse_fragment.cpp)
leptris_add_test(test_doctype_api abi/test_doctype_api.cpp)
leptris_add_test(test_xpath_custom_fns abi/test_xpath_custom_fns.cpp)

# High-document-count stress test (issues #256, #261). Parses 5,000
# docs in batches of 500, verifying tree integrity on each.
leptris_add_test(test_stress_high_doc_count perf/test_stress_high_doc_count.cpp)
# TODO.concurrency/08: thread-spawning spec needs pthreads on
# POSIX; the helper above cannot express that.
find_package(Threads REQUIRED)
leptris_add_test(test_concurrency concurrency/test_concurrency.cpp)
target_link_libraries(test_concurrency PRIVATE Threads::Threads)


# FlatDoc + lazy-promote tests removed — direct_parse is the sole
# parser and builds the LeptrisElement tree eagerly. The flat/
# subsystem (TODO 139) has been deleted.

# Shared-library export check (TODO 122).  Runs the bash script that
# builds a one-off libleptris.dylib / .so and asserts SAX + DOM + XPath
# symbols appear in the export table.  Catches missing LEPTRIS_API
# annotations that would silently break FFI bindings.
add_test(
    NAME SymbolExportCheck
    COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/check_shared_exports.sh
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
leptris_add_test(test_dtd        dtd/test_dtd_validate.cpp)
leptris_add_test(test_dtd_w3c    dtd/test_w3c_conformance.cpp)
leptris_add_test(test_xinclude   xinclude/test_xinclude.cpp)
leptris_add_cli_test(test_cli    cli/test_cli.cpp)

# FFI mirror drift gate (architecture review D): the Ruby/Python
# binding mirrors must match the public headers (no phantom symbols,
# no arity drift, core surface present). Python-based; skipped when
# no interpreter is found.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME ffi_mirror_drift
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/check_ffi_mirrors.py
                ${PROJECT_SOURCE_DIR})
endif()

# Export-surface gate (TODO.concurrency/02): every symbol exported
# from the shared library must be declared in src/include/ and vice
# versa. Skips (exit 0) when no shared library was built.
if(Python3_FOUND)
    add_test(NAME export_surface_drift
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/check_export_surface.py
                ${CMAKE_BINARY_DIR}/src/libleptris.dylib
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(export_surface_drift PROPERTIES
        ENVIRONMENT "PYTHONPATH=")
endif()

# libFuzzer harness (TODO 40) — built only when LEPTRIS_ENABLE_FUZZING=ON.
# Not a ctest target; invoked manually with corpus.
if(LEPTRIS_ENABLE_FUZZING)
    add_executable(fuzz_parse ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_parse.c)
    target_link_libraries(fuzz_parse PRIVATE leptris)
    target_include_directories(fuzz_parse PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include)
endif()

# Standalone MME instruction semantics; includes the ARM64 interpreter shift rules.
add_executable(test_macro_opcodes macro_opcode.cpp)
target_include_directories(test_macro_opcodes PRIVATE "${NXEMU_FAST_TEST_SOURCE_DIR}")
target_compile_options(test_macro_opcodes PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
nxemu_fast_test(test_macro_opcodes)

# Execute the production MME interpreter with a fake GPU, including FIFO invalidation on Send.
add_executable(test_macro_interpreter_runtime macro_interpreter_runtime.cpp)
target_include_directories(test_macro_interpreter_runtime BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${NXEMU_FAST_TEST_SOURCE_DIR}")
target_compile_options(test_macro_interpreter_runtime PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
if(NOT MSVC)
    target_compile_options(test_macro_interpreter_runtime PRIVATE
        -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_macro_interpreter_runtime PRIVATE -fsanitize=address,undefined)
endif()
nxemu_fast_test(test_macro_interpreter_runtime)

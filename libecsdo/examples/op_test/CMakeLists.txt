# op_test: libecsdo + libethercat, brings the bus to OP (outputs held at 0).
# Built from the top-level CMakeLists.txt only when libethercat is found.
add_executable(op_test op_test.c)
target_include_directories(op_test PRIVATE ${ECRT_INCLUDE_DIR})
target_link_libraries(op_test PRIVATE ecsdo_util ecsdo ${ETHERCAT_LIBRARY})
target_compile_options(op_test PRIVATE -Wall -Wextra)
# Default config (op_test reads ./wanted.conf) next to the binary.
configure_file(wanted.conf ${CMAKE_CURRENT_BINARY_DIR}/wanted.conf COPYONLY)

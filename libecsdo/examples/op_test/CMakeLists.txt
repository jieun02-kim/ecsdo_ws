# op_test: libecsdo + libethercat, brings the bus to OP (outputs held at 0).
# Built from the top-level CMakeLists.txt only when libethercat is found.
add_executable(op_test op_test.c)
target_include_directories(op_test PRIVATE ${ECRT_INCLUDE_DIR})
target_link_libraries(op_test PRIVATE ecsdo_util ecsdo ${ETHERCAT_LIBRARY} m)
target_compile_options(op_test PRIVATE -Wall -Wextra)
# Default lists (op_test reads ./pdo.conf and ./sdo.conf) next to the binary.
configure_file(pdo.conf ${CMAKE_CURRENT_BINARY_DIR}/pdo.conf COPYONLY)
configure_file(sdo.conf ${CMAKE_CURRENT_BINARY_DIR}/sdo.conf COPYONLY)

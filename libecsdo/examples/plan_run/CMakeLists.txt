# plan_run: applies tools/ecat_rules plans, goes to OP and runs a small test.
# Built from the top-level CMakeLists.txt only when libethercat is found.
add_executable(plan_run plan_run.c)
target_include_directories(plan_run PRIVATE ${ECRT_INCLUDE_DIR})
target_link_libraries(plan_run PRIVATE ${ETHERCAT_LIBRARY} m)
target_compile_options(plan_run PRIVATE -Wall -Wextra)

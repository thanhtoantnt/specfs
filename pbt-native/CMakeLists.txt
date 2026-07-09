cmake_minimum_required(VERSION 3.14)
project(specfs_pbt C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "Use a single-config generator")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- Vendor theft (PBT framework for C) via FetchContent ---
# theft ships a Makefile, not CMake, so we compile its sources directly.
include(FetchContent)
FetchContent_Declare(theft
  GIT_REPOSITORY https://github.com/silentbicycle/theft.git
  GIT_TAG        v0.4.5)
FetchContent_GetProperties(theft)
if(NOT theft_POPULATED)
  FetchContent_Populate(theft)
endif()

add_library(theft STATIC
  ${theft_SOURCE_DIR}/src/theft.c
  ${theft_SOURCE_DIR}/src/theft_autoshrink.c
  ${theft_SOURCE_DIR}/src/theft_aux.c
  ${theft_SOURCE_DIR}/src/theft_aux_builtin.c
  ${theft_SOURCE_DIR}/src/theft_bloom.c
  ${theft_SOURCE_DIR}/src/theft_call.c
  ${theft_SOURCE_DIR}/src/theft_hash.c
  ${theft_SOURCE_DIR}/src/theft_random.c
  ${theft_SOURCE_DIR}/src/theft_rng.c
  ${theft_SOURCE_DIR}/src/theft_run.c
  ${theft_SOURCE_DIR}/src/theft_shrink.c
  ${theft_SOURCE_DIR}/src/theft_trial.c
)
target_include_directories(theft PUBLIC ${theft_SOURCE_DIR}/inc)

enable_testing()

# --- Target: hash_func (hashing.c) ---
add_executable(hash_func_pbt_test
  hash_func_pbt_test.c
  ../eval/delay_alloc/optimization/hashing.c
)
target_include_directories(hash_func_pbt_test PRIVATE
  ../eval/delay_alloc/optimization
)
target_link_libraries(hash_func_pbt_test PRIVATE theft)
add_test(NAME hash_func_pbt_test COMMAND hash_func_pbt_test)

# --- Target: inode_write (extent variant) ---
add_executable(inode_write_pbt_test
  inode_write_pbt_test.c
  ../eval/extent/optimization/inode_management.c
)
target_include_directories(inode_write_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(inode_write_pbt_test PRIVATE theft)
add_test(NAME inode_write_pbt_test COMMAND inode_write_pbt_test)

# --- Target: inode_write (rbtree variant) ---
add_executable(inode_write_rbtree_pbt_test
  inode_write_rbtree_pbt_test.c
  ../eval/rbtree/optimization/inode_management.c
)
target_include_directories(inode_write_rbtree_pbt_test PRIVATE
  ../eval/rbtree/optimization
)
target_link_libraries(inode_write_rbtree_pbt_test PRIVATE theft)
add_test(NAME inode_write_rbtree_pbt_test COMMAND inode_write_rbtree_pbt_test)

# --- Target: lowlevel_file (lowlevel_file.c) ---
add_executable(lowlevel_file_pbt_test
  lowlevel_file_pbt_test.c
  ../eval/extent/optimization/lowlevel_file.c
)
target_include_directories(lowlevel_file_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(lowlevel_file_pbt_test PRIVATE theft)
add_test(NAME lowlevel_file_pbt_test COMMAND lowlevel_file_pbt_test)

# --- Target: calculate (path_handling.c) ---
add_executable(calculate_pbt_test
  calculate_pbt_test.c
)
target_include_directories(calculate_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(calculate_pbt_test PRIVATE theft)
add_test(NAME calculate_pbt_test COMMAND calculate_pbt_test)

# --- Target: inode_truncate (inode_management.c) ---
add_executable(inode_truncate_pbt_test
  inode_truncate_pbt_test.c
  ../eval/extent/optimization/inode_management.c
)
target_include_directories(inode_truncate_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(inode_truncate_pbt_test PRIVATE theft)
add_test(NAME inode_truncate_pbt_test COMMAND inode_truncate_pbt_test)

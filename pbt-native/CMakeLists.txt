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

# --- Target: file_write (rbtree lowlevel_file.c) ---
add_executable(rbtree_lowlevel_file_write_pbt_test
  rbtree_lowlevel_file_write_pbt_test.c
  ../eval/rbtree/optimization/lowlevel_file.c
  ../eval/rbtree/optimization/mballoc.c
  ../eval/rbtree/optimization/rbtree.c
)
target_include_directories(rbtree_lowlevel_file_write_pbt_test PRIVATE
  ../eval/rbtree/optimization
)
target_link_libraries(rbtree_lowlevel_file_write_pbt_test PRIVATE theft)
add_test(NAME rbtree_lowlevel_file_write_pbt_test COMMAND rbtree_lowlevel_file_write_pbt_test)

# --- Target: file_write (pre_alloc lowlevel_file.c) ---
add_executable(prealloc_lowlevel_file_write_pbt_test
  prealloc_lowlevel_file_write_pbt_test.c
  ../eval/pre_alloc/optimization/lowlevel_file.c
  ../eval/pre_alloc/optimization/mballoc.c
)
target_include_directories(prealloc_lowlevel_file_write_pbt_test PRIVATE
  ../eval/pre_alloc/optimization
)
target_link_libraries(prealloc_lowlevel_file_write_pbt_test PRIVATE theft)
add_test(NAME prealloc_lowlevel_file_write_pbt_test COMMAND prealloc_lowlevel_file_write_pbt_test)

# --- Target: inode_write (pre_alloc variant) ---
add_executable(inode_write_prealloc_pbt_test
  inode_write_prealloc_pbt_test.c
  ../eval/pre_alloc/optimization/inode_management.c
)
target_include_directories(inode_write_prealloc_pbt_test PRIVATE
  ../eval/pre_alloc/optimization
)
target_link_libraries(inode_write_prealloc_pbt_test PRIVATE theft)
add_test(NAME inode_write_prealloc_pbt_test COMMAND inode_write_prealloc_pbt_test)

# --- Target: inode_write (inline_data variant) ---
add_executable(inode_write_inline_data_pbt_test
  inode_write_inline_data_pbt_test.c
  ../eval/inline_data/optimization/inode_management.c
)
target_include_directories(inode_write_inline_data_pbt_test PRIVATE
  ../eval/inline_data/optimization
)
target_link_libraries(inode_write_inline_data_pbt_test PRIVATE theft)
add_test(NAME inode_write_inline_data_pbt_test COMMAND inode_write_inline_data_pbt_test)

# --- Target: inode_write (delay_alloc variant) ---
add_executable(inode_write_delay_alloc_pbt_test
  inode_write_delay_alloc_pbt_test.c
  ../eval/delay_alloc/optimization/inode_management.c
)
target_include_directories(inode_write_delay_alloc_pbt_test PRIVATE
  ../eval/delay_alloc/optimization
)
target_link_libraries(inode_write_delay_alloc_pbt_test PRIVATE theft)
add_test(NAME inode_write_delay_alloc_pbt_test COMMAND inode_write_delay_alloc_pbt_test)

# --- Target: file_write (delay_alloc lowlevel_file.c) ---
add_executable(delay_alloc_file_write_pbt_test
  delay_alloc_file_write_pbt_test.c
  ../eval/delay_alloc/optimization/lowlevel_file.c
  ../eval/delay_alloc/optimization/delalloc.c
)
target_include_directories(delay_alloc_file_write_pbt_test PRIVATE
  ../eval/delay_alloc/optimization
)
target_link_libraries(delay_alloc_file_write_pbt_test PRIVATE theft)
add_test(NAME delay_alloc_file_write_pbt_test COMMAND delay_alloc_file_write_pbt_test)

# --- Target: inode_truncate (inline_data variant) ---
add_executable(inode_truncate_inline_data_pbt_test
  inode_truncate_inline_data_pbt_test.c
  ../eval/inline_data/optimization/inode_management.c
)
target_include_directories(inode_truncate_inline_data_pbt_test PRIVATE
  ../eval/inline_data/optimization
)
target_link_libraries(inode_truncate_inline_data_pbt_test PRIVATE theft)
add_test(NAME inode_truncate_inline_data_pbt_test COMMAND inode_truncate_inline_data_pbt_test)

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

# --- Target: file_read (extent lowlevel_file.c) ---
add_executable(extent_file_read_pbt_test
  extent_file_read_pbt_test.c
  ../eval/extent/optimization/lowlevel_file.c
)
target_include_directories(extent_file_read_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(extent_file_read_pbt_test PRIVATE theft)
add_test(NAME extent_file_read_pbt_test COMMAND extent_file_read_pbt_test)

# --- Target: encrypt lowlevel_file (eval/loc/gen/encrypt/lowlevel_file.c) ---
add_executable(encrypt_lowlevel_file_write_pbt_test
  encrypt_lowlevel_file_write_pbt_test.c
)
target_include_directories(encrypt_lowlevel_file_write_pbt_test PRIVATE
  ../eval/loc/gen/encrypt
)
target_link_libraries(encrypt_lowlevel_file_write_pbt_test PRIVATE theft)
add_test(NAME encrypt_lowlevel_file_write_pbt_test COMMAND encrypt_lowlevel_file_write_pbt_test)


# --- Target: checksum lowlevel_file (eval/loc/gen/checksum/lowlevel_file.c) ---
add_executable(checksum_lowlevel_file_write_pbt_test
  checksum_lowlevel_file_write_pbt_test.c
)
target_link_libraries(checksum_lowlevel_file_write_pbt_test PRIVATE theft)
add_test(NAME checksum_lowlevel_file_write_pbt_test COMMAND checksum_lowlevel_file_write_pbt_test)


# --- Target: inode_truncate (extent variant) ---
add_executable(inode_truncate_pbt_test
  inode_truncate_pbt_test.c
  ../eval/extent/optimization/inode_management.c
)
target_include_directories(inode_truncate_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(inode_truncate_pbt_test PRIVATE theft)
add_test(NAME inode_truncate_pbt_test COMMAND inode_truncate_pbt_test)

# --- Target: inode_truncate (pre_alloc variant) ---
add_executable(inode_truncate_prealloc_pbt_test
  inode_truncate_prealloc_pbt_test.c
  ../eval/pre_alloc/optimization/inode_management.c
)
target_include_directories(inode_truncate_prealloc_pbt_test PRIVATE
  ../eval/pre_alloc/optimization
)
target_link_libraries(inode_truncate_prealloc_pbt_test PRIVATE theft)
add_test(NAME inode_truncate_prealloc_pbt_test COMMAND inode_truncate_prealloc_pbt_test)

# --- Target: inode_truncate (rbtree variant) ---
add_executable(inode_truncate_rbtree_pbt_test
  inode_truncate_rbtree_pbt_test.c
  ../eval/rbtree/optimization/inode_management.c
)
target_include_directories(inode_truncate_rbtree_pbt_test PRIVATE
  ../eval/rbtree/optimization
)
target_link_libraries(inode_truncate_rbtree_pbt_test PRIVATE theft)
add_test(NAME inode_truncate_rbtree_pbt_test COMMAND inode_truncate_rbtree_pbt_test)

# --- Target: calculate (extent variant path_handling.c) ---
add_executable(calculate_pbt_test
  calculate_pbt_test.c
)
target_include_directories(calculate_pbt_test PRIVATE
  ../eval/extent/optimization
)
target_link_libraries(calculate_pbt_test PRIVATE theft)
add_test(NAME calculate_pbt_test COMMAND calculate_pbt_test)

# --- Target: inode_read (extent variant) ---
add_executable(inode_read_pbt_test
  inode_read_pbt_test.c
)
target_link_libraries(inode_read_pbt_test PRIVATE theft)
add_test(NAME inode_read_pbt_test COMMAND inode_read_pbt_test)

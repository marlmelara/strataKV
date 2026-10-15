add_executable(stratakv_tests
               test_crash_point.cc
               test_crc32c.cc
               test_db.cc
               test_env.cc
               test_internal_key.cc
               test_memtable.cc
               test_skiplist.cc
               test_status.cc
               test_wal.cc
               test_write_batch.cc)

target_link_libraries(stratakv_tests PRIVATE stratakv GTest::gtest_main)

# Tests reach into src/ because Env and CRC32C are engine internals, not part
# of the public include/stratakv API.
target_include_directories(stratakv_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_options(stratakv_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(stratakv_tests)

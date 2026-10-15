# A separate program the crash-recovery tests run as a child process. It has to
# be its own binary because a crash point calls _Exit, which would take the test
# runner down with it.
add_executable(stratakv_crash_helper crash_helper_main.cc)
target_link_libraries(stratakv_crash_helper PRIVATE stratakv)
target_compile_options(stratakv_crash_helper PRIVATE -Wall -Wextra)

add_executable(stratakv_tests
               test_crash_point.cc
               test_crc32c.cc
               test_bloom.cc
               test_db.cc
               test_flush.cc
               test_env.cc
               test_internal_key.cc
               test_memtable.cc
               test_skiplist.cc
               test_sstable.cc
               test_status.cc
               test_wal.cc
               test_write_batch.cc)

target_link_libraries(stratakv_tests PRIVATE stratakv GTest::gtest_main)

# Tests reach into src/ because Env and CRC32C are engine internals, not part
# of the public include/stratakv API.
target_include_directories(stratakv_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_options(stratakv_tests PRIVATE -Wall -Wextra)

# The tests need to launch the helper, so they are told where it ended up.
target_compile_definitions(stratakv_tests
                           PRIVATE STRATAKV_CRASH_HELPER="$<TARGET_FILE:stratakv_crash_helper>")
add_dependencies(stratakv_tests stratakv_crash_helper)

include(GoogleTest)
gtest_discover_tests(stratakv_tests)

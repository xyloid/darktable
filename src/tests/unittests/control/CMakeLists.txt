add_cmocka_test(test_remote_edit
                SOURCES test_remote_edit.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_edit lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_protocol
                SOURCES test_remote_protocol.c
                LINK_LIBRARIES lib_darktable cmocka)

target_compile_definitions(test_remote_protocol PRIVATE
  TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_protocol lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_frame
                SOURCES test_remote_frame.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_frame lib_darktable)
endif(WIN32)

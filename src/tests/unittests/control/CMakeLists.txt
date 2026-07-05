add_cmocka_test(test_remote_edit
                SOURCES test_remote_edit.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_edit lib_darktable)
endif(WIN32)

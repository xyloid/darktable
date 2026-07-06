add_cmocka_test(test_crypto_random
                SOURCES test_crypto_random.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_crypto_random lib_darktable)
endif(WIN32)

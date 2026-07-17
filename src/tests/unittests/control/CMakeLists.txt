add_cmocka_test(test_remote_edit
                SOURCES test_remote_edit.c
                LINK_LIBRARIES lib_darktable cmocka)

# test_schema_marks_denylisted_fields_unwritable loads real module .so's via
# dt_init(); point it at this build's own plugin directory instead of the
# (nonexistent, install-layout) default relative to the test binary.
target_compile_definitions(test_remote_edit PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_edit lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_band
                SOURCES test_remote_band.c
                LINK_LIBRARIES lib_darktable cmocka)

# test_registry_* / test_apply_* load the real lowlight and atrous module
# .so's via dt_init(); point it at this build's own plugin directory instead
# of the (nonexistent, install-layout) default relative to the test binary --
# same rationale as test_remote_curve's DT_TEST_MODULEDIR above.
target_compile_definitions(test_remote_band PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_band lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_curve
                SOURCES test_remote_curve.c
                LINK_LIBRARIES lib_darktable cmocka)

# test_path_resolve_* loads the real rgbcurve module .so via dt_init(); point
# it at this build's own plugin directory instead of the (nonexistent,
# install-layout) default relative to the test binary -- same rationale as
# test_remote_edit's DT_TEST_MODULEDIR above.
target_compile_definitions(test_remote_curve PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_curve lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_curve_registry
                SOURCES test_remote_curve_registry.c
                LINK_LIBRARIES lib_darktable cmocka)

# test_registry_* / test_read_values_* load the real rgbcurve module .so via
# dt_init(); point it at this build's own plugin directory instead of the
# (nonexistent, install-layout) default relative to the test binary -- same
# rationale as test_remote_curve's DT_TEST_MODULEDIR above.
target_compile_definitions(test_remote_curve_registry PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_curve_registry lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_vector
                SOURCES test_remote_vector.c
                LINK_LIBRARIES lib_darktable cmocka)

# test_registry_* / test_vector_adapter_* load the real borders module .so
# via dt_init(); point it at this build's own plugin directory instead of
# the (nonexistent, install-layout) default relative to the test binary --
# same rationale as test_remote_curve's DT_TEST_MODULEDIR above.
target_compile_definitions(test_remote_vector PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_vector lib_darktable)
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

add_cmocka_test(test_remote_discovery
                SOURCES test_remote_discovery.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_discovery lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_server
                SOURCES test_remote_server.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_server lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_revision
                SOURCES test_remote_revision.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_revision lib_darktable)
endif(WIN32)

add_cmocka_test(test_remote_scopes
                SOURCES test_remote_scopes.c
                LINK_LIBRARIES lib_darktable cmocka)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_remote_scopes lib_darktable)
endif(WIN32)

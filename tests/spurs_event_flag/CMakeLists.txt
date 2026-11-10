get_filename_component(PS3RECOMP_EVENT_FLAG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(spurs_event_flag_test
    main.c
    test_shims.c
    "${PS3RECOMP_EVENT_FLAG_ROOT}/libs/spurs/cellSpurs.c"
    "${PS3RECOMP_EVENT_FLAG_ROOT}/runtime/spu/spu_reservation.c"
)

target_include_directories(spurs_event_flag_test PRIVATE
    "${PS3RECOMP_EVENT_FLAG_ROOT}/include"
    "${PS3RECOMP_EVENT_FLAG_ROOT}/libs/spurs"
    "${PS3RECOMP_EVENT_FLAG_ROOT}/runtime/spu"
)

target_compile_definitions(spurs_event_flag_test PRIVATE
    PS3RECOMP_SPURS_EVENT_FLAG_TEST=1)

if(MSVC)
    target_compile_options(spurs_event_flag_test PRIVATE /W3 /Gy /experimental:c11atomics)
    target_compile_definitions(spurs_event_flag_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(spurs_event_flag_test PRIVATE /OPT:REF)
endif()

add_test(NAME spurs_event_flag COMMAND spurs_event_flag_test)
set_tests_properties(spurs_event_flag PROPERTIES
    LABELS "runtime;spurs;event-flag"
    TIMEOUT 10
)

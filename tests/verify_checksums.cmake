if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(files
    "traces/monkey-playground/0001-application.tracy"
    "traces/synthetic/0001-core.tracy"
    "traces/synthetic/0002-crash.tracy"
    "traces/synthetic/0003-sampling.tracy"
    "traces/synthetic/0004-structural.tracy"
)
set(expected
    "7b20e50534b1b3f2da094602af0a2f6c8049056a9e9bfcc91fae1964de07c765"
    "a007fe87edfb13a82a9642c082926a2c9d07d27393bffbd6bf2a39a34053abd4"
    "1f7bf2d484ac6ddc01f1f5f37b084cd1f387711e9ed561bc0c6d539bcf97f8e1"
    "c24da410c252c97e82456b1e587b80f475c993b7f72243ba28c709f13ad38481"
    "153064159193d7dbde12393509a317289510fca43146bfddc9594b9c572d0d5a"
)
list(LENGTH files count)
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    list(GET files ${index} relative)
    list(GET expected ${index} wanted)
    file(SHA256 "${SOURCE_DIR}/${relative}" actual)
    if(NOT actual STREQUAL wanted)
        message(FATAL_ERROR "checksum mismatch for ${relative}: expected ${wanted}, got ${actual}")
    endif()
endforeach()

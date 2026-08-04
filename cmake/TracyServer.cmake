include_guard(GLOBAL)

include(FetchContent)

# Tracy's installed CMake package only exposes the instrumentation client. Trace
# loading lives in the version-specific server API, so fetch the exact 0.13.1
# source revision and compile the same source set used by Tracy's CLI tools.
FetchContent_Declare(
    tracy_0131
    URL https://github.com/wolfpld/tracy/archive/05cceee0df3b8d7c6fa87e9638af311dbabc63cb.tar.gz
    URL_HASH SHA256=9447edfc59838348e94f7057999b65612d6a9073020efa5a9bea75200377ee1d
    SOURCE_SUBDIR tracy-query-no-cmakelists
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

# These revisions match Tracy 0.13.1's own dependency declarations. They are
# also commit- and content-pinned so a tag moving cannot change the build.
FetchContent_Declare(
    tracy_ppqsort
    URL https://github.com/GabTux/PPQSort/archive/21c5a20ca3daf572caaf0a8522db32e27e28fa1d.tar.gz
    URL_HASH SHA256=b9b3a1b226bc19fffac2d080d1775b5350573856a61893e444f5ffa7cc982242
    SOURCE_SUBDIR tracy-query-no-cmakelists
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(tracy_0131 tracy_ppqsort)

set(CAPSTONE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_LEGACY_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTEST OFF CACHE BOOL "" FORCE)
set(CAPSTONE_INSTALL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_AARCH64_SUPPORT ON CACHE BOOL "" FORCE)
set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "" FORCE)
set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy_capstone
    URL https://github.com/capstone-engine/capstone/archive/fad9f80564501f083adc92db3ef37f999af28dd0.tar.gz
    URL_HASH SHA256=bcd3940bd989cab5fd960ced81a8de99168363b1baaeb943da581a8d0e0ac96a
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
# Capstone inherits PROJECT_VERSION when embedded, so temporarily provide its
# upstream version instead of letting it consume tracy-query's version.
set(_tracy_query_project_version "${PROJECT_VERSION}")
set(PROJECT_VERSION 6.0.0)
FetchContent_MakeAvailable(tracy_capstone)
set(PROJECT_VERSION "${_tracy_query_project_version}")
unset(_tracy_query_project_version)
# Alpha5 creates this helper unconditionally; it is not needed by the parser.
set_target_properties(fuzz_disasm PROPERTIES EXCLUDE_FROM_ALL TRUE)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_COMPRESSION ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DECOMPRESSION ON CACHE BOOL "" FORCE)
# TracyWorker also contains trace-writing code that references zdict symbols,
# even though this executable only reads traces.
set(ZSTD_BUILD_DICTBUILDER ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy_zstd
    URL https://github.com/facebook/zstd/archive/f8745da6ff1ad1e7bab384bd1f9d742439278e99.tar.gz
    URL_HASH SHA256=4b0bd1f0cfb25e61b9103c35f27395530ff5b4c0d2513a00fd745849e85ea52c
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(tracy_zstd)
# zstd 1.5.7 has no install-disable option. Excluding its subdirectory keeps
# this application's install component limited to the tracy-query executable.
set_property(
    DIRECTORY "${tracy_zstd_SOURCE_DIR}/build/cmake"
    PROPERTY EXCLUDE_FROM_ALL TRUE
)

find_package(Threads REQUIRED)

add_library(tracy_ppqsort_headers INTERFACE)
target_include_directories(
    tracy_ppqsort_headers
    INTERFACE "${tracy_ppqsort_SOURCE_DIR}/include"
)
target_link_libraries(tracy_ppqsort_headers INTERFACE Threads::Threads)

set(_tracy_common_dir "${tracy_0131_SOURCE_DIR}/public/common")
set(_tracy_server_dir "${tracy_0131_SOURCE_DIR}/server")

add_library(
    tracy_query_tracy_server STATIC
    "${_tracy_common_dir}/tracy_lz4.cpp"
    "${_tracy_common_dir}/tracy_lz4hc.cpp"
    "${_tracy_common_dir}/TracySocket.cpp"
    "${_tracy_common_dir}/TracyStackFrames.cpp"
    "${_tracy_common_dir}/TracySystem.cpp"
    "${_tracy_server_dir}/TracyMemory.cpp"
    "${_tracy_server_dir}/TracyMmap.cpp"
    "${_tracy_server_dir}/TracyPrint.cpp"
    "${_tracy_server_dir}/TracySysUtil.cpp"
    "${_tracy_server_dir}/TracyTaskDispatch.cpp"
    "${_tracy_server_dir}/TracyTextureCompression.cpp"
    "${_tracy_server_dir}/TracyThreadCompress.cpp"
    "${_tracy_server_dir}/TracyWorker.cpp"
)
add_library(TracyQuery::TracyServer ALIAS tracy_query_tracy_server)

target_compile_features(tracy_query_tracy_server PUBLIC cxx_std_20)
target_include_directories(
    tracy_query_tracy_server
    SYSTEM PUBLIC
        "${_tracy_common_dir}"
        "${_tracy_server_dir}"
    PRIVATE
        # Tracy includes this as <capstone.h>, while Capstone's public target
        # exposes the parent directory for <capstone/capstone.h>.
        "${tracy_capstone_SOURCE_DIR}/include/capstone"
)
target_link_libraries(
    tracy_query_tracy_server
    PUBLIC
        capstone_static
        libzstd_static
        tracy_ppqsort_headers
)

unset(_tracy_common_dir)
unset(_tracy_server_dir)

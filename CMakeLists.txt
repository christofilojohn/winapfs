cmake_minimum_required(VERSION 3.20)

project(winapfs VERSION 0.7.6 LANGUAGES C CXX)

if(NOT MSVC)
	message(FATAL_ERROR "winapfs is built with MSVC")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/utf-8 /MP)
# PDBs in Release too (symbolized crash stacks, apfsrw-bench --profile); no effect on code
add_compile_options($<$<CONFIG:Release>:/Zi>)
add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(APFSFUSE ${TP}/apfs-fuse)

# ---------------------------------------------------------------- third party
add_library(zlib STATIC
	${TP}/zlib/adler32.c ${TP}/zlib/compress.c ${TP}/zlib/crc32.c ${TP}/zlib/deflate.c
	${TP}/zlib/infback.c ${TP}/zlib/inffast.c ${TP}/zlib/inflate.c ${TP}/zlib/inftrees.c
	${TP}/zlib/trees.c ${TP}/zlib/uncompr.c ${TP}/zlib/zutil.c)
target_include_directories(zlib PUBLIC ${TP}/zlib)
target_compile_options(zlib PRIVATE /W0)

add_library(bz2 STATIC
	${TP}/bzip2/blocksort.c ${TP}/bzip2/huffman.c ${TP}/bzip2/crctable.c ${TP}/bzip2/randtable.c
	${TP}/bzip2/compress.c ${TP}/bzip2/decompress.c ${TP}/bzip2/bzlib.c)
target_include_directories(bz2 PUBLIC ${TP}/bzip2)
target_compile_options(bz2 PRIVATE /W0)

add_library(lzfse STATIC
	${APFSFUSE}/3rdparty/lzfse/src/lzfse_decode.c
	${APFSFUSE}/3rdparty/lzfse/src/lzfse_decode_base.c
	${APFSFUSE}/3rdparty/lzfse/src/lzfse_encode.c
	${APFSFUSE}/3rdparty/lzfse/src/lzfse_encode_base.c
	${APFSFUSE}/3rdparty/lzfse/src/lzfse_fse.c
	${APFSFUSE}/3rdparty/lzfse/src/lzvn_decode_base.c
	${APFSFUSE}/3rdparty/lzfse/src/lzvn_encode_base.c)
target_include_directories(lzfse PUBLIC ${APFSFUSE}/3rdparty/lzfse/src)
target_compile_options(lzfse PRIVATE /W0)

file(GLOB APFS_CRYPTO_SRC ${APFSFUSE}/Crypto/*.cpp)
add_library(apfscrypto STATIC ${APFS_CRYPTO_SRC})
target_include_directories(apfscrypto SYSTEM PUBLIC ${APFSFUSE})
target_compile_options(apfscrypto PRIVATE /W3)

file(GLOB APFSLIB_SRC ${APFSFUSE}/ApfsLib/*.cpp)
list(FILTER APFSLIB_SRC EXCLUDE REGEX "Device(Linux|Mac)\\.cpp$")
add_library(apfslib STATIC ${APFSLIB_SRC})
target_include_directories(apfslib SYSTEM PUBLIC ${APFSFUSE})
target_include_directories(apfslib PRIVATE ${APFSFUSE}/ApfsLib)
target_link_libraries(apfslib PUBLIC apfscrypto zlib bz2 lzfse)
target_compile_definitions(apfslib PUBLIC NOMINMAX)
target_compile_options(apfslib PRIVATE /W3 /wd4244 /wd4267 /wd4100)

# linux-apfs-rw (read-write engine) on the kernel shim, C23 for typeof
file(GLOB APFSRW_SRC ${TP}/linux-apfs-rw/*.c)
file(GLOB KSHIM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/kshim/*.c)
add_library(apfsrw STATIC ${APFSRW_SRC} ${KSHIM_SRC})
set_target_properties(apfsrw PROPERTIES C_STANDARD 23)
target_include_directories(apfsrw BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/kshim/include ${TP}/linux-apfs-rw)
target_include_directories(apfsrw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/kshim/api)
target_link_libraries(apfsrw PRIVATE lzfse zlib)
target_compile_definitions(apfsrw PRIVATE NOMINMAX)
target_compile_options(apfsrw PRIVATE /W3 /wd4244 /wd4267 /wd4018 /wd4146)

# ---------------------------------------------------------------- winapfs
add_subdirectory(src/core)
add_subdirectory(src/cli)
add_subdirectory(src/fsck)

find_path(WINFSP_INCLUDE_DIR winfsp/winfsp.h
	HINTS "$ENV{ProgramFiles\(x86\)}/WinFsp/inc" "C:/Program Files (x86)/WinFsp/inc")
find_library(WINFSP_LIBRARY winfsp-x64
	HINTS "$ENV{ProgramFiles\(x86\)}/WinFsp/lib" "C:/Program Files (x86)/WinFsp/lib")
if(WINFSP_INCLUDE_DIR AND WINFSP_LIBRARY)
	add_subdirectory(src/mount)
	add_subdirectory(src/tray)
else()
	message(WARNING "WinFsp SDK not found; skipping the winapfs mount tool")
endif()

enable_testing()
add_subdirectory(tests)

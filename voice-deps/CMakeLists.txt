# Builds the libraries that the voice support of Discord Messenger needs:
#
#   - opus      (deps/opus):    the audio codec
#   - mlspp     (deps/mlspp):   an implementation of the MLS protocol, which DAVE is built on
#   - libdave   (deps/libdave): DAVE, Discord's end-to-end encryption of voice
#
# They are built as static libraries into <build dir>/lib/<config>/.  OpenSSL is needed as well,
# the same one that Discord Messenger itself uses.  This is only needed for builds that have
# voice support turned on (ENABLE_VOICE), see the README.
#
# Usage (Visual Studio generator, 64-bit):
#   cmake -S voice-deps -B build-voice/x64 -A x64 -DOPENSSL_ROOT_DIR=<openssl dir>
#   cmake --build build-voice/x64 --config Release
#   cmake --build build-voice/x64 --config Debug
# or use build-voice-deps.cmd, which does just that.

cmake_minimum_required(VERSION 3.20)

project(dm_voice_deps LANGUAGES C CXX)

set(DM_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(DM_ROOT "${DM_ROOT}" ABSOLUTE)

foreach(dep opus mlspp libdave)
	if(NOT EXISTS "${DM_ROOT}/deps/${dep}")
		message(FATAL_ERROR "deps/${dep} is missing, run: git submodule update --init")
	endif()
endforeach()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Everything ends up in one place, which is where the Visual Studio project and the makefile look.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---- OpenSSL ----

find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL ${OPENSSL_VERSION}: ${OPENSSL_INCLUDE_DIR}")

# ---- nlohmann/json ----
#
# mlspp wants the nlohmann_json package, but Discord Messenger already carries the very same
# single header file, so a stand-in package pointing there is made up.

set(SHIM_DIR "${CMAKE_BINARY_DIR}/shim")
file(MAKE_DIRECTORY "${SHIM_DIR}/include/nlohmann")
file(WRITE "${SHIM_DIR}/include/nlohmann/json.hpp"
	"#include \"${DM_ROOT}/deps/nlohmann/json.h\"\n")
file(WRITE "${SHIM_DIR}/nlohmann_json/nlohmann_jsonConfig.cmake"
"if(NOT TARGET nlohmann_json::nlohmann_json)
	add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
	set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
		INTERFACE_INCLUDE_DIRECTORIES \"${SHIM_DIR}/include\")
endif()
set(nlohmann_json_FOUND TRUE)
")
set(nlohmann_json_DIR "${SHIM_DIR}/nlohmann_json" CACHE PATH "" FORCE)

# ---- Opus ----

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)   # also works on processors without SSE
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory("${DM_ROOT}/deps/opus" opus EXCLUDE_FROM_ALL)

# ---- MLS ----

set(TESTING OFF CACHE BOOL "" FORCE)
set(DISABLE_GREASE ON CACHE BOOL "" FORCE)
set(MLS_CXX_NAMESPACE "mlspp" CACHE STRING "" FORCE)
add_subdirectory("${DM_ROOT}/deps/mlspp" mlspp EXCLUDE_FROM_ALL)

# mlspp builds with warnings as errors, which is no use to anyone who has a newer compiler than
# its authors did.
foreach(target mlspp bytes tls_syntax hpke)
	if(MSVC AND TARGET ${target})
		target_compile_options(${target} PRIVATE /WX-)
	endif()
endforeach()

# ---- libdave ----
#
# libdave's own CMake project insists on getting its dependencies through vcpkg, so its sources
# are compiled here directly instead.

set(DAVE_DIR "${DM_ROOT}/deps/libdave/cpp")
file(GLOB_RECURSE DAVE_SOURCES CONFIGURE_DEPENDS "${DAVE_DIR}/src/*.cpp")
list(FILTER DAVE_SOURCES EXCLUDE REGEX ".*persisted_key.*")
list(FILTER DAVE_SOURCES EXCLUDE REGEX ".*_wasm\\.cpp")
list(FILTER DAVE_SOURCES EXCLUDE REGEX ".*boringssl_cryptor.*")
list(APPEND DAVE_SOURCES "${DAVE_DIR}/src/mls/persisted_key_pair_null.cpp")

add_library(libdave STATIC ${DAVE_SOURCES})
set_target_properties(libdave PROPERTIES PREFIX "")
target_include_directories(libdave
	PUBLIC  "${DAVE_DIR}/includes"
	PRIVATE "${DAVE_DIR}/src")
target_link_libraries(libdave PUBLIC mlspp OpenSSL::Crypto)

if(OPENSSL_VERSION VERSION_GREATER_EQUAL 3)
	target_compile_definitions(libdave PUBLIC WITH_OPENSSL3)
endif()

if(MSVC)
	target_compile_definitions(libdave PRIVATE WINDOWS _CRT_SECURE_NO_WARNINGS NOMINMAX)
	target_compile_options(libdave PRIVATE /W3)
endif()

# One target to build them all.
add_custom_target(voice_deps DEPENDS opus libdave mlspp bytes tls_syntax hpke)

# ---- Tests ----
#
# Checks the transport encryption against published test vectors.

option(DM_VOICE_TESTS "Build the tests of the voice engine" OFF)
if(DM_VOICE_TESTS)
	add_executable(voice_crypto_test
		test/VoiceCryptoTest.cpp
		"${DM_ROOT}/src/voice/VoiceCrypto.cpp")
	target_include_directories(voice_crypto_test PRIVATE "${DM_ROOT}/src")
	target_link_libraries(voice_crypto_test PRIVATE OpenSSL::Crypto)
	if(MSVC)
		target_compile_definitions(voice_crypto_test PRIVATE _CRT_SECURE_NO_WARNINGS)
		target_link_libraries(voice_crypto_test PRIVATE crypt32 ws2_32 advapi32 user32)
	endif()
endif()

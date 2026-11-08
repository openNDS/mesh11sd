cmake_minimum_required(VERSION 3.13)
project(mesh11sd C)

# Host checks are the default so a normal host configure does not require
# OpenWrt libraries. The OpenWrt package turns the daemon on and the checks off.
option(BUILD_DAEMON "Build mesh11sd-v8 and mesh11sd-v8-plan (libuci, libubus, libubox, libblobmsg_json)" OFF)
option(BUILD_HOST_CHECKS "Build the portable read-only planner and its host checks" ON)

set(MESH11SD_PACKAGE_VERSION "0.0.0_pre8-r0" CACHE STRING
	"Experimental packaging identifier. Not a release and not a Git tag.")

# .build_commit is "$Format:%h$" until git archive export-subst runs on a real
# commit. Do not replace that token with git HEAD: this working tree's HEAD is
# the historical v7 commit until an authorized v8 commit exists.
set(MESH11SD_BUILD_COMMIT "" CACHE STRING "Short commit from an export-subst archive, or uncommitted.")
if(MESH11SD_BUILD_COMMIT STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/.build_commit")
	file(STRINGS "${CMAKE_SOURCE_DIR}/.build_commit" _mesh11sd_build_commit LIMIT_COUNT 1)
	set(MESH11SD_BUILD_COMMIT "${_mesh11sd_build_commit}")
endif()
if(MESH11SD_BUILD_COMMIT STREQUAL "" OR MESH11SD_BUILD_COMMIT MATCHES "Format")
	set(MESH11SD_BUILD_COMMIT "uncommitted")
endif()
message(STATUS "mesh11sd ${MESH11SD_PACKAGE_VERSION} build_commit=${MESH11SD_BUILD_COMMIT}")

function(mesh11sd_lib_sources out_var listfile)
	file(STRINGS "${CMAKE_SOURCE_DIR}/scripts/${listfile}" _names)
	set(_paths "")
	foreach(_name ${_names})
		if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/lib/${_name}")
			message(FATAL_ERROR "missing src/lib/${_name} listed in scripts/${listfile}")
		endif()
		list(APPEND _paths "${CMAKE_SOURCE_DIR}/src/lib/${_name}")
	endforeach()
	set(${out_var} "${_paths}" PARENT_SCOPE)
endfunction()

# Older host UAPI (linux-libc-dev 6.8) has no NL80211_ATTR_WIPHY_RADIOS.
# Point this at a directory that contains linux/nl80211.h from kernel 6.12 or
# newer. It is a host compile aid only. The OpenWrt recipe does not use it.
set(MESH11SD_NL80211_INCLUDE "" CACHE PATH "Optional system include directory with linux/nl80211.h")

if(BUILD_HOST_CHECKS)
	if(CMAKE_CROSSCOMPILING)
		message(FATAL_ERROR "BUILD_HOST_CHECKS uses the host compiler. The OpenWrt recipe passes -DBUILD_HOST_CHECKS=OFF.")
	endif()
	include(CheckCSourceCompiles)
	if(MESH11SD_NL80211_INCLUDE)
		set(_mesh11sd_uapi "${MESH11SD_NL80211_INCLUDE}")
	else()
		set(_mesh11sd_uapi "system")
	endif()
	if(NOT _mesh11sd_uapi STREQUAL "${MESH11SD_NL80211_CHECKED}")
		unset(MESH11SD_HAVE_WIPHY_RADIOS CACHE)
		set(MESH11SD_NL80211_CHECKED "${_mesh11sd_uapi}" CACHE INTERNAL "")
	endif()
	set(CMAKE_REQUIRED_FLAGS "-D_DEFAULT_SOURCE")
	if(MESH11SD_NL80211_INCLUDE)
		set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -isystem ${MESH11SD_NL80211_INCLUDE}")
	endif()
	check_c_source_compiles("
#include <linux/nl80211.h>
int main(void) { return NL80211_ATTR_WIPHY_RADIOS; }
" MESH11SD_HAVE_WIPHY_RADIOS)
	if(NOT MESH11SD_HAVE_WIPHY_RADIOS)
		message(FATAL_ERROR "Host linux/nl80211.h does not provide NL80211_ATTR_WIPHY_RADIOS (linux-libc-dev 6.8 does not). Reconfigure with -DMESH11SD_NL80211_INCLUDE=<directory containing linux/nl80211.h> from kernel uapi 6.12 or newer. That overlay is not an SDK and is not shipped in this tree.")
	endif()
	mesh11sd_lib_sources(MESH11SD_CORE_SOURCES core-sources.txt)
	add_executable(mesh11sd-plan src/mesh11sd/main.c ${MESH11SD_CORE_SOURCES})
	add_executable(test-core tests/test_core.c ${MESH11SD_CORE_SOURCES})
	foreach(_target mesh11sd-plan test-core)
		target_include_directories(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/src/lib")
		if(MESH11SD_NL80211_INCLUDE)
			target_include_directories(${_target} SYSTEM BEFORE PRIVATE "${MESH11SD_NL80211_INCLUDE}")
		endif()
		target_compile_definitions(${_target} PRIVATE _DEFAULT_SOURCE)
		target_compile_features(${_target} PRIVATE c_std_11)
		set_target_properties(${_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
	endforeach()
	add_executable(test-wpad-guard tests/test_wpad_guard.c src/lib/wpad_guard.c src/lib/safeio.c src/lib/sha256.c)
	target_include_directories(test-wpad-guard PRIVATE "${CMAKE_SOURCE_DIR}/src/lib")
	target_compile_definitions(test-wpad-guard PRIVATE _DEFAULT_SOURCE)
	set_target_properties(test-wpad-guard PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
	target_compile_options(test-wpad-guard PRIVATE -Wall -Wextra -Werror)
	enable_testing()
	add_test(NAME core COMMAND test-core)
	add_test(NAME planner-cli COMMAND python3 "${CMAKE_SOURCE_DIR}/tests/test_cli.py" $<TARGET_FILE:mesh11sd-plan>)
	add_test(NAME wpad-guard COMMAND test-wpad-guard)
endif()

if(BUILD_DAEMON)
	mesh11sd_lib_sources(MESH11SD_NATIVE_SOURCES native-sources.txt)
	mesh11sd_lib_sources(MESH11SD_PEER_SOURCES peer-sources.txt)
	add_executable(mesh11sd-v8-plan src/mesh11sd/native_main.c ${MESH11SD_NATIVE_SOURCES})
	add_executable(mesh11sd-v8 src/mesh11sd/peer_main.c ${MESH11SD_NATIVE_SOURCES} ${MESH11SD_PEER_SOURCES})
	foreach(_target mesh11sd-v8-plan mesh11sd-v8)
		target_include_directories(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/src/lib")
		target_compile_definitions(${_target} PRIVATE _DEFAULT_SOURCE)
		set_target_properties(${_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
		target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror)
		target_link_options(${_target} PRIVATE -static-libgcc)
	endforeach()
	# Base OpenWrt libraries only. libm resolves airtime math; on musl it does
	# not add a separate DT_NEEDED entry beyond libc.
	target_link_libraries(mesh11sd-v8-plan PRIVATE uci blobmsg_json ubox m)
	target_link_libraries(mesh11sd-v8 PRIVATE uci blobmsg_json ubus ubox m)
	install(TARGETS mesh11sd-v8 mesh11sd-v8-plan RUNTIME DESTINATION sbin)
endif()

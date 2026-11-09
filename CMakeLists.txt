cmake_minimum_required(VERSION 3.13)
project(mesh11sd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -D_GNU_SOURCE)

set(MESH11SD_PACKAGE_VERSION "8.0.0_alpha8" CACHE STRING "Package version string")
if(NOT DEFINED MESH11SD_BUILD_COMMIT)
	set(MESH11SD_BUILD_COMMIT "")
endif()
if(MESH11SD_BUILD_COMMIT STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
	execute_process(COMMAND git rev-parse --short HEAD
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE MESH11SD_BUILD_COMMIT
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(MESH11SD_BUILD_COMMIT STREQUAL "")
	set(MESH11SD_BUILD_COMMIT "unknown")
endif()
message(STATUS "mesh11sd ${MESH11SD_PACKAGE_VERSION} commit=${MESH11SD_BUILD_COMMIT}")

set(MESH11SD_SOURCES
	src/mesh11sd/main.c
	src/mesh11sd/cli.c
	src/mesh11sd/daemon.c
	src/lib/config.c
	src/lib/uci_ops.c
	src/lib/util.c
	src/lib/log.c
	src/lib/hex.c
	src/lib/factory_mac.c
	src/lib/wireless.c
	src/lib/auto_config.c
	src/lib/portal.c
	src/lib/bridge.c
	src/lib/portal_metrics.c
	src/lib/mobility.c
	src/lib/remote_ops.c
	src/lib/json_fmt.c
	src/lib/commit_ops.c
	src/lib/led.c
	src/lib/status.c
)

add_executable(mesh11sd ${MESH11SD_SOURCES})
target_include_directories(mesh11sd PRIVATE src/lib)
target_compile_definitions(mesh11sd PRIVATE
	MESH11SD_VERSION="${MESH11SD_PACKAGE_VERSION}"
	MESH11SD_BUILD_COMMIT="${MESH11SD_BUILD_COMMIT}")
install(TARGETS mesh11sd RUNTIME DESTINATION sbin)

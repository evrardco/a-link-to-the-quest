cmake_minimum_required(VERSION 3.20)
project(monadolink LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)
if(NOT EXISTS ${EXTERNAL_DIR}/openvr/headers/openvr_driver.h)
	message(FATAL_ERROR "Submodules missing: run ./init.sh")
endif()

# SteamVR expects <driver>/bin/linux64/driver_<driver>.so next to driver.vrdrivermanifest,
# so the build tree mirrors the installed driver folder: build/monadolink/...
set(DRIVER_NAME monadolink)
set(DRIVER_OUT ${CMAKE_BINARY_DIR}/${DRIVER_NAME})

option(MONADOLINK_BUILD_TESTS "Build the unit tests (run with ctest)" ON)

# The OpenXR loader, built statically into the driver: nothing extra has to exist inside the Steam
# Linux Runtime. Vendored jsoncpp for the same reason. It finds Monado through the usual
# active_runtime.json search or XR_RUNTIME_JSON (our openxr_runtime_json setting).
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXTERNAL_DIR}/OpenXR-SDK ${CMAKE_BINARY_DIR}/OpenXR-SDK EXCLUDE_FROM_ALL)
set_target_properties(openxr_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The driver code, compiled once and shared by the SteamVR driver and the tests.
add_library(monadolink_core OBJECT
	src/driver_main.cpp
	src/device_provider.cpp
	src/hmd_device.cpp
	src/direct/direct_mode.cpp
	src/xr/xr_client.cpp
	src/xr/xr_input.cpp
	src/controller_device.cpp
	src/vk/vk_context.cpp
	src/vk/vk_copy.cpp
	src/vk/fsr.cpp
)
set_target_properties(monadolink_core PROPERTIES
	POSITION_INDEPENDENT_CODE ON
	CXX_VISIBILITY_PRESET hidden            # only HmdDriverFactory is exported
	VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(monadolink_core PUBLIC
	src
	${EXTERNAL_DIR}/openvr/headers         # header only: SteamVR provides the implementations
	${EXTERNAL_DIR}/Vulkan-Headers/include # libvulkan.so.1 itself is dlopen'ed at run time
)
# The layer compositing shader (M3), compiled to SPIR-V at build time into a C array. glslangValidator
# comes with glslang (Fedora: glslang, which build-monado-fedora.sh installs; Debian: glslang-tools).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(MONADOLINK_SHADER_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
	OUTPUT ${MONADOLINK_SHADER_DIR}/composite_comp.h
	COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn ml_composite_comp_spv
		${CMAKE_CURRENT_SOURCE_DIR}/src/vk/composite.comp -o ${MONADOLINK_SHADER_DIR}/composite_comp.h
	DEPENDS src/vk/composite.comp
	COMMENT "Compiling composite.comp"
)
# FSR 1 upscaling (fsr_render_scale): AMD's headers from the FidelityFX-FSR submodule.
set(FSR_INCLUDE_DIR ${EXTERNAL_DIR}/FidelityFX-FSR/ffx-fsr)
foreach(pass easu rcas)
	add_custom_command(
		OUTPUT ${MONADOLINK_SHADER_DIR}/fsr_${pass}_comp.h
		COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 -I${FSR_INCLUDE_DIR} --vn ml_fsr_${pass}_comp_spv
			${CMAKE_CURRENT_SOURCE_DIR}/src/vk/fsr_${pass}.comp -o ${MONADOLINK_SHADER_DIR}/fsr_${pass}_comp.h
		DEPENDS src/vk/fsr_${pass}.comp ${FSR_INCLUDE_DIR}/ffx_a.h ${FSR_INCLUDE_DIR}/ffx_fsr1.h
		COMMENT "Compiling fsr_${pass}.comp"
	)
endforeach()
add_custom_target(monadolink_shaders DEPENDS ${MONADOLINK_SHADER_DIR}/composite_comp.h
	${MONADOLINK_SHADER_DIR}/fsr_easu_comp.h ${MONADOLINK_SHADER_DIR}/fsr_rcas_comp.h)
add_dependencies(monadolink_core monadolink_shaders)
target_include_directories(monadolink_core PRIVATE ${MONADOLINK_SHADER_DIR} ${FSR_INCLUDE_DIR})

target_compile_options(monadolink_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-exceptions
	-Wno-missing-field-initializers) # the OpenXR idiom: XrFoo foo{XR_TYPE_FOO};
find_package(Threads REQUIRED)
set(MONADOLINK_TIMING_STATS_S 1 CACHE STRING
	"Seconds between frame timing statistics lines in vrserver.txt (0 = compiled out)")
target_compile_definitions(monadolink_core PUBLIC XR_USE_TIMESPEC XR_USE_GRAPHICS_API_VULKAN
	ML_TIMING_STATS_INTERVAL_S=${MONADOLINK_TIMING_STATS_S})
target_link_libraries(monadolink_core PUBLIC Threads::Threads openxr_loader ${CMAKE_DL_LIBS})

add_library(driver_${DRIVER_NAME} SHARED $<TARGET_OBJECTS:monadolink_core>)
set_target_properties(driver_${DRIVER_NAME} PROPERTIES
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY ${DRIVER_OUT}/bin/linux64
)
# Prefer linking the C++ runtime statically: SteamVR runs drivers inside the Steam Linux Runtime,
# whose libstdc++ may be older than the host compiler's. Needs the static library (Fedora:
# libstdc++-static); without it, fall back to dynamic linking, which pressure-vessel normally
# handles by using the host's newer libstdc++.
include(CheckLinkerFlag)
check_linker_flag(CXX "-static-libstdc++" MONADOLINK_HAVE_STATIC_LIBSTDCXX)
if(MONADOLINK_HAVE_STATIC_LIBSTDCXX)
	target_link_options(driver_${DRIVER_NAME} PRIVATE -static-libstdc++ -static-libgcc)
else()
	message(WARNING "Static libstdc++ not found (Fedora: sudo dnf install libstdc++-static); "
		"linking the driver against the shared libstdc++ instead.")
endif()
# --exclude-libs: keep the static loader's xr* symbols private, so they can't clash with another
# OpenXR loader in vrserver (only HmdDriverFactory is exported).
target_link_options(driver_${DRIVER_NAME} PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
target_link_libraries(driver_${DRIVER_NAME} PRIVATE Threads::Threads openxr_loader ${CMAKE_DL_LIBS})

# Inside vrserver the default OpenXR runtime is SteamVR itself, so the driver's default settings name
# Monado's manifest. Default: this project's host build of Monado (the driver runs on the host, so
# that's the right one even for a sandbox build of the driver).
get_filename_component(MONADOLINK_DEFAULT_RUNTIME_JSON
	${CMAKE_CURRENT_SOURCE_DIR}/../monado/build-fedora/openxr_monado-dev.json ABSOLUTE)
set(MONADOLINK_OPENXR_RUNTIME_JSON ${MONADOLINK_DEFAULT_RUNTIME_JSON} CACHE STRING
	"Monado's OpenXR runtime manifest, the default of the driver's openxr_runtime_json setting")
set(MONADOLINK_GENERATED_SETTINGS ${CMAKE_BINARY_DIR}/generated/default.vrsettings)
configure_file(driver/resources/settings/default.vrsettings ${MONADOLINK_GENERATED_SETTINGS} @ONLY)

# Copy the static driver files (manifest, resources) next to the binary, then the configured settings.
add_custom_target(driver_files ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/driver ${DRIVER_OUT}
	COMMAND ${CMAKE_COMMAND} -E copy ${MONADOLINK_GENERATED_SETTINGS} ${DRIVER_OUT}/resources/settings/default.vrsettings
	COMMENT "Copying driver manifest and resources"
)
add_dependencies(driver_${DRIVER_NAME} driver_files)

# The driver's OpenXR client against the active runtime, without SteamVR (checks Monado reachability).
add_executable(monadolink_xr_probe tools/xr_probe.cpp)
target_link_libraries(monadolink_xr_probe PRIVATE monadolink_core)

# GPU self-test of the frame copy (import + blit) on the real GPU, without SteamVR or Monado.
add_executable(monadolink_vk_selftest tools/vk_selftest.cpp)
target_link_libraries(monadolink_vk_selftest PRIVATE monadolink_core)

# Live controller offset tuning while SteamVR runs (an OpenVR client app, not part of the driver).
# Links the client library shipped in the openvr submodule; rpath to it, so it runs from the build tree.
add_executable(monadolink_tune tools/tune.cpp)
target_include_directories(monadolink_tune PRIVATE ${EXTERNAL_DIR}/openvr/headers)
target_link_libraries(monadolink_tune PRIVATE ${EXTERNAL_DIR}/openvr/lib/linux64/libopenvr_api.so)
set_target_properties(monadolink_tune PROPERTIES BUILD_RPATH ${EXTERNAL_DIR}/openvr/bin/linux64)

if(MONADOLINK_BUILD_TESTS)
	enable_testing()
	add_executable(monadolink_tests
		tests/test_main.cpp
		tests/test_factory.cpp
		tests/test_device_provider.cpp
		tests/test_display.cpp
		tests/test_direct_mode.cpp
		tests/test_xr_math.cpp
		tests/test_xr_client.cpp
		tests/test_stderr_capture.cpp
		tests/test_timing_stats.cpp
		tests/test_controllers.cpp
		tests/test_offset_tuner.cpp
	)
	target_include_directories(monadolink_tests PRIVATE tests ${EXTERNAL_DIR}/doctest)
	target_link_libraries(monadolink_tests PRIVATE monadolink_core ${CMAKE_DL_LIBS}) # also links its object files

	# A minimal OpenXR runtime that the real (static) loader loads through a runtime manifest, so the
	# tests exercise loader + session code without Monado.
	add_library(fake_openxr_runtime MODULE tests/fake_openxr_runtime.cpp)
	target_include_directories(fake_openxr_runtime PRIVATE ${EXTERNAL_DIR}/OpenXR-SDK/include)
	target_compile_definitions(fake_openxr_runtime PRIVATE XR_USE_TIMESPEC)
	set(FAKE_RUNTIME_JSON ${CMAKE_BINARY_DIR}/fake_openxr_runtime.json)
	file(GENERATE OUTPUT ${FAKE_RUNTIME_JSON} CONTENT
		"{\"file_format_version\": \"1.0.0\", \"runtime\": {\"name\": \"monadolink fake runtime\", \"library_path\": \"$<TARGET_FILE:fake_openxr_runtime>\"}}\n")
	add_dependencies(monadolink_tests fake_openxr_runtime)
	target_compile_definitions(monadolink_tests PRIVATE FAKE_RUNTIME_JSON="${FAKE_RUNTIME_JSON}"
		FAKE_RUNTIME_PATH="$<TARGET_FILE:fake_openxr_runtime>")
	add_test(NAME monadolink_tests COMMAND monadolink_tests)
endif()


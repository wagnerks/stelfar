# Add source to this project's executable.
cmake_minimum_required (VERSION 3.10)

file(GLOB_RECURSE SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

set(EXCLUDE_DIR_1 ${CMAKE_CURRENT_SOURCE_DIR}/submodules/JoltPhysics ${CMAKE_CURRENT_SOURCE_DIR}/submodules/ecss ${CMAKE_CURRENT_SOURCE_DIR}/submodules/meshoptimizer ${CMAKE_CURRENT_SOURCE_DIR}/submodules/miniaudio ${CMAKE_CURRENT_SOURCE_DIR}/submodules/enet)
# file(GLOB_RECURSE SUBMODULES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/submodules/*")
# list(REMOVE_ITEM SRC ${SUBMODULES_FILES})
# file(GLOB_RECURSE SUBMODULES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/submodules/ecss/*")
# list(REMOVE_ITEM SRC ${SUBMODULES_FILES})

foreach(EXCLUDE_DIR ${EXCLUDE_DIR_1})
   	file(GLOB_RECURSE EXCLUDE_FILES ${EXCLUDE_DIR}/*)
    list(REMOVE_ITEM SRC ${EXCLUDE_FILES})
endforeach()

list(REMOVE_ITEM SRC
	"${CMAKE_CURRENT_SOURCE_DIR}/core/main.cpp"
)
list(APPEND SRC "${CMAKE_CURRENT_SOURCE_DIR}/assetsModule/modelModule/mikktspace.c")

if (EDITOR)
	message("-- engine with editor")
else()
	message("-- engine without editor")
endif()

message("-- engine as library")
add_library(${ENGINE_NAME} STATIC ${SRC})
# Extends the RHI value types with the engine's math types; see rhi/RhiTypes.h.
target_compile_definitions(${ENGINE_NAME} PUBLIC SFE_RHI_USER_CONFIG="core/RhiConfig.h")
target_link_libraries(${ENGINE_NAME} PUBLIC sfe_rhi)

if(EDITOR)
	target_compile_definitions(${ENGINE_NAME} PRIVATE EDITOR_ENABLED)
endif()

# The engine is a library. Whoever builds a game with it brings its own executable, links this and calls its
# registration before Engine::init; see core/GameModule.h.
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRC})

add_subdirectory(submodules/ecss)
target_link_libraries(${ENGINE_NAME} PUBLIC ecss)

# Mesh simplification for the LOD chain the loader builds.
add_subdirectory(submodules/meshoptimizer)
target_link_libraries(${ENGINE_NAME} PRIVATE meshoptimizer)

# Audio. miniaudio is one C file; its own CMakeLists also looks for libvorbis/libopus and builds extra nodes, of
# which the engine takes only the reverb, for AudioSystem's echo of the wood.
add_library(miniaudio STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/submodules/miniaudio/miniaudio.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/submodules/miniaudio/extras/nodes/ma_reverb_node/ma_reverb_node.c"
)
target_include_directories(miniaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/submodules/miniaudio")
target_link_libraries(${ENGINE_NAME} PRIVATE miniaudio)

# Block compression of the textures read from files (TextureHandler): bc7enc's BC7 and rgbcx's BC4, two files of
# bc7enc_rdo. rgbcx takes its smaller table: BC4 does not use it at all.
add_library(bc7enc STATIC
	"${ENGINE_PATH}/lib/bc7enc/bc7enc.cpp"
	"${ENGINE_PATH}/lib/bc7enc/rgbcx.cpp"
)
target_compile_definitions(bc7enc PUBLIC RGBCX_USE_SMALLER_TABLES=1)
target_include_directories(bc7enc PUBLIC "${ENGINE_PATH}/lib/bc7enc")
target_link_libraries(${ENGINE_NAME} PRIVATE bc7enc)

# Online play over UDP (networkModule/EnetTransport). ENet is a handful of C files; its own CMakeLists builds them the
# same way, but also sets up install rules and a shared variant the engine has no use for.
file(GLOB ENET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/submodules/enet/*.c")
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/submodules/enet/include")
if (WIN32)
	target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()
target_link_libraries(${ENGINE_NAME} PRIVATE enet)

# ecss is header-only and templated, so every translation unit that opens a view has to
# parse the whole thing, and only a fifth of that is ecss's own code -- the rest is the
# <vector>/<atomic>/<algorithm>/<memory> closure underneath it. Thirty-one files here
# include it, most of them transitively through core/ECSHandler.h.
#
# Worth 32% of compiler front-end time over a six-file sample, and not a net loss on any
# single file in it, including files that never name ecss themselves. Re-check with
# scripts/measure-frontend.ps1 -- and note why that script exists rather than a stopwatch
# on a full build: see the warning in the top-level CMakeLists.txt.
#
# Angle brackets so it is looked up on the include path; a bare name would be taken as a
# path relative to this directory. mikktspace.c opts out rather than the header being
# wrapped in a COMPILE_LANGUAGE genex, because the '>' of <ecss/Registry.h> would close the
# genex early -- and a C compile cannot consume a C++ precompiled header either way.
target_precompile_headers(${ENGINE_NAME} PRIVATE <ecss/Registry.h>)
set_source_files_properties(
	"${CMAKE_CURRENT_SOURCE_DIR}/assetsModule/modelModule/mikktspace.c"
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_include_directories(${ENGINE_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/submodules/myPhysicsEngine")
target_include_directories(${ENGINE_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/submodules/JoltPhysics")
target_include_directories(${ENGINE_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/lib/jsoncpp/include")
target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/lib/stb")

target_link_libraries(${ENGINE_NAME} LINK_PUBLIC Jolt)

find_package(OpenGL REQUIRED)

target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/lib/glfw/include")
target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/lib/assimp/include")
target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/lib/freetype/include")



target_link_libraries(${ENGINE_NAME}
	PUBLIC
		glad
		imgui
		OpenGL::GL
	)

# Shared third-party DLLs: IMPORTED location is the source of truth.
# CMake stages them into out/<Config>/ThirdParty; Windows has no rpath.
set(SFE_NATIVE_SHARED_LIBS "")
if(MSVC)
	add_library(sfe_assimp SHARED IMPORTED GLOBAL)
	set_target_properties(sfe_assimp PROPERTIES
		IMPORTED_IMPLIB "${ENGINE_PATH}/lib/assimp/Release/assimp-vc143-mt.lib"
		IMPORTED_LOCATION "${ENGINE_PATH}/lib/assimp/Release/assimp-vc143-mt.dll"
	)
	add_library(sfe_jsoncpp SHARED IMPORTED GLOBAL)
	set_target_properties(sfe_jsoncpp PROPERTIES
		IMPORTED_IMPLIB "${ENGINE_PATH}/lib/jsoncpp/lib/release/jsoncpp.lib"
		IMPORTED_LOCATION "${ENGINE_PATH}/lib/jsoncpp/lib/release/jsoncpp.dll"
	)
	# Steam Audio (Apache 2.0): the HRTF and the air absorption AudioSystem hears world sounds through. Delay-loaded, so
	# a run without an audio device (the None backend, the tests) never loads it.
	add_library(sfe_phonon SHARED IMPORTED GLOBAL)
	set_target_properties(sfe_phonon PROPERTIES
		IMPORTED_IMPLIB "${ENGINE_PATH}/lib/steamaudio/lib/windows-x64/phonon.lib"
		IMPORTED_LOCATION "${ENGINE_PATH}/lib/steamaudio/lib/windows-x64/phonon.dll"
	)
	target_include_directories(${ENGINE_NAME} PRIVATE "${ENGINE_PATH}/lib/steamaudio/include")
	set(SFE_NATIVE_SHARED_LIBS sfe_assimp sfe_jsoncpp sfe_phonon)
	if(VLD)
		add_library(sfe_vld SHARED IMPORTED GLOBAL)
		set_target_properties(sfe_vld PROPERTIES
			IMPORTED_IMPLIB "${ENGINE_PATH}/utils/VisualLeakDetector/lib/Win64/vld.lib"
			IMPORTED_LOCATION "${ENGINE_PATH}/utils/VisualLeakDetector/bin/Win64/vld_x64.dll"
		)
		list(APPEND SFE_NATIVE_SHARED_LIBS sfe_vld)
		target_include_directories(${ENGINE_NAME} PUBLIC "${ENGINE_PATH}/utils/VisualLeakDetector/include")
		target_compile_definitions(${ENGINE_NAME} PUBLIC VLD=true)
		target_link_libraries(${ENGINE_NAME} PUBLIC sfe_vld)
		message(STATUS "visual leak detector: ON")
	endif()
	set(SFE_NATIVE_SHARED_LIBS "${SFE_NATIVE_SHARED_LIBS}" CACHE INTERNAL "Imported native shared libs")
endif()

macro(sfe_stage_app tgt)
	set_target_properties(${tgt} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${SFE_STAGE_ROOT}/${tgt}/$<CONFIG>"
		RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SFE_STAGE_ROOT}/${tgt}/Debug"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SFE_STAGE_ROOT}/${tgt}/Release"
		RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SFE_STAGE_ROOT}/${tgt}/RelWithDebInfo"
		RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SFE_STAGE_ROOT}/${tgt}/MinSizeRel"
	)
	if(MSVC AND SFE_NATIVE_SHARED_LIBS)
		foreach(_lib ${SFE_NATIVE_SHARED_LIBS})
			target_link_options(${tgt} PRIVATE "/DELAYLOAD:$<TARGET_FILE_NAME:${_lib}>")
		endforeach()
		target_link_libraries(${tgt} PRIVATE delayimp)
		set(_dlls)
		foreach(_lib ${SFE_NATIVE_SHARED_LIBS})
			list(APPEND _dlls "$<TARGET_FILE:${_lib}>")
		endforeach()
		add_custom_command(TARGET ${tgt} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${tgt}>/ThirdParty"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:${tgt}>/ThirdParty/"
			COMMENT "Stage ThirdParty DLLs for ${tgt}"
		)
		if(TARGET sfe_vld)
			add_custom_command(TARGET ${tgt} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${ENGINE_PATH}/utils/VisualLeakDetector/bin/Win64/dbghelp.dll"
					"${ENGINE_PATH}/utils/VisualLeakDetector/bin/Win64/Microsoft.DTfW.DHL.manifest"
					"$<TARGET_FILE_DIR:${tgt}>/ThirdParty/"
			)
		endif()
	endif()
endmacro()
# A game's executable calls this itself: sfe_stage_app(MyGame) puts the runtime libraries beside it.

if(MSVC)
	# Add PDH for SystemLoadHelper + Windows version/UNICODE defines
	target_compile_definitions(${ENGINE_NAME} PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)

	target_link_libraries(${ENGINE_NAME}
	PUBLIC
		"${ENGINE_PATH}/lib/glfw/glfw3.lib"
		ws2_32
		pdh
		"${ENGINE_PATH}/lib/freetype/lib/win/release/freetype.lib"
		sfe_jsoncpp
		sfe_assimp
		sfe_phonon
		MSVCRT
	)

	set(CMAKE_EXE_LINKER_FLAGS "/NODEFAULTLIB:MSVCRT")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
	set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
elseif(XCODE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
	add_definitions(-std=c++20)

	target_link_libraries(${ENGINE_NAME}
	PUBLIC
		${ENGINE_PATH}/lib/glfw/lib-arm64/libglfw.3.dylib
		${ENGINE_PATH}/lib/glfw/lib-arm64/libglfw3.a
		pthread
	debug
		${ENGINE_PATH}/lib/jsoncpp/lib/macos/debug/libjsoncpp.dylib
		optimized
		${ENGINE_PATH}/lib/jsoncpp/lib/macos/release/libjsoncpp.dylib
		debug
		${ENGINE_PATH}/lib/assimp/macos/debug/libassimpd.dylib
		optimized
		${ENGINE_PATH}/lib/assimp/macos/release/libassimp.dylib
	)
endif()


# RHI conformance suite. Off by default: it fetches googletest and needs a GPU, neither of which
# an ordinary build of the engine should pay for. Same shape as the ecss suite next door.
option(SFE_BUILD_RHI_TESTS "Build the RHI conformance tests" OFF)
# Engine unit tests: the engine on the None backend, no window and no GPU, with a fixed frame step.
option(SFE_BUILD_ENGINE_TESTS "Build the engine unit tests (no rendering)" OFF)

if(SFE_BUILD_RHI_TESTS OR SFE_BUILD_ENGINE_TESTS)
	include(CTest)
	include(FetchContent)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
		UPDATE_DISCONNECTED TRUE
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
	set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
	set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(googletest)
endif()

if(SFE_BUILD_RHI_TESTS)
	file(GLOB RHI_TEST_SOURCES CONFIGURE_DEPENDS "${ENGINE_PATH}/tests/*.cpp")
	add_executable(rhi_tests ${RHI_TEST_SOURCES})
	# Links the RHI and nothing else of the engine. The tests borrow only the header-only math types, through
	# the same RhiTypes extension the engine uses, for their push-constant structs.
	# bc7enc: the compressed levels are encoded as TextureHandler encodes them.
	target_link_libraries(rhi_tests PRIVATE sfe_rhi gtest bc7enc)
	target_include_directories(rhi_tests PRIVATE
		"${ENGINE_PATH}/tests"
		"${CMAKE_CURRENT_SOURCE_DIR}"
	)
	target_compile_definitions(rhi_tests PRIVATE SFE_RHI_USER_CONFIG="core/RhiConfig.h")
	set_target_properties(rhi_tests PROPERTIES CXX_STANDARD 23)
	# A console subsystem, unlike the editor: the suite reports through stdout.
	if(MSVC)
		set_target_properties(rhi_tests PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
		# The project links with /NODEFAULTLIB:MSVCRT and names the CRT explicitly; the engine target does
		# that for its consumers, and the suite no longer links the engine.
		target_link_libraries(rhi_tests PRIVATE MSVCRT)
	endif()
	set_target_properties(rhi_tests PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${SFE_STAGE_ROOT}/rhi_tests/$<CONFIG>"
		RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SFE_STAGE_ROOT}/rhi_tests/RelWithDebInfo"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SFE_STAGE_ROOT}/rhi_tests/Release"
		RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SFE_STAGE_ROOT}/rhi_tests/Debug"
	)
	# The tests load their shaders through relative paths, so they only run from the asset root.
	set_target_properties(rhi_tests PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_PATH}/bin")
	add_test(NAME rhi_tests COMMAND rhi_tests WORKING_DIRECTORY "${ENGINE_PATH}/bin")
endif()

if(SFE_BUILD_ENGINE_TESTS)
	file(GLOB ENGINE_TEST_SOURCES CONFIGURE_DEPENDS "${ENGINE_PATH}/tests/engine/*.cpp")
	add_executable(engine_tests ${ENGINE_TEST_SOURCES})
	target_link_libraries(engine_tests PRIVATE ${ENGINE_NAME} gtest)
	target_include_directories(engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${ENGINE_PATH}/tests/engine")
	set_target_properties(engine_tests PROPERTIES
		FOLDER Engine
		CXX_STANDARD 23
		VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_PATH}/bin"
	)
	if(MSVC)
		set_target_properties(engine_tests PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
	endif()
	# Links the whole engine, so it needs the native DLLs next to it like the apps.
	sfe_stage_app(engine_tests)
	add_test(NAME engine_tests COMMAND engine_tests WORKING_DIRECTORY "${ENGINE_PATH}/bin")
endif()

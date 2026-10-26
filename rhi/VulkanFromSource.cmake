# Vulkan and shaderc when no Vulkan SDK is installed: the SDK's own pieces built from source, at the versions of one SDK
# release, standing in as the targets FindVulkan makes from an SDK -- Vulkan::Vulkan and Vulkan::shaderc_combined -- so
# nothing past this file tells the two apart.
#
# The loader is built for its import library; at run time the driver's own vulkan-1.dll answers, as with the SDK.
# shaderc has no SDK tags: v2026.4 pins the same glslang and SPIRV-Tools as SDK 1.4.363.0, and adds them and
# SPIRV-Headers itself from where it is pointed. Only what the engine links is built (EXCLUDE_FROM_ALL), once per build
# directory; glslang and SPIRV-Tools make that first build noticeably longer. Their builds generate sources with
# Python 3, which has to be installed.

if(CMAKE_VERSION VERSION_LESS 3.28)
	message(FATAL_ERROR "No Vulkan SDK found. Install it, or configure with CMake 3.28 or newer, which builds Vulkan and "
		"shaderc from source instead.")
endif()

block()
	# Each dependency downloaded at configure time directly, not through a sub-build of its own.
	if(POLICY CMP0168)
		cmake_policy(SET CMP0168 NEW)
	endif()

	set(sdk_tag vulkan-sdk-1.4.363.0)
	message(STATUS "No Vulkan SDK: building the Vulkan loader and shaderc from source (${sdk_tag}, shaderc v2026.4)")

	include(FetchContent)
	FetchContent_Declare(VulkanHeaders
		URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${sdk_tag}.tar.gz
		URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SYSTEM EXCLUDE_FROM_ALL
	)
	FetchContent_Declare(VulkanLoader
		URL https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/${sdk_tag}.tar.gz
		URL_HASH SHA256=941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SYSTEM EXCLUDE_FROM_ALL
	)
	# Downloaded only: shaderc adds them as its third_party projects. Named apart from their projects, whose own
	# <project>_BINARY_DIR a populated-only dependency's variables would shadow.
	FetchContent_Declare(shaderc_spirv_headers
		URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/${sdk_tag}.tar.gz
		URL_HASH SHA256=5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SOURCE_SUBDIR not-added
	)
	FetchContent_Declare(shaderc_spirv_tools
		URL https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/${sdk_tag}.tar.gz
		URL_HASH SHA256=e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SOURCE_SUBDIR not-added
	)
	FetchContent_Declare(shaderc_glslang
		URL https://github.com/KhronosGroup/glslang/archive/refs/tags/${sdk_tag}.tar.gz
		URL_HASH SHA256=cda24ec765e6d2845d106b7caa593f34127195a88524cf87821ff0088fb6edec
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SOURCE_SUBDIR not-added
	)
	FetchContent_Declare(shaderc
		URL https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz
		URL_HASH SHA256=f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SYSTEM EXCLUDE_FROM_ALL
	)

	# The libraries alone, on the engine's dynamic CRT, GLSL only (the engine compiles no HLSL), and no warning taken
	# for an error: a newer compiler than theirs must not stop the build.
	set(SHADERC_SKIP_TESTS ON)
	set(SHADERC_SKIP_EXAMPLES ON)
	set(SHADERC_SKIP_EXECUTABLES ON)
	set(SHADERC_SKIP_COPYRIGHT_CHECK ON)
	set(SHADERC_ENABLE_SHARED_CRT ON)
	set(SHADERC_ENABLE_HLSL OFF)
	set(SHADERC_ENABLE_WERROR_COMPILE OFF)
	set(SPIRV_SKIP_EXECUTABLES ON)
	set(SPIRV_WERROR OFF)
	set(ENABLE_GLSLANG_BINARIES OFF)

	FetchContent_MakeAvailable(VulkanHeaders VulkanLoader shaderc_spirv_headers shaderc_spirv_tools shaderc_glslang)
	set(SHADERC_SPIRV_HEADERS_DIR "${shaderc_spirv_headers_SOURCE_DIR}")
	set(SHADERC_SPIRV_TOOLS_DIR "${shaderc_spirv_tools_SOURCE_DIR}")
	set(SHADERC_GLSLANG_DIR "${shaderc_glslang_SOURCE_DIR}")
	FetchContent_MakeAvailable(shaderc)

	add_library(Vulkan::Vulkan INTERFACE IMPORTED)
	target_link_libraries(Vulkan::Vulkan INTERFACE Vulkan::Headers Vulkan::Loader)
	add_library(Vulkan::shaderc_combined INTERFACE IMPORTED)
	target_link_libraries(Vulkan::shaderc_combined INTERFACE shaderc)
endblock()

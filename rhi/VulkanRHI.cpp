#include <cstdlib>
#include <string_view>
#include "rhi/VulkanRHI.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "rhi/RhiLog.h"
#include "rhi/SpirvCompiler.h"

namespace SFE::Render {
	namespace {
		constexpr VkClearColorValue kClearColor{ { 0.05f, 0.08f, 0.22f, 1.f } };

		struct QueueFamilyIndices {
			uint32_t graphics = UINT32_MAX;
			uint32_t present = UINT32_MAX;

			bool complete() const {
				return graphics != UINT32_MAX && present != UINT32_MAX;
			}
		};

		struct SwapchainSupport {
			VkSurfaceCapabilitiesKHR capabilities{};
			std::vector<VkSurfaceFormatKHR> formats;
			std::vector<VkPresentModeKHR> presentModes;
		};

		VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT,
			const VkDebugUtilsMessengerCallbackDataEXT* data,
			void*)
		{
			if (!data || !data->pMessage) {
				return VK_FALSE;
			}
			static int logged = 0;
			if (logged >= 24) {
				return VK_FALSE;
			}
			++logged;
			if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
				RhiLog::fatal("Vulkan validation: %s", data->pMessage);
			}
			else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
				RhiLog::error("Vulkan validation: %s", data->pMessage);
			}
			return VK_FALSE;
		}

		VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerInfo() {
			VkDebugUtilsMessengerCreateInfoEXT info{};
			info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			info.pfnUserCallback = debugMessengerCallback;
			return info;
		}

		bool hasInstanceExtension(const char* name) {
			uint32_t count = 0;
			vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> exts(count);
			vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
			for (const auto& ext : exts) {
				if (std::strcmp(ext.extensionName, name) == 0) {
					return true;
				}
			}
			return false;
		}

		bool hasInstanceLayer(const char* name) {
			uint32_t count = 0;
			vkEnumerateInstanceLayerProperties(&count, nullptr);
			std::vector<VkLayerProperties> layers(count);
			vkEnumerateInstanceLayerProperties(&count, layers.data());
			for (const auto& layer : layers) {
				if (std::strcmp(layer.layerName, name) == 0) {
					return true;
				}
			}
			return false;
		}

		QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
			QueueFamilyIndices indices;
			uint32_t count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
			std::vector<VkQueueFamilyProperties> families(count);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

			for (uint32_t i = 0; i < count; ++i) {
				if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
					indices.graphics = i;
				}

				VkBool32 presentSupport = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
				if (presentSupport) {
					indices.present = i;
				}

				if (indices.complete()) {
					break;
				}
			}
			return indices;
		}

		SwapchainSupport querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
			SwapchainSupport details;
			vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
			uint32_t formatCount = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
			if (formatCount > 0) {
				details.formats.resize(formatCount);
				vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
			}
			uint32_t presentCount = 0;
			vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, nullptr);
			if (presentCount > 0) {
				details.presentModes.resize(presentCount);
				vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentCount, details.presentModes.data());
			}
			return details;
		}

		bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
			uint32_t count = 0;
			vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> available(count);
			vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
			for (const char* required : { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME }) {
				bool found = false;
				for (const auto& ext : available) {
					if (std::strcmp(ext.extensionName, required) == 0) {
						found = true;
						break;
					}
				}
				if (!found) {
					return false;
				}
			}
			return true;
		}

		bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
			const auto indices = findQueueFamilies(device, surface);
			if (!indices.complete() || !checkDeviceExtensionSupport(device)) {
				return false;
			}
			const auto support = querySwapchainSupport(device, surface);
			return !support.formats.empty() && !support.presentModes.empty();
		}

		VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
			// Match OpenGL default FB (GLFW_SRGB_CAPABLE is off): display-referred UNORM, no extra sRGB encode on present.
			for (const auto& format : formats) {
				if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
					return format;
				}
			}
			for (const auto& format : formats) {
				if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
					return format;
				}
			}
			for (const auto& format : formats) {
				if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
					return format;
				}
			}
			return formats.front();
		}

		VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
			for (const auto mode : modes) {
				if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
					return mode;
				}
			}
			for (const auto mode : modes) {
				if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
					return mode;
				}
			}
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window) {
			if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
				return capabilities.currentExtent;
			}
			int width = 0;
			int height = 0;
			glfwGetFramebufferSize(window, &width, &height);
			VkExtent2D extent{
				static_cast<uint32_t>(std::max(1, width)),
				static_cast<uint32_t>(std::max(1, height))
			};
			extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
			return extent;
		}
	}

	VulkanRHI::VulkanRHI() = default;

	VulkanRHI::~VulkanRHI() {
		cleanup();
	}

	void VulkanRHI::init() {
		if (m_inited) {
			return;
		}

		if (!hostSurface().window) {
			RhiLog::fatal("VulkanRHI::init: window is not created");
			return;
		}

		createInstance();
		createSurface();
		pickPhysicalDevice();
		createDevice();
		createAllocator();
		createDescriptorPool();
		createPipelineCache();

		VkCommandPoolCreateInfo uploadPoolInfo{};
		uploadPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		uploadPoolInfo.queueFamilyIndex = m_graphicsQueueFamily;
		if (vkCreateCommandPool(m_device, &uploadPoolInfo, nullptr, &m_uploadCommandPool) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create upload command pool");
		}

		createSwapchain();
		createImageViews();
		createSwapchainRenderPass();
		createFramebuffers();
		createCommandPool();
		createSceneCommandPool();
		createSyncObjects();
		createSwapchainSyncObjects();
		createCommandBuffers();
		createBlitPipeline();
		createUploadRings();
		createTimestampQueries();

		ensureBackbufferHandle();
		m_inited = true;
		RhiLog::info("VulkanRHI initialized (full pipeline)");
	}

	void VulkanRHI::waitIdle() {
		if (m_device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(m_device);
		}
	}

	void VulkanRHI::cleanup() {
		if (!m_inited && m_instance == VK_NULL_HANDLE) {
			return;
		}

		if (m_device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(m_device);
		}

		savePipelineCache();

		for (auto& buffer : m_buffers) {
			if (buffer.live) {
				destroyBufferInternal(buffer);
			}
		}
		m_buffers.clear();
		m_freeBuffers.clear();
		// The device is idle: nothing binds them any more.
		for (const auto& retired : m_retiredBuffers) {
			vmaDestroyBuffer(m_allocator, retired.buffer, retired.allocation);
		}
		for (auto& retired : m_retiredTextures) {
			destroyTextureInternal(retired.texture);
		}
		m_retiredTextures.clear();
		m_retiredBuffers.clear();
		m_transientBuffer = {};
		m_transientRegion = 0;
		for (auto& overflow : m_transientOverflow) {
			overflow.clear();
		}

		for (auto& texture : m_textures) {
			if (texture.live) {
				destroyTextureInternal(texture);
			}
		}
		m_textures.clear();
		m_freeTextures.clear();

		for (auto& sampler : m_samplers) {
			if (sampler.live && sampler.sampler != VK_NULL_HANDLE) {
				vkDestroySampler(m_device, sampler.sampler, nullptr);
			}
		}
		m_samplers.clear();
		m_freeSamplers.clear();

		for (auto& layout : m_descriptorSetLayouts) {
			if (layout.live && layout.layout != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(m_device, layout.layout, nullptr);
			}
		}
		m_descriptorSetLayouts.clear();
		m_freeDescriptorSetLayouts.clear();
		m_descriptorSets.clear();
		m_freeDescriptorSets.clear();
		m_commandLists.clear();
		m_freeCommandLists.clear();

		for (auto& shader : m_shaders) {
			if (shader.live && shader.module != VK_NULL_HANDLE) {
				vkDestroyShaderModule(m_device, shader.module, nullptr);
			}
		}
		m_shaders.clear();
		m_freeShaders.clear();
		m_shaderByName.clear();

		for (auto& pipeline : m_pipelines) {
			if (pipeline.live) {
				if (pipeline.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, pipeline.pipeline, nullptr);
				if (pipeline.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, pipeline.layout, nullptr);
			}
		}
		m_pipelines.clear();
		m_freePipelines.clear();

		for (auto& pass : m_renderPasses) {
			if (pass.live && pass.renderPass != VK_NULL_HANDLE) {
				vkDestroyRenderPass(m_device, pass.renderPass, nullptr);
			}
		}
		m_renderPasses.clear();
		m_freeRenderPasses.clear();

		for (auto& fb : m_framebuffers) {
			if (!fb.live) {
				continue;
			}
			if (fb.framebuffer != VK_NULL_HANDLE) {
				vkDestroyFramebuffer(m_device, fb.framebuffer, nullptr);
			}
			// destroyFramebuffer frees these; this sweep used not to, so every framebuffer still alive
			// at shutdown leaked one image view per pinned attachment layer and vkDestroyDevice named
			// each of them.
			for (VkImageView view : fb.ownedViews) {
				vkDestroyImageView(m_device, view, nullptr);
			}
			fb.ownedViews.clear();
		}
		m_framebuffers.clear();
		m_freeFramebuffers.clear();

		destroyBlitPipeline();
		destroyPipelineCache();
		destroyUploadRings();
		destroyTimestampQueries();

		if (m_sceneFence != VK_NULL_HANDLE) {
			vkDestroyFence(m_device, m_sceneFence, nullptr);
			m_sceneFence = VK_NULL_HANDLE;
		}
		if (m_uploadFence != VK_NULL_HANDLE) {
			vkDestroyFence(m_device, m_uploadFence, nullptr);
			m_uploadFence = VK_NULL_HANDLE;
		}
		if (m_sceneCommandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_device, m_sceneCommandPool, nullptr);
			m_sceneCommandPool = VK_NULL_HANDLE;
		}
		m_sceneCmds[0] = VK_NULL_HANDLE;
		m_sceneCmds[1] = VK_NULL_HANDLE;

		destroyDescriptorPool();
		// Freed with their pools.
		for (auto& retired : m_retiredSets) {
			retired.clear();
		}

		if (m_uploadCommandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_device, m_uploadCommandPool, nullptr);
			m_uploadCommandPool = VK_NULL_HANDLE;
		}

		destroySwapchainSyncObjects();
		cleanupSwapchain();

		for (size_t i = 0; i < m_inFlightFences.size(); ++i) {
			if (m_inFlightFences[i] != VK_NULL_HANDLE) {
				vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
			}
			if (i < m_imageAvailableSemaphores.size() && m_imageAvailableSemaphores[i] != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
			}
		}
		m_inFlightFences.clear();
		m_imageAvailableSemaphores.clear();

		if (m_commandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_device, m_commandPool, nullptr);
			m_commandPool = VK_NULL_HANDLE;
		}

		destroyAllocator();

		if (m_device != VK_NULL_HANDLE) {
			vkDestroyDevice(m_device, nullptr);
			m_device = VK_NULL_HANDLE;
		}

		if (m_surface != VK_NULL_HANDLE) {
			vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
			m_surface = VK_NULL_HANDLE;
		}

		if (m_debugMessenger != VK_NULL_HANDLE) {
			if (const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
					vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"))) {
				destroy(m_instance, m_debugMessenger, nullptr);
			}
			m_debugMessenger = VK_NULL_HANDLE;
		}

		if (m_instance != VK_NULL_HANDLE) {
			vkDestroyInstance(m_instance, nullptr);
			m_instance = VK_NULL_HANDLE;
		}

		m_physicalDevice = VK_NULL_HANDLE;
		m_inited = false;
	}

	void VulkanRHI::createInstance() {
		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "StelForgeRuntime";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "StelForgeEngine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_2;

		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
		const bool debugUtils = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		bool alreadyHasDebugUtils = false;
		for (const char* ext : extensions) {
			if (std::strcmp(ext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
				alreadyHasDebugUtils = true;
				break;
			}
		}
		if (debugUtils && !alreadyHasDebugUtils) {
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		// The validation layer only when asked for (STELFORGE_VALIDATION=1): on wherever the SDK is installed, it made the
		// main thread several times slower.
		const bool useValidation = debugUtils && std::getenv("STELFORGE_VALIDATION") && hasInstanceLayer("VK_LAYER_KHRONOS_validation");
		const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
		auto debugInfo = makeDebugMessengerInfo();

		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();
		if (useValidation) {
			createInfo.enabledLayerCount = 1;
			createInfo.ppEnabledLayerNames = layers;
			createInfo.pNext = &debugInfo;
		}

		if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateInstance failed");
		}
		if (useValidation) {
			setupDebugMessenger();
		}
	}

	void VulkanRHI::setupDebugMessenger() {
		const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
		if (!create) {
			return;
		}
		auto info = makeDebugMessengerInfo();
		if (create(m_instance, &info, nullptr, &m_debugMessenger) != VK_SUCCESS) {
			m_debugMessenger = VK_NULL_HANDLE;
			RhiLog::error("VulkanRHI: debug messenger create failed");
		}
	}

	void VulkanRHI::createSurface() {
		auto* glfwWindow = hostSurface().window;
		if (glfwCreateWindowSurface(m_instance, glfwWindow, nullptr, &m_surface) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: glfwCreateWindowSurface failed");
		}
	}

	void VulkanRHI::pickPhysicalDevice() {
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
		if (deviceCount == 0) {
			RhiLog::fatal("VulkanRHI: no Vulkan-capable GPU found");
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

		// Prefer a discrete GPU. Enumeration lists the integrated one first on hybrid laptops, which ran Vulkan
		// on the iGPU while OpenGL, steered by NvOptimusEnablement, ran on the discrete card. SFE_GPU=integrated
		// prefers the integrated one instead, to see how the game runs on a machine without a discrete card.
		const char* wanted = std::getenv("SFE_GPU");
		const bool preferIntegrated = wanted && std::string_view(wanted) == "integrated";
		int bestScore = -1;
		for (const auto device : devices) {
			if (!isDeviceSuitable(device, m_surface)) {
				continue;
			}
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(device, &props);
			const int discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
			const int integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
			const int score = preferIntegrated ? integrated * 2 + discrete : discrete * 2 + integrated;
			if (score > bestScore) {
				bestScore = score;
				m_physicalDevice = device;
			}
		}

		if (m_physicalDevice == VK_NULL_HANDLE) {
			RhiLog::fatal("VulkanRHI: no suitable Vulkan GPU found");
		}

		VkPhysicalDeviceProperties picked{};
		vkGetPhysicalDeviceProperties(m_physicalDevice, &picked);
		m_deviceName = picked.deviceName;
	}

	void VulkanRHI::createDevice() {
		const auto indices = findQueueFamilies(m_physicalDevice, m_surface);
		m_graphicsQueueFamily = indices.graphics;
		m_presentQueueFamily = indices.present;

		std::set<uint32_t> uniqueFamilies{ indices.graphics, indices.present };
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		const float queuePriority = 1.f;
		for (const uint32_t family : uniqueFamilies) {
			VkDeviceQueueCreateInfo queueInfo{};
			queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex = family;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueInfo);
		}

		VkPhysicalDeviceFeatures supportedFeatures{};
		vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);

		VkPhysicalDeviceFeatures deviceFeatures{};
		if (supportedFeatures.samplerAnisotropy) {
			deviceFeatures.samplerAnisotropy = VK_TRUE;
			m_samplerAnisotropy = true;
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
			// A sampler asking for more than the device allows is invalid, so a caller can ask for 16 without
			// knowing what it runs on. OpenGL clamps to GL_MAX_TEXTURE_MAX_ANISOTROPY the same way.
			m_maxAnisotropy = props.limits.maxSamplerAnisotropy;
		}
		if (supportedFeatures.geometryShader) {
			deviceFeatures.geometryShader = VK_TRUE;
		}
		if (supportedFeatures.tessellationShader) {
			deviceFeatures.tessellationShader = VK_TRUE;
		}
		if (supportedFeatures.fillModeNonSolid) {
			deviceFeatures.fillModeNonSolid = VK_TRUE;
			m_fillModeNonSolid = true;
		}
		if (supportedFeatures.multiDrawIndirect) {
			deviceFeatures.multiDrawIndirect = VK_TRUE;
		}
		// A non-zero firstInstance inside an indirect record needs this; the direct vkCmdDraw path
		// does not. Without it the drivers that honour it anyway do so as undefined behaviour, and
		// the ones that do not draw the wrong instances with no diagnostic outside validation.
		if (supportedFeatures.drawIndirectFirstInstance) {
			deviceFeatures.drawIndirectFirstInstance = VK_TRUE;
		}
		// A fragment shader that writes a storage buffer: bloom_extract.fs telling the next frames it saw a glare.
		if (supportedFeatures.fragmentStoresAndAtomics) {
			deviceFeatures.fragmentStoresAndAtomics = VK_TRUE;
		}

		VkPhysicalDeviceDepthClipControlFeaturesEXT depthClipControl{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT
		};
		// The 1.2 aggregate rather than the individual promoted structs: hostQueryReset lives in it,
		// and so does samplerMirrorClampToEdge, which AddressMode::MirrorClampToEdge requires and which
		// was previously used without being enabled. Chaining both the aggregate and a struct it
		// promotes is illegal, so it has to be one or the other.
		VkPhysicalDeviceVulkan12Features supported12{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
		};
		supported12.pNext = &depthClipControl;
		VkPhysicalDeviceVulkan11Features supported11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
		supported11.pNext = &supported12;
		VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features2.pNext = &supported11;
		vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

		if (!depthClipControl.depthClipControl) {
			RhiLog::fatal("VulkanRHI: VK_EXT_depth_clip_control required so shaders keep OpenGL clip Z [-1, 1]");
		}
		// GPU-driven rendering: compute writes the draw count, shaders read gl_DrawID / gl_BaseInstance.
		// Glass picks its albedo out of a sampler array by an index that is constant within each draw.
		if (!supported12.drawIndirectCount || !supported11.shaderDrawParameters
			|| !supportedFeatures.multiDrawIndirect || !supportedFeatures.drawIndirectFirstInstance
			|| !supportedFeatures.shaderSampledImageArrayDynamicIndexing) {
			RhiLog::fatal("VulkanRHI: drawIndirectCount, shaderDrawParameters, multiDrawIndirect, drawIndirectFirstInstance and shaderSampledImageArrayDynamicIndexing are required");
		}
		deviceFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

		const char* deviceExtensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME
		};
		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();
		createInfo.pEnabledFeatures = &deviceFeatures;
		createInfo.enabledExtensionCount = 2;
		createInfo.ppEnabledExtensionNames = deviceExtensions;

		depthClipControl.depthClipControl = VK_TRUE;
		depthClipControl.pNext = nullptr;
		m_negativeOneToOneDepth = true;
		// Only the bits the device reported: asking for an unsupported feature fails device creation.
		VkPhysicalDeviceVulkan12Features enabled12{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
		};
		enabled12.pNext = &depthClipControl;
		enabled12.hostQueryReset = supported12.hostQueryReset;
		enabled12.samplerMirrorClampToEdge = supported12.samplerMirrorClampToEdge;
		enabled12.drawIndirectCount = VK_TRUE;
		m_hostQueryReset = supported12.hostQueryReset == VK_TRUE;
		VkPhysicalDeviceVulkan11Features enabled11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
		enabled11.shaderDrawParameters = VK_TRUE;
		enabled11.pNext = &enabled12;
		createInfo.pNext = &enabled11;

		if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateDevice failed");
		}

		vkGetDeviceQueue(m_device, indices.graphics, 0, &m_graphicsQueue);
		vkGetDeviceQueue(m_device, indices.present, 0, &m_presentQueue);
	}

	void VulkanRHI::createAllocator() {
		VmaVulkanFunctions fns{};
		fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
		fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo info{};
		info.physicalDevice = m_physicalDevice;
		info.device = m_device;
		info.instance = m_instance;
		info.vulkanApiVersion = VK_API_VERSION_1_2;
		info.pVulkanFunctions = &fns;
		if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vmaCreateAllocator failed");
		}
	}

	void VulkanRHI::destroyAllocator() {
		if (m_allocator != VK_NULL_HANDLE) {
			vmaDestroyAllocator(m_allocator);
			m_allocator = VK_NULL_HANDLE;
		}
	}

	void VulkanRHI::createPipelineCache() {
		VkPipelineCacheCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		if (vkCreatePipelineCache(m_device, &info, nullptr, &m_pipelineCache) != VK_SUCCESS) {
			m_pipelineCache = VK_NULL_HANDLE;
			RhiLog::error("VulkanRHI: vkCreatePipelineCache failed");
		}
	}

	void VulkanRHI::savePipelineCache() {
	}

	void VulkanRHI::destroyPipelineCache() {
		if (m_pipelineCache != VK_NULL_HANDLE) {
			vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
			m_pipelineCache = VK_NULL_HANDLE;
		}
	}

	void VulkanRHI::createSwapchain() {
		const auto support = querySwapchainSupport(m_physicalDevice, m_surface);
		const auto surfaceFormat = chooseSwapSurfaceFormat(support.formats);
		const auto presentMode = chooseSwapPresentMode(support.presentModes);
		const auto extent = chooseSwapExtent(support.capabilities, hostSurface().window);

		uint32_t imageCount = support.capabilities.minImageCount + 1;
		if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
			imageCount = support.capabilities.maxImageCount;
		}

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = m_surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		m_swapchainTransferDst = false;
		if (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
			createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			m_swapchainTransferDst = true;
		}

		const auto indices = findQueueFamilies(m_physicalDevice, m_surface);
		const uint32_t queueFamilyIndices[] = { indices.graphics, indices.present };
		if (indices.graphics != indices.present) {
			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = queueFamilyIndices;
		}
		else {
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		createInfo.preTransform = support.capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;
		createInfo.oldSwapchain = VK_NULL_HANDLE;

		if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateSwapchainKHR failed");
		}

		uint32_t swapImageCount = 0;
		vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, nullptr);
		m_swapchainImages.resize(swapImageCount);
		vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, m_swapchainImages.data());

		m_swapchainFormat = surfaceFormat.format;
		m_swapchainExtent = extent;
		m_swapchainLayouts.assign(m_swapchainImages.size(), VK_IMAGE_LAYOUT_UNDEFINED);
	}

	void VulkanRHI::createImageViews() {
		m_swapchainImageViews.resize(m_swapchainImages.size());
		for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
			VkImageViewCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			createInfo.image = m_swapchainImages[i];
			createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			createInfo.format = m_swapchainFormat;
			createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			createInfo.subresourceRange.baseMipLevel = 0;
			createInfo.subresourceRange.levelCount = 1;
			createInfo.subresourceRange.baseArrayLayer = 0;
			createInfo.subresourceRange.layerCount = 1;

			if (vkCreateImageView(m_device, &createInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkCreateImageView failed");
			}
		}
	}

	void VulkanRHI::createSwapchainRenderPass() {
		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = m_swapchainFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = 1;
		renderPassInfo.pAttachments = &colorAttachment;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_swapchainRenderPass) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateRenderPass failed");
		}
	}

	void VulkanRHI::createFramebuffers() {
		m_swapchainFramebuffers.resize(m_swapchainImageViews.size());
		for (size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
			VkImageView attachments[] = { m_swapchainImageViews[i] };

			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = m_swapchainRenderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = attachments;
			framebufferInfo.width = m_swapchainExtent.width;
			framebufferInfo.height = m_swapchainExtent.height;
			framebufferInfo.layers = 1;

			if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkCreateFramebuffer failed");
			}
		}
	}

	void VulkanRHI::createCommandPool() {
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

		if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateCommandPool failed");
		}
	}

	void VulkanRHI::createTimestampQueries() {
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
		m_timestampPeriod = props.limits.timestampPeriod;

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());
		if (m_graphicsQueueFamily < familyCount) {
			m_timestampValidBits = families[m_graphicsQueueFamily].timestampValidBits;
		}
		if (m_timestampPeriod <= 0.f || m_timestampValidBits == 0 || !m_hostQueryReset) {
			return;
		}

		VkQueryPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = kMaxFramesInFlight * kTimestampsPerFrame;
		if (vkCreateQueryPool(m_device, &info, nullptr, &m_timestampPool) != VK_SUCCESS) {
			RhiLog::error("VulkanRHI: failed to create timestamp query pool");
			m_timestampPool = VK_NULL_HANDLE;
		}
		info.queryCount = kMaxFramesInFlight * kMaxGpuMarks;
		if (vkCreateQueryPool(m_device, &info, nullptr, &m_markPool) != VK_SUCCESS) {
			m_markPool = VK_NULL_HANDLE;
		}
	}

	void VulkanRHI::destroyTimestampQueries() {
		if (m_timestampPool != VK_NULL_HANDLE) {
			vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
			m_timestampPool = VK_NULL_HANDLE;
		}
		if (m_markPool != VK_NULL_HANDLE) {
			vkDestroyQueryPool(m_device, m_markPool, nullptr);
			m_markPool = VK_NULL_HANDLE;
		}
		m_timestampPending[0] = false;
		m_timestampPending[1] = false;
	}

	void VulkanRHI::collectGpuTimestamp() {
		const uint64_t validMask = m_timestampValidBits >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << m_timestampValidBits) - 1);
		if (const uint32_t marks = m_markCount[m_currentFrame]; m_markPool != VK_NULL_HANDLE && marks > 1) {
			uint64_t stamps[kMaxGpuMarks]{};
			if (vkGetQueryPoolResults(m_device, m_markPool, m_currentFrame * kMaxGpuMarks, marks, sizeof(uint64_t) * marks, stamps,
				sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
				double atMs[kMaxGpuMarks]{};
				for (uint32_t i = 0; i < marks; ++i) {
					atMs[i] = static_cast<double>((stamps[i] - stamps[0]) & validMask) * static_cast<double>(m_timestampPeriod) / 1'000'000.0;
				}
				publishGpuSpans(m_markLabels[m_currentFrame], atMs, marks);
			}
		}
		if (m_timestampPool == VK_NULL_HANDLE || !m_timestampPending[m_currentFrame]) {
			return;
		}
		uint64_t ts[kTimestampsPerFrame]{};
		const uint32_t first = m_currentFrame * kTimestampsPerFrame;
		const VkResult result = vkGetQueryPoolResults(
			m_device, m_timestampPool, first, kTimestampsPerFrame,
			sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (result != VK_SUCCESS) {
			return;
		}
		const uint64_t mask = m_timestampValidBits >= 64
			? ~uint64_t{ 0 }
			: ((uint64_t{ 1 } << m_timestampValidBits) - 1);
		const uint64_t delta = (ts[1] - ts[0]) & mask;
		mLastGpuMs = static_cast<double>(delta) * static_cast<double>(m_timestampPeriod) / 1'000'000.0;
		m_timestampPending[m_currentFrame] = false;
	}

	void VulkanRHI::createSyncObjects() {
		m_imageAvailableSemaphores.resize(kMaxFramesInFlight);
		m_inFlightFences.resize(kMaxFramesInFlight);

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (int i = 0; i < kMaxFramesInFlight; ++i) {
			if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
				vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: failed to create sync objects");
			}
		}
	}

	void VulkanRHI::destroySwapchainSyncObjects() {
		for (auto semaphore : m_renderFinishedSemaphores) {
			if (semaphore != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_device, semaphore, nullptr);
			}
		}
		m_renderFinishedSemaphores.clear();
		m_imagesInFlight.clear();
	}

	void VulkanRHI::createSwapchainSyncObjects() {
		destroySwapchainSyncObjects();

		const size_t imageCount = m_swapchainImages.size();
		m_renderFinishedSemaphores.resize(imageCount, VK_NULL_HANDLE);
		m_imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		for (size_t i = 0; i < imageCount; ++i) {
			if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: failed to create swapchain sync objects");
			}
		}
	}

	void VulkanRHI::createCommandBuffers() {
		m_commandBuffers.resize(kMaxFramesInFlight);
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

		if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkAllocateCommandBuffers failed");
		}
	}

	void VulkanRHI::cleanupSwapchain() {
		for (auto framebuffer : m_swapchainFramebuffers) {
			if (framebuffer != VK_NULL_HANDLE) {
				vkDestroyFramebuffer(m_device, framebuffer, nullptr);
			}
		}
		m_swapchainFramebuffers.clear();

		for (auto view : m_swapchainImageViews) {
			if (view != VK_NULL_HANDLE) {
				vkDestroyImageView(m_device, view, nullptr);
			}
		}
		m_swapchainImageViews.clear();
		m_swapchainImages.clear();
		m_swapchainLayouts.clear();

		if (m_swapchainRenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(m_device, m_swapchainRenderPass, nullptr);
			m_swapchainRenderPass = VK_NULL_HANDLE;
		}

		if (m_swapchain != VK_NULL_HANDLE) {
			vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
			m_swapchain = VK_NULL_HANDLE;
		}
	}

	void VulkanRHI::recreateSwapchain() {
		int width = 0;
		int height = 0;
		auto* glfwWindow = hostSurface().window;
		glfwGetFramebufferSize(glfwWindow, &width, &height);
		while (width == 0 || height == 0) {
			glfwGetFramebufferSize(glfwWindow, &width, &height);
			glfwWaitEvents();
		}

		vkDeviceWaitIdle(m_device);
		destroyBlitPipeline();
		cleanupSwapchain();
		createSwapchain();
		createImageViews();
		createSwapchainRenderPass();
		createFramebuffers();
		createSwapchainSyncObjects();
		createBlitPipeline();
		ensureBackbufferHandle();
		m_framebufferResized = false;
	}

	void VulkanRHI::recordClearCommandBuffer(const uint32_t imageIndex) {
		VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
		vkResetCommandBuffer(cmd, 0);

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkBeginCommandBuffer failed");
		}

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = m_swapchainRenderPass;
		renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = m_swapchainExtent;
		renderPassInfo.clearValueCount = 1;
		VkClearValue clearValue{};
		clearValue.color = kClearColor;
		renderPassInfo.pClearValues = &clearValue;

		vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(cmd);

		if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkEndCommandBuffer failed");
		}
	}

	void VulkanRHI::recycleSceneList(const CommandListHandle handle) {
		if (!handle) {
			return;
		}
		if (auto* list = getLive(m_commandLists, handle.id)) {
			if (list->cmd != VK_NULL_HANDLE && list->cmd != m_sceneCmds[0] && list->cmd != m_sceneCmds[1]) {
				vkFreeCommandBuffers(m_device, m_sceneCommandPool, 1, &list->cmd);
			}
			list->cmd = VK_NULL_HANDLE;
			recycleResource(m_commandLists, m_freeCommandLists, handle.id);
		}
	}

	bool VulkanRHI::prepareInlinePresent() {
		m_swapchainAcquired = false;
		m_inlinePresent = false;
		m_presentedThisFrame = false;
		m_swapchainBlitted = false;
		if (!m_inited || m_swapchain == VK_NULL_HANDLE) {
			return false;
		}
		if (m_framebufferResized || swapchainOutOfDate()) {
			recreateSwapchain();
		}

		waitForInFlightFrame();

		if (!hostPresentsToScreen()) {
			return false;
		}

		uint32_t imageIndex = 0;
		const VkResult acquireResult = vkAcquireNextImageKHR(
			m_device, m_swapchain, UINT64_MAX,
			m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapchain();
			return false;
		}
		if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
			RhiLog::fatal("VulkanRHI: vkAcquireNextImageKHR failed");
			return false;
		}
		if (imageIndex < m_imagesInFlight.size() && m_imagesInFlight[imageIndex] != VK_NULL_HANDLE
			&& m_imagesInFlight[imageIndex] != m_inFlightFences[m_currentFrame]) {
			vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
		}
		m_acquiredImageIndex = imageIndex;
		if (imageIndex < m_swapchainLayouts.size()) {
			m_swapchainLayouts[imageIndex] = VK_IMAGE_LAYOUT_UNDEFINED;
		}
		m_swapchainAcquired = true;
		return true;
	}

	void VulkanRHI::submitInlinePresent(const VkCommandBuffer cmd) {
		vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
		if (m_acquiredImageIndex < m_imagesInFlight.size()) {
			m_imagesInFlight[m_acquiredImageIndex] = m_inFlightFences[m_currentFrame];
		}

		VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphores[m_currentFrame] };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		if (m_acquiredImageIndex >= m_renderFinishedSemaphores.size()) {
			RhiLog::fatal("VulkanRHI: swapchain image index out of range");
			return;
		}
		VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[m_acquiredImageIndex] };

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_swapchain;
		presentInfo.pImageIndices = &m_acquiredImageIndex;

		VkResult presentResult = VK_SUCCESS;
		{
			std::lock_guard lock(m_queueMutex);
			const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);
			if (submitResult != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkQueueSubmit failed (%d)", static_cast<int>(submitResult));
			}
			presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
		}
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
			m_framebufferResized = true;
		}
		else if (presentResult != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkQueuePresentKHR failed (%d)", static_cast<int>(presentResult));
		}

		m_presentedThisFrame = true;
		m_swapchainAcquired = false;
		m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
		m_frameWaited = false;
	}

	void VulkanRHI::finishPendingScene() {
		if (!m_hasPendingScene || m_sceneFence == VK_NULL_HANDLE) {
			return;
		}
		vkWaitForFences(m_device, 1, &m_sceneFence, VK_TRUE, UINT64_MAX);
		if (auto* sceneList = getLive(m_commandLists, m_pendingSceneCmd.id)) {
			if (sceneList->cmd != VK_NULL_HANDLE && sceneList->cmd != m_sceneCmds[0] && sceneList->cmd != m_sceneCmds[1]) {
				vkFreeCommandBuffers(m_device, m_sceneCommandPool, 1, &sceneList->cmd);
			}
			sceneList->cmd = VK_NULL_HANDLE;
			recycleResource(m_commandLists, m_freeCommandLists, m_pendingSceneCmd.id);
		}
		vkResetFences(m_device, 1, &m_sceneFence);
		m_hasPendingScene = false;
		m_pendingSceneCmd = {};
	}

	// The swapchain follows the host's backbuffer size. It used to hook the engine window's resize
	// callback; asking the surface each frame needs no window class and no callback chain.
	bool VulkanRHI::swapchainOutOfDate() const {
		const RhiSurface surface = hostSurface();
		return surface.window && (surface.backbufferWidth != static_cast<int>(m_swapchainExtent.width)
			|| surface.backbufferHeight != static_cast<int>(m_swapchainExtent.height));
	}

	void VulkanRHI::drawFrame() {
		if (m_framebufferResized || swapchainOutOfDate()) {
			recreateSwapchain();
		}

		vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

		finishPendingScene();

		uint32_t imageIndex = 0;
		const VkResult acquireResult = vkAcquireNextImageKHR(
			m_device, m_swapchain, UINT64_MAX,
			m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapchain();
			return;
		}
		if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
			RhiLog::fatal("VulkanRHI: vkAcquireNextImageKHR failed");
		}

		if (imageIndex < m_imagesInFlight.size() && m_imagesInFlight[imageIndex] != VK_NULL_HANDLE
			&& m_imagesInFlight[imageIndex] != m_inFlightFences[m_currentFrame]) {
			vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
		}

		vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);
		if (imageIndex < m_imagesInFlight.size()) {
			m_imagesInFlight[imageIndex] = m_inFlightFences[m_currentFrame];
		}

		VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkBeginCommandBuffer failed");
		}

		if (m_presentBlitTexture) {
			recordPresentBlit(cmd, imageIndex);
		}
		else {
			VkRenderPassBeginInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = m_swapchainRenderPass;
			renderPassInfo.framebuffer = m_swapchainFramebuffers[imageIndex];
			renderPassInfo.renderArea.offset = { 0, 0 };
			renderPassInfo.renderArea.extent = m_swapchainExtent;
			VkClearValue clearValue{};
			clearValue.color = kClearColor;
			renderPassInfo.clearValueCount = 1;
			renderPassInfo.pClearValues = &clearValue;
			vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cmd);
		}

		if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkEndCommandBuffer failed");
		}

		VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphores[m_currentFrame] };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		if (imageIndex >= m_renderFinishedSemaphores.size()) {
			RhiLog::fatal("VulkanRHI: swapchain image index out of range");
		}
		VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[imageIndex] };

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_swapchain;
		presentInfo.pImageIndices = &imageIndex;

		VkResult presentResult = VK_SUCCESS;
		{
			std::lock_guard lock(m_queueMutex);
			const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);
			if (submitResult != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: vkQueueSubmit failed (%d)", static_cast<int>(submitResult));
			}
			presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
		}
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
			m_framebufferResized = true;
		}
		else if (presentResult != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkQueuePresentKHR failed");
		}

		m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
		m_frameWaited = false;
	}

	void VulkanRHI::present() {
		if (!m_inited) {
			return;
		}
		if (m_presentedThisFrame) {
			m_presentedThisFrame = false;
			return;
		}
		drawFrame();
	}
}

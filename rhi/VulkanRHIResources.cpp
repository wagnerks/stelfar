#include "rhi/VulkanRHI.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "rhi/VulkanRHIVkUtil.h"
#include "rhi/RhiLog.h"

using namespace SFE::Render::VulkanVkUtil;

namespace SFE::Render {
	uint32_t VulkanRHI::findMemoryType(const uint32_t typeFilter, const VkMemoryPropertyFlags properties) const {
		VkPhysicalDeviceMemoryProperties memProperties{};
		vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
		auto match = [&](const VkMemoryPropertyFlags required) -> uint32_t {
			for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
				if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & required) == required) {
					return i;
				}
			}
			return UINT32_MAX;
		};
		// Prefer ReBAR (DEVICE_LOCAL | HOST_VISIBLE) so GPU-read SSBOs/UBOs stay in VRAM.
		if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			if (const uint32_t id = match(properties | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); id != UINT32_MAX) {
				return id;
			}
		}
		if (const uint32_t id = match(properties); id != UINT32_MAX) {
			return id;
		}
		RhiLog::fatal("VulkanRHI: failed to find suitable memory type");
		return 0;
	}

	VkBuffer VulkanRHI::createStagingBuffer(const size_t size, VmaAllocation& allocation, const void* data) {
		allocation = VK_NULL_HANDLE;
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size < 16 ? 16 : size;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo mapped{};
		if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &stagingBuffer, &allocation, &mapped) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create staging buffer");
		}
		if (data && mapped.pMappedData) {
			std::memcpy(mapped.pMappedData, data, size);
			vmaFlushAllocation(m_allocator, allocation, 0, size);
		}
		return stagingBuffer;
	}

	void VulkanRHI::destroyStagingBuffer(VkBuffer buffer, VmaAllocation allocation) {
		if (buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_allocator, buffer, allocation);
		}
	}

	void VulkanRHI::createUploadRings() {
		for (int i = 0; i < kMaxFramesInFlight; ++i) {
			VkBufferCreateInfo bufferInfo{};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = kUploadRingSize;
			bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocInfo{};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo mapped{};
			if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_uploadRing[i], &m_uploadRingAlloc[i], &mapped) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: failed to create upload ring");
			}
			m_uploadRingMapped[i] = mapped.pMappedData;
		}
	}

	void VulkanRHI::destroyUploadRings() {
		for (int i = 0; i < kMaxFramesInFlight; ++i) {
			if (m_uploadRing[i] != VK_NULL_HANDLE) {
				vmaDestroyBuffer(m_allocator, m_uploadRing[i], m_uploadRingAlloc[i]);
				m_uploadRing[i] = VK_NULL_HANDLE;
				m_uploadRingAlloc[i] = VK_NULL_HANDLE;
				m_uploadRingMapped[i] = nullptr;
			}
		}
		m_uploadOps.clear();
		m_textureUploadOps.clear();
		m_uploadRingUsed = 0;
	}

	void VulkanRHI::waitForInFlightFrame() {
		if (m_frameWaited) {
			return;
		}
		if (m_currentFrame < m_inFlightFences.size() && m_inFlightFences[m_currentFrame] != VK_NULL_HANDLE) {
			vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);
		}
		recycleSceneList(m_inflightSceneCmd[m_currentFrame]);
		m_inflightSceneCmd[m_currentFrame] = {};
		collectGpuTimestamp();
		for (const auto& [pool, set] : m_retiredSets[m_currentFrame]) {
			vkFreeDescriptorSets(m_device, pool, 1, &set);
		}
		m_retiredSets[m_currentFrame].clear();
		std::erase_if(m_retiredBuffers, [this](RetiredBuffer& retired) {
			if (--retired.waitsLeft > 0) {
				return false;
			}
			vmaDestroyBuffer(m_allocator, retired.buffer, retired.allocation);
			return true;
		});
		// The framebuffers among these before the images under them.
		std::erase_if(m_retiredObjects, [](RetiredObject& retired) {
			if (--retired.waitsLeft > 0) {
				return false;
			}
			retired.destroy();
			return true;
		});
		std::erase_if(m_retiredTextures, [this](RetiredTexture& retired) {
			if (--retired.waitsLeft > 0) {
				return false;
			}
			destroyTextureInternal(retired.texture);
			return true;
		});
		// After the retired buffers: what this retires must wait its own full round.
		resetTransient();
		if (m_timestampPool != VK_NULL_HANDLE) {
			vkResetQueryPool(m_device, m_timestampPool, m_currentFrame * kTimestampsPerFrame, kTimestampsPerFrame);
			m_timestampStartWritten[m_currentFrame] = false;
			m_timestampEndWritten[m_currentFrame] = false;
		}
		if (m_markPool != VK_NULL_HANDLE) {
			vkResetQueryPool(m_device, m_markPool, m_currentFrame * kMaxGpuMarks, kMaxGpuMarks);
			m_markCount[m_currentFrame] = 0;
		}
		m_uploadRingUsed = 0;
		m_uploadOps.clear();
		m_textureUploadOps.clear();
		m_uploadsFlushed = false;
		m_frameWaited = true;
	}

	void VulkanRHI::queueBufferUpload(const uint32_t bufferId, size_t offset, size_t size, const void* data) {
		auto* buffer = getLive(m_buffers, bufferId);
		if (!buffer || !buffer->buffer || !data || size == 0) {
			return;
		}
		waitForInFlightFrame();
		buffer = getLive(m_buffers, bufferId);
		if (!buffer || !buffer->buffer) {
			return;
		}
		if (m_uploadsFlushed) {
			static bool reported = false;
			if (!reported) {
				reported = true;
				RhiLog::error("VulkanRHI: updateBuffer on '%s' after the frame's uploads were flushed: submitted and waited on "
					"the spot. Write device-local buffers before the frame's first command list.",
					buffer->desc.debugName ? buffer->desc.debugName : "unnamed");
			}
			VmaAllocation stagingMemory = VK_NULL_HANDLE;
			const VkBuffer staging = createStagingBuffer(size, stagingMemory, data);
			const VkBuffer dst = buffer->buffer;
			executeImmediate([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = offset;
				copyRegion.size = size;
				vkCmdCopyBuffer(cmd, staging, dst, 1, &copyRegion);
			});
			destroyStagingBuffer(staging, stagingMemory);
			return;
		}
		const VkDeviceSize aligned = (static_cast<VkDeviceSize>(size) + 3u) & ~VkDeviceSize{ 3 };
		if (!m_uploadRingMapped[m_currentFrame] || m_uploadRingUsed + aligned > kUploadRingSize) {
			VmaAllocation stagingMemory = VK_NULL_HANDLE;
			const VkBuffer staging = createStagingBuffer(size, stagingMemory, data);
			const VkBuffer dst = buffer->buffer;
			executeImmediate([&](VkCommandBuffer cmd) {
				VkBufferCopy copyRegion{};
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = offset;
				copyRegion.size = size;
				vkCmdCopyBuffer(cmd, staging, dst, 1, &copyRegion);
			});
			destroyStagingBuffer(staging, stagingMemory);
			return;
		}
		std::memcpy(static_cast<uint8_t*>(m_uploadRingMapped[m_currentFrame]) + m_uploadRingUsed, data, size);
		m_uploadOps.push_back({
			bufferId,
			static_cast<VkDeviceSize>(offset),
			m_uploadRingUsed,
			static_cast<VkDeviceSize>(size)
		});
		m_uploadRingUsed += aligned;
	}

	// A texture written on the device thread before the frame's first command list goes the way a device-local buffer
	// does: into this frame's upload ring, copied at the start of that list. The other way, a submit waited for on the
	// spot, waits for all the GPU was given before it: the frames in flight, and the loading threads' texture copies.
	// Textures take the ring up to half, so the frame's buffers still find room; false when it cannot go this way.
	bool VulkanRHI::queueTextureUpload(const uint32_t textureId, const uint32_t x, const uint32_t y, const uint32_t w,
		const uint32_t h, const uint32_t mip, const uint32_t layer, const void* data, const size_t size) {
		waitForInFlightFrame();
		// Where a copy may start: on a whole texel of any format (RGBA32F's 16 bytes).
		const VkDeviceSize offset = (m_uploadRingUsed + 15u) & ~VkDeviceSize{ 15 };
		if (m_uploadsFlushed || !m_uploadRingMapped[m_currentFrame] || offset + size > kUploadRingSize / 2) {
			return false;
		}
		std::memcpy(static_cast<uint8_t*>(m_uploadRingMapped[m_currentFrame]) + offset, data, size);
		m_textureUploadOps.push_back({ textureId, offset, x, y, w, h, mip, layer });
		m_uploadRingUsed = (offset + size + 3u) & ~VkDeviceSize{ 3 };
		return true;
	}

	// The texture writes waiting in the ring recorded into `cmd`, ahead of whatever comes after them: at the start of
	// the frame, or before an immediate write or a readback, so all land in the order they were asked for.
	void VulkanRHI::recordTextureUploads(const VkCommandBuffer cmd) {
		if (m_textureUploadOps.empty()) {
			return;
		}
		vmaFlushAllocation(m_allocator, m_uploadRingAlloc[m_currentFrame], 0, m_uploadRingUsed);
		// Every texture written turned to be copied into in one barrier, the copies, and every one turned back in another:
		// a barrier stops the GPU until all before it is done, and the ground's marks alone are a hundred tiles a frame.
		std::vector<VkTextureResource*> written;
		for (const auto& op : m_textureUploadOps) {
			auto* texture = getLive(m_textures, op.dstId);
			if (texture && texture->image && std::find(written.begin(), written.end(), texture) == written.end()) {
				written.push_back(texture);
			}
		}
		transitionTextures(cmd, written, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		for (const auto& op : m_textureUploadOps) {
			auto* texture = getLive(m_textures, op.dstId);
			if (!texture || !texture->image) {
				continue;
			}
			VkBufferImageCopy region{};
			region.bufferOffset = op.srcOffset;
			region.imageSubresource.aspectMask = aspectMaskForFormat(texture->format);
			region.imageSubresource.mipLevel = op.mip;
			region.imageSubresource.baseArrayLayer = op.layer;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { static_cast<int32_t>(op.x), static_cast<int32_t>(op.y), 0 };
			region.imageExtent = { op.w, op.h, 1 };
			vkCmdCopyBufferToImage(cmd, m_uploadRing[m_currentFrame], texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}
		transitionTextures(cmd, written, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_textureUploadOps.clear();
	}

	void VulkanRHI::flushBufferUploads(const VkCommandBuffer cmd) {
		if (cmd == VK_NULL_HANDLE) {
			return;
		}
		if (m_uploadOps.empty() && m_textureUploadOps.empty()) {
			m_uploadsFlushed = true;
			return;
		}
		if (m_uploadRingAlloc[m_currentFrame] != VK_NULL_HANDLE && m_uploadRingUsed > 0) {
			vmaFlushAllocation(m_allocator, m_uploadRingAlloc[m_currentFrame], 0, m_uploadRingUsed);
		}
		for (const auto& op : m_uploadOps) {
			const auto* dst = getLive(m_buffers, op.dstId);
			if (!dst || !dst->buffer || op.size == 0) {
				continue;
			}
			VkBufferCopy copyRegion{};
			copyRegion.srcOffset = op.srcOffset;
			copyRegion.dstOffset = op.dstOffset;
			copyRegion.size = op.size;
			vkCmdCopyBuffer(cmd, m_uploadRing[m_currentFrame], dst->buffer, 1, &copyRegion);
		}
		recordTextureUploads(cmd);
		VkMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &barrier, 0, nullptr, 0, nullptr);
		m_uploadOps.clear();
		m_uploadsFlushed = true;
	}

	void VulkanRHI::executeImmediate(const std::function<void(VkCommandBuffer)>& fn) {
		executeImmediate(fn, m_uploadCommandPool, m_uploadFence);
	}

	void VulkanRHI::executeImmediate(const std::function<void(VkCommandBuffer)>& fn, const VkCommandPool pool, const VkFence fence) {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = pool;
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commandBuffer, &beginInfo);
		fn(commandBuffer);
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (fence != VK_NULL_HANDLE) {
			vkResetFences(m_device, 1, &fence);
			{
				std::lock_guard lock(m_queueMutex);
				const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence);
				if (submitResult != VK_SUCCESS) {
					RhiLog::fatal("VulkanRHI: upload submit failed (%d)", static_cast<int>(submitResult));
				}
			}
			vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
		}
		else {
			{
				std::lock_guard lock(m_queueMutex);
				vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
			}
			vkQueueWaitIdle(m_graphicsQueue);
		}

		vkFreeCommandBuffers(m_device, pool, 1, &commandBuffer);
	}

	void VulkanRHI::createDescriptorPool() {
		const std::array<VkDescriptorPoolSize, 5> poolSizes{ {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 512 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 512 },
		} };

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 2048;
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();

		VkDescriptorPool pool = VK_NULL_HANDLE;
		if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: failed to create descriptor pool");
			return;
		}
		m_descriptorPools.push_back(pool);
		m_descriptorPool = pool;
	}

	void VulkanRHI::destroyDescriptorPool() {
		for (const auto pool : m_descriptorPools) {
			vkDestroyDescriptorPool(m_device, pool, nullptr);
		}
		m_descriptorPools.clear();
		m_descriptorPool = VK_NULL_HANDLE;
	}

	VkDescriptorSet VulkanRHI::allocateVkDescriptorSet(const VkDescriptorSetLayout layout, VkDescriptorPool& pool) {
		for (int attempt = 0; attempt < 2; ++attempt) {
			VkDescriptorSetAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.descriptorPool = m_descriptorPool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &layout;
			VkDescriptorSet set = VK_NULL_HANDLE;
			const VkResult result = vkAllocateDescriptorSets(m_device, &allocInfo, &set);
			if (result == VK_SUCCESS) {
				pool = m_descriptorPool;
				return set;
			}
			if (attempt > 0 || (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)) {
				RhiLog::error("VulkanRHI: descriptor set allocation failed (%d)", static_cast<int>(result));
				return VK_NULL_HANDLE;
			}
			RhiLog::info("VulkanRHI: descriptor pool %zu is full, adding another", m_descriptorPools.size());
			createDescriptorPool();
		}
		return VK_NULL_HANDLE;
	}

	void VulkanRHI::destroyBufferInternal(VkBufferResource& buffer) {
		if (buffer.buffer != VK_NULL_HANDLE) {
			const uint32_t id = static_cast<uint32_t>(&buffer - m_buffers.data());
			if (id < m_buffers.size() && &m_buffers[id] == &buffer) {
				m_uploadOps.erase(
					std::remove_if(m_uploadOps.begin(), m_uploadOps.end(),
						[id](const PendingBufferUpload& op) { return handleSlot(op.dstId) == id; }),
					m_uploadOps.end());
			}
			if (m_inited) {
				m_retiredBuffers.push_back({ buffer.buffer, buffer.allocation, kMaxFramesInFlight });
			}
			else {
				vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
			}
		}
	}

	BufferHandle VulkanRHI::createTransientBuffer(const VkDeviceSize size) {
		// Not createBuffer: that puts a large storage buffer in device-local memory, and this one is written
		// through its mapping.
		VkBufferResource resource{};
		resource.desc.size = size;
		resource.desc.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index;
		resource.desc.cpuAccess = CpuAccess::Write;
		resource.desc.debugName = "Transient";
		resource.live = true;

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = toVkBufferUsage(resource.desc.usage);
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo mapped{};
		if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &resource.buffer, &resource.allocation, &mapped) != VK_SUCCESS
			|| !mapped.pMappedData) {
			RhiLog::fatal("VulkanRHI: a mapped transient buffer of %zu bytes could not be created", static_cast<size_t>(size));
			return {};
		}
		resource.mapped = mapped.pMappedData;
		resource.persistentMapped = true;
		BufferHandle handle{};
		handle.id = storeResource(m_buffers, m_freeBuffers, std::move(resource));
		return handle;
	}

	void VulkanRHI::resetTransient() {
		// This slot's previous frame has finished: its region and overflow buffers are free again.
		for (const auto overflow : m_transientOverflow[m_currentFrame]) {
			destroyBuffer(overflow);
		}
		m_transientOverflow[m_currentFrame].clear();
		m_transientUsed[m_currentFrame] = 0;
		if (!m_transientBuffer || m_transientPeak <= m_transientRegion) {
			return;
		}
		// Too small last time. The other slot's frame may still read the old buffer; destroyBuffer keeps it
		// until that has finished, and the next allocation creates one sized to the peak.
		destroyBuffer(m_transientBuffer);
		m_transientBuffer = {};
		m_transientRegion = 0;
	}

	TransientAllocation VulkanRHI::allocTransient(const size_t size) {
		if (size == 0) {
			return {};
		}
		waitForInFlightFrame();
		if (!getLive(m_buffers, m_transientBuffer.id)) {
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
			m_transientAlign = (std::max)({ props.limits.minUniformBufferOffsetAlignment, props.limits.minStorageBufferOffsetAlignment, VkDeviceSize{ 16 } });
			VkDeviceSize region = VkDeviceSize{ 1 } << 20;
			while (region < m_transientPeak) {
				region *= 2;
			}
			m_transientBuffer = createTransientBuffer(region * kMaxFramesInFlight);
			m_transientRegion = m_transientBuffer ? region : 0;
			for (auto& used : m_transientUsed) {
				used = 0;
			}
		}

		auto& used = m_transientUsed[m_currentFrame];
		const VkDeviceSize offset = (used + m_transientAlign - 1) & ~(m_transientAlign - 1);
		m_transientPeak = (std::max)(m_transientPeak, offset + size);
		if (offset + size > m_transientRegion) {
			const auto overflow = createTransientBuffer(size);
			const auto* buffer = getLive(m_buffers, overflow.id);
			if (!buffer) {
				return {};
			}
			m_transientOverflow[m_currentFrame].push_back(overflow);
			return { overflow, 0, buffer->mapped };
		}
		used = offset + size;
		const VkDeviceSize base = m_currentFrame * m_transientRegion + offset;
		const auto* buffer = getLive(m_buffers, m_transientBuffer.id);
		return { m_transientBuffer, static_cast<uint32_t>(base), static_cast<uint8_t*>(buffer->mapped) + base };
	}

	BufferHandle VulkanRHI::createBuffer(const BufferDesc& desc, const void* initialData) {
		VkBufferResource resource{};
		resource.desc = desc;
		resource.live = true;

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = desc.size < 16 ? 16 : desc.size;
		bufferInfo.usage = toVkBufferUsage(desc.usage);
		const bool gpuStorage = (desc.usage & BufferUsage::Storage) && desc.cpuAccess != CpuAccess::None
			&& desc.size >= (1u << 20);
		// Always: a CpuAccess::None buffer is still written through updateBuffer's queued upload, and
		// cmdCopyBuffer can name any buffer as either end.
		bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		// Read counts as host access too. Leaving it out put a buffer the caller declared it would read
		// into device-local memory, where vmaMapMemory cannot reach it -- so the one CpuAccess value
		// that names a GPU-to-CPU transfer was the one that could not be mapped.
		const bool hostMapped = !gpuStorage && desc.cpuAccess != CpuAccess::None;
		if (gpuStorage) {
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			resource.deviceLocal = true;
		}
		else if (hostMapped) {
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		}
		else {
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			resource.deviceLocal = true;
		}

		VmaAllocationInfo mapped{};
		VkResult allocResult = vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &resource.buffer, &resource.allocation, &mapped);
		if (allocResult != VK_SUCCESS && hostMapped) {
			allocInfo = {};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			resource.deviceLocal = true;
			allocResult = vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &resource.buffer, &resource.allocation, &mapped);
		}
		if (allocResult != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vmaCreateBuffer failed (%d)", static_cast<int>(allocResult));
			return {};
		}
		if (hostMapped && mapped.pMappedData) {
			resource.mapped = mapped.pMappedData;
			resource.persistentMapped = true;
			resource.deviceLocal = false;
		}

		trackBufferMemory(desc, true);
		BufferHandle handle{};
		handle.id = storeResource(m_buffers, m_freeBuffers, std::move(resource));

		// Without initial data the buffer is zeroed (RenderHardwareInterface::createBuffer): its memory, a block VMA
		// hands on, holds what was there before, as OpenGL's glBufferData(nullptr) does (that backend clears it too). A
		// pass that read one before anything wrote it -- bones of a frame slot not yet computed -- drew uninitialised
		// memory, different on every run.
		if (auto* stored = getLive(m_buffers, handle.id); stored && stored->mapped) {
			if (initialData) {
				std::memcpy(stored->mapped, initialData, desc.size);
			}
			else {
				std::memset(stored->mapped, 0, desc.size);
			}
			if (stored->allocation != VK_NULL_HANDLE) {
				vmaFlushAllocation(m_allocator, stored->allocation, 0, desc.size);
			}
		}
		else if (stored && stored->buffer) {
			std::vector<uint8_t> zeros;
			if (!initialData) {
				zeros.assign(desc.size, 0);
			}
			copyThroughStaging(stored->buffer, 0, desc.size, initialData ? initialData : zeros.data());
		}
		return handle;
	}

	void VulkanRHI::destroyBuffer(const BufferHandle handle) {
		if (auto* buffer = getLive(m_buffers, handle.id)) {
			destroyBufferInternal(*buffer);
			trackBufferMemory(buffer->desc, false);
			recycleResource(m_buffers, m_freeBuffers, handle.id);
		}
	}

	void VulkanRHI::updateBuffer(const BufferHandle handle, size_t offset, size_t size, const void* data) {
		auto* buffer = getLive(m_buffers, handle.id);
		if (!buffer || !data) {
			return;
		}
		if (size == 0) {
			size = buffer->desc.size;
		}

		if (buffer->mapped) {
			if (offset + size > buffer->desc.size) {
				RhiLog::error("VulkanRHI: updateBuffer OOB offset=%zu size=%zu cap=%zu",
					offset, size, static_cast<size_t>(buffer->desc.size));
				return;
			}
			std::memcpy(static_cast<uint8_t*>(buffer->mapped) + offset, data, size);
			if (buffer->allocation != VK_NULL_HANDLE) {
				vmaFlushAllocation(m_allocator, buffer->allocation, offset, size);
			}
			return;
		}
		if (buffer->deviceLocal) {
			queueBufferUpload(handle.id, offset, size, data);
			return;
		}

		copyThroughStaging(buffer->buffer, offset, size, data);
	}

	void VulkanRHI::copyThroughStaging(const VkBuffer dst, const size_t offset, const size_t size, const void* data) {
		VmaAllocation stagingMemory = VK_NULL_HANDLE;
		const VkBuffer staging = createStagingBuffer(size, stagingMemory, data);
		executeImmediate([&](VkCommandBuffer cmd) {
			VkBufferCopy copyRegion{};
			copyRegion.srcOffset = 0;
			copyRegion.dstOffset = offset;
			copyRegion.size = size;
			vkCmdCopyBuffer(cmd, staging, dst, 1, &copyRegion);
		});
		destroyStagingBuffer(staging, stagingMemory);
	}

	void* VulkanRHI::mapBuffer(const BufferHandle handle, MapMode) {
		auto* buffer = getLive(m_buffers, handle.id);
		if (!buffer) {
			return nullptr;
		}
		if (buffer->mapped) {
			return buffer->mapped;
		}
		void* mapped = nullptr;
		vmaMapMemory(m_allocator, buffer->allocation, &mapped);
		buffer->mapped = mapped;
		return mapped;
	}

	void VulkanRHI::unmapBuffer(const BufferHandle handle) {
		auto* buffer = getLive(m_buffers, handle.id);
		if (!buffer || !buffer->mapped) {
			return;
		}
		if (buffer->allocation != VK_NULL_HANDLE) {
			vmaFlushAllocation(m_allocator, buffer->allocation, 0, VK_WHOLE_SIZE);
		}
		if (buffer->persistentMapped) {
			return;
		}
		vmaUnmapMemory(m_allocator, buffer->allocation);
		buffer->mapped = nullptr;
	}

	namespace {
		VkAccessFlags accessForLayout(const VkImageLayout layout) {
			switch (layout) {
			case VK_IMAGE_LAYOUT_UNDEFINED:
			case VK_IMAGE_LAYOUT_PREINITIALIZED:
				return 0;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				return VK_ACCESS_TRANSFER_WRITE_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				return VK_ACCESS_TRANSFER_READ_BIT;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				return VK_ACCESS_SHADER_READ_BIT;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
				return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			case VK_IMAGE_LAYOUT_GENERAL:
				return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				return 0;
			default:
				return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			}
		}

		VkPipelineStageFlags stagesForLayout(const VkImageLayout layout) {
			switch (layout) {
			case VK_IMAGE_LAYOUT_UNDEFINED:
			case VK_IMAGE_LAYOUT_PREINITIALIZED:
				return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				return VK_PIPELINE_STAGE_TRANSFER_BIT;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
				return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			case VK_IMAGE_LAYOUT_GENERAL:
				return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			default:
				return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			}
		}
	}

	void VulkanRHI::transitionImageLayout(const VkCommandBuffer cmd, const VkImage image, const VkFormat format,
		const VkImageLayout oldLayout, const VkImageLayout newLayout, const uint32_t layerCount, const uint32_t mipLevels)
	{
		if (oldLayout == newLayout) {
			return;
		}

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = aspectMaskForFormat(format);
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = (std::max)(1u, mipLevels);
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = (std::max)(1u, layerCount);

		barrier.srcAccessMask = accessForLayout(oldLayout);
		barrier.dstAccessMask = accessForLayout(newLayout);
		vkCmdPipelineBarrier(cmd, stagesForLayout(oldLayout), stagesForLayout(newLayout), 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void VulkanRHI::ensureTextureLayout(VkCommandBuffer cmd, VkTextureResource& texture, const VkImageLayout newLayout) {
		if (!texture.image || texture.layout == newLayout) {
			return;
		}
		const uint32_t layers = (texture.desc.dimension == TextureDimension::Cube) ? 6u
			: (texture.desc.dimension == TextureDimension::Array)
			? static_cast<uint32_t>((std::max)(1u, static_cast<uint32_t>(texture.desc.depth)))
			: 1u;
		const uint32_t mips = (std::max)(1u, texture.desc.mipLevels);
		transitionImageLayout(cmd, texture.image, texture.format, texture.layout, newLayout, layers, mips);
		texture.layout = newLayout;
	}

	void VulkanRHI::transitionTextures(const VkCommandBuffer cmd, const std::vector<VkTextureResource*>& textures, const VkImageLayout newLayout) {
		std::vector<VkImageMemoryBarrier> barriers;
		VkPipelineStageFlags from = 0;
		for (auto* texture : textures) {
			if (!texture->image || texture->layout == newLayout) {
				continue;
			}
			VkImageMemoryBarrier& barrier = barriers.emplace_back();
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = texture->layout;
			barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = texture->image;
			barrier.subresourceRange.aspectMask = aspectMaskForFormat(texture->format);
			barrier.subresourceRange.levelCount = (std::max)(1u, texture->desc.mipLevels);
			barrier.subresourceRange.layerCount = (texture->desc.dimension == TextureDimension::Cube) ? 6u
				: (texture->desc.dimension == TextureDimension::Array) ? (std::max)(1u, static_cast<uint32_t>(texture->desc.depth)) : 1u;
			barrier.srcAccessMask = accessForLayout(texture->layout);
			barrier.dstAccessMask = accessForLayout(newLayout);
			from |= stagesForLayout(texture->layout);
			texture->layout = newLayout;
		}
		if (!barriers.empty()) {
			vkCmdPipelineBarrier(cmd, from, stagesForLayout(newLayout), 0, 0, nullptr, 0, nullptr,
				static_cast<uint32_t>(barriers.size()), barriers.data());
		}
	}

	void VulkanRHI::generateMipmaps(VkCommandBuffer cmd, VkTextureResource& texture) {
		const uint32_t mipLevels = (std::max)(1u, texture.desc.mipLevels);
		if (mipLevels <= 1 || !texture.image) {
			return;
		}
		const uint32_t layers = (texture.desc.dimension == TextureDimension::Cube) ? 6u
			: (texture.desc.dimension == TextureDimension::Array)
			? static_cast<uint32_t>((std::max)(1u, static_cast<uint32_t>(texture.desc.depth)))
			: 1u;

		ensureTextureLayout(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		int32_t mipWidth = static_cast<int32_t>(texture.desc.width);
		int32_t mipHeight = static_cast<int32_t>(texture.desc.height);
		for (uint32_t i = 1; i < mipLevels; ++i) {
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.image = texture.image;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange.aspectMask = aspectMaskForFormat(texture.format);
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = layers;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseMipLevel = i - 1;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);

			VkImageBlit blit{};
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
			blit.srcSubresource.aspectMask = aspectMaskForFormat(texture.format);
			blit.srcSubresource.mipLevel = i - 1;
			blit.srcSubresource.baseArrayLayer = 0;
			blit.srcSubresource.layerCount = layers;
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
			blit.dstSubresource.aspectMask = aspectMaskForFormat(texture.format);
			blit.dstSubresource.mipLevel = i;
			blit.dstSubresource.baseArrayLayer = 0;
			blit.dstSubresource.layerCount = layers;
			vkCmdBlitImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);

			if (mipWidth > 1) mipWidth /= 2;
			if (mipHeight > 1) mipHeight /= 2;
		}

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.image = texture.image;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange.aspectMask = aspectMaskForFormat(texture.format);
		barrier.subresourceRange.baseMipLevel = mipLevels - 1;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = layers;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void VulkanRHI::copyBufferToImage(const VkCommandBuffer cmd, const VkBuffer buffer, const VkImage image,
		const uint32_t width, const uint32_t height, const uint32_t layer, const uint32_t mip)
	{
		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mip;
		region.imageSubresource.baseArrayLayer = layer;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };
		vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanRHI::createImageObjects(VkTextureResource& out, const TextureDesc& desc, uint32_t& mipLevels, uint32_t& arrayLayers) {
		out.desc = desc;
		out.format = toVkFormat(desc.format);

		mipLevels = desc.sampleCount > 1 ? 1u : (std::max)(1u, desc.mipLevels);
		arrayLayers = 1;
		VkImageCreateFlags flags = 0;
		VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
		if (desc.dimension == TextureDimension::Cube) {
			arrayLayers = 6;
			flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		}
		else if (desc.dimension == TextureDimension::Array) {
			arrayLayers = (std::max)(1u, static_cast<uint32_t>(desc.depth));
			viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		}

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.flags = flags;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { (std::max)(1u, desc.width), (std::max)(1u, desc.height), 1 };
		imageInfo.mipLevels = mipLevels;
		imageInfo.arrayLayers = arrayLayers;
		imageInfo.format = out.format;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// Transfer destination always: a texture created without data is cleared to zero below. Transfer
		// source always: mip generation blits from the level above, and readbackTexture copies or blits out
		// of any texture -- without it a stage dump of a G-buffer target was a validation error.
		imageInfo.usage = toVkImageUsage(desc.usage) | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		imageInfo.samples = toVkSampleCount(desc.sampleCount);
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		if (vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vmaCreateImage failed");
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = out.image;
		viewInfo.viewType = viewType;
		viewInfo.format = out.format;
		viewInfo.subresourceRange.aspectMask = aspectMaskForFormat(out.format);
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = mipLevels;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = arrayLayers;
		if (desc.swizzle == TextureSwizzle::Grey) {
			viewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
		}
		if (vkCreateImageView(m_device, &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkCreateImageView failed");
		}
	}

	void VulkanRHI::createImage(VkTextureResource& out, const TextureDesc& desc, const void* data) {
		uint32_t mipLevels = 1;
		uint32_t arrayLayers = 1;
		createImageObjects(out, desc, mipLevels, arrayLayers);

		out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (data && desc.dimension == TextureDimension::Tex2D && desc.sampleCount <= 1) {
			const size_t imageSize = imageBytes(desc.format, desc.width, desc.height);
			VmaAllocation stagingMemory = VK_NULL_HANDLE;
			const VkBuffer staging = createStagingBuffer(imageSize, stagingMemory, data);
			executeImmediate([&](VkCommandBuffer cmd) {
				transitionImageLayout(cmd, out.image, out.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, arrayLayers, mipLevels);
				out.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				copyBufferToImage(cmd, staging, out.image, desc.width, desc.height);
				if (mipLevels > 1) {
					generateMipmaps(cmd, out);
				}
				else {
					transitionImageLayout(cmd, out.image, out.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, arrayLayers, mipLevels);
					out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			});
			destroyStagingBuffer(staging, stagingMemory);
		}
		else {
			// Render passes leave their attachments in SHADER_READ_ONLY, so a sampled texture rests there from
			// the start. Otherwise one read before anything renders into it -- a history buffer on its first
			// frame -- is sampled while still in COLOR_ATTACHMENT.
			const VkImageLayout targetLayout = (desc.usage & TextureUsage::Sampled)
				? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: (desc.usage & TextureUsage::DepthStencilAttachment)
					? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
					: ((desc.usage & TextureUsage::ColorAttachment)
						? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
						: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			// Zero, as OpenGL drivers hand it back. Uninitialised memory read by a pass before anything
			// rendered into it made early frames differ between runs, and TAA kept the difference.
			executeImmediate([&](VkCommandBuffer cmd) {
				transitionImageLayout(cmd, out.image, out.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, arrayLayers, mipLevels);
				VkImageSubresourceRange range{};
				range.aspectMask = aspectMaskForFormat(out.format);
				range.levelCount = mipLevels;
				range.layerCount = arrayLayers;
				if (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
					// A block compressed image cannot be cleared; its levels are all written before it is read.
					if (!isBlockCompressed(desc.format)) {
						const VkClearColorValue zero{};
						vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
					}
				}
				else {
					const VkClearDepthStencilValue zero{};
					vkCmdClearDepthStencilImage(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
				}
				transitionImageLayout(cmd, out.image, out.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, targetLayout, arrayLayers, mipLevels);
			});
			out.layout = targetLayout;
		}
	}

	void VulkanRHI::destroyTextureInternal(VkTextureResource& texture) {
		if (texture.view != VK_NULL_HANDLE) {
			vkDestroyImageView(m_device, texture.view, nullptr);
		}
		if (texture.image != VK_NULL_HANDLE) {
			vmaDestroyImage(m_allocator, texture.image, texture.allocation);
		}
	}

	namespace {
		// The usage Vulkan needs on top of what the caller asked for.
		TextureDesc vulkanTextureDesc(const TextureDesc& desc, const bool hasData) {
			TextureDesc fixed = desc;
			if (hasData || (fixed.usage & TextureUsage::Sampled)) {
				fixed.usage = fixed.usage | TextureUsage::TransferDst;
			}
			if (fixed.dimension == TextureDimension::Cube) {
				// Cubes are filled via updateCubeFace; always allow transfer + sampling.
				fixed.usage = fixed.usage | TextureUsage::TransferDst | TextureUsage::Sampled;
			}
			if (fixed.mipLevels > 1) {
				fixed.usage = fixed.usage | TextureUsage::TransferSrc | TextureUsage::TransferDst;
			}
			// Blit / layout transitions between passes need transfer usage on RT textures.
			if ((fixed.usage & TextureUsage::ColorAttachment) || (fixed.usage & TextureUsage::DepthStencilAttachment)) {
				fixed.usage = fixed.usage | TextureUsage::TransferSrc | TextureUsage::TransferDst;
			}
			if (!(fixed.usage & TextureUsage::Sampled) && !(fixed.usage & TextureUsage::ColorAttachment)
				&& !(fixed.usage & TextureUsage::DepthStencilAttachment)) {
				fixed.usage = fixed.usage | TextureUsage::Sampled;
			}
			return fixed;
		}
	}

	TextureHandle VulkanRHI::createTexture(const TextureDesc& desc, const void* data) {
		if (desc.dimension == TextureDimension::Tex3D) {
			RhiLog::error("VulkanRHI: Tex3D textures not supported yet");
			return {};
		}

		const TextureDesc fixed = vulkanTextureDesc(desc, data != nullptr);
		VkTextureResource resource{};
		resource.live = true;
		createImage(resource, fixed, data);
		if (resource.image != VK_NULL_HANDLE) {
			trackTextureMemory(resource.desc, true);
		}

		TextureHandle handle{};
		handle.id = storeResource(m_textures, m_freeTextures, std::move(resource));
		return handle;
	}

	namespace {
		// Texture copies the loading threads have on the graphics queue at once, bytes. Each waits for its own fence, so
		// without a cap a dozen loaders finishing together put their copies all ahead of the next frame, and the frame
		// waited 50-200 ms for them (a 2K texture with its mips is 22 MB). Past the cap a loader waits for those before
		// it, which spreads the copies over frames; one bigger than the cap goes alone.
		constexpr size_t kTextureCopiesAtOnce = 32u << 20;
		std::mutex gTextureCopiesMutex;
		std::condition_variable gTextureCopiesDone;
		size_t gTextureCopiesBytes = 0;

		struct VulkanPreparedTexture : PreparedTexture {
			VkTextureResource resource;
			std::function<void(VkTextureResource&)> destroy;
			~VulkanPreparedTexture() override {
				if (resource.image != VK_NULL_HANDLE) {
					destroy(resource);
				}
			}
		};
	}

	std::unique_ptr<PreparedTexture> VulkanRHI::prepareTexture(const TextureDesc& desc, const std::vector<const void*>& levels) {
		const uint32_t levelCount = (std::max)(1u, desc.mipLevels);
		if (desc.dimension != TextureDimension::Tex2D || desc.sampleCount > 1 || levels.size() != levelCount) {
			return nullptr;
		}
		auto prepared = std::make_unique<VulkanPreparedTexture>();
		prepared->destroy = [this](VkTextureResource& resource) { destroyTextureInternal(resource); };
		auto& resource = prepared->resource;
		resource.live = true;
		uint32_t mipLevels = 1;
		uint32_t arrayLayers = 1;
		createImageObjects(resource, vulkanTextureDesc(desc, true), mipLevels, arrayLayers);

		// Every level in one staging buffer, one after another, and one command buffer copying them all.
		std::vector<VkBufferImageCopy> regions(levelCount);
		std::vector<size_t> levelBytes(levelCount);
		size_t total = 0;
		for (uint32_t level = 0; level < levelCount; ++level) {
			const uint32_t w = (std::max)(1u, desc.width >> level);
			const uint32_t h = (std::max)(1u, desc.height >> level);
			auto& region = regions[level];
			region.bufferOffset = total;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = level;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = { w, h, 1 };
			levelBytes[level] = imageBytes(desc.format, w, h);
			// A block compressed copy starts on a whole block, which the 16 bytes keep for both sizes.
			total += (levelBytes[level] + 15u) & ~size_t{ 15 };
		}
		VmaAllocation stagingMemory = VK_NULL_HANDLE;
		const VkBuffer staging = createStagingBuffer(total, stagingMemory, nullptr);
		void* mapped = nullptr;
		vmaMapMemory(m_allocator, stagingMemory, &mapped);
		for (uint32_t level = 0; level < levelCount; ++level) {
			std::memcpy(static_cast<char*>(mapped) + regions[level].bufferOffset, levels[level], levelBytes[level]);
		}
		vmaFlushAllocation(m_allocator, stagingMemory, 0, total);
		vmaUnmapMemory(m_allocator, stagingMemory);

		// This thread's own pool and fence: the shared ones belong to the device thread.
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
		VkCommandPool pool = VK_NULL_HANDLE;
		vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool);
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence = VK_NULL_HANDLE;
		vkCreateFence(m_device, &fenceInfo, nullptr, &fence);
		{
			std::unique_lock lock(gTextureCopiesMutex);
			gTextureCopiesDone.wait(lock, [&] { return gTextureCopiesBytes == 0 || gTextureCopiesBytes + total <= kTextureCopiesAtOnce; });
			gTextureCopiesBytes += total;
		}
		executeImmediate([&](VkCommandBuffer cmd) {
			transitionImageLayout(cmd, resource.image, resource.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, arrayLayers, mipLevels);
			vkCmdCopyBufferToImage(cmd, staging, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levelCount, regions.data());
			transitionImageLayout(cmd, resource.image, resource.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, arrayLayers, mipLevels);
		}, pool, fence);
		{
			std::lock_guard lock(gTextureCopiesMutex);
			gTextureCopiesBytes -= total;
		}
		gTextureCopiesDone.notify_all();
		vkDestroyFence(m_device, fence, nullptr);
		vkDestroyCommandPool(m_device, pool, nullptr);
		destroyStagingBuffer(staging, stagingMemory);
		resource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		return prepared;
	}

	TextureHandle VulkanRHI::adoptTexture(std::unique_ptr<PreparedTexture> texture) {
		auto* prepared = dynamic_cast<VulkanPreparedTexture*>(texture.get());
		if (!prepared || prepared->resource.image == VK_NULL_HANDLE) {
			return {};
		}
		trackTextureMemory(prepared->resource.desc, true);
		TextureHandle handle{};
		handle.id = storeResource(m_textures, m_freeTextures, std::move(prepared->resource));
		prepared->resource = {};
		return handle;
	}

	void VulkanRHI::destroyTexture(const TextureHandle texture) {
		if (auto* tex = getLive(m_textures, texture.id)) {
			if (tex->image != VK_NULL_HANDLE) {
				trackTextureMemory(tex->desc, false);
			}
			if (m_inited) {
				m_retiredTextures.push_back({ *tex, kMaxFramesInFlight });
			}
			else {
				destroyTextureInternal(*tex);
			}
			recycleResource(m_textures, m_freeTextures, texture.id);
		}
	}

	void VulkanRHI::updateTexture(const TextureHandle tex, const uint32_t x, const uint32_t y,
		const uint32_t w, const uint32_t h, const TextureFormat, const void* data, const uint32_t mip, const uint32_t layer)
	{
		auto* texture = getLive(m_textures, tex.id);
		if (!texture || !data || w == 0 || h == 0) {
			return;
		}
		const size_t imageSize = imageBytes(texture->desc.format, w, h);
		if (queueTextureUpload(tex.id, x, y, w, h, mip, layer, data, imageSize)) {
			return;
		}
		VmaAllocation stagingMemory = VK_NULL_HANDLE;
		const VkBuffer staging = createStagingBuffer(imageSize, stagingMemory, data);
		executeImmediate([&](VkCommandBuffer cmd) {
			recordTextureUploads(cmd);
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy region{};
			region.bufferOffset = 0;
			region.imageSubresource.aspectMask = aspectMaskForFormat(texture->format);
			region.imageSubresource.mipLevel = mip;
			region.imageSubresource.baseArrayLayer = layer;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { static_cast<int32_t>(x), static_cast<int32_t>(y), 0 };
			region.imageExtent = { w, h, 1 };
			vkCmdCopyBufferToImage(cmd, staging, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
		destroyStagingBuffer(staging, stagingMemory);
	}

	void VulkanRHI::updateCubeFace(const TextureHandle tex, const uint32_t face, const uint32_t mip, const void* data) {
		auto* texture = getLive(m_textures, tex.id);
		if (!texture || !data || texture->desc.dimension != TextureDimension::Cube || face > 5) {
			return;
		}
		uint32_t w = texture->desc.width >> mip;
		uint32_t h = texture->desc.height >> mip;
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		const size_t imageSize = static_cast<size_t>(w) * h * bytesPerPixel(texture->desc.format);
		VmaAllocation stagingMemory = VK_NULL_HANDLE;
		const VkBuffer staging = createStagingBuffer(imageSize, stagingMemory, data);
		executeImmediate([&](VkCommandBuffer cmd) {
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copyBufferToImage(cmd, staging, texture->image, w, h, face, mip);
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
		destroyStagingBuffer(staging, stagingMemory);
	}

	void VulkanRHI::updateCubeMip(const TextureHandle tex, const uint32_t mip, const void* packed6Faces, const size_t faceBytes) {
		auto* texture = getLive(m_textures, tex.id);
		if (!texture || !packed6Faces || faceBytes == 0 || texture->desc.dimension != TextureDimension::Cube) {
			return;
		}
		uint32_t w = texture->desc.width >> mip;
		uint32_t h = texture->desc.height >> mip;
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		const size_t total = faceBytes * 6;
		VmaAllocation stagingMemory = VK_NULL_HANDLE;
		const VkBuffer staging = createStagingBuffer(total, stagingMemory, packed6Faces);
		executeImmediate([&](VkCommandBuffer cmd) {
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			for (uint32_t face = 0; face < 6; ++face) {
				VkBufferImageCopy region{};
				region.bufferOffset = static_cast<VkDeviceSize>(face) * faceBytes;
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.mipLevel = mip;
				region.imageSubresource.baseArrayLayer = face;
				region.imageSubresource.layerCount = 1;
				region.imageExtent = { w, h, 1 };
				vkCmdCopyBufferToImage(cmd, staging, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}
			ensureTextureLayout(cmd, *texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
		destroyStagingBuffer(staging, stagingMemory);
	}

	uint64_t VulkanRHI::getNativeTextureId(const TextureHandle tex) const {
		if (const auto* texture = getLive(m_textures, tex.id)) {
			return reinterpret_cast<uint64_t>(texture->view);
		}
		return 0;
	}

	SamplerHandle VulkanRHI::createSampler(const SamplerDesc& desc) {
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = toVkFilter(desc.magFilter);
		samplerInfo.minFilter = toVkFilter(desc.minFilter);
		const bool wantMips = desc.minFilter == FilterMode::LinearMipmapLinear;
		samplerInfo.mipmapMode = wantMips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.minLod = 0.f;
		samplerInfo.maxLod = wantMips ? VK_LOD_CLAMP_NONE : 0.f;
		samplerInfo.addressModeU = toVkAddressMode(desc.addressU);
		samplerInfo.addressModeV = toVkAddressMode(desc.addressV);
		samplerInfo.addressModeW = toVkAddressMode(desc.addressW);
		const bool aniso = m_samplerAnisotropy && desc.maxAnisotropy > 1.f;
		samplerInfo.anisotropyEnable = aniso ? VK_TRUE : VK_FALSE;
		samplerInfo.maxAnisotropy = aniso ? (std::min)(desc.maxAnisotropy, m_maxAnisotropy) : 1.f;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = desc.compareOp != CompareOp::None ? VK_TRUE : VK_FALSE;
		samplerInfo.compareOp = toVkCompareOp(desc.compareOp);

		VkSamplerResource resource{};
		resource.desc = desc;
		if (vkCreateSampler(m_device, &samplerInfo, nullptr, &resource.sampler) != VK_SUCCESS) {
			RhiLog::error("VulkanRHI: vkCreateSampler failed");
			return {};
		}
		resource.live = true;
		SamplerHandle handle{};
		handle.id = storeResource(m_samplers, m_freeSamplers, std::move(resource));
		return handle;
	}

	void VulkanRHI::retire(std::function<void()> destroy) {
		if (m_inited) {
			m_retiredObjects.push_back({ std::move(destroy), kMaxFramesInFlight });
		}
		else {
			destroy();
		}
	}

	void VulkanRHI::destroySampler(const SamplerHandle sampler) {
		if (auto* samp = getLive(m_samplers, sampler.id)) {
			if (samp->sampler != VK_NULL_HANDLE) {
				retire([device = m_device, vkSampler = samp->sampler] { vkDestroySampler(device, vkSampler, nullptr); });
			}
			recycleResource(m_samplers, m_freeSamplers, sampler.id);
		}
	}

	DescriptorSetLayoutHandle VulkanRHI::createDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
		auto makeVkLayout = [&](const std::vector<DescriptorSetLayoutBinding>& src, VkDescriptorSetLayout& out) -> bool {
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			bindings.reserve(src.size());
			for (const auto& binding : src) {
				VkDescriptorSetLayoutBinding vkBinding{};
				vkBinding.binding = binding.binding;
				vkBinding.descriptorType = toVkDescriptorType(binding.type);
				vkBinding.descriptorCount = binding.count;
				vkBinding.stageFlags = toVkShaderStage(binding.stageMask);
				bindings.push_back(vkBinding);
			}
			VkDescriptorSetLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			layoutInfo.pBindings = bindings.empty() ? nullptr : bindings.data();
			return vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &out) == VK_SUCCESS;
		};

		// A repeated binding number used to be answered by splitting the layout into two Vulkan sets,
		// images in one and buffers in the other, which silently shifted the set index of every layout
		// after it: a shader written SFE_SET(1, ...) was then right on OpenGL and wrong on Vulkan.
		// Nothing ever used it -- every layout and every shader here keeps its binding numbers distinct
		// -- so the shape is rejected instead of quietly reinterpreted.
		if (descriptorLayoutHasDuplicateBinding(desc)) {
			RhiLog::error(
				"createDescriptorSetLayout: a binding number is declared twice; one binding, one descriptor");
			return {};
		}

		VkDescriptorSetLayoutResource resource{};
		resource.desc = desc;
		if (!makeVkLayout(desc.bindings, resource.layout)) {
			return {};
		}

		resource.live = true;
		DescriptorSetLayoutHandle handle{};
		handle.id = storeResource(m_descriptorSetLayouts, m_freeDescriptorSetLayouts, std::move(resource));
		return handle;
	}

	void VulkanRHI::destroyDescriptorSetLayout(const DescriptorSetLayoutHandle layout) {
		if (auto* layoutObj = getLive(m_descriptorSetLayouts, layout.id)) {
			if (layoutObj->layout != VK_NULL_HANDLE) {
				retire([device = m_device, vkLayout = layoutObj->layout] { vkDestroyDescriptorSetLayout(device, vkLayout, nullptr); });
			}
			recycleResource(m_descriptorSetLayouts, m_freeDescriptorSetLayouts, layout.id);
		}
	}

	DescriptorSetHandle VulkanRHI::allocateDescriptorSet(const DescriptorSetLayoutHandle layout) {
		const auto* layoutObj = getLive(m_descriptorSetLayouts, layout.id);
		if (!layoutObj) {
			return {};
		}

		VkDescriptorSetResource resource{};
		resource.layoutHandle = layout;
		resource.set = allocateVkDescriptorSet(layoutObj->layout, resource.pool);
		if (resource.set == VK_NULL_HANDLE) {
			return {};
		}
		resource.live = true;
		DescriptorSetHandle handle{};
		handle.id = storeResource(m_descriptorSets, m_freeDescriptorSets, std::move(resource));
		return handle;
	}

	void VulkanRHI::freeDescriptorSet(const DescriptorSetHandle set) {
		if (auto* setObj = getLive(m_descriptorSets, set.id)) {
			if (setObj->set != VK_NULL_HANDLE) {
				retire([device = m_device, pool = setObj->pool, vkSet = setObj->set] { vkFreeDescriptorSets(device, pool, 1, &vkSet); });
			}
			recycleResource(m_descriptorSets, m_freeDescriptorSets, set.id);
		}
	}

	bool VulkanRHI::boundInUnfinishedList(const VkDescriptorSet set) const {
		const auto binds = [&](const CommandListHandle handle) {
			const auto* list = getLive(m_commandLists, handle.id);
			return list && std::find(list->boundSets.begin(), list->boundSets.end(), set) != list->boundSets.end();
		};
		if ((m_openCommandList && binds(m_openCommandList)) || (m_hasPendingScene && binds(m_pendingSceneCmd))) {
			return true;
		}
		for (const auto& inflight : m_inflightSceneCmd) {
			if (inflight && binds(inflight)) {
				return true;
			}
		}
		return false;
	}

	// Writing a set that is bound in an unfinished command buffer invalidates that buffer. Give the handle
	// a fresh copy instead: binds already recorded keep the old contents, later binds see the write --
	// what OpenGL does, since it captures a set's contents when the bind is recorded.
	void VulkanRHI::detachBoundSet(VkDescriptorSetResource& set, const VkDescriptorSetLayoutResource& layout) {
		VkDescriptorPool copyPool = VK_NULL_HANDLE;
		const VkDescriptorSet copy = allocateVkDescriptorSet(layout.layout, copyPool);
		if (copy == VK_NULL_HANDLE) {
			RhiLog::error("VulkanRHI: no descriptor set to detach into, writing a bound set in place");
			return;
		}

		std::vector<VkCopyDescriptorSet> copies;
		copies.reserve(layout.desc.bindings.size());
		for (const auto& binding : layout.desc.bindings) {
			VkCopyDescriptorSet c{};
			c.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
			c.srcSet = set.set;
			c.srcBinding = binding.binding;
			c.dstSet = copy;
			c.dstBinding = binding.binding;
			c.descriptorCount = (std::max)(1u, binding.count);
			copies.push_back(c);
		}
		vkUpdateDescriptorSets(m_device, 0, nullptr, static_cast<uint32_t>(copies.size()), copies.data());

		m_retiredSets[m_currentFrame].push_back({ set.pool, set.set });
		set.set = copy;
		set.pool = copyPool;
	}

	void VulkanRHI::updateDescriptorSets(const std::vector<WriteDescriptor>& writes) {
		// Scratch kept between calls, so a frame's writes do not allocate.
		auto& vkWrites = m_writeScratch.writes;
		auto& bufferInfos = m_writeScratch.buffers;
		auto& imageInfos = m_writeScratch.images;
		auto& spans = m_writeScratch.spans;
		vkWrites.clear();
		bufferInfos.clear();
		imageInfos.clear();
		spans.clear();

		for (const auto& write : writes) {
			auto* setObj = getLive(m_descriptorSets, write.set.id);
			if (!setObj) {
				continue;
			}

			// The layout fixes the descriptor type at every binding. Passing the caller's type straight
			// through meant a write that disagreed with the layout reached the driver, which then read
			// pImageInfo where the layout promised a buffer and took an access violation -- the same
			// input that OpenGL used to accept and silently ignore.
			const auto* layoutObj = getLive(m_descriptorSetLayouts, setObj->layoutHandle.id);
			if (!layoutObj) {
				continue;
			}
			// The layout defines the binding's type, and binding numbers are unique within it.
			const DescriptorSetLayoutBinding* declared = nullptr;
			for (const auto& b : layoutObj->desc.bindings) {
				if (b.binding == write.binding) {
					declared = &b;
					break;
				}
			}
			if (!declared) {
				RhiLog::error("updateDescriptorSets: set has no binding %u", write.binding);
				continue;
			}
			const DescriptorType type = declared->type;

			std::vector<uint64_t> contents;
			if (type == DescriptorType::CombinedImageSampler) {
				for (const auto& image : write.images) {
					contents.push_back(image.texture.id);
					contents.push_back(image.sampler.id);
				}
			}
			else {
				for (const auto& buf : write.buffers) {
					contents.push_back(buf.buffer.id);
					contents.push_back(buf.offset);
					contents.push_back(buf.range);
				}
			}
			auto written = std::find_if(setObj->written.begin(), setObj->written.end(),
				[&](const auto& entry) { return entry.first == write.binding; });
			if (written != setObj->written.end() && written->second == contents) {
				continue;
			}

			if (boundInUnfinishedList(setObj->set)) {
				detachBoundSet(*setObj, *layoutObj);
			}

			VkWriteDescriptorSet vkWrite{};
			vkWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			vkWrite.dstSet = setObj->set;
			vkWrite.dstBinding = write.binding;
			vkWrite.descriptorType = toVkDescriptorType(type);


			// The whole array in one write, element 0 upwards -- WriteDescriptor has no
			// dstArrayElement, so a partial update is not expressible. A dead handle anywhere rejects
			// the write rather than shortening it, since a short array would silently shift every
			// element after the gap and leave the tail never written, which is a device loss on the
			// first draw that reads it.
			if (type == DescriptorType::CombinedImageSampler) {
				if (write.images.empty()) {
					continue;
				}
				const size_t first = imageInfos.size();
				bool ok = true;
				for (const auto& image : write.images) {
					const auto* tex = getLive(m_textures, image.texture.id);
					if (!tex) {
						ok = false;
						break;
					}
					const auto* samp = getLive(m_samplers, image.sampler.id);
					VkDescriptorImageInfo info{};
					info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					info.imageView = tex->view;
					info.sampler = samp ? samp->sampler : VK_NULL_HANDLE;
					imageInfos.push_back(info);
				}
				if (!ok) {
					imageInfos.resize(first);
					continue;
				}
				vkWrite.descriptorCount = static_cast<uint32_t>(imageInfos.size() - first);
				spans.push_back({ first, true });
			}
			else {
				if (write.buffers.empty()) {
					continue;
				}
				const size_t first = bufferInfos.size();
				bool ok = true;
				for (const auto& buf : write.buffers) {
					const auto* buffer = getLive(m_buffers, buf.buffer.id);
					if (!buffer) {
						ok = false;
						break;
					}
					if (isDynamicDescriptor(type) && buf.range == 0) {
						// The whole buffer plus a dynamic offset past zero would run off its end.
						RhiLog::error("updateDescriptorSets: dynamic binding %u needs a range", write.binding);
						ok = false;
						break;
					}
					VkDescriptorBufferInfo info{};
					info.buffer = buffer->buffer;
					info.offset = buf.offset;
					info.range = buf.range > 0 ? buf.range : buffer->desc.size;
					bufferInfos.push_back(info);
				}
				if (!ok) {
					bufferInfos.resize(first);
					continue;
				}
				vkWrite.descriptorCount = static_cast<uint32_t>(bufferInfos.size() - first);
				spans.push_back({ first, false });
			}
			vkWrites.push_back(vkWrite);
			if (written != setObj->written.end()) {
				written->second = std::move(contents);
			}
			else {
				setObj->written.emplace_back(write.binding, std::move(contents));
			}
		}

		for (size_t i = 0; i < vkWrites.size(); ++i) {
			if (spans[i].images) {
				vkWrites[i].pImageInfo = imageInfos.data() + spans[i].first;
			}
			else {
				vkWrites[i].pBufferInfo = bufferInfos.data() + spans[i].first;
			}
		}

		if (!vkWrites.empty()) {
			vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(vkWrites.size()), vkWrites.data(), 0, nullptr);
		}
	}

	CommandListHandle VulkanRHI::beginCommandList(const CommandListType type) {
		prepareInlinePresent();
		// Every list of a frame reuses m_sceneCmds[m_currentFrame]; resetting it while the previous
		// submit still executes is undefined.
		finishPendingScene();
		if (m_openCommandList) {
			RhiLog::error("beginCommandList: the previous command list was never submitted");
		}

		VkRecordedCommandList list{};
		list.type = type;
		list.recording = true;
		list.live = true;
		if (m_sceneCmds[m_currentFrame] != VK_NULL_HANDLE) {
			list.cmd = m_sceneCmds[m_currentFrame];
			vkResetCommandBuffer(list.cmd, 0);
		}
		else {
			VkCommandBufferAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocInfo.commandPool = m_sceneCommandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			if (vkAllocateCommandBuffers(m_device, &allocInfo, &list.cmd) != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: failed to allocate command buffer");
				return {};
			}
		}

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(list.cmd, &beginInfo) != VK_SUCCESS) {
			RhiLog::fatal("VulkanRHI: vkBeginCommandBuffer failed");
		}
		flushBufferUploads(list.cmd);
		if (m_timestampPool != VK_NULL_HANDLE && !m_timestampStartWritten[m_currentFrame]) {
			vkCmdWriteTimestamp(list.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				m_timestampPool, m_currentFrame * kTimestampsPerFrame);
			m_timestampStartWritten[m_currentFrame] = true;
		}

		CommandListHandle handle{};
		handle.id = storeResource(m_commandLists, m_freeCommandLists, std::move(list));
		m_openCommandList = handle;
		return handle;
	}

	void VulkanRHI::endCommandList(const CommandListHandle cmd) {
		if (auto* list = getLive(m_commandLists, cmd.id)) {
			if (list->recording && list->cmd != VK_NULL_HANDLE) {
				for (const auto& barrier : list->barriers) {
					VkMemoryBarrier memoryBarrier{};
					memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
					memoryBarrier.srcAccessMask = toVkAccess(barrier.srcAccess);
					memoryBarrier.dstAccessMask = toVkAccess(barrier.dstAccess);
					vkCmdPipelineBarrier(list->cmd,
						toVkPipelineStage(barrier.srcStage),
						toVkPipelineStage(barrier.dstStage),
						0,
						1, &memoryBarrier,
						0, nullptr,
						0, nullptr);
				}
				list->barriers.clear();
				if (list->activeFramebuffer) {
					vkCmdEndRenderPass(list->cmd);
					if (auto* fb = getLive(m_framebuffers, list->activeFramebuffer.id)) {
						for (const auto& color : fb->desc.colorAttachments) {
							if (auto* tex = getLive(m_textures, color.texture.id)) {
								tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							}
						}
						if (fb->desc.depthStencilAttachment.has_value()) {
							if (auto* tex = getLive(m_textures, fb->desc.depthStencilAttachment->texture.id)) {
								tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							}
						}
					}
					list->activeFramebuffer = {};
				}
				if (m_swapchainAcquired) {
					if (!m_swapchainBlitted) {
						recordPresentBlit(list->cmd, m_acquiredImageIndex);
						if (m_acquiredImageIndex < m_swapchainLayouts.size()) {
							m_swapchainLayouts[m_acquiredImageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
						}
						m_swapchainBlitted = true;
					}
					if (m_swapchainBlitted && m_acquiredImageIndex < m_swapchainImages.size()
						&& m_acquiredImageIndex < m_swapchainLayouts.size()) {
						auto& layout = m_swapchainLayouts[m_acquiredImageIndex];
						if (layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
							transitionImageLayout(list->cmd, m_swapchainImages[m_acquiredImageIndex],
								m_swapchainFormat, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
							layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
						}
					}
					m_inlinePresent = true;
				}
				if (m_timestampPool != VK_NULL_HANDLE) {
					// A query already written this frame must be reset before it is written again; the
					// earlier list has finished, because beginCommandList waits for it.
					if (m_timestampEndWritten[m_currentFrame]) {
						vkResetQueryPool(m_device, m_timestampPool, m_currentFrame * kTimestampsPerFrame + 1, 1);
					}
					m_timestampEndWritten[m_currentFrame] = true;
					vkCmdWriteTimestamp(list->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						m_timestampPool, m_currentFrame * kTimestampsPerFrame + 1);
					m_timestampPending[m_currentFrame] = true;
				}
				if (vkEndCommandBuffer(list->cmd) != VK_SUCCESS) {
					RhiLog::fatal("VulkanRHI: vkEndCommandBuffer failed");
				}
			}
			list->recording = false;
		}
	}

	void VulkanRHI::cmdPipelineBarrier(const CommandListHandle cmd, const PipelineBarrier& barrier) {
		auto* list = getLive(m_commandLists, cmd.id);
		if (!list || !list->recording || list->cmd == VK_NULL_HANDLE) {
			return;
		}

		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = toVkAccess(barrier.srcAccess);
		memoryBarrier.dstAccessMask = toVkAccess(barrier.dstAccess);
		vkCmdPipelineBarrier(list->cmd,
			toVkPipelineStage(barrier.srcStage),
			toVkPipelineStage(barrier.dstStage),
			0,
			1, &memoryBarrier,
			0, nullptr,
			0, nullptr);
	}

	void VulkanRHI::submit(const CommandListHandle handle) {
		if (handle.id == m_openCommandList.id) {
			m_openCommandList = {};
		}
		auto* list = getLive(m_commandLists, handle.id);
		if (!list || list->cmd == VK_NULL_HANDLE) {
			return;
		}

		if (m_inlinePresent) {
			m_inflightSceneCmd[m_currentFrame] = handle;
			submitInlinePresent(list->cmd);
			list->submitted = true;
			return;
		}

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &list->cmd;
		{
			std::lock_guard lock(m_queueMutex);
			const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_sceneFence);
			if (submitResult != VK_SUCCESS) {
				RhiLog::fatal("VulkanRHI: scene submit failed (%d)", static_cast<int>(submitResult));
			}
		}

		m_hasPendingScene = true;
		m_pendingSceneCmd = handle;
		list->submitted = true;
	}

	void VulkanRHI::cmdUpdateBuffer(const CommandListHandle cmd, const BufferHandle handle,
		const size_t offset, const size_t size, const void* data)
	{
		const auto* list = getLive(m_commandLists, cmd.id);
		const auto* buffer = getLive(m_buffers, handle.id);
		if (!list || !list->recording || !buffer || !data || size == 0) {
			return;
		}
		if (offset + size > buffer->desc.size) {
			RhiLog::error("VulkanRHI: cmdUpdateBuffer OOB offset=%zu size=%zu cap=%zu",
				offset, size, static_cast<size_t>(buffer->desc.size));
			return;
		}
		if (buffer->mapped) {
			updateBuffer(handle, offset, size, data);
			return;
		}
		// Not queued like updateBuffer does for device-local buffers: that lands in the next list.
		copyThroughStaging(buffer->buffer, offset, size, data);
	}

	void VulkanRHI::cmdCopyBuffer(const CommandListHandle cmd, const BufferHandle src, const BufferHandle dst,
		const size_t srcOffset, const size_t dstOffset, const size_t size)
	{
		const auto* list = getLive(m_commandLists, cmd.id);
		const auto* source = getLive(m_buffers, src.id);
		const auto* destination = getLive(m_buffers, dst.id);
		if (!list || !list->recording || !source || !destination || size == 0) {
			return;
		}
		if (srcOffset + size > source->desc.size || dstOffset + size > destination->desc.size) {
			RhiLog::error("VulkanRHI: cmdCopyBuffer OOB src=%zu dst=%zu size=%zu", srcOffset, dstOffset, size);
			return;
		}
		VkBufferCopy region{};
		region.srcOffset = srcOffset;
		region.dstOffset = dstOffset;
		region.size = size;
		vkCmdCopyBuffer(list->cmd, source->buffer, destination->buffer, 1, &region);
	}

	void VulkanRHI::writeGpuMark(const CommandListHandle cmd, const char* label, const bool endsPass) {
		auto* list = getLive(m_commandLists, cmd.id);
		auto& count = m_markCount[m_currentFrame];
		if (!list || m_markPool == VK_NULL_HANDLE || count >= (endsPass ? kMaxGpuMarks : kGpuShaderMarks)) {
			return;
		}
		// SFE_GPU_SERIAL set: what comes after a pass waits for all of it to end, so a pass's span is its own work alone,
		// not shared with the passes it overlaps or a wait it falls on. The frame is slower so; only its parts are true.
		static const bool serial = std::getenv("SFE_GPU_SERIAL") != nullptr;
		if (serial && endsPass && !list->activeFramebuffer) {
			VkMemoryBarrier all{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			all.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			all.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			vkCmdPipelineBarrier(list->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all,
				0, nullptr, 0, nullptr);
		}
		vkCmdWriteTimestamp(list->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_markPool, m_currentFrame * kMaxGpuMarks + count);
		m_markLabels[m_currentFrame][count] = { label, endsPass };
		++count;
	}

	void VulkanRHI::cmdBeginRegion(const CommandListHandle cmd, const char* name) {
		static const auto begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
			vkGetInstanceProcAddr(m_instance, "vkCmdBeginDebugUtilsLabelEXT"));
		auto* list = getLive(m_commandLists, cmd.id);
		if (begin && list && name) {
			VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
			label.pLabelName = name;
			begin(list->cmd, &label);
		}
	}

	void VulkanRHI::cmdEndRegion(const CommandListHandle cmd) {
		static const auto end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
			vkGetInstanceProcAddr(m_instance, "vkCmdEndDebugUtilsLabelEXT"));
		if (auto* list = getLive(m_commandLists, cmd.id); end && list) {
			end(list->cmd);
		}
	}
}

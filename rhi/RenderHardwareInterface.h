#pragma once
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rhi/GraphicsBackend.h"
#include "rhi/RhiTypes.h"

struct GLFWwindow;

namespace SFE::Render
{
#define IS_VALID_FUNC operator bool() const { return id != UINT32_MAX; }

	// A handle id is a pool slot in the low bits and the slot's generation in the high bits. Destroyed
	// slots are handed out again, but with a new generation, so an id cached for a destroyed resource never
	// equals the id of the one that took its slot.
	inline constexpr uint32_t kHandleSlotBits = 20;
	inline constexpr uint32_t kHandleSlotMask = (1u << kHandleSlotBits) - 1;
	inline constexpr uint32_t kHandleGenerationMask = (1u << (32 - kHandleSlotBits)) - 1;
	inline constexpr uint32_t handleSlot(uint32_t id) { return id & kHandleSlotMask; }
	inline constexpr uint32_t handleGeneration(uint32_t id) { return id >> kHandleSlotBits; }

	// Handles
	struct DescriptorSetLayoutHandle	{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct DescriptorSetHandle			{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct TextureHandle				{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct SamplerHandle				{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct ShaderModuleHandle			{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct BufferHandle					{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct RenderPassHandle				{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct PipelineHandle				{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct FramebufferHandle			{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };
	struct CommandListHandle			{ uint32_t id = UINT32_MAX; IS_VALID_FUNC };

	// Enums
	enum class IndexType : uint8_t { Uint16, Uint32 };

	enum class ShaderStage : uint8_t { Vertex = 1 << 0, Fragment = 1 << 1, Geometry = 1 << 2, TessControl = 1 << 3, TessEval = 1 << 4, Compute = 1 << 5, };

	inline ShaderStage operator|(ShaderStage a, ShaderStage b) { return static_cast<ShaderStage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
	inline bool operator&(ShaderStage a, ShaderStage b) { return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0; }

	enum class TextureDimension : uint8_t { Tex2D, Tex3D, Array, Cube };

	enum class TextureFormat : uint8_t {
		R8,
		R8_SNORM,
		R16,
		R16_SNORM,
		RG8,
		RG8_SNORM,
		RG16,
		RG16_SNORM,
		R3_G3_B2,
		RGB4,
		RGB5,
		RGB8,
		RGB8_SNORM,
		RGB10,
		RGB12,
		RGB16_SNORM,
		RGBA2,
		RGBA4,
		RGB5_A1,
		RGBA8,
		RGBA8_SNORM,
		RGB10_A2,
		RGB10_A2UI,
		RGBA12,
		RGBA16,
		SRGB8,
		SRGB8_ALPHA8,
		R16F,
		RG16F,
		RGB16F,
		RGBA16F,
		R32F,
		RG32F,
		RGB32F,
		RGBA32F,
		R11F_G11F_B10F,
		RGB9_E5,
		R8I,
		R8UI,
		R16I,
		R16UI,
		R32I,
		R32UI,
		RG8I,
		RG8UI,
		RG16I,
		RG16UI,
		RG32I,
		RG32UI,
		RGB8I,
		RGB8UI,
		RGB16I,
		RGB16UI,
		RGB32I,
		RGB32UI,
		RGBA8I,
		RGBA8UI,
		RGBA16I,
		RGBA16UI,
		RGBA32I,
		RGBA32UI,
		DEPTH_COMPONENT32F,
		DEPTH_COMPONENT16,
		DEPTH_COMPONENT24,
		DEPTH_COMPONENT32,
		DEPTH24_STENCIL8,
		DEPTH32F_STENCIL8,
	};

	enum class TextureUsage : uint8_t {
		None = 0,
		Sampled = 1 << 0,
		ColorAttachment = 1 << 1,
		DepthStencilAttachment = 1 << 2,
		TransferSrc = 1 << 3,
		TransferDst = 1 << 4,
	};

	inline TextureUsage operator|(TextureUsage a, TextureUsage b) { return static_cast<TextureUsage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
	inline bool operator&(TextureUsage a, TextureUsage b) { return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0; }

	enum class FilterMode : uint8_t { Nearest, Linear, LinearMipmapLinear };

	enum class AddressMode : uint8_t { Repeat, ClampToEdge, ClampToBorder, MirrorRepeat, MirrorClampToEdge };

	// Flags: a GPU-driven buffer is written by compute as Storage and read by the draw as Indirect.
	enum class BufferUsage : uint8_t {
		None = 0,
		Vertex = 1 << 0,
		Index = 1 << 1,
		Uniform = 1 << 2,
		Storage = 1 << 3,
		Indirect = 1 << 4,
	};

	inline BufferUsage operator|(BufferUsage a, BufferUsage b) { return static_cast<BufferUsage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
	inline bool operator&(BufferUsage a, BufferUsage b) { return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0; }

	enum class CpuAccess : uint8_t {
		None,       // GPU-only
		Write,      // CPU → GPU
		Read,       // GPU → CPU
		ReadWrite
	};

	enum class MapMode : uint8_t { Read, Write, ReadWrite };

	enum class VertexFormat : uint8_t {
		Float,
		Float2,
		Float3,
		Float4,

		Int,
		Int2,
		Int3,
		Int4,

		UInt,
		UInt2,
		UInt3,
		UInt4
	};

	enum class CompareOp : uint8_t {
		None,
		Less,
		LessEqual,
		Greater,
		GreaterEqual,
		Equal,
		NotEqual,
		Always,
		Never,
	};

	enum class VertexInputRate : uint8_t { PerVertex, PerInstance };

	enum class BlitMask : uint8_t { Color = 0x1, Depth = 0x2, Stencil = 0x4 };

	inline BlitMask operator|(BlitMask a, BlitMask b) { return static_cast<BlitMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
	inline bool operator&(BlitMask a, BlitMask b) { return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0; }

	enum class BlitFilter : uint8_t { Nearest, Linear };

	enum class AttachmentLoadOp : uint8_t { Load, Clear, DontCare };

	enum class AttachmentStoreOp : uint8_t { Store, DontCare };

	// LineStrip rather than a line loop: Vulkan has no loop topology, so the closing segment is the
	// caller's job -- repeat the first vertex. The enum used to carry LineLoop, which OpenGL honoured
	// and Vulkan silently downgraded to a strip.
	enum class PrimitiveTopology : uint8_t { Triangles, TriangleStrip, Lines, Points, LineStrip, Patches };

	enum class CullMode : uint8_t { None, Front, Back };

	enum class FrontFace : uint8_t { ClockWise, CounterClockWise };

	enum class PolygonFillMode : uint8_t { Fill, Line };

	enum class BlendFactor : uint8_t { One, Zero, SrcAlpha, OneMinusSrcAlpha };
	
	enum class CommandListType : uint8_t { Graphics, Compute, Transfer };

	enum class PipelineStage : uint32_t {
		None = 0,
		DrawIndirect = 1u << 0,
		VertexInput = 1u << 1,
		VertexShader = 1u << 2,
		FragmentShader = 1u << 3,
		ColorAttachmentOutput = 1u << 4,
		LateFragmentTests = 1u << 5,
		Transfer = 1u << 6,
		Host = 1u << 7,
		AllGraphics = 1u << 8,
		AllCommands = 1u << 9,
		Compute = 1u << 10,
	};

	inline PipelineStage operator|(PipelineStage a, PipelineStage b) {
		return static_cast<PipelineStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}
	inline bool operator&(PipelineStage a, PipelineStage b) {
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	enum class AccessMask : uint32_t {
		None = 0,
		IndirectCommandRead = 1u << 0,
		IndexRead = 1u << 1,
		VertexAttributeRead = 1u << 2,
		UniformRead = 1u << 3,
		ShaderRead = 1u << 4,
		ShaderWrite = 1u << 5,
		ColorAttachmentRead = 1u << 6,
		ColorAttachmentWrite = 1u << 7,
		DepthStencilAttachmentRead = 1u << 8,
		DepthStencilAttachmentWrite = 1u << 9,
		TransferRead = 1u << 10,
		TransferWrite = 1u << 11,
		HostRead = 1u << 12,
		HostWrite = 1u << 13,
	};

	inline AccessMask operator|(AccessMask a, AccessMask b) {
		return static_cast<AccessMask>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}
	inline bool operator&(AccessMask a, AccessMask b) {
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	struct PipelineBarrier {
		PipelineStage srcStage = PipelineStage::AllCommands;
		PipelineStage dstStage = PipelineStage::AllCommands;
		AccessMask srcAccess = AccessMask::None;
		AccessMask dstAccess = AccessMask::None;
	};

	// The *Dynamic buffer types take their offset at cmdBindDescriptorSets instead of at the write, so a set
	// written once can point at a new allocTransient range every frame. Their write must give the range.
	enum class DescriptorType : uint8_t { UniformBuffer, StorageBuffer,	CombinedImageSampler, UniformBufferDynamic, StorageBufferDynamic, };

	inline bool isDynamicDescriptor(DescriptorType type) {
		return type == DescriptorType::UniformBufferDynamic || type == DescriptorType::StorageBufferDynamic;
	}

	// Descriptions
	struct ShaderModuleDesc {
		ShaderStage stage;
		std::string name;     // identifies the shader in messages and, on Vulkan, in the binary cache
		std::string source;   // include-expanded GLSL; both backends compile this text
	};

	struct TextureDesc {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depth = 1;
		uint32_t mipLevels = 1;
		uint32_t arrayLayers = 1;

		TextureDimension dimension = TextureDimension::Tex2D;
		TextureFormat    format = TextureFormat::RGBA8;
		TextureUsage     usage = TextureUsage::Sampled;
		uint32_t         sampleCount = 1; // 1, 2, 4, or 8
	};

	inline uint32_t bytesPerPixel(TextureFormat format) {
		switch (format) {
		case TextureFormat::R8: return 1;
		case TextureFormat::RG8: return 2;
		case TextureFormat::RGB8: return 3;
		case TextureFormat::RGBA8:
		case TextureFormat::SRGB8_ALPHA8:
		case TextureFormat::R32F:
		case TextureFormat::DEPTH_COMPONENT32:
		case TextureFormat::DEPTH_COMPONENT32F:
		case TextureFormat::DEPTH_COMPONENT24:
		case TextureFormat::DEPTH24_STENCIL8: return 4;
		case TextureFormat::R16F: return 2;
		case TextureFormat::RG16F:
		case TextureFormat::DEPTH_COMPONENT16: return 4;
		case TextureFormat::RGBA16F:
		case TextureFormat::RG32F:
		case TextureFormat::DEPTH32F_STENCIL8: return 8;
		case TextureFormat::RGBA32F: return 16;
		default: return 4;
		}
	}

	// Bytes a texture of this description takes on the device, every level, layer and sample of it.
	inline uint64_t textureBytes(const TextureDesc& desc) {
		// An array gives its layers in depth or arrayLayers (usually both); depth also counts for a volume.
		const uint64_t layers = desc.dimension == TextureDimension::Cube ? 6u
			: desc.dimension == TextureDimension::Array ? (std::max)({ 1u, desc.depth, desc.arrayLayers })
			: desc.dimension == TextureDimension::Tex3D ? (std::max)(1u, desc.depth) : 1u;
		const uint32_t levels = desc.sampleCount > 1 ? 1u : (std::max)(1u, desc.mipLevels);
		uint64_t texels = 0;
		for (uint32_t level = 0; level < levels; ++level) {
			texels += static_cast<uint64_t>((std::max)(1u, desc.width >> level)) * (std::max)(1u, desc.height >> level);
		}
		return texels * layers * (std::max)(1u, desc.sampleCount) * bytesPerPixel(desc.format);
	}

	// Full mip chain for a texture of this size. Both backends build the levels from level 0 when the
	// description asks for more than one.
	inline uint32_t mipLevelsFor(uint32_t width, uint32_t height) {
		uint32_t levels = 1;
		for (uint32_t side = (std::max)(width, height); side > 1u; side >>= 1) {
			++levels;
		}
		return levels;
	}

	struct SamplerDesc {
		FilterMode  minFilter = FilterMode::Linear;
		FilterMode  magFilter = FilterMode::Linear;
		AddressMode addressU = AddressMode::ClampToEdge; //wrap.S
		AddressMode addressV = AddressMode::ClampToEdge; //wrap.T
		AddressMode addressW = AddressMode::ClampToEdge; //wrap.R
		CompareOp   compareOp = CompareOp::None;
		float       maxAnisotropy = 1.f;
	};

	struct FramebufferAttachmentDesc {
		TextureHandle		texture;
		uint32_t            mipLevel = 0;
		uint32_t            layer = 0;   // for 2D-array / 3D / cubemap; UINT32_MAX = all layers (gl_Layer)
	};

	struct FramebufferDesc {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t layers = 1;
		RenderPassHandle renderPass = {}; // Vulkan: compatible render pass

		std::vector<FramebufferAttachmentDesc> colorAttachments;
		std::optional<FramebufferAttachmentDesc> depthStencilAttachment;
	};

	struct BufferDesc {
		size_t size = 0;
		BufferUsage usage = BufferUsage::Vertex;
		CpuAccess cpuAccess = CpuAccess::Write;
		const char* debugName = nullptr;
	};

	struct VertexAttributeDesc {
		std::uint32_t location = 0;
		std::uint32_t binding = 0;
		VertexFormat  format = VertexFormat::Float3;
		std::uint32_t offset = 0;
		bool          normalized = false; // for openGL only
	};

	struct VertexBindingDesc {
		std::uint32_t binding = 0;   
		std::uint32_t stride = 0; // sizeof(Vertex)
		VertexInputRate inputRate = VertexInputRate::PerVertex;
	};

	struct VertexLayoutDesc {
		std::vector<VertexBindingDesc>   bindings;
		std::vector<VertexAttributeDesc> attributes;
	};

	struct RenderPassColorAttachmentDesc {
		AttachmentLoadOp  loadOp = AttachmentLoadOp::Clear;
		AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
		RhiVec4           clearColor{ 0.f, 0.f, 0.f, 1.f };
		TextureFormat     format = TextureFormat::RGBA8; // Vulkan: attachment format
	};

	struct RenderPassDepthAttachmentDesc {
		AttachmentLoadOp  loadOp = AttachmentLoadOp::Clear;
		AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
		float             clearDepth = 1.f;
		uint8_t           clearStencil = 0;
		bool              hasStencil = false;
		TextureFormat     format = TextureFormat::DEPTH_COMPONENT24;
	};

	struct RenderPassDesc {
		uint32_t						colorAttachmentCount = 0;
		RenderPassColorAttachmentDesc	colorAttachments[8];
		bool							hasDepthStencil = false;
		RenderPassDepthAttachmentDesc	depthAttachment;
		uint32_t						sampleCount = 1;
	};

	struct RenderPassBeginInfo {
		RenderPassHandle   renderPass;
		FramebufferHandle  framebuffer;
		int                x = 0, y = 0;
		int                width = 0, height = 0;

		const RhiVec4* overrideClearColors = nullptr; // size >= colorAttachmentCount
		const float* overrideClearDepth = nullptr;
		const uint8_t* overrideClearStencil = nullptr;
	};

	struct DepthStencilStateDesc {
		bool      depthTest = true;
		bool      depthWrite = true;
		CompareOp depthCompareOp = CompareOp::Less;
	};

	struct BlendStateDesc {
		bool       enabled = false;
		BlendFactor srcColor = BlendFactor::SrcAlpha;
		BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
		// The alpha's own factors; unset, it is blended as the colour is.
		std::optional<BlendFactor> srcAlpha;
		std::optional<BlendFactor> dstAlpha;
	};

	struct PipelineShaders {
		ShaderModuleHandle vertex;
		ShaderModuleHandle fragment;
		ShaderModuleHandle geometry;     // optional
		ShaderModuleHandle tessControl;  // optional
		ShaderModuleHandle tessEval;     // optional
		// compute separately in ComputePipelineDesc
	};

	struct ComputePipelineDesc {
		ShaderModuleHandle compute;
		std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;
		uint32_t pushConstantSize = 0;
	};

	struct GraphicsPipelineDesc {
		PipelineShaders shaders;

		VertexLayoutDesc		layout;

		PrimitiveTopology		topology = PrimitiveTopology::Triangles;
		CullMode				cullMode = CullMode::Back;
		FrontFace				frontFace = FrontFace::CounterClockWise;
		PolygonFillMode			fillMode = PolygonFillMode::Fill;
		DepthStencilStateDesc	depthStencilState;
		BlendStateDesc			blend;
		// False: nothing reaches the colour attachments, only the depth -- a depth pre-pass inside a pass that has them.
		// A fragment shader with no colour outputs does not leave them as they were: what it writes there is undefined.
		bool					colorWrite = true;
		// True: the depth it writes is pushed further off by cmdSetDepthBias, set after binding it (a shadow map's casters).
		bool					depthBias = false;

		// For openGL backend it can be ignored.
		// For Vulkan backend: which render pass and subpass this pipeline is compatible with.
		RenderPassHandle		renderPass = {};
		uint32_t				subpassIndex = 0; // meanwhile only 0
		std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts; // Vulkan pipeline layout
		uint32_t				pushConstantSize = 0;
		ShaderStage				pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
		uint32_t				sampleCount = 1;
		uint32_t				patchControlPoints = 3;
	};

	struct DescriptorBufferInfo {
		BufferHandle buffer;
		size_t       offset = 0;
		size_t       range = 0; // 0 - whole buffer
	};

	struct DescriptorImageInfo {
		TextureHandle  texture;
		SamplerHandle  sampler;
	};

	// No type here: the set's layout fixes one DescriptorType per binding, and a binding number appears
	// at most once in a layout, so (set, binding) already names the descriptor. Carrying a second copy
	// of the type only allowed the caller to contradict the layout -- which OpenGL silently ignored and
	// Vulkan handed to the driver, where it read pImageInfo through a buffer descriptor and faulted.
	// Memory that lives for one frame: per-pass constants, vertices drawn once. Write it through `data`
	// before the list that reads it is submitted; it stays valid until that frame has finished on the GPU,
	// while the next frame writes elsewhere. Bind it as a *Dynamic descriptor (written once with
	// range = size, `offset` passed to cmdBindDescriptorSets) or as a vertex or index buffer at `offset`.
	struct TransientAllocation {
		BufferHandle buffer;
		uint32_t     offset = 0;
		void*        data = nullptr;
		explicit operator bool() const { return data != nullptr; }
	};

	struct WriteDescriptor {
		DescriptorSetHandle set; // in which set to write
		uint32_t            binding = 0; // binding in shader layout

		std::vector<DescriptorBufferInfo> buffers;
		std::vector<DescriptorImageInfo>  images;
	};

	struct DescriptorSetLayoutBinding {
		uint32_t		binding = 0;
		DescriptorType	type;
		ShaderStage		stageMask;          // which shader stages will access this binding
		uint32_t		count = 1;          // for texture arrays
	};

	struct DescriptorSetLayoutDesc {
		std::vector<DescriptorSetLayoutBinding> bindings;
	};

	// One binding number, one descriptor. OpenGL would tolerate a repeat, because there an image and a
	// buffer land in separate namespaces, but Vulkan cannot express it inside a set and no shader or
	// layout in this engine asks for it -- every one keeps its numbers distinct and puts images and
	// buffers in separate sets.
	inline bool descriptorLayoutHasDuplicateBinding(const DescriptorSetLayoutDesc& desc) {
		for (size_t i = 0; i < desc.bindings.size(); ++i) {
			for (size_t j = i + 1; j < desc.bindings.size(); ++j) {
				if (desc.bindings[i].binding == desc.bindings[j].binding) {
					return true;
				}
			}
		}
		return false;
	}

	// The window a backend draws into, as the host sees it now. GLFW is the windowing layer both
	// backends are written against; the host's own window class stays on its side.
	struct RhiSurface {
		GLFWwindow* window = nullptr;   // null: nothing to present to
		bool visible = false;
		int width = 0;                  // window size
		int height = 0;
		int renderWidth = 0;            // resolution frames are rendered at
		int renderHeight = 0;
		int backbufferWidth = 0;        // default framebuffer, in pixels
		int backbufferHeight = 0;
	};

	// Everything the RHI needs from whoever owns it. It used to call Engine::instance() for these
	// things, which meant a backend could not be created without a whole engine standing
	// behind it, and that is what made the RHI untestable on its own. The owner implements this;
	// a test implements it with a stub and no engine at all.
	class RhiHost {
	public:
		virtual ~RhiHost() = default;
		virtual RhiSurface surface() const = 0;
		virtual bool presentToScreen() const = 0;
		// Include-expanded GLSL for a path as given to loadShader; empty when it does not exist.
		virtual std::string loadShaderSource(std::string_view path) const = 0;
		// Where a backend may cache compiled shader binaries. Empty: compile every time.
		virtual std::filesystem::path shaderCacheDir() const { return {}; }
		// Around a shader compile that could not use the cache, which can take a moment on a cold start.
		virtual void onShaderCompile(std::string_view name, bool finished) const { (void)name; (void)finished; }
	};

	// A texture made off the device thread: the device object with its contents, not a handle yet. See prepareTexture.
	class PreparedTexture {
	public:
		virtual ~PreparedTexture() = default;
	};

	class RenderHardwareInterface {
	public:
		RenderHardwareInterface(const RenderHardwareInterface& other) = delete;
		RenderHardwareInterface(RenderHardwareInterface&& other) noexcept = delete;
		RenderHardwareInterface& operator=(const RenderHardwareInterface& other) = delete;
		RenderHardwareInterface& operator=(RenderHardwareInterface&& other) noexcept = delete;

		RenderHardwareInterface() = default;
		virtual ~RenderHardwareInterface() = default;

		// Set before init(). Without a host the RHI runs headless: it presents nothing and reports
		// no window, which is exactly what an offscreen test wants.
		void setHost(RhiHost* host) { m_host = host; }
		RhiHost* host() const { return m_host; }

		// What the textures and buffers created through this RHI take on the device, by their descriptions: the
		// driver's own overhead and the swapchain are not in it. Any thread.
		struct GpuMemory {
			uint64_t targets = 0;  // textures rendered into
			uint64_t depth = 0;    // depth attachments: shadow maps, depth buffers
			uint64_t textures = 0; // only sampled: material textures, lookup tables
			uint64_t meshes = 0;   // vertex and index buffers
			uint64_t buffers = 0;  // every other buffer
			uint64_t total() const { return targets + depth + textures + meshes + buffers; }
		};
		GpuMemory gpuMemory() const {
			const auto read = [this](const MemoryKind kind) { return static_cast<uint64_t>(m_memory[static_cast<size_t>(kind)].load(std::memory_order_relaxed)); };
			return { read(MemoryKind::Targets), read(MemoryKind::Depth), read(MemoryKind::Textures), read(MemoryKind::Meshes), read(MemoryKind::Buffers) };
		}

		virtual void init() = 0;
		virtual void cleanup() = 0;
		virtual void waitIdle() {}
		// Runs `work` on the calling thread with the RHI usable there -- resource uploads from a loader
		// thread -- and returns once the GPU has finished it. Calls are serialised. OpenGL makes a context
		// shared with the main one current around it; a backend without thread affinity just runs it.
		virtual void runOnWorkerThread(const std::function<void()>& work) { work(); }

		virtual GraphicsBackend backend() const { return GraphicsBackend::OpenGL; }
		// The texture present() puts on the window. The only presentation state the RHI keeps: streaming
		// or capturing a frame is the host's job, built on readbackTexture and cmdBlit.
		virtual void setPresentBlitSource(TextureHandle texture, SamplerHandle sampler) { (void)texture; (void)sampler; }
		virtual void beginFrame() {}
		virtual uint32_t frameInFlight() const { return 0; }
		virtual uint32_t framesInFlight() const { return 1; }

		virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
		virtual void destroyGraphicsPipeline(PipelineHandle pipeline) = 0;
		virtual TextureHandle createTexture(const TextureDesc& desc, const void* data = nullptr) = 0;
		// Whether prepareTexture works, and from any thread. Where it does not, textures are made with createTexture
		// and updateTexture on the device thread.
		virtual bool preparesTextures() const { return false; }
		// Any thread, when preparesTextures(): a sampled 2D texture with each of its desc.mipLevels levels given in
		// `levels`, uploaded in one go. adoptTexture, on the device thread, makes it a handle.
		virtual std::unique_ptr<PreparedTexture> prepareTexture(const TextureDesc& desc, const std::vector<const void*>& levels) { return nullptr; }
		virtual TextureHandle adoptTexture(std::unique_ptr<PreparedTexture> texture) { return {}; }
		virtual void destroyTexture(TextureHandle texture) = 0;

		virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
		virtual void destroySampler(SamplerHandle sampler) = 0;

		virtual ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) = 0;
		virtual void destroyShaderModule(ShaderModuleHandle handle) = 0;
		// Reads the source through the host and creates a module from it; the same text on both backends.
		ShaderModuleHandle loadShader(ShaderStage stage, std::string_view path);
		// Between frames: every module whose source, read again through the host with its includes, is not the text it
		// was made from is made again, and every pipeline made with it remade as it was, under the same handle -- a
		// shader edited while the engine runs is drawn with at once. One that no longer compiles keeps the old one,
		// and says why. How many modules were remade.
		virtual size_t reloadShaders() { return 0; }

		virtual FramebufferHandle createFramebuffer(const FramebufferDesc& desc) = 0;
		virtual void destroyFramebuffer(FramebufferHandle framebuffer) = 0;
		
		virtual FramebufferHandle getBackbufferFramebuffer() = 0;

		virtual BufferHandle createBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;
		virtual void destroyBuffer(BufferHandle handle) = 0;
		virtual void updateBuffer(BufferHandle handle, size_t offset = 0, size_t size = 0, const void* data = nullptr) = 0;
		
		virtual void* mapBuffer(BufferHandle handle, MapMode mode) = 0;
		virtual void  unmapBuffer(BufferHandle handle) = 0;

		// See TransientAllocation. Aligned for use as a uniform or storage buffer.
		virtual TransientAllocation allocTransient(size_t size) = 0;
		template<typename T>
		TransientAllocation allocTransient(const T& value) {
			const auto allocation = allocTransient(sizeof(T));
			if (allocation) {
				std::memcpy(allocation.data, &value, sizeof(T));
			}
			return allocation;
		}

		virtual RenderPassHandle createRenderPass(const RenderPassDesc& desc) = 0;
		virtual void destroyRenderPass(RenderPassHandle renderPass) = 0;

		virtual DescriptorSetLayoutHandle createDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
		virtual void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) = 0;

		virtual DescriptorSetHandle allocateDescriptorSet(DescriptorSetLayoutHandle layout) = 0;
		virtual void freeDescriptorSet(DescriptorSetHandle set) = 0;

		virtual void updateDescriptorSets(const std::vector<WriteDescriptor>& writes) = 0;

		virtual CommandListHandle beginCommandList(CommandListType type) = 0;
		virtual void endCommandList(CommandListHandle cmd) = 0;

		virtual void cmdBeginRenderPass(CommandListHandle cmd, const RenderPassBeginInfo& beginInfo) = 0;
		virtual void cmdEndRenderPass(CommandListHandle cmd) = 0;

		virtual void cmdBindPipeline(CommandListHandle cmd, PipelineHandle pipeline) = 0;

		virtual void cmdBindVertexBuffer(CommandListHandle cmd, uint32_t slot, BufferHandle handle, uint32_t offset = 0) = 0;
		virtual void cmdBindIndexBuffer(CommandListHandle cmd, BufferHandle handle, IndexType type, uint32_t offset = 0) = 0;
		// One dynamic offset per *Dynamic binding of the sets, set by set and in ascending binding number.
		virtual void cmdBindDescriptorSets(CommandListHandle cmd, PipelineHandle layout, uint32_t firstSet, const DescriptorSetHandle* sets, uint32_t setCount,
			const uint32_t* dynamicOffsets = nullptr, uint32_t dynamicOffsetCount = 0) = 0;
		virtual void cmdPushConstants(CommandListHandle cmd, PipelineHandle pipeline, ShaderStage stages = ShaderStage::Vertex | ShaderStage::Fragment, const void* data = nullptr, uint32_t size = 0) = 0;

		virtual void cmdDraw(CommandListHandle cmd, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
		virtual void cmdDrawIndexed(CommandListHandle cmd, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t  vertexOffset = 0, uint32_t firstInstance = 0) = 0;
		virtual void cmdDrawIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset = 0, uint32_t drawCount = 1, uint32_t stride = 16) = 0;
		virtual void cmdDrawIndexedIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset = 0, uint32_t drawCount = 1, uint32_t stride = 20) = 0;
		// Draws min(uint32 at countOffset in countBuffer, maxDrawCount) records, so a compute pass decides
		// how many draws there are without a readback.
		virtual void cmdDrawIndexedIndirectCount(CommandListHandle cmd, BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride = 20) = 0;
		virtual void cmdDispatch(CommandListHandle cmd, uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1) = 0;

		virtual void cmdBlit(CommandListHandle cmd, FramebufferHandle src, FramebufferHandle dst, BlitMask mask = BlitMask::Color, BlitFilter filter = BlitFilter::Nearest) = 0;
		// Scissor is top-left ImGui/display origin. Both backends flip Y: the RT stores
		// OpenGL-convention +Y-up (Vulkan present blit / readback then un-flips).
		virtual void cmdSetScissor(CommandListHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) {
			(void)cmd;
			(void)x;
			(void)y;
			(void)width;
			(void)height;
		}
		// For the bound pipeline made with depthBias: its depth pushed off by `constant` of the depth's least steps and
		// `slope` times the triangle's own steepest change of depth across a pixel, the whole held to `clamp` (depth, 0
		// none).
		virtual void cmdSetDepthBias(CommandListHandle cmd, float constant, float slope, float clamp) {
			(void)cmd;
			(void)constant;
			(void)slope;
			(void)clamp;
		}
		virtual void cmdPipelineBarrier(CommandListHandle cmd, const PipelineBarrier& barrier) = 0;
		// Writes the data at the call, not when the list runs: every draw in the list reads the new
		// contents, including draws recorded before this call. Write a frame's data before the draws that
		// read it, and never reuse one buffer region for two different draws in the same list.
		virtual void cmdUpdateBuffer(CommandListHandle cmd, BufferHandle handle, size_t offset, size_t size, const void* data) = 0;
		// Copies between two buffers at this point in the list, so the destination takes the source as it
		// stands here rather than after whatever the rest of the frame writes into it.
		virtual void cmdCopyBuffer(CommandListHandle cmd, BufferHandle src, BufferHandle dst, size_t srcOffset, size_t dstOffset, size_t size) = 0;

		// One list per call. Submit a list before beginning the next one: Vulkan records every list of a
		// frame into the same command buffer, so a second open list overwrites the first.
		virtual void submit(CommandListHandle list) = 0;
		virtual void present() = 0;
		virtual double lastGpuMs() const { return 0.0; }
		// GPU timestamps through a frame, each the end of a span of its work and the start of the next. A mark names the
		// span it ends: a render pass when `endsPass`, nullptr the rest of a pass between what is named; the frame's first
		// mark ends nothing. While gpuShaderMarks is on, a backend also marks each change of pipeline, the span before it
		// named by the pipeline drawn with (pipelineLabel: "animal.vs | g_buffer_animal.fs"). Labels must outlive the
		// program. gpuSpans() has the spans of the latest frame the GPU finished, in order, none where the backend has no
		// timestamps. Main thread, as the frame is recorded.
		struct GpuSpan {
			const char* label = nullptr;
			bool endsPass = false;
			double ms = 0.0;
		};
		static constexpr uint32_t kMaxGpuMarks = 512;
		void cmdGpuMark(CommandListHandle cmd, const char* label, bool endsPass);
		const std::vector<GpuSpan>& gpuSpans() const { return mGpuSpans; }
		// Marks by pipeline cost the GPU a timestamp at each change of one and OpenGL the CPU a read of each; off, a frame
		// has a mark a pass.
		std::atomic<bool> gpuShaderMarks{ false };
		// The GPU this backend picked, as the driver names it. Empty before the device exists.
		virtual const char* deviceName() const { return ""; }
		// Exact readback of one colour texture -- full resolution, no rescale, converted to RGBA8
		// with rows top-down on every backend, so two backends' dumps compare pixel for pixel.
		// `layer` picks the slice of an array texture (shadow cascades); ignored for plain 2D.
		virtual bool readbackTexture(TextureHandle tex, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t layer = 0) { (void)tex; (void)rgba; (void)width; (void)height; (void)layer; return false; }
		// The texture present() blits to the screen.
		virtual TextureHandle presentSourceTexture() const { return {}; }

		// Writes one mip level and nothing else: it never builds the rest of the chain, so a caller that wants
		// the same pixels on every backend uploads every level itself.
		// `layer` picks the layer of a texture array; other textures have only 0.
		virtual void updateTexture(TextureHandle tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, TextureFormat fmt, const void* data, uint32_t mip = 0, uint32_t layer = 0) = 0;
		virtual void updateCubeFace(TextureHandle tex, uint32_t face, uint32_t mip, const void* data) = 0;
		virtual void updateCubeMip(TextureHandle tex, uint32_t mip, const void* packed6Faces, size_t faceBytes);
		virtual uint64_t getNativeTextureId(TextureHandle tex) const = 0;

		// helpers
		template<typename T>
		void cmdPushConstants(CommandListHandle cmd, PipelineHandle pipeline, const T& data, ShaderStage stages = ShaderStage::Vertex | ShaderStage::Fragment) {
			static_assert(std::is_trivially_copyable_v<T>);
			cmdPushConstants(cmd, pipeline, stages, static_cast<const void*>(&data), static_cast<uint32_t>(sizeof(T)));
		}

		template<typename T>
		void updateBuffer(BufferHandle handle, const T& data, size_t offset = 0) {
			static_assert(std::is_trivially_copyable_v<T>);
			updateBuffer(handle, offset, sizeof(T), static_cast<const void*>(&data));
		}

		template<typename T>
		void cmdUpdateBuffer(CommandListHandle cmd, BufferHandle handle, const T& data, size_t offset = 0) {
			static_assert(std::is_trivially_copyable_v<T>);
			cmdUpdateBuffer(cmd, handle, offset, sizeof(T), static_cast<const void*>(&data));
		}
	protected:
		RhiSurface hostSurface() const { return m_host ? m_host->surface() : RhiSurface{}; }
		bool hostPresentsToScreen() const { return m_host && m_host->presentToScreen(); }
		// A backend's own mark: a timestamp, its label kept with it until the frame's times are read (cmdGpuMark).
		virtual void writeGpuMark(CommandListHandle, const char* /*label*/, bool /*endsPass*/) {}
		// A backend calls it as a pipeline is bound, with the pipeline's label.
		void pipelineBound(CommandListHandle cmd, const char* label);
		// What a pipeline is called in the GPU's spans: its shaders' names without their folders, kept for good.
		static const char* pipelineLabel(std::string_view first, std::string_view second = {});
		// What a backend keeps of a mark until the frame's timestamps are read back.
		struct GpuMarkLabel {
			const char* label = nullptr;
			bool endsPass = false;
		};
		// Past this many a frame no more marks by pipeline, so the passes' own marks still fit.
		static constexpr uint32_t kGpuShaderMarks = kMaxGpuMarks - 64;
		// The spans of a frame the GPU finished, from its marks' labels and their times in ms.
		void publishGpuSpans(const GpuMarkLabel* labels, const double* atMs, uint32_t count);
		// Backends call these where a texture or buffer is created and where it is finally released.
		void trackTextureMemory(const TextureDesc& desc, const bool created) {
			const auto kind = (desc.usage & TextureUsage::DepthStencilAttachment) ? MemoryKind::Depth
				: (desc.usage & TextureUsage::ColorAttachment) ? MemoryKind::Targets : MemoryKind::Textures;
			track(kind, static_cast<int64_t>(textureBytes(desc)), created);
		}
		void trackBufferMemory(const BufferDesc& desc, const bool created) {
			const auto kind = (desc.usage & (BufferUsage::Vertex | BufferUsage::Index)) ? MemoryKind::Meshes : MemoryKind::Buffers;
			track(kind, static_cast<int64_t>(desc.size), created);
		}

	private:
		enum class MemoryKind : uint8_t { Targets, Depth, Textures, Meshes, Buffers, Count };
		void track(const MemoryKind kind, const int64_t bytes, const bool created) {
			m_memory[static_cast<size_t>(kind)].fetch_add(created ? bytes : -bytes, std::memory_order_relaxed);
		}
		std::atomic<int64_t> m_memory[static_cast<size_t>(MemoryKind::Count)]{};

	private:
		RhiHost* m_host = nullptr;
		std::vector<GpuSpan> mGpuSpans;
		const char* mMarkedShader = nullptr; // the pipeline drawn with since the last mark, marked by pipeline
		CommandListHandle mMarkedList{};     // the list the marks go in: pipelines bound in others are not marked

	};
}

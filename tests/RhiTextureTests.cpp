#include "RhiTestFixture.h"

#include <bc7enc.h>
#include <rgbcx.h>
#include <cstdlib>

// Textures beyond the create-and-read-back round trip RhiResourceTests already pins. `updateTexture`
// is the only call in the interface that writes *part* of a texture, so it carries an origin the
// round trip never exercises: every existing upload covers the whole image, where an x/y convention
// cannot be wrong because there is nothing outside it to land in.
namespace SFE::Render::Tests {
	namespace {

		constexpr int kSize = 4;

		// Deliberately off-centre in both axes: rows 2..3 of four, so a flipped Y puts the block in
		// rows 0..1 and every texel of both regions disagrees, and columns 1..2, which a flipped X
		// maps onto itself — so the four sub-texels are given four distinct colours and an X flip
		// shows up as red and green trading places rather than as nothing at all.
		constexpr uint32_t kSubX = 1;
		constexpr uint32_t kSubY = 2;
		constexpr uint32_t kSubW = 2;
		constexpr uint32_t kSubH = 2;

		// Shares no channel value with any sub-rect texel, so "the sub upload spilled outside its
		// rectangle" and "the base fill never happened" are different failures.
		constexpr Rgba kBase{ 16, 48, 80, 255 };

		// Indexed [row of the sub-image][column], row 0 being the first row of the uploaded bytes.
		constexpr Rgba kSub[kSubH][kSubW] = {
			{ { 255, 0, 0, 255 }, { 0, 255, 0, 255 } },
			{ { 0, 0, 255, 255 }, { 255, 255, 0, 255 } },
		};

		std::vector<uint8_t> solid(uint32_t w, uint32_t h, const Rgba& color) {
			std::vector<uint8_t> bytes;
			bytes.reserve(static_cast<size_t>(w) * h * 4u);
			for (uint32_t i = 0; i < w * h; ++i) {
				bytes.insert(bytes.end(), { color.r, color.g, color.b, color.a });
			}
			return bytes;
		}

		// One w x h level of a single colour, block compressed as TextureHandler's encoders do it: every block alike.
		std::vector<uint8_t> solidBlocks(TextureFormat format, uint32_t w, uint32_t h, const Rgba& color) {
			static std::once_flag once;
			std::call_once(once, [] {
				bc7enc_compress_block_init();
				rgbcx::init();
			});
			const auto texels = solid(4, 4, color);
			uint8_t block[16];
			if (format == TextureFormat::BC4_UNORM) {
				rgbcx::encode_bc4_hq(block, texels.data());
			}
			else {
				bc7enc_compress_block_params params;
				bc7enc_compress_block_params_init(&params);
				bc7enc_compress_block(block, texels.data(), &params);
			}
			const size_t blockBytes = format == TextureFormat::BC4_UNORM ? 8u : 16u;
			std::vector<uint8_t> bytes;
			for (uint64_t i = 0; i < imageBytes(format, w, h) / blockBytes; ++i) {
				bytes.insert(bytes.end(), block, block + blockBytes);
			}
			return bytes;
		}

		Rgba pixelAt(const std::vector<uint8_t>& pixels, int x, int y) {
			const size_t offset = (static_cast<size_t>(y) * kSize + static_cast<size_t>(x)) * 4u;
			if (offset + 3 >= pixels.size()) {
				return {};
			}
			return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
		}
	}

	// Two uploads into one texture: a full-size fill, then a sub-rectangle that must replace exactly
	// its own texels and nothing else. Both halves matter. The fill alone would pass on a backend that
	// ignored x and y entirely, and the sub-rect alone would pass on one that overwrote the whole
	// image, because a texture with no initial data has no defined contents to be destroyed. Together
	// they pin the origin: sixteen texels, four of which changed and twelve of which did not.
	TEST_P(RhiBackendTest, UpdateTextureSubRectLandsAtItsOriginAndLeavesTheRestAlone) {
		TextureDesc desc;
		desc.width = kSize;
		desc.height = kSize;
		desc.depth = 1;
		desc.arrayLayers = 1;
		desc.dimension = TextureDimension::Tex2D;
		desc.format = TextureFormat::RGBA8;
		desc.usage = TextureUsage::Sampled | TextureUsage::TransferSrc | TextureUsage::TransferDst;
		// No initial data: the base colour arrives through updateTexture as well, so the fill and the
		// sub-rect go down the same code path and the only difference between them is the rectangle.
		const auto tex = rhi().createTexture(desc);
		ASSERT_TRUE(tex);

		const auto base = solid(kSize, kSize, kBase);
		rhi().updateTexture(tex, 0, 0, kSize, kSize, TextureFormat::RGBA8, base.data());

		std::vector<uint8_t> sub;
		for (uint32_t row = 0; row < kSubH; ++row) {
			for (uint32_t col = 0; col < kSubW; ++col) {
				const Rgba& c = kSub[row][col];
				sub.insert(sub.end(), { c.r, c.g, c.b, c.a });
			}
		}
		rhi().updateTexture(tex, kSubX, kSubY, kSubW, kSubH, TextureFormat::RGBA8, sub.data());

		std::vector<uint8_t> pixels;
		int w = 0;
		int h = 0;
		ASSERT_TRUE(rhi().readbackTexture(tex, pixels, w, h));
		EXPECT_EQ(w, kSize);
		EXPECT_EQ(h, kSize);
		ASSERT_EQ(pixels.size(), base.size());

		for (int imageY = 0; imageY < kSize; ++imageY) {
			// The flip RhiResourceTests pins on the create-then-read round trip: an upload treats its
			// first row as the texture's row 0 while readbackTexture hands rows back top down, so
			// texture row y comes back as image row kSize - 1 - y. `updateTexture`'s y is a row index
			// in that same upload space, which is exactly what this mapping asserts. X is not flipped
			// by either half, so the column is the texel's own x.
			const uint32_t texY = static_cast<uint32_t>(kSize - 1 - imageY);
			for (int imageX = 0; imageX < kSize; ++imageX) {
				const uint32_t texX = static_cast<uint32_t>(imageX);
				const bool inside = texX >= kSubX && texX < kSubX + kSubW
					&& texY >= kSubY && texY < kSubY + kSubH;
				const Rgba expected = inside ? kSub[texY - kSubY][texX - kSubX] : kBase;
				EXPECT_EQ(pixelAt(pixels, imageX, imageY), expected)
					<< "texel (" << texX << ", " << texY << ") — image row " << imageY << " — is "
					<< (inside ? "inside" : "outside") << " the sub-rectangle uploaded at x = " << kSubX
					<< ", y = " << kSubY << ", " << kSubW << "x" << kSubH << ": the sub-rect upload "
					<< (inside ? "did not land here" : "spilled outside its rectangle or the base fill was lost")
					<< ", so updateTexture's x/y origin is not the corner the other backend uses";
			}
		}

		rhi().destroyTexture(tex);
	}

	namespace {

		// A 4x4 whose 2x2 block averages and whole-image average are exact whole bytes, so the three
		// levels of the chain are 32, 64 and 128 at the corner this test samples and no rounding rule
		// is left inside an expectation. Rows in upload order, first row first. Grey rather than three
		// colours because the probe uses the three channels of one pixel to carry three lods.
		//
		// The upload's first row is the V = 0 end of the texture — the composition
		// DescriptorCombinedImageSamplerReturnsTheTexelAtTheSampledUv spells out — so a V flip would
		// make the probe read row 3 instead of row 0. Rows 2 and 3 are a flat 160 exactly so that a
		// flip is a wrong answer rather than the same one.
		constexpr uint8_t kMipTexels[kSize][kSize] = {
			{  32,  64, 160, 160 },
			{  64,  96, 160, 160 },
			{ 160, 160, 128, 128 },
			{ 160, 160, 128, 128 },
		};

		// Level 0 texel (0, 0); level 1 texel (0, 0), the average of the 2x2 block 32/64/64/96; and
		// level 2, the average of all sixteen. Three distinct bytes, so a lod that resolved to the
		// wrong level names the level it hit.
		constexpr uint8_t kLod0Byte = 32;
		constexpr uint8_t kLod1Byte = 64;
		constexpr uint8_t kLod2Byte = 128;

		// The corner texel's centre in a 4x4: an exact binary fraction, at the centre of level 0's
		// texel and outside the outermost texel centre of levels 1 and 2, where ClampToEdge holds the
		// corner texel whole. So the answer at every level is one texel and never a blend of two,
		// which is what lets a Linear filter be used without putting filtering into the expectation.
		constexpr float kProbeUv = 0.125f;

		TextureHandle makeLodTarget(RenderHardwareInterface& rhi) {
			TextureDesc desc;
			desc.width = kSize;
			desc.height = kSize;
			desc.depth = 1;
			desc.arrayLayers = 1;
			desc.dimension = TextureDimension::Tex2D;
			desc.format = TextureFormat::RGBA8;
			desc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::TransferSrc;
			return rhi.createTexture(desc);
		}

		RenderPassHandle makeLodPass(RenderHardwareInterface& rhi, const Math::Vec4& clear) {
			RenderPassDesc desc;
			desc.colorAttachmentCount = 1;
			desc.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
			desc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
			desc.colorAttachments[0].format = TextureFormat::RGBA8;
			desc.colorAttachments[0].clearColor = clear;
			return rhi.createRenderPass(desc);
		}

		PipelineHandle makeLodPipeline(RenderHardwareInterface& rhi, RenderPassHandle pass,
			DescriptorSetLayoutHandle layout) {
			GraphicsPipelineDesc desc;
			desc.renderPass = pass;
			desc.topology = PrimitiveTopology::Triangles;
			desc.cullMode = CullMode::None;
			desc.frontFace = FrontFace::CounterClockWise;
			desc.fillMode = PolygonFillMode::Fill;
			desc.depthStencilState = { false, false, CompareOp::Always };
			desc.blend = { false };
			desc.descriptorSetLayouts = { layout };
			desc.shaders.vertex = rhi.loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
			desc.shaders.fragment = rhi.loadShader(ShaderStage::Fragment, "shaders/rhi_test_texture_lod.fs");
			return rhi.createGraphicsPipeline(desc);
		}

		std::vector<uint8_t> drawWithSet(RenderHardwareInterface& rhi, TextureHandle target,
			RenderPassHandle pass, PipelineHandle pipeline, DescriptorSetHandle set) {
			FramebufferDesc fbDesc;
			fbDesc.width = kSize;
			fbDesc.height = kSize;
			fbDesc.layers = 1;
			fbDesc.renderPass = pass;
			FramebufferAttachmentDesc att;
			att.texture = target;
			att.layer = UINT32_MAX;
			fbDesc.colorAttachments = { att };
			const auto fb = rhi.createFramebuffer(fbDesc);
			if (!fb) {
				return {};
			}

			rhi.beginFrame();
			const auto cmd = rhi.beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = kSize;
			begin.height = kSize;
			rhi.cmdBeginRenderPass(cmd, begin);
			rhi.cmdBindPipeline(cmd, pipeline);
			rhi.cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi.cmdDraw(cmd, 3, 1, 0, 0);
			rhi.cmdEndRenderPass(cmd);
			rhi.endCommandList(cmd);
			rhi.submit(cmd);
			rhi.present();
			rhi.waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			if (!rhi.readbackTexture(target, pixels, w, h)) {
				return {};
			}
			rhi.destroyFramebuffer(fb);
			return pixels;
		}
	}

	// TextureDesc.mipLevels: that a texture asked for a mip chain has one, that the levels hold
	// different things, and that an explicit lod picks the level it names. Every other texture test
	// in the suite lives at level 0, where mipLevels could be ignored entirely and nothing would
	// notice.
	//
	// Two things this test cannot do, both of them limits of the interface rather than choices:
	//
	//   1. The levels are not uploaded, they are generated. `updateTexture` has no mip parameter and
	//      writes `mipLevel = 0` on both backends (OpenGLRHI.cpp:2298, VulkanRHIResources.cpp:769),
	//      and `createTexture` with data fills level 0 and then calls glGenerateMipmap / a blit chain
	//      for the rest. The only mip-addressed writes in the interface are updateCubeFace and
	//      updateCubeMip, which are cube-only, and FramebufferAttachmentDesc::mipLevel, which Vulkan
	//      drops for a Tex2D (VulkanRHIPipelines.cpp:768 returns the texture's whole-chain view). So
	//      the level contents below are the box averages of the uploaded image, chosen to be exact
	//      whole bytes, and what is pinned is that each level differs and each lod finds its own.
	//   2. The sampler cannot be Nearest. SamplerDesc has no mip filter field: the mip mode is
	//      derived from minFilter alone, and anything but LinearMipmapLinear leaves Vulkan's maxLod
	//      at 0 (VulkanRHIResources.cpp:849-851) while OpenGL gets a non-mipmap GL_TEXTURE_MIN_FILTER
	//      — on both backends that pins every lookup to level 0. The second probe below measures
	//      exactly that, so the constraint is a pinned fact rather than a remark. The blending a
	//      Linear filter would otherwise smuggle into the answer is removed by the UV instead: it
	//      sits on level 0's corner texel centre and outside the outermost texel centre of the two
	//      smaller levels, where ClampToEdge returns that texel whole.
	TEST_P(RhiBackendTest, TextureMipLevelsAreSelectedByExplicitLod) {
		std::vector<uint8_t> source;
		source.reserve(static_cast<size_t>(kSize) * kSize * 4u);
		for (int row = 0; row < kSize; ++row) {
			for (int col = 0; col < kSize; ++col) {
				const uint8_t v = kMipTexels[row][col];
				source.insert(source.end(), { v, v, v, 255 });
			}
		}

		TextureDesc texDesc;
		texDesc.width = kSize;
		texDesc.height = kSize;
		texDesc.depth = 1;
		texDesc.arrayLayers = 1;
		// 4x4 -> 2x2 -> 1x1: the full chain, so lod 2 is the last level and not a level the backend
		// silently clamped to.
		texDesc.mipLevels = 3;
		texDesc.dimension = TextureDimension::Tex2D;
		texDesc.format = TextureFormat::RGBA8;
		texDesc.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
		const auto texture = rhi().createTexture(texDesc, source.data());
		ASSERT_TRUE(texture);

		const auto makeSampler = [&](FilterMode filter) {
			SamplerDesc desc;
			desc.minFilter = filter;
			desc.magFilter = filter;
			desc.addressU = AddressMode::ClampToEdge;
			desc.addressV = AddressMode::ClampToEdge;
			desc.addressW = AddressMode::ClampToEdge;
			return rhi().createSampler(desc);
		};
		const auto mipped = makeSampler(FilterMode::LinearMipmapLinear);
		const auto flat = makeSampler(FilterMode::Nearest);
		ASSERT_TRUE(mipped);
		ASSERT_TRUE(flat);

		const auto target = makeLodTarget(rhi());
		ASSERT_TRUE(target);
		// Magenta: no channel of it is any of the three level bytes, so a draw that never happened is
		// told apart from a draw that read the wrong level.
		const auto pass = makeLodPass(rhi(), { 1.f, 0.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		// Binding 0 and 1, not 15, which SFE_PUSH_BLOCK reserves on OpenGL; one set, so OpenGL
		// discarding the set index cannot matter here.
		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		const auto pipeline = makeLodPipeline(rhi(), pass, layout);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		BufferDesc uvDesc;
		uvDesc.size = sizeof(Math::Vec4);
		uvDesc.usage = BufferUsage::Uniform;
		uvDesc.cpuAccess = CpuAccess::Write;
		uvDesc.debugName = "rhi_test_mip_uv";
		const Math::Vec4 uvValue{ kProbeUv, kProbeUv, 0.f, 0.f };
		const auto uvBuffer = rhi().createBuffer(uvDesc, &uvValue);
		ASSERT_TRUE(uvBuffer);

		// One draw per sampler, each through its own freshly allocated set, so neither probe can
		// inherit what the other wrote.
		const auto probe = [&](SamplerHandle sampler) -> Rgba {
			const auto set = rhi().allocateDescriptorSet(layout);
			EXPECT_TRUE(set);

			WriteDescriptor image{};
			image.set = set;
			image.binding = 0;
			image.images.push_back({ texture, sampler });
			WriteDescriptor uv{};
			uv.set = set;
			uv.binding = 1;
			uv.buffers.push_back({ uvBuffer, 0, 0 });
			rhi().updateDescriptorSets({ image, uv });

			const auto pixels = drawWithSet(rhi(), target, pass, pipeline, set);
			EXPECT_FALSE(pixels.empty()) << "readback failed";
			const Rgba first = pixelAt(pixels, 0, 0);
			// Every fragment samples the same uniform UV at the same three lods, so the target must be
			// one colour throughout: a clipped draw fails here rather than passing on a lucky pixel.
			// That also makes the readback's row flip irrelevant to this test, which is why the probe
			// is a single colour rather than a picture.
			for (int y = 0; y < kSize; ++y) {
				for (int x = 0; x < kSize; ++x) {
					EXPECT_EQ(pixelAt(pixels, x, y), first)
						<< "pixel (" << x << ", " << y << ") differs from the rest although every fragment "
						   "sampled the same UV at the same lods: the draw did not cover the target";
				}
			}

			rhi().freeDescriptorSet(set);
			return first;
		};

		// The claim: three lods, three levels, three different bytes. R is lod 0, G lod 1, B lod 2.
		EXPECT_EQ(probe(mipped), (Rgba{ kLod0Byte, kLod1Byte, kLod2Byte, 255 }))
			<< "the three explicit lods did not each return their own level: equal channels mean the "
			   "chain has one level's contents in every level or the lod argument was ignored, and a "
			   "channel carrying another level's byte names the level that lod actually reached";

		// And the constraint the comment above states, measured: a sampler that is not
		// LinearMipmapLinear sees level 0 at every lod, on both backends, so mip selection is not
		// something a Nearest sampler can express through this interface.
		EXPECT_EQ(probe(flat), (Rgba{ kLod0Byte, kLod0Byte, kLod0Byte, 255 }))
			<< "a Nearest sampler reached a level other than 0: the lod clamp that both backends "
			   "derive from minFilter alone no longer holds, so SamplerDesc's mip behaviour has "
			   "changed and the reason this test cannot use Nearest is gone";

		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyBuffer(uvBuffer);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroySampler(flat);
		rhi().destroySampler(mipped);
		rhi().destroyTexture(texture);
		rhi().destroyRenderPass(pass);
		rhi().destroyTexture(target);
	}

	namespace {

		// 2x2 faces, so level 0 is four texels of one colour and level 1 is a single texel. A linear
		// filter at the face centre then returns the face's bytes whole — the four texels it blends
		// are all the same — which is what keeps the expectations exact bytes while still letting the
		// sampler be LinearMipmapLinear, the only filter that opens the chain past level 0.
		constexpr uint32_t kCubeSize = 2;

		// The claim under test, written as a table: index i is the `face` argument of updateCubeFace,
		// and kCubeDirs[i] is the direction the standard cube mapping says that index covers. If a
		// backend numbers its faces differently, one of these rows returns another row's colour.
		constexpr float kCubeDirs[6][3] = {
			{  1.f,  0.f,  0.f },
			{ -1.f,  0.f,  0.f },
			{  0.f,  1.f,  0.f },
			{  0.f, -1.f,  0.f },
			{  0.f,  0.f,  1.f },
			{  0.f,  0.f, -1.f },
		};

		constexpr const char* kCubeDirNames[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

		constexpr Rgba kFaceColors[6] = {
			{ 255,   0,   0, 255 },
			{   0, 255,   0, 255 },
			{   0,   0, 255, 255 },
			{ 255, 255,   0, 255 },
			{ 255,   0, 255, 255 },
			{   0, 255, 255, 255 },
		};

		// No channel triple here is any of the level 0 colours, so a probe answered from the wrong
		// level names the level it reached rather than merely disagreeing.
		constexpr Rgba kCubeMipColors[6] = {
			{  16,  32,  48, 255 },
			{  64,  80,  96, 255 },
			{ 112, 128, 144, 255 },
			{ 160, 176, 192, 255 },
			{ 208, 224, 240, 255 },
			{  24,  56,  88, 255 },
		};

		struct CubeProbePush {
			float x = 0.f;
			float y = 0.f;
			float z = 0.f;
			float lod = 0.f;
		};

		PipelineHandle makeCubePipeline(RenderHardwareInterface& rhi, RenderPassHandle pass,
			DescriptorSetLayoutHandle layout) {
			GraphicsPipelineDesc desc;
			desc.renderPass = pass;
			desc.topology = PrimitiveTopology::Triangles;
			desc.cullMode = CullMode::None;
			desc.frontFace = FrontFace::CounterClockWise;
			desc.fillMode = PolygonFillMode::Fill;
			desc.depthStencilState = { false, false, CompareOp::Always };
			desc.blend = { false };
			desc.descriptorSetLayouts = { layout };
			desc.pushConstantSize = sizeof(CubeProbePush);
			desc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
			desc.shaders.vertex = rhi.loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
			desc.shaders.fragment = rhi.loadShader(ShaderStage::Fragment, "shaders/rhi_test_cube_face.fs");
			return rhi.createGraphicsPipeline(desc);
		}

		// drawWithSet above with the probe's direction pushed in: twelve draws differ only by four
		// floats, so nothing else about the frame can change between one face and the next.
		std::vector<uint8_t> drawCubeProbe(RenderHardwareInterface& rhi, TextureHandle target,
			RenderPassHandle pass, PipelineHandle pipeline, DescriptorSetHandle set,
			const CubeProbePush& push) {
			FramebufferDesc fbDesc;
			fbDesc.width = kSize;
			fbDesc.height = kSize;
			fbDesc.layers = 1;
			fbDesc.renderPass = pass;
			FramebufferAttachmentDesc att;
			att.texture = target;
			att.layer = UINT32_MAX;
			fbDesc.colorAttachments = { att };
			const auto fb = rhi.createFramebuffer(fbDesc);
			if (!fb) {
				return {};
			}

			rhi.beginFrame();
			const auto cmd = rhi.beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = kSize;
			begin.height = kSize;
			rhi.cmdBeginRenderPass(cmd, begin);
			rhi.cmdBindPipeline(cmd, pipeline);
			rhi.cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi.cmdPushConstants(cmd, pipeline, ShaderStage::Vertex | ShaderStage::Fragment,
				&push, sizeof(push));
			rhi.cmdDraw(cmd, 3, 1, 0, 0);
			rhi.cmdEndRenderPass(cmd);
			rhi.endCommandList(cmd);
			rhi.submit(cmd);
			rhi.present();
			rhi.waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			if (!rhi.readbackTexture(target, pixels, w, h)) {
				return {};
			}
			rhi.destroyFramebuffer(fb);
			return pixels;
		}
	}

	// updateCubeFace / updateCubeMip, the only mip-addressed writes the interface has and the only
	// way to fill a cube at all. Six distinct colours go in by face index and come back out by
	// direction: the test is the pairing between the two, because a backend that agreed about
	// everything except which index is which face would look correct in every single-face check and
	// render a skybox inside out.
	//
	// The face contents are read through a samplerCube rather than through readbackTexture, which
	// takes a `layer` but is a 2D path: a cube's faces are only reachable as faces through a
	// sampler, and reading them any other way would pin the layout of the image instead of the
	// mapping callers actually depend on.
	//
	// The sampler has to be LinearMipmapLinear for the same reason
	// TextureMipLevelsAreSelectedByExplicitLod states: SamplerDesc has no mip filter field, so
	// anything else pins every lod to level 0 and the updateCubeMip half of this test would read
	// level 0 and pass for the wrong reason. The blend that filter would otherwise smuggle in is
	// removed by the face being one flat colour.
	TEST_P(RhiBackendTest, CubeFaceUploadsAreReadBackByTheDirectionThatFaceCovers) {
		TextureDesc cubeDesc;
		cubeDesc.width = kCubeSize;
		cubeDesc.height = kCubeSize;
		cubeDesc.depth = 1;
		cubeDesc.arrayLayers = 6;
		// 2x2 -> 1x1: a complete chain, so lod 1 is a real level and not one a backend clamped to.
		cubeDesc.mipLevels = 2;
		cubeDesc.dimension = TextureDimension::Cube;
		cubeDesc.format = TextureFormat::RGBA8;
		cubeDesc.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
		const auto cube = rhi().createTexture(cubeDesc);
		ASSERT_TRUE(cube);

		for (uint32_t face = 0; face < 6; ++face) {
			const auto bytes = solid(kCubeSize, kCubeSize, kFaceColors[face]);
			rhi().updateCubeFace(cube, face, 0, bytes.data());
		}

		// updateCubeMip takes all six faces of one level packed back to back. OpenGL inherits the
		// interface's default, which loops updateCubeFace; Vulkan overrides it with six copy regions
		// out of one staging buffer. Two implementations, one expectation.
		std::vector<uint8_t> packedMip;
		for (const auto& color : kCubeMipColors) {
			const auto bytes = solid(1, 1, color);
			packedMip.insert(packedMip.end(), bytes.begin(), bytes.end());
		}
		rhi().updateCubeMip(cube, 1, packedMip.data(), 4);

		SamplerDesc samplerDesc;
		samplerDesc.minFilter = FilterMode::LinearMipmapLinear;
		samplerDesc.magFilter = FilterMode::Linear;
		samplerDesc.addressU = AddressMode::ClampToEdge;
		samplerDesc.addressV = AddressMode::ClampToEdge;
		samplerDesc.addressW = AddressMode::ClampToEdge;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		const auto target = makeLodTarget(rhi());
		ASSERT_TRUE(target);
		// White appears in neither colour table, so "the draw never happened" cannot be mistaken for
		// any face's answer at either level.
		const auto pass = makeLodPass(rhi(), { 1.f, 1.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		const auto pipeline = makeCubePipeline(rhi(), pass, layout);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		const auto set = rhi().allocateDescriptorSet(layout);
		ASSERT_TRUE(set);
		WriteDescriptor image{};
		image.set = set;
		image.binding = 0;
		image.images.push_back({ cube, sampler });
		rhi().updateDescriptorSets({ image });

		const auto probe = [&](uint32_t dir, float lod) -> Rgba {
			const CubeProbePush push{ kCubeDirs[dir][0], kCubeDirs[dir][1], kCubeDirs[dir][2], lod };
			const auto pixels = drawCubeProbe(rhi(), target, pass, pipeline, set, push);
			EXPECT_FALSE(pixels.empty()) << "readback failed";
			const Rgba first = pixelAt(pixels, 0, 0);
			// Every fragment samples the same direction, so the target must be one colour throughout.
			// That also makes the readback's row flip irrelevant here, which is why the probe is a
			// single colour rather than a picture.
			for (int y = 0; y < kSize; ++y) {
				for (int x = 0; x < kSize; ++x) {
					EXPECT_EQ(pixelAt(pixels, x, y), first)
						<< "pixel (" << x << ", " << y << ") differs from the rest although every "
						   "fragment sampled the same direction: the draw did not cover the target";
				}
			}
			return first;
		};

		for (uint32_t face = 0; face < 6; ++face) {
			EXPECT_EQ(probe(face, 0.f), kFaceColors[face])
				<< "the direction " << kCubeDirNames[face] << " did not return the colour uploaded to "
				   "face index " << face << ": either that face's upload did not land, or this backend "
				   "orders its cube faces differently from the other one — the returned colour names "
				   "the face index this direction actually reached";
		}

		for (uint32_t face = 0; face < 6; ++face) {
			EXPECT_EQ(probe(face, 1.f), kCubeMipColors[face])
				<< "lod 1 in the direction " << kCubeDirNames[face] << " did not return what "
				   "updateCubeMip wrote into level 1 of face index " << face << ": a level 0 colour "
				   "here means the mip write never reached level 1 or the lod was clamped, and another "
				   "face's level 1 colour means the packed faces went in in a different order";
		}

		rhi().freeDescriptorSet(set);
		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroySampler(sampler);
		rhi().destroyTexture(cube);
		rhi().destroyRenderPass(pass);
		rhi().destroyTexture(target);
	}

	namespace {

		constexpr int kFormatSize = 2;
		constexpr uint32_t kFormatTexels = kFormatSize * kFormatSize;

		// One level table serving both encodings is what lets the byte formats and the half-float
		// formats share a single expectation column. A byte level is its own readback value; a half
		// level is chosen so that its value times 255 sits well clear of the halfway point, because
		// readbackTexture turns a float texel into a byte through glReadPixels on one backend and
		// through a vkCmdBlitImage format conversion on the other, and neither spells out its
		// rounding rule. 0.625, 0.75, 0.875 and 1.0 give 159.375, 191.25, 223.125 and 255, which
		// round and truncate to the same byte.
		constexpr uint8_t kLevelByte[4] = { 159, 191, 223, 255 };
		constexpr uint16_t kLevelHalf[4] = { 0x3900, 0x3A00, 0x3B00, 0x3C00 };

		// A Latin square over the four levels: every texel carries four different levels and every
		// channel carries four different levels down the texels, so a swapped channel and a swapped
		// texel are both wrong answers rather than the same answer twice. No level is 0, which is
		// what a channel the format does not carry reads back as, so "the blue channel was dropped"
		// cannot be mistaken for "the blue channel held the lowest level".
		constexpr uint8_t kLevel[kFormatTexels][4] = {
			{ 0, 1, 2, 3 },
			{ 1, 2, 3, 0 },
			{ 2, 3, 0, 1 },
			{ 3, 0, 1, 2 },
		};

		struct FormatCase {
			TextureFormat format;
			const char* name;
			uint32_t channels;
			uint32_t bytesPerTexel;
			bool halfFloat;
			// What a channel the format has no storage for reads back as. Pinned, not assumed: it is
			// the one part of this round trip neither backend is copying byte for byte.
			uint8_t missingColor;
			uint8_t missingAlpha;
		};

		constexpr FormatCase kFormatCases[] = {
			{ TextureFormat::R8,      "R8",      1, 1, false, 0, 255 },
			{ TextureFormat::RG8,     "RG8",     2, 2, false, 0, 255 },
			{ TextureFormat::RGBA8,   "RGBA8",   4, 4, false, 0, 255 },
			{ TextureFormat::R16F,    "R16F",    1, 2, true,  0, 255 },
			{ TextureFormat::RG16F,   "RG16F",   2, 4, true,  0, 255 },
			{ TextureFormat::RGBA16F, "RGBA16F", 4, 8, true,  0, 255 },
		};

		std::vector<uint8_t> encodeFormatSource(const FormatCase& fc) {
			std::vector<uint8_t> bytes;
			bytes.reserve(static_cast<size_t>(kFormatTexels) * fc.bytesPerTexel);
			for (uint32_t texel = 0; texel < kFormatTexels; ++texel) {
				for (uint32_t c = 0; c < fc.channels; ++c) {
					const uint8_t level = kLevel[texel][c];
					if (fc.halfFloat) {
						const uint16_t h = kLevelHalf[level];
						bytes.push_back(static_cast<uint8_t>(h & 0xFFu));
						bytes.push_back(static_cast<uint8_t>(h >> 8));
					}
					else {
						bytes.push_back(kLevelByte[level]);
					}
				}
			}
			return bytes;
		}

		Rgba expectedTexel(const FormatCase& fc, uint32_t texel) {
			uint8_t c[4];
			for (uint32_t i = 0; i < 4; ++i) {
				c[i] = i < fc.channels ? kLevelByte[kLevel[texel][i]]
					: (i == 3 ? fc.missingAlpha : fc.missingColor);
			}
			return { c[0], c[1], c[2], c[3] };
		}

		Rgba texelAt(const std::vector<uint8_t>& pixels, int x, int y) {
			const size_t offset = (static_cast<size_t>(y) * kFormatSize + static_cast<size_t>(x)) * 4u;
			if (offset + 3 >= pixels.size()) {
				return {};
			}
			return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
		}

		struct DepthCase {
			float clearDepth;
			Rgba packed;
		};

		// readbackTexture does not hand depth back as depth: both backends quantise it to 16 bits and
		// pack it into RGBA — R the high byte, G the low byte, B repeating the high byte as a
		// greyscale preview, A opaque (OpenGLRHI.cpp:1026-1034, VulkanRHIPipelines.cpp:151-163). So
		// 0.25 becomes 16384 = 0x4000 and 0.75 becomes 49151 = 0xBFFF. Both values are far enough
		// from a quantisation boundary that the last bit of the depth buffer cannot move the answer.
		constexpr DepthCase kDepthCases[] = {
			{ 0.25f, {  64,   0,  64, 255 } },
			{ 0.75f, { 191, 255, 191, 255 } },
		};
	}

	// The TextureFormat matrix through the two calls that carry raw texels across the interface:
	// createTexture with initial data and readbackTexture. Every other texture test in the suite is
	// RGBA8, where a format is never translated and the channel count is never anything but four.
	//
	// readbackTexture always answers in RGBA8 — OpenGL reads GL_RGBA/GL_UNSIGNED_BYTE out of an FBO,
	// Vulkan blits anything that is not already RGBA8 through a scratch RGBA8 image
	// (VulkanRHIPipelines.cpp:78-96) — so what is pinned here is a round trip through that
	// conversion, and the interesting half of it is the channels the format does not have. Both
	// backends must invent them, and they must invent the same ones.
	//
	// DEPTH_COMPONENT32 is not in the table because it cannot be created with initial data at all on
	// Vulkan: createImage's upload path copies through copyBufferToImage, which hardcodes
	// VK_IMAGE_ASPECT_COLOR_BIT (VulkanRHIResources.cpp:608) and so cannot address a depth image. The
	// two backends do not even agree what the bytes would mean — OpenGL takes GL_UNSIGNED_INT into a
	// 32-bit unorm (OpenGLRHI.cpp:295), Vulkan maps the format to VK_FORMAT_D32_SFLOAT
	// (VulkanRHIVkUtil.h:23), so one buffer of bytes cannot serve both. What is expressible is
	// clearing depth in a render pass and reading that back, which is the second half of this test.
	TEST_P(RhiBackendTest, TextureFormatMatrixRoundTripsThroughCreateAndReadback) {
		for (const auto& fc : kFormatCases) {
			SCOPED_TRACE(fc.name);

			TextureDesc desc;
			desc.width = kFormatSize;
			desc.height = kFormatSize;
			desc.depth = 1;
			desc.arrayLayers = 1;
			desc.dimension = TextureDimension::Tex2D;
			desc.format = fc.format;
			desc.usage = TextureUsage::Sampled | TextureUsage::TransferSrc | TextureUsage::TransferDst;

			const auto source = encodeFormatSource(fc);
			ASSERT_EQ(source.size(), static_cast<size_t>(kFormatTexels) * fc.bytesPerTexel);
			const auto tex = rhi().createTexture(desc, source.data());
			ASSERT_TRUE(tex) << "createTexture refused " << fc.name << " with initial data";

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			ASSERT_TRUE(rhi().readbackTexture(tex, pixels, w, h))
				<< "readbackTexture refused " << fc.name;
			EXPECT_EQ(w, kFormatSize);
			EXPECT_EQ(h, kFormatSize);
			ASSERT_EQ(pixels.size(), static_cast<size_t>(kFormatTexels) * 4u)
				<< "readbackTexture answers in RGBA8 whatever the source format, so the size is the "
				   "texel count times four regardless of " << fc.name << "'s own texel size";

			for (int imageY = 0; imageY < kFormatSize; ++imageY) {
				// The row flip TextureInitialDataReadsBackUnchanged pins: the upload's first row is
				// texture row 0 and readbackTexture hands rows back top down.
				const uint32_t texRow = static_cast<uint32_t>(kFormatSize - 1 - imageY);
				for (int x = 0; x < kFormatSize; ++x) {
					const uint32_t texel = texRow * kFormatSize + static_cast<uint32_t>(x);
					EXPECT_EQ(texelAt(pixels, x, imageY), expectedTexel(fc, texel))
						<< fc.name << " texel (" << x << ", " << texRow << ") — image row " << imageY
						<< " — did not survive the round trip: a channel below index " << fc.channels
						<< " means the upload or the readback conversion changed the value, and a "
						   "channel at or above it means the two backends disagree about what a "
						   "channel this format does not store reads back as";
				}
			}

			rhi().destroyTexture(tex);
		}

		for (const auto& dc : kDepthCases) {
			SCOPED_TRACE(::testing::Message() << "depth clear " << dc.clearDepth);

			TextureDesc depthDesc;
			depthDesc.width = kFormatSize;
			depthDesc.height = kFormatSize;
			depthDesc.depth = 1;
			depthDesc.arrayLayers = 1;
			depthDesc.dimension = TextureDimension::Tex2D;
			depthDesc.format = TextureFormat::DEPTH_COMPONENT32;
			depthDesc.usage = TextureUsage::DepthStencilAttachment | TextureUsage::Sampled
				| TextureUsage::TransferSrc;
			const auto depthTex = rhi().createTexture(depthDesc);
			ASSERT_TRUE(depthTex);

			TextureDesc colorDesc;
			colorDesc.width = kFormatSize;
			colorDesc.height = kFormatSize;
			colorDesc.depth = 1;
			colorDesc.arrayLayers = 1;
			colorDesc.dimension = TextureDimension::Tex2D;
			colorDesc.format = TextureFormat::RGBA8;
			colorDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled
				| TextureUsage::TransferSrc;
			// A colour attachment alongside the depth one, because a depth-only pass is its own
			// checkbox and would put an untested shape of RenderPassDesc into a format test.
			const auto colorTex = rhi().createTexture(colorDesc);
			ASSERT_TRUE(colorTex);

			RenderPassDesc passDesc;
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
			passDesc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
			passDesc.colorAttachments[0].format = TextureFormat::RGBA8;
			passDesc.colorAttachments[0].clearColor = { 0.f, 0.f, 0.f, 1.f };
			passDesc.hasDepthStencil = true;
			passDesc.depthAttachment.loadOp = AttachmentLoadOp::Clear;
			passDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
			passDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT32;
			passDesc.depthAttachment.clearDepth = dc.clearDepth;
			const auto pass = rhi().createRenderPass(passDesc);
			ASSERT_TRUE(pass);

			FramebufferDesc fbDesc;
			fbDesc.width = kFormatSize;
			fbDesc.height = kFormatSize;
			fbDesc.layers = 1;
			fbDesc.renderPass = pass;
			FramebufferAttachmentDesc colorAtt;
			colorAtt.texture = colorTex;
			colorAtt.layer = UINT32_MAX;
			fbDesc.colorAttachments = { colorAtt };
			FramebufferAttachmentDesc depthAtt;
			depthAtt.texture = depthTex;
			depthAtt.layer = UINT32_MAX;
			fbDesc.depthStencilAttachment = depthAtt;
			const auto fb = rhi().createFramebuffer(fbDesc);
			ASSERT_TRUE(fb);

			// No draw: the clear is the whole write, so nothing about a pipeline or a depth compare
			// can reach the value under test.
			rhi().beginFrame();
			const auto cmd = rhi().beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = kFormatSize;
			begin.height = kFormatSize;
			rhi().cmdBeginRenderPass(cmd, begin);
			rhi().cmdEndRenderPass(cmd);
			rhi().endCommandList(cmd);
			rhi().submit(cmd);
			rhi().present();
			rhi().waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			ASSERT_TRUE(rhi().readbackTexture(depthTex, pixels, w, h))
				<< "readbackTexture refused a DEPTH_COMPONENT32 texture";
			EXPECT_EQ(w, kFormatSize);
			EXPECT_EQ(h, kFormatSize);
			ASSERT_EQ(pixels.size(), static_cast<size_t>(kFormatTexels) * 4u);

			// Every texel of the attachment was cleared to the same value, so the row flip is not
			// what this half is measuring and the whole image must be one packed colour.
			for (int y = 0; y < kFormatSize; ++y) {
				for (int x = 0; x < kFormatSize; ++x) {
					EXPECT_EQ(texelAt(pixels, x, y), dc.packed)
						<< "depth texel (" << x << ", " << y << ") after a clear to " << dc.clearDepth
						<< ": the two backends store this format differently — a 32-bit unorm on "
						   "OpenGL, VK_FORMAT_D32_SFLOAT on Vulkan — and both are expected to answer "
						   "with the same 16-bit packing, so a difference here is either a lost clear "
						   "or a changed packing";
				}
			}

			rhi().destroyFramebuffer(fb);
			rhi().destroyRenderPass(pass);
			rhi().destroyTexture(colorTex);
			rhi().destroyTexture(depthTex);
		}
	}

	namespace {

		TextureHandle makeNativeIdTexture(RenderHardwareInterface& rhi) {
			TextureDesc desc;
			desc.width = kSize;
			desc.height = kSize;
			desc.depth = 1;
			desc.arrayLayers = 1;
			desc.dimension = TextureDimension::Tex2D;
			desc.format = TextureFormat::RGBA8;
			desc.usage = TextureUsage::Sampled | TextureUsage::TransferSrc | TextureUsage::TransferDst;
			return rhi.createTexture(desc);
		}
	}

	// getNativeTextureId is the one call in the interface that hands a backend object back to the
	// caller, so the two backends cannot possibly return the same *number* — OpenGL returns the GL
	// texture name (OpenGLRHI.cpp:2272-2276), Vulkan the VkImageView reinterpreted as a uint64
	// (VulkanRHIResources.cpp:836-841). What they can share, and what this pins, is the contract
	// around that number: live means non-zero, the same live handle always answers the same, two live
	// textures never answer the same, and anything not live answers 0.
	//
	// Nothing here asserts an ordering or a magnitude. Both backends allocate their ids elsewhere —
	// the GL driver, then the Vulkan allocator — so "the second id is larger" or "the first live
	// texture is 1" would be a property of this run's allocation order, not of the interface.
	TEST_P(RhiBackendTest, NativeTextureIdIsNonZeroStableAndDistinctPerTexture) {
		// Asked before anything is created, so the pool is empty and the answer cannot be a live
		// texture that happens to sit at the default handle's slot. Both backends route an
		// out-of-range id through the same getLive miss as a dead one.
		EXPECT_EQ(rhi().getNativeTextureId(TextureHandle{}), 0u)
			<< "a default-constructed handle named a native object: both backends look their handle up "
			   "through getLive and answer 0 on a miss, so a non-zero here means an unchecked index or "
			   "a sentinel other than 0 for 'no texture'";

		const auto first = makeNativeIdTexture(rhi());
		ASSERT_TRUE(first);

		const uint64_t firstId = rhi().getNativeTextureId(first);
		EXPECT_NE(firstId, 0u)
			<< "a live texture reported 0, which is the same answer both backends give for a handle "
			   "they cannot resolve: a caller handing this to ImGui or to the editor's viewport cannot "
			   "tell 'no texture' from 'this texture'";

		EXPECT_EQ(rhi().getNativeTextureId(first), firstId)
			<< "two calls on one unchanged handle disagreed, so the id is minted per call rather than "
			   "being the backend object's own identity, and a caller cannot cache it";

		const auto second = makeNativeIdTexture(rhi());
		ASSERT_TRUE(second);

		const uint64_t secondId = rhi().getNativeTextureId(second);
		EXPECT_NE(secondId, 0u);
		EXPECT_NE(firstId, secondId)
			<< "two textures alive at the same time share one native id: whatever the id names is "
			   "shared between them, so binding one would display the other";

		EXPECT_EQ(rhi().getNativeTextureId(first), firstId)
			<< "creating a second texture moved the first one's id: the pool reallocating must not "
			   "change what a live handle answers";

		// Asked before anything else is created, deliberately. Handles carry no generation counter and
		// both backends recycle ids through a free list, so the next createTexture would hand this
		// exact handle to a different texture and turn a dead-handle probe into a live one.
		rhi().destroyTexture(second);
		EXPECT_EQ(rhi().getNativeTextureId(second), 0u)
			<< "a destroyed handle still named a native object: the id would outlive the texture it "
			   "came from, and both backends have already deleted the underlying object by this point";

		EXPECT_EQ(rhi().getNativeTextureId(first), firstId)
			<< "destroying one texture changed another live texture's id";

		rhi().destroyTexture(first);
		EXPECT_EQ(rhi().getNativeTextureId(first), 0u);
	}

	namespace {

		// 2x2 slices, so a slice is four texels of one colour and a sample anywhere inside it returns
		// that colour whole. Four of them, because the failure this test exists to catch is an ordering
		// one and two slices cannot tell a swap from a reversal.
		constexpr uint32_t kVolumeSize = 2;
		constexpr uint32_t kVolumeDepth = 4;

		// Distinct in every channel pattern, so a slice answered from the wrong depth names the depth
		// it actually came from rather than merely disagreeing.
		constexpr Rgba kSliceColors[kVolumeDepth] = {
			{ 255,   0,   0, 255 },
			{   0, 255,   0, 255 },
			{   0,   0, 255, 255 },
			{ 255, 255,   0, 255 },
		};

		// Texel centres along W for a depth of four: exact binary fractions, each at the centre of its
		// own slice and half a slice away from either neighbour. So the answer is one slice and never a
		// blend of two, whatever the filter does between slices.
		constexpr float kSliceW[kVolumeDepth] = { 0.125f, 0.375f, 0.625f, 0.875f };

		PipelineHandle makeVolumePipeline(RenderHardwareInterface& rhi, RenderPassHandle pass,
			DescriptorSetLayoutHandle layout) {
			GraphicsPipelineDesc desc;
			desc.renderPass = pass;
			desc.topology = PrimitiveTopology::Triangles;
			desc.cullMode = CullMode::None;
			desc.frontFace = FrontFace::CounterClockWise;
			desc.fillMode = PolygonFillMode::Fill;
			desc.depthStencilState = { false, false, CompareOp::Always };
			desc.blend = { false };
			desc.descriptorSetLayouts = { layout };
			desc.pushConstantSize = sizeof(CubeProbePush);
			desc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
			desc.shaders.vertex = rhi.loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
			desc.shaders.fragment = rhi.loadShader(ShaderStage::Fragment, "shaders/rhi_test_texture_3d.fs");
			return rhi.createGraphicsPipeline(desc);
		}
	}

	// TextureDimension::Tex3D: that a volume can be created with initial data, and that a sample at a
	// given depth returns the slice uploaded at that depth index. Slice *order* is the whole point —
	// a backend that agreed about everything except which depth index is which slice would look
	// correct in any single-slice check and render a volume inside out — which is why the four slices
	// carry four colours rather than one gradient.
	//
	// The slices are read through a sampler3D, not through readbackTexture, and that is not a
	// preference: readbackTexture's `layer` does not reach a 3D texture's depth on either backend. It
	// is a 2D path that attaches the texture to a read framebuffer, and OpenGL rejects any dimension
	// that is not Tex2D or Array outright (OpenGLRHI.cpp:990) — glFramebufferTextureLayer is reached
	// only for an Array. The EXPECT_FALSE below pins that refusal, so the route this test takes is a
	// measured fact rather than a remark.
	//
	// The upload is createTexture's initial data and nothing else. updateTexture cannot write a
	// volume: OpenGL sends it through glTexSubImage2D against whatever target the dimension maps to
	// (OpenGLRHI.cpp:2296-2303), which is not a call GL_TEXTURE_3D accepts, and Vulkan's region is a
	// fixed one-layer, depth-one extent (VulkanRHIResources.cpp:772-773). There is no z or depth
	// parameter in the signature to carry one either.
	TEST_P(RhiBackendTest, Texture3DSlicesReadBackByTheDepthIndexTheyWereUploadedAt) {
		std::vector<uint8_t> source;
		source.reserve(static_cast<size_t>(kVolumeSize) * kVolumeSize * kVolumeDepth * 4u);
		for (const auto& color : kSliceColors) {
			const auto slice = solid(kVolumeSize, kVolumeSize, color);
			source.insert(source.end(), slice.begin(), slice.end());
		}

		TextureDesc desc;
		desc.width = kVolumeSize;
		desc.height = kVolumeSize;
		desc.depth = kVolumeDepth;
		desc.arrayLayers = 1;
		desc.dimension = TextureDimension::Tex3D;
		desc.format = TextureFormat::RGBA8;
		desc.usage = TextureUsage::Sampled | TextureUsage::TransferSrc | TextureUsage::TransferDst;
		const auto volume = rhi().createTexture(desc, source.data());
		if (!volume) {
			GTEST_SKIP() << "this backend has no 3D textures: VulkanRHI::createTexture rejects the "
							"dimension before it reaches createImage (VulkanRHIResources.cpp:713-716), "
							"and createImage has no path that could serve one anyway — it hardcodes "
							"imageType = VK_IMAGE_TYPE_2D (VulkanRHIResources.cpp:638) and picks its "
							"view type from Cube/Array/2D only (:624-632)";
		}

		// Pinned, not assumed: this is why the probe below exists. `layer` is an array-layer index on
		// both backends and there is no argument in the signature that names a depth.
		std::vector<uint8_t> pixels;
		int w = 0;
		int h = 0;
		EXPECT_FALSE(rhi().readbackTexture(volume, pixels, w, h, 1))
			<< "readbackTexture answered for a Tex3D: if its `layer` now reaches a volume's depth, the "
			   "shader probe this test uses instead is no longer the only route to a slice and the "
			   "cheaper one should replace it";

		SamplerDesc samplerDesc;
		// Nearest, not Linear: the texture has one mip level, so nothing here needs the chain that
		// only LinearMipmapLinear opens, and a filter that cannot blend keeps every expectation an
		// exact byte even if a probe point were to drift off a texel centre.
		samplerDesc.minFilter = FilterMode::Nearest;
		samplerDesc.magFilter = FilterMode::Nearest;
		samplerDesc.addressU = AddressMode::ClampToEdge;
		samplerDesc.addressV = AddressMode::ClampToEdge;
		samplerDesc.addressW = AddressMode::ClampToEdge;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		const auto target = makeLodTarget(rhi());
		ASSERT_TRUE(target);
		// White is in no slice's colour, so "the draw never happened" cannot be mistaken for a slice.
		const auto pass = makeLodPass(rhi(), { 1.f, 1.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		const auto pipeline = makeVolumePipeline(rhi(), pass, layout);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		const auto set = rhi().allocateDescriptorSet(layout);
		ASSERT_TRUE(set);
		WriteDescriptor image{};
		image.set = set;
		image.binding = 0;
		image.images.push_back({ volume, sampler });
		rhi().updateDescriptorSets({ image });

		const auto probe = [&](uint32_t slice) -> Rgba {
			// drawCubeProbe rather than a copy of it: it pushes four floats and draws a full-screen
			// triangle through the pipeline it is handed, which is exactly what this probe needs, and
			// the four floats here are the sample point instead of a direction and a lod.
			const CubeProbePush push{ 0.5f, 0.5f, kSliceW[slice], 0.f };
			const auto probePixels = drawCubeProbe(rhi(), target, pass, pipeline, set, push);
			EXPECT_FALSE(probePixels.empty()) << "readback failed";
			const Rgba first = pixelAt(probePixels, 0, 0);
			// Every fragment samples the same point, so the target must be one colour throughout. That
			// also makes the readback's row flip irrelevant here, which is why the probe is a single
			// colour rather than a picture.
			for (int y = 0; y < kSize; ++y) {
				for (int x = 0; x < kSize; ++x) {
					EXPECT_EQ(pixelAt(probePixels, x, y), first)
						<< "pixel (" << x << ", " << y << ") differs from the rest although every "
						   "fragment sampled the same point: the draw did not cover the target";
				}
			}
			return first;
		};

		for (uint32_t slice = 0; slice < kVolumeDepth; ++slice) {
			EXPECT_EQ(probe(slice), kSliceColors[slice])
				<< "depth index " << slice << " did not return the colour uploaded at that index: the "
				   "upload is one buffer of " << kVolumeDepth << " slices back to back, first slice "
				   "first, so another slice's colour here means this backend orders a volume's depth "
				   "differently from the other one — the returned colour names the slice this depth "
				   "actually reached";
		}

		rhi().freeDescriptorSet(set);
		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroySampler(sampler);
		rhi().destroyTexture(volume);
		rhi().destroyRenderPass(pass);
		rhi().destroyTexture(target);
	}

	// A linear sample whose footprint straddles a cube edge. Design value is the seamless answer:
	// Vulkan always filters across the edge, and OpenGL does too once GL_TEXTURE_CUBE_MAP_SEAMLESS is
	// enabled. Without it OpenGL clamps inside the one face and returns that face's colour unblended.
	//
	// The direction is picked so the weights are exact rather than just "near the edge". On the +X
	// face t = (1 - y/x) / 2, so y/x = 0.75 gives t = 0.125, which on a 2-texel face is texel
	// coordinate -0.25: 0.75 of the weight on +X's edge row and 0.25 on the adjacent +Y row. Both
	// faces are flat and the column coordinate is exactly 0.5, so orientation across the edge cannot
	// matter. A quarter needs only 2 bits of sub-texel precision (Vulkan guarantees at least 4), so
	// the only error left is 8-bit rounding of 191.25 and 63.75: a tolerance of 2 covers it, while
	// the unblended answer is 64 away in both channels.
	TEST_P(RhiBackendTest, CubeLinearSampleAtAFaceEdgeBlendsTheAdjacentFace) {
		TextureDesc cubeDesc;
		cubeDesc.width = kCubeSize;
		cubeDesc.height = kCubeSize;
		cubeDesc.depth = 1;
		cubeDesc.arrayLayers = 6;
		cubeDesc.mipLevels = 1;
		cubeDesc.dimension = TextureDimension::Cube;
		cubeDesc.format = TextureFormat::RGBA8;
		cubeDesc.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
		const auto cube = rhi().createTexture(cubeDesc);
		ASSERT_TRUE(cube);

		for (uint32_t face = 0; face < 6; ++face) {
			const auto bytes = solid(kCubeSize, kCubeSize, kFaceColors[face]);
			rhi().updateCubeFace(cube, face, 0, bytes.data());
		}

		SamplerDesc samplerDesc;
		samplerDesc.minFilter = FilterMode::Linear;
		samplerDesc.magFilter = FilterMode::Linear;
		samplerDesc.addressU = AddressMode::ClampToEdge;
		samplerDesc.addressV = AddressMode::ClampToEdge;
		samplerDesc.addressW = AddressMode::ClampToEdge;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		const auto target = makeLodTarget(rhi());
		ASSERT_TRUE(target);
		const auto pass = makeLodPass(rhi(), { 1.f, 1.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		const auto pipeline = makeCubePipeline(rhi(), pass, layout);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		const auto set = rhi().allocateDescriptorSet(layout);
		ASSERT_TRUE(set);
		WriteDescriptor image{};
		image.set = set;
		image.binding = 0;
		image.images.push_back({ cube, sampler });
		rhi().updateDescriptorSets({ image });

		const auto pixels = drawCubeProbe(rhi(), target, pass, pipeline, set, { 1.f, 0.75f, 0.f, 0.f });
		ASSERT_FALSE(pixels.empty()) << "readback failed";
		const Rgba got = pixelAt(pixels, 0, 0);

		// 0.75 of +X (red) and 0.25 of +Y (blue).
		const Rgba expected{ 191, 0, 64, 255 };
		const auto near = [](uint8_t a, uint8_t b) { return std::abs(int(a) - int(b)) <= 2; };
		EXPECT_TRUE(near(got.r, expected.r) && near(got.g, expected.g) && near(got.b, expected.b)
			&& got.a == expected.a)
			<< "sample at (1, 0.75, 0) returned (" << int(got.r) << ", " << int(got.g) << ", "
			<< int(got.b) << ", " << int(got.a) << "), expected about (191, 0, 64, 255). (255, 0, 0, 255) "
			   "means the filter clamped inside +X instead of blending across the edge into +Y: "
			   "seamless cube filtering is off";

		rhi().freeDescriptorSet(set);
		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroySampler(sampler);
		rhi().destroyTexture(cube);
		rhi().destroyRenderPass(pass);
		rhi().destroyTexture(target);
	}

	namespace {

		// One level of a mip chain, texels in texture row order (row 0 is the first uploaded row).
		struct MipImage {
			uint32_t w = 0;
			uint32_t h = 0;
			std::vector<uint8_t> rgba;
		};

		// A 2x2 box average of the level above, rounded to nearest. Odd sizes reduce by floor (the
		// size rule both backends use for the chain) and drop the last row/column; an axis already at
		// 1 averages along the other axis only.
		MipImage boxReduce(const MipImage& src) {
			MipImage dst;
			dst.w = (std::max)(1u, src.w / 2);
			dst.h = (std::max)(1u, src.h / 2);
			dst.rgba.resize(static_cast<size_t>(dst.w) * dst.h * 4u);
			const uint32_t sx = src.w > 1 ? 2u : 1u;
			const uint32_t sy = src.h > 1 ? 2u : 1u;
			const uint32_t n = sx * sy;
			for (uint32_t y = 0; y < dst.h; ++y) {
				for (uint32_t x = 0; x < dst.w; ++x) {
					for (uint32_t c = 0; c < 4; ++c) {
						uint32_t sum = 0;
						for (uint32_t dy = 0; dy < sy; ++dy) {
							for (uint32_t dx = 0; dx < sx; ++dx) {
								sum += src.rgba[((static_cast<size_t>(y) * sy + dy) * src.w + x * sx + dx) * 4u + c];
							}
						}
						dst.rgba[(static_cast<size_t>(y) * dst.w + x) * 4u + c] = static_cast<uint8_t>((sum + n / 2) / n);
					}
				}
			}
			return dst;
		}

		int maxChannelDiff(const MipImage& a, const MipImage& b) {
			if (a.rgba.size() != b.rgba.size()) {
				return 256;
			}
			int worst = 0;
			for (size_t i = 0; i < a.rgba.size(); ++i) {
				worst = (std::max)(worst, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
			}
			return worst;
		}

		// Levels read on the backend that ran first, keyed by "WxH/level", so the backend that runs
		// second in the same process can say whether the two chains are identical.
		std::map<std::string, MipImage>& mipChainsSeen() {
			static std::map<std::string, MipImage> seen;
			return seen;
		}

		// Draws one mip level of `texture` into a w x h target with textureLod and hands it back in texture
		// row order: the target's pixel centres are that level's texel centres, so each fragment returns one
		// texel whole. Shared by the test that judges a chain the driver built and the one that judges a
		// chain the caller uploaded level by level.
		MipImage readMipLevel(RenderHardwareInterface& rhi, RenderPassHandle pass, PipelineHandle pipeline,
			DescriptorSetLayoutHandle layout, SamplerHandle sampler, TextureHandle texture,
			uint32_t lod, uint32_t w, uint32_t h) {
			MipImage out;
			TextureDesc targetDesc;
			targetDesc.width = w;
			targetDesc.height = h;
			targetDesc.format = TextureFormat::RGBA8;
			targetDesc.dimension = TextureDimension::Tex2D;
			targetDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::TransferSrc;
			const auto target = rhi.createTexture(targetDesc);
			FramebufferDesc fbDesc;
			fbDesc.width = w;
			fbDesc.height = h;
			fbDesc.layers = 1;
			fbDesc.renderPass = pass;
			FramebufferAttachmentDesc att;
			att.texture = target;
			att.layer = UINT32_MAX;
			fbDesc.colorAttachments = { att };
			const auto fb = rhi.createFramebuffer(fbDesc);

			BufferDesc paramsDesc;
			paramsDesc.size = sizeof(Math::Vec4);
			paramsDesc.usage = BufferUsage::Uniform;
			paramsDesc.cpuAccess = CpuAccess::Write;
			paramsDesc.debugName = "rhi_test_mip_level";
			const Math::Vec4 params{ static_cast<float>(lod), 0.f, 0.f, 0.f };
			const auto paramsBuffer = rhi.createBuffer(paramsDesc, &params);

			const auto set = rhi.allocateDescriptorSet(layout);
			WriteDescriptor image{};
			image.set = set;
			image.binding = 0;
			image.images.push_back({ texture, sampler });
			WriteDescriptor uniform{};
			uniform.set = set;
			uniform.binding = 1;
			uniform.buffers.push_back({ paramsBuffer, 0, 0 });
			rhi.updateDescriptorSets({ image, uniform });

			rhi.beginFrame();
			const auto cmd = rhi.beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = w;
			begin.height = h;
			rhi.cmdBeginRenderPass(cmd, begin);
			rhi.cmdBindPipeline(cmd, pipeline);
			rhi.cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi.cmdDraw(cmd, 3, 1, 0, 0);
			rhi.cmdEndRenderPass(cmd);
			rhi.endCommandList(cmd);
			rhi.submit(cmd);
			rhi.present();
			rhi.waitIdle();

			std::vector<uint8_t> pixels;
			int rw = 0;
			int rh = 0;
			if (rhi.readbackTexture(target, pixels, rw, rh) && rw == int(w) && rh == int(h)
				&& pixels.size() == static_cast<size_t>(w) * h * 4u) {
				out.w = w;
				out.h = h;
				out.rgba.resize(pixels.size());
				// Readback rows come back flipped: image row i is texture row h - 1 - i.
				for (uint32_t row = 0; row < h; ++row) {
					std::copy_n(pixels.begin() + static_cast<ptrdiff_t>((h - 1 - row) * w * 4u), w * 4u,
						out.rgba.begin() + static_cast<ptrdiff_t>(row * w * 4u));
				}
			}

			rhi.freeDescriptorSet(set);
			rhi.destroyBuffer(paramsBuffer);
			rhi.destroyFramebuffer(fb);
			rhi.destroyTexture(target);
			return out;
		}
	}

	// Generated mip contents, level by level, against a CPU 2x2 box filter. The frame comparison sees
	// a small systematic albedo difference on textured surfaces; OpenGL builds the chain with
	// glGenerateMipmap and Vulkan with a VK_FILTER_LINEAR vkCmdBlitImage chain, and
	// TextureMipLevelsAreSelectedByExplicitLod uses data whose averages are exact, so it cannot see a
	// filter difference. Here the data is a fixed LCG byte pattern at a power-of-two size. Odd sizes are
	// left out on purpose: both backends reduce 37 to 18 texels by stretching, byte-identically, so a 2x2
	// box expectation there would pin a filter neither backend uses rather than a divergence.
	//
	// The texture is created the way TextureHandler::loadTexture creates material textures (RGBA8,
	// Tex2D, usage Sampled, data passed to createTexture) plus the full mip chain under test.
	//
	// Each level is read by drawing a target of that level's size with textureLod(uv, level): the
	// fragment centres are the level's texel centres, so the LinearMipmapLinear sampler (the only one
	// that reaches past level 0) returns one texel whole with a lod blend weight of zero.
	//
	// Expectation per level: the box average of the backend's own previous level as read back, so a
	// level is judged on its own filter and errors do not accumulate down the chain. Tolerance 1: a
	// 2x2 average falls on .25/.5/.75, and a correct implementation may resolve .5 either way.
	TEST_P(RhiBackendTest, GeneratedMipLevelsMatchA2x2BoxFilter) {
		const char* backend = graphicsBackendName(GetParam());

		SamplerDesc samplerDesc;
		samplerDesc.minFilter = FilterMode::LinearMipmapLinear;
		samplerDesc.magFilter = FilterMode::Linear;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		const auto pass = makeLodPass(rhi(), { 1.f, 0.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.renderPass = pass;
		pipelineDesc.topology = PrimitiveTopology::Triangles;
		pipelineDesc.cullMode = CullMode::None;
		pipelineDesc.frontFace = FrontFace::CounterClockWise;
		pipelineDesc.fillMode = PolygonFillMode::Fill;
		pipelineDesc.depthStencilState = { false, false, CompareOp::Always };
		pipelineDesc.blend = { false };
		pipelineDesc.descriptorSetLayouts = { layout };
		pipelineDesc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
		pipelineDesc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, "shaders/rhi_test_texture_mip_level.fs");
		const auto pipeline = rhi().createGraphicsPipeline(pipelineDesc);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		const auto readLevel = [&](TextureHandle texture, uint32_t lod, uint32_t w, uint32_t h) {
			return readMipLevel(rhi(), pass, pipeline, layout, sampler, texture, lod, w, h);
		};

		struct SizeCase { uint32_t w; uint32_t h; };
		for (const SizeCase sc : { SizeCase{ 64, 64 } }) {
			MipImage source{ sc.w, sc.h, {} };
			source.rgba.resize(static_cast<size_t>(sc.w) * sc.h * 4u);
			uint32_t lcg = 12345u;
			for (auto& byte : source.rgba) {
				lcg = lcg * 1664525u + 1013904223u;
				byte = static_cast<uint8_t>(lcg >> 24);
			}

			uint32_t levels = 1;
			while ((std::max)(sc.w, sc.h) >> levels) {
				++levels;
			}

			TextureDesc desc;
			desc.width = sc.w;
			desc.height = sc.h;
			desc.format = TextureFormat::RGBA8;
			desc.dimension = TextureDimension::Tex2D;
			desc.mipLevels = levels;
			const auto texture = rhi().createTexture(desc, source.rgba.data());
			ASSERT_TRUE(texture);

			MipImage previous = source;
			MipImage cpuChain = source;
			for (uint32_t lod = 0; lod < levels; ++lod) {
				const uint32_t w = (std::max)(1u, sc.w >> lod);
				const uint32_t h = (std::max)(1u, sc.h >> lod);
				const MipImage got = readLevel(texture, lod, w, h);
				ASSERT_EQ(got.rgba.size(), static_cast<size_t>(w) * h * 4u) << "level " << lod << " readback failed";

				const MipImage expected = lod == 0 ? source : boxReduce(previous);
				if (lod > 0) {
					cpuChain = boxReduce(cpuChain);
				}
				const int diffOwn = maxChannelDiff(got, expected);
				const int diffChain = maxChannelDiff(got, cpuChain);

				const std::string key = std::to_string(sc.w) + "x" + std::to_string(sc.h) + "/" + std::to_string(lod);
				auto& seen = mipChainsSeen();
				const auto other = seen.find(key);
				std::string versus = "first backend to run";
				if (other != seen.end()) {
					versus = "max diff vs other backend " + std::to_string(maxChannelDiff(got, other->second));
				}
				else {
					seen.emplace(key, got);
				}
				std::cout << "[mip] " << backend << " " << key << " (" << w << "x" << h << "): max diff vs box of "
					<< "own previous level " << diffOwn << ", vs CPU box chain " << diffChain << ", " << versus << "\n";

				EXPECT_LE(diffOwn, lod == 0 ? 0 : 1)
					<< sc.w << "x" << sc.h << " level " << lod << " (" << w << "x" << h << ") differs from the 2x2 "
					   "box average of the level above by up to " << diffOwn << "/255: the backend's mip "
					   "generation is not a box filter at this size";
				previous = got;
			}
			rhi().destroyTexture(texture);
		}

		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroyRenderPass(pass);
		rhi().destroySampler(sampler);
	}

	// The other half of the contract updateTexture's mip parameter buys: a caller that builds the chain
	// itself gets exactly its own bytes in every level, on both backends. TextureHandler does this so that
	// OpenGL and Vulkan sample the same texels -- their own generated chains differ, which the test above
	// measures. Solid colours, one per level, and none of them a blend of any other pair: a level that came
	// from the driver's filter instead of from the upload reads as a mixture and names itself. The same for
	// the block compressed formats TextureHandler keeps the textures read from files in.
	TEST_P(RhiBackendTest, UploadedMipLevelsAreTheOnesSampledBack) {
		constexpr uint32_t kBase = 64;
		constexpr uint32_t kLevels = 7; // 64, 32, 16, 8, 4, 2, 1
		const Rgba kLevelColours[kLevels] = {
			{ 10, 20, 30, 255 }, { 200, 40, 60, 255 }, { 70, 210, 90, 255 }, { 110, 120, 220, 255 },
			{ 240, 230, 50, 255 }, { 130, 30, 200, 255 }, { 80, 170, 160, 255 },
		};

		SamplerDesc samplerDesc;
		samplerDesc.minFilter = FilterMode::LinearMipmapLinear;
		samplerDesc.magFilter = FilterMode::Linear;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		const auto pass = makeLodPass(rhi(), { 1.f, 0.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.renderPass = pass;
		pipelineDesc.topology = PrimitiveTopology::Triangles;
		pipelineDesc.cullMode = CullMode::None;
		pipelineDesc.frontFace = FrontFace::CounterClockWise;
		pipelineDesc.fillMode = PolygonFillMode::Fill;
		pipelineDesc.depthStencilState = { false, false, CompareOp::Always };
		pipelineDesc.blend = { false };
		pipelineDesc.descriptorSetLayouts = { layout };
		pipelineDesc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
		pipelineDesc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, "shaders/rhi_test_texture_mip_level.fs");
		const auto pipeline = rhi().createGraphicsPipeline(pipelineDesc);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		// RGBA8 as the caller's own bytes. BC7 and BC4 as TextureHandler writes a compressed image's levels: each
		// level's colour encoded by the encoders it uses, over a base no multiple of 4, whose levels end in part
		// blocks. BC4 is red alone, read as grey (TextureSwizzle::Grey): the red in r, g and b, as an RGBA8 grey image
		// reads. An encoder may miss a colour by a level or two; a level from the driver's filter misses by far more.
		struct FormatCase {
			const char* name;
			TextureFormat format;
			TextureSwizzle swizzle;
			uint32_t w;
			uint32_t h;
			int tolerance;
		};
		for (const FormatCase fc : { FormatCase{ "RGBA8", TextureFormat::RGBA8, TextureSwizzle::None, kBase, kBase, 0 },
			FormatCase{ "BC7", TextureFormat::BC7_UNORM, TextureSwizzle::None, 20, 12, 2 },
			FormatCase{ "BC4 grey", TextureFormat::BC4_UNORM, TextureSwizzle::Grey, 20, 12, 2 } }) {
			TextureDesc desc;
			desc.width = fc.w;
			desc.height = fc.h;
			desc.format = fc.format;
			desc.swizzle = fc.swizzle;
			desc.dimension = TextureDimension::Tex2D;
			desc.mipLevels = mipLevelsFor(fc.w, fc.h);
			ASSERT_LE(desc.mipLevels, kLevels);
			// No pixels here: with them a backend builds the rest of the chain, and this test is about the
			// levels the caller writes.
			const auto texture = rhi().createTexture(desc);
			ASSERT_TRUE(texture) << fc.name;

			const auto colourOf = [&](const uint32_t lod) {
				const Rgba& colour = kLevelColours[lod];
				return fc.swizzle == TextureSwizzle::Grey ? Rgba{ colour.r, colour.r, colour.r, 255 } : colour;
			};
			for (uint32_t lod = 0; lod < desc.mipLevels; ++lod) {
				const uint32_t w = (std::max)(1u, fc.w >> lod);
				const uint32_t h = (std::max)(1u, fc.h >> lod);
				const auto pixels = isBlockCompressed(fc.format) ? solidBlocks(fc.format, w, h, colourOf(lod)) : solid(w, h, colourOf(lod));
				rhi().updateTexture(texture, 0, 0, w, h, fc.format, pixels.data(), lod);
			}

			for (uint32_t lod = 0; lod < desc.mipLevels; ++lod) {
				const uint32_t w = (std::max)(1u, fc.w >> lod);
				const uint32_t h = (std::max)(1u, fc.h >> lod);
				const MipImage got = readMipLevel(rhi(), pass, pipeline, layout, sampler, texture, lod, w, h);
				ASSERT_EQ(got.rgba.size(), static_cast<size_t>(w) * h * 4u) << fc.name << " level " << lod << " readback failed";
				const Rgba want = colourOf(lod);
				for (size_t texel = 0; texel < got.rgba.size(); texel += 4) {
					ASSERT_LE(std::abs(got.rgba[texel + 0] - want.r), fc.tolerance) << fc.name << " level " << lod << " texel " << texel / 4;
					ASSERT_LE(std::abs(got.rgba[texel + 1] - want.g), fc.tolerance) << fc.name << " level " << lod << " texel " << texel / 4;
					ASSERT_LE(std::abs(got.rgba[texel + 2] - want.b), fc.tolerance) << fc.name << " level " << lod << " texel " << texel / 4;
				}
			}
			rhi().destroyTexture(texture);
		}

		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroyRenderPass(pass);
		rhi().destroySampler(sampler);
	}

	namespace {

		constexpr uint32_t kBilinearTex = 8;

		// One sampling case: target size, and uv = offset + (pixel + 0.5) / size * scale.
		struct BilinearCase {
			const char* name;
			uint32_t w;
			uint32_t h;
			float scaleX;
			float scaleY;
			float offsetX;
			float offsetY;
		};

		// Filtered RGB, indexed [row][column] by the row the shader reported, so both backends are
		// compared pixel for pixel whatever order their readback returns rows in.
		struct BilinearImage {
			uint32_t w = 0;
			uint32_t h = 0;
			std::vector<uint8_t> rgb;
		};

		std::map<std::string, BilinearImage>& bilinearSeen() {
			static std::map<std::string, BilinearImage> seen;
			return seen;
		}

		// High contrast on purpose: a checker in R, row stripes in G and scattered mid values in B,
		// so every bilinear footprint blends texels that differ by up to 255 and a weight error shows.
		std::vector<uint8_t> bilinearSource() {
			std::vector<uint8_t> bytes;
			for (uint32_t y = 0; y < kBilinearTex; ++y) {
				for (uint32_t x = 0; x < kBilinearTex; ++x) {
					const uint8_t r = ((x + y) & 1u) ? 255 : 0;
					const uint8_t g = (y & 1u) ? 0 : 255;
					const uint8_t b = static_cast<uint8_t>((x * 37u + y * 91u + 13u) % 256u);
					bytes.insert(bytes.end(), { r, g, b, 255 });
				}
			}
			return bytes;
		}

		void bilinearUv(const BilinearCase& c, uint32_t x, uint32_t y, float& u, float& v) {
			u = c.offsetX + (static_cast<float>(x) + 0.5f) / static_cast<float>(c.w) * c.scaleX;
			v = c.offsetY + (static_cast<float>(y) + 0.5f) / static_cast<float>(c.h) * c.scaleY;
		}

		// Exact bilinear weights with Repeat wrapping, rounded to nearest: what a GL_LINEAR /
		// VK_FILTER_LINEAR lookup on level 0 is defined to return, before any sub-texel quantisation.
		uint8_t cpuBilinear(const std::vector<uint8_t>& tex, float u, float v, uint32_t channel) {
			const double s = static_cast<double>(u) * kBilinearTex - 0.5;
			const double t = static_cast<double>(v) * kBilinearTex - 0.5;
			const double fs = std::floor(s);
			const double ft = std::floor(t);
			const double a = s - fs;
			const double b = t - ft;
			const auto wrap = [](double i) {
				const long long n = static_cast<long long>(i) % static_cast<long long>(kBilinearTex);
				return static_cast<uint32_t>(n < 0 ? n + kBilinearTex : n);
			};
			const uint32_t x0 = wrap(fs);
			const uint32_t x1 = wrap(fs + 1.0);
			const uint32_t y0 = wrap(ft);
			const uint32_t y1 = wrap(ft + 1.0);
			const auto at = [&](uint32_t x, uint32_t y) {
				return static_cast<double>(tex[(static_cast<size_t>(y) * kBilinearTex + x) * 4u + channel]);
			};
			const double value = (1.0 - a) * (1.0 - b) * at(x0, y0) + a * (1.0 - b) * at(x1, y0)
				+ (1.0 - a) * b * at(x0, y1) + a * b * at(x1, y1);
			return static_cast<uint8_t>(std::floor(value + 0.5));
		}
	}

	// Bilinear filtering itself, compared across backends at arbitrary sub-texel positions. The frame
	// comparison shows G-buffer albedo differing by up to 12/255 on about 1.4% of pixels of textured
	// characters; mip generation, edge coverage, sampler units and depth formats are already ruled
	// out, which leaves the Linear lookup on level 0. Every other sampling test here lands on texel
	// centres or flat colours, where the filter weights cannot matter.
	//
	// Texture and sampler are made exactly as TextureHandler::loadTexture makes material ones: RGBA8
	// Tex2D with the default mipLevels 1 and usage Sampled, data passed to createTexture, and a
	// Linear/Linear Repeat sampler with maxAnisotropy 1. The lookup is texture(), not textureLod.
	//
	// Three cases: magnification sweeping the fractional offset through each texel, minification
	// where each fragment spans more than one texel (no mips, so still one bilinear footprint), and
	// UVs beyond 1 through Repeat. Each is measured against the CPU bilinear result and, on the second
	// backend to run, against the first backend's bytes. Only the latter is asserted: tolerance 1,
	// the rounding two correct implementations may differ by.
	TEST_P(RhiBackendTest, BilinearSamplingMatchesAcrossBackendsAtSubTexelUvs) {
		const char* backend = graphicsBackendName(GetParam());
		const auto source = bilinearSource();

		TextureDesc texDesc;
		texDesc.width = kBilinearTex;
		texDesc.height = kBilinearTex;
		texDesc.format = TextureFormat::RGBA8;
		texDesc.dimension = TextureDimension::Tex2D;
		const auto texture = rhi().createTexture(texDesc, source.data());
		ASSERT_TRUE(texture);

		SamplerDesc samplerDesc;
		samplerDesc.minFilter = FilterMode::Linear;
		samplerDesc.magFilter = FilterMode::Linear;
		samplerDesc.addressU = AddressMode::Repeat;
		samplerDesc.addressV = AddressMode::Repeat;
		samplerDesc.addressW = AddressMode::Repeat;
		samplerDesc.maxAnisotropy = 1.f;
		const auto sampler = rhi().createSampler(samplerDesc);
		ASSERT_TRUE(sampler);

		// Magenta, alpha 255: a pixel the draw never touched reports row 255 and is caught below.
		const auto pass = makeLodPass(rhi(), { 1.f, 0.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.renderPass = pass;
		pipelineDesc.topology = PrimitiveTopology::Triangles;
		pipelineDesc.cullMode = CullMode::None;
		pipelineDesc.frontFace = FrontFace::CounterClockWise;
		pipelineDesc.fillMode = PolygonFillMode::Fill;
		pipelineDesc.depthStencilState = { false, false, CompareOp::Always };
		pipelineDesc.blend = { false };
		pipelineDesc.descriptorSetLayouts = { layout };
		pipelineDesc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
		pipelineDesc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, "shaders/rhi_test_texture_bilinear.fs");
		const auto pipeline = rhi().createGraphicsPipeline(pipelineDesc);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		// Scales are not binary fractions, so the sub-texel offsets land on arbitrary values rather than
		// on multiples of 1/16 or 1/256 that a quantised filter would reproduce exactly.
		const BilinearCase cases[] = {
			{ "magnify", 64, 64, 0.9137f, 0.7731f, 0.0313f, 0.0471f },
			{ "minify", 3, 3, 1.0f, 1.0f, 0.0123f, 0.0371f },
			{ "repeat", 64, 64, 2.3071f, 1.9113f, 0.6173f, 0.8519f },
		};

		for (const auto& c : cases) {
			SCOPED_TRACE(c.name);

			TextureDesc targetDesc;
			targetDesc.width = c.w;
			targetDesc.height = c.h;
			targetDesc.format = TextureFormat::RGBA8;
			targetDesc.dimension = TextureDimension::Tex2D;
			targetDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::TransferSrc;
			const auto target = rhi().createTexture(targetDesc);
			ASSERT_TRUE(target);
			FramebufferDesc fbDesc;
			fbDesc.width = c.w;
			fbDesc.height = c.h;
			fbDesc.layers = 1;
			fbDesc.renderPass = pass;
			FramebufferAttachmentDesc att;
			att.texture = target;
			att.layer = UINT32_MAX;
			fbDesc.colorAttachments = { att };
			const auto fb = rhi().createFramebuffer(fbDesc);
			ASSERT_TRUE(fb);

			const float params[8] = { c.scaleX, c.scaleY, c.offsetX, c.offsetY,
				static_cast<float>(c.w), static_cast<float>(c.h), 0.f, 0.f };
			BufferDesc paramsDesc;
			paramsDesc.size = sizeof(params);
			paramsDesc.usage = BufferUsage::Uniform;
			paramsDesc.cpuAccess = CpuAccess::Write;
			paramsDesc.debugName = "rhi_test_texture_bilinear";
			const auto paramsBuffer = rhi().createBuffer(paramsDesc, params);
			ASSERT_TRUE(paramsBuffer);

			const auto set = rhi().allocateDescriptorSet(layout);
			ASSERT_TRUE(set);
			WriteDescriptor image{};
			image.set = set;
			image.binding = 0;
			image.images.push_back({ texture, sampler });
			WriteDescriptor uniform{};
			uniform.set = set;
			uniform.binding = 1;
			uniform.buffers.push_back({ paramsBuffer, 0, 0 });
			rhi().updateDescriptorSets({ image, uniform });

			rhi().beginFrame();
			const auto cmd = rhi().beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = c.w;
			begin.height = c.h;
			rhi().cmdBeginRenderPass(cmd, begin);
			rhi().cmdBindPipeline(cmd, pipeline);
			rhi().cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi().cmdDraw(cmd, 3, 1, 0, 0);
			rhi().cmdEndRenderPass(cmd);
			rhi().endCommandList(cmd);
			rhi().submit(cmd);
			rhi().present();
			rhi().waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			const bool readOk = rhi().readbackTexture(target, pixels, w, h);
			rhi().freeDescriptorSet(set);
			rhi().destroyBuffer(paramsBuffer);
			rhi().destroyFramebuffer(fb);
			rhi().destroyTexture(target);
			ASSERT_TRUE(readOk) << "readback failed";
			ASSERT_EQ(pixels.size(), static_cast<size_t>(c.w) * c.h * 4u);

			// File each readback row under the row index the shader wrote into alpha.
			BilinearImage got;
			got.w = c.w;
			got.h = c.h;
			got.rgb.assign(static_cast<size_t>(c.w) * c.h * 3u, 0);
			std::vector<bool> filled(c.h, false);
			for (uint32_t row = 0; row < c.h; ++row) {
				const uint8_t reported = pixels[static_cast<size_t>(row) * c.w * 4u + 3u];
				ASSERT_LT(reported, c.h) << "readback row " << row << " reports row " << int(reported)
					<< ": the draw did not cover the target";
				ASSERT_FALSE(filled[reported]) << "two readback rows report row " << int(reported);
				filled[reported] = true;
				for (uint32_t x = 0; x < c.w; ++x) {
					const size_t src = (static_cast<size_t>(row) * c.w + x) * 4u;
					const size_t dst = (static_cast<size_t>(reported) * c.w + x) * 3u;
					got.rgb[dst] = pixels[src];
					got.rgb[dst + 1] = pixels[src + 1];
					got.rgb[dst + 2] = pixels[src + 2];
				}
			}

			// Worst channel against the CPU filter, and against the other backend when it has run.
			const auto describe = [&](uint32_t x, uint32_t y) {
				float u = 0.f;
				float v = 0.f;
				bilinearUv(c, x, y, u, v);
				std::ostringstream out;
				out << "pixel (" << x << ", " << y << ") uv (" << u << ", " << v << ")";
				return out.str();
			};
			const auto rgbAt = [](const BilinearImage& img, uint32_t x, uint32_t y) {
				const size_t i = (static_cast<size_t>(y) * img.w + x) * 3u;
				std::ostringstream out;
				out << "(" << int(img.rgb[i]) << ", " << int(img.rgb[i + 1]) << ", " << int(img.rgb[i + 2]) << ")";
				return out.str();
			};

			BilinearImage cpu = got;
			int cpuWorst = 0;
			uint32_t cpuX = 0;
			uint32_t cpuY = 0;
			for (uint32_t y = 0; y < c.h; ++y) {
				for (uint32_t x = 0; x < c.w; ++x) {
					float u = 0.f;
					float v = 0.f;
					bilinearUv(c, x, y, u, v);
					for (uint32_t ch = 0; ch < 3; ++ch) {
						const size_t i = (static_cast<size_t>(y) * c.w + x) * 3u + ch;
						cpu.rgb[i] = cpuBilinear(source, u, v, ch);
						const int d = std::abs(int(got.rgb[i]) - int(cpu.rgb[i]));
						if (d > cpuWorst) {
							cpuWorst = d;
							cpuX = x;
							cpuY = y;
						}
					}
				}
			}
			std::cout << "[bilinear] " << backend << " " << c.name << " (" << c.w << "x" << c.h << "): max diff vs "
				"CPU bilinear " << cpuWorst << " at " << describe(cpuX, cpuY) << " got " << rgbAt(got, cpuX, cpuY)
				<< " cpu " << rgbAt(cpu, cpuX, cpuY) << "\n";

			auto& seen = bilinearSeen();
			const auto other = seen.find(c.name);
			if (other == seen.end()) {
				seen.emplace(c.name, got);
				continue;
			}
			int crossWorst = 0;
			uint32_t crossX = 0;
			uint32_t crossY = 0;
			for (uint32_t y = 0; y < c.h; ++y) {
				for (uint32_t x = 0; x < c.w; ++x) {
					for (uint32_t ch = 0; ch < 3; ++ch) {
						const size_t i = (static_cast<size_t>(y) * c.w + x) * 3u + ch;
						const int d = std::abs(int(got.rgb[i]) - int(other->second.rgb[i]));
						if (d > crossWorst) {
							crossWorst = d;
							crossX = x;
							crossY = y;
						}
					}
				}
			}
			std::cout << "[bilinear] " << backend << " " << c.name << ": max diff vs other backend " << crossWorst
				<< " at " << describe(crossX, crossY) << " this " << rgbAt(got, crossX, crossY) << " other "
				<< rgbAt(other->second, crossX, crossY) << " cpu " << rgbAt(cpu, crossX, crossY) << "\n";
			EXPECT_LE(crossWorst, 1)
				<< c.name << ": bilinear lookups differ between backends by up to " << crossWorst << "/255 at "
				<< describe(crossX, crossY) << ", this backend " << rgbAt(got, crossX, crossY) << ", other backend "
				<< rgbAt(other->second, crossX, crossY) << ", CPU " << rgbAt(cpu, crossX, crossY);
		}

		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroyRenderPass(pass);
		rhi().destroySampler(sampler);
		rhi().destroyTexture(texture);
	}

	namespace {

		constexpr uint32_t kShadowTex = 8;
		constexpr uint32_t kShadowTarget = 64;
		constexpr uint32_t kShadowLayer = 1;
		// Magnification with non-binary scales, as in the bilinear "magnify" case, so sub-texel
		// offsets land on arbitrary values rather than on a quantised filter's grid.
		constexpr float kShadowScale[2] = { 0.9137f, 0.7731f };
		constexpr float kShadowOffset[2] = { 0.0313f, 0.0471f };

		enum class ShadowLookup { LinearCompare, NearestCompare, RawDepth };

		struct ShadowCase {
			const char* name;
			ShadowLookup lookup;
			float ref;
		};

		// Result per pixel, indexed [row][column] by the row the shader reported: the 8-bit value the
		// lighting pass would see and the same value carried as 16 bits.
		struct ShadowImage {
			std::vector<uint8_t> narrow;
			std::vector<uint16_t> wide;
		};

		std::map<std::string, ShadowImage>& shadowSeen() {
			static std::map<std::string, ShadowImage> seen;
			return seen;
		}

		// The formula rhi_test_depth_pattern.fs writes, in window depth.
		double shadowDepthAt(uint32_t x, uint32_t y) {
			return (static_cast<double>((x * 5u + y * 3u) % kShadowTex) + 0.5) / kShadowTex;
		}

		void shadowUv(uint32_t x, uint32_t y, float& u, float& v) {
			u = kShadowOffset[0] + (static_cast<float>(x) + 0.5f) / static_cast<float>(kShadowTarget) * kShadowScale[0];
			v = kShadowOffset[1] + (static_cast<float>(y) + 0.5f) / static_cast<float>(kShadowTarget) * kShadowScale[1];
		}

		// What the lookup is defined to return with exact weights and ClampToEdge: LessEqual passes when
		// ref <= stored depth, Linear weights the four compare results bilinearly, Nearest takes one.
		double cpuShadow(const ShadowCase& c, float u, float v) {
			const auto clampIndex = [](double i) {
				return static_cast<uint32_t>(std::clamp(i, 0.0, static_cast<double>(kShadowTex - 1)));
			};
			const auto lit = [&](uint32_t x, uint32_t y) {
				return static_cast<double>(c.ref) <= shadowDepthAt(x, y) ? 1.0 : 0.0;
			};
			if (c.lookup != ShadowLookup::LinearCompare) {
				const uint32_t x = clampIndex(std::floor(static_cast<double>(u) * kShadowTex));
				const uint32_t y = clampIndex(std::floor(static_cast<double>(v) * kShadowTex));
				return c.lookup == ShadowLookup::RawDepth ? shadowDepthAt(x, y) : lit(x, y);
			}
			const double s = static_cast<double>(u) * kShadowTex - 0.5;
			const double t = static_cast<double>(v) * kShadowTex - 0.5;
			const double fs = std::floor(s);
			const double ft = std::floor(t);
			const double a = s - fs;
			const double b = t - ft;
			const uint32_t x0 = clampIndex(fs);
			const uint32_t x1 = clampIndex(fs + 1.0);
			const uint32_t y0 = clampIndex(ft);
			const uint32_t y1 = clampIndex(ft + 1.0);
			return (1.0 - a) * (1.0 - b) * lit(x0, y0) + a * (1.0 - b) * lit(x1, y0)
				+ (1.0 - a) * b * lit(x0, y1) + a * b * lit(x1, y1);
		}
	}

	// Hardware PCF compared across backends at sub-texel positions. The frame comparison shows the
	// lighting pass differing by 1/255 on ~26k pixels, in streaks inside shadow penumbrae, where
	// deferred_shading.fs samples the cascade map through sampler2DArrayShadow. How a Linear compare
	// lookup weights its 2x2 footprint, and how finely it resolves the sub-texel position, is left to
	// the implementation, so two correct backends may disagree there.
	//
	// Texture and samplers are made as CascadedShadowPass::initRender makes them: a DEPTH_COMPONENT32
	// array rendered one layer at a time, a LessEqual Linear ClampToEdge compare sampler, and a
	// Nearest non-compare one for the raw depth. A Nearest compare sampler is added to separate the
	// compare itself from the filter. The depth is rendered, not uploaded: Vulkan cannot create a
	// depth texture with initial data (see TextureFormatMatrixRoundTripsThroughCreateAndReadback).
	//
	// References 0.25, 0.5 and 0.75 never equal a stored depth (odd sixteenths), so those cases can
	// only differ in weights or position. The "equal" cases use 9/16, which is stored exactly as
	// D32_SFLOAT on Vulkan but rounded as a 32-bit unorm on OpenGL, so they probe depth precision.
	//
	// Unlike the bilinear test the cross-backend assert is exact on the 8-bit value: a one-step
	// difference is the symptom being documented.
	TEST_P(RhiBackendTest, ShadowCompareSamplingMatchesAcrossBackendsAtSubTexelUvs) {
		const char* backend = graphicsBackendName(GetParam());

		TextureDesc depthDesc;
		depthDesc.width = kShadowTex;
		depthDesc.height = kShadowTex;
		depthDesc.depth = 2;
		depthDesc.arrayLayers = 2;
		depthDesc.dimension = TextureDimension::Array;
		depthDesc.format = TextureFormat::DEPTH_COMPONENT32;
		depthDesc.usage = TextureUsage::DepthStencilAttachment | TextureUsage::Sampled;
		const auto depth = rhi().createTexture(depthDesc);
		ASSERT_TRUE(depth);

		RenderPassDesc depthPassDesc;
		depthPassDesc.hasDepthStencil = true;
		depthPassDesc.depthAttachment.loadOp = AttachmentLoadOp::Clear;
		depthPassDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
		depthPassDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT32;
		depthPassDesc.depthAttachment.clearDepth = 1.f;
		const auto depthPass = rhi().createRenderPass(depthPassDesc);
		ASSERT_TRUE(depthPass);

		GraphicsPipelineDesc depthPipeDesc;
		depthPipeDesc.renderPass = depthPass;
		depthPipeDesc.topology = PrimitiveTopology::Triangles;
		depthPipeDesc.cullMode = CullMode::None;
		depthPipeDesc.frontFace = FrontFace::CounterClockWise;
		depthPipeDesc.fillMode = PolygonFillMode::Fill;
		depthPipeDesc.depthStencilState = { true, true, CompareOp::Always };
		depthPipeDesc.blend = { false };
		depthPipeDesc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
		depthPipeDesc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, "shaders/rhi_test_depth_pattern.fs");
		const auto depthPipeline = rhi().createGraphicsPipeline(depthPipeDesc);
		ASSERT_TRUE(depthPipeline) << "depth pattern pipeline creation failed";

		// Layer 0 keeps its clear of 1.0 and only the sampled layer gets the pattern, so a lookup that
		// read the wrong layer passes every compare and is far off the CPU reference.
		std::vector<FramebufferHandle> layerFbs;
		for (uint32_t layer = 0; layer < 2; ++layer) {
			FramebufferDesc fbDesc;
			fbDesc.width = kShadowTex;
			fbDesc.height = kShadowTex;
			fbDesc.layers = 1;
			fbDesc.renderPass = depthPass;
			FramebufferAttachmentDesc att;
			att.texture = depth;
			att.layer = layer;
			fbDesc.depthStencilAttachment = att;
			layerFbs.push_back(rhi().createFramebuffer(fbDesc));
			ASSERT_TRUE(layerFbs.back()) << "layer " << layer;

			rhi().beginFrame();
			const auto cmd = rhi().beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = depthPass;
			begin.framebuffer = layerFbs.back();
			begin.width = kShadowTex;
			begin.height = kShadowTex;
			rhi().cmdBeginRenderPass(cmd, begin);
			if (layer == kShadowLayer) {
				rhi().cmdBindPipeline(cmd, depthPipeline);
				rhi().cmdDraw(cmd, 3, 1, 0, 0);
			}
			rhi().cmdEndRenderPass(cmd);
			rhi().endCommandList(cmd);
			rhi().submit(cmd);
			rhi().present();
			rhi().waitIdle();
		}

		const auto makeSampler = [&](FilterMode filter, CompareOp compare) {
			SamplerDesc desc;
			desc.minFilter = filter;
			desc.magFilter = filter;
			desc.addressU = AddressMode::ClampToEdge;
			desc.addressV = AddressMode::ClampToEdge;
			desc.addressW = AddressMode::ClampToEdge;
			desc.compareOp = compare;
			return rhi().createSampler(desc);
		};
		const auto linearCompare = makeSampler(FilterMode::Linear, CompareOp::LessEqual);
		const auto nearestCompare = makeSampler(FilterMode::Nearest, CompareOp::LessEqual);
		const auto nearestRaw = makeSampler(FilterMode::Nearest, CompareOp::None);
		ASSERT_TRUE(linearCompare);
		ASSERT_TRUE(nearestCompare);
		ASSERT_TRUE(nearestRaw);

		// Magenta, alpha 255: a pixel the draw never touched reports row 255 and is caught below.
		const auto pass = makeLodPass(rhi(), { 1.f, 0.f, 1.f, 1.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		const auto makePipeline = [&](const char* fragment) {
			GraphicsPipelineDesc desc;
			desc.renderPass = pass;
			desc.topology = PrimitiveTopology::Triangles;
			desc.cullMode = CullMode::None;
			desc.frontFace = FrontFace::CounterClockWise;
			desc.fillMode = PolygonFillMode::Fill;
			desc.depthStencilState = { false, false, CompareOp::Always };
			desc.blend = { false };
			desc.descriptorSetLayouts = { layout };
			desc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_fullscreen.vs");
			desc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, fragment);
			return rhi().createGraphicsPipeline(desc);
		};
		const auto comparePipeline = makePipeline("shaders/rhi_test_shadow_compare.fs");
		const auto rawPipeline = makePipeline("shaders/rhi_test_shadow_raw.fs");
		ASSERT_TRUE(comparePipeline) << "compare pipeline creation failed";
		ASSERT_TRUE(rawPipeline) << "raw depth pipeline creation failed";

		TextureDesc targetDesc;
		targetDesc.width = kShadowTarget;
		targetDesc.height = kShadowTarget;
		targetDesc.format = TextureFormat::RGBA8;
		targetDesc.dimension = TextureDimension::Tex2D;
		targetDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::TransferSrc;
		const auto target = rhi().createTexture(targetDesc);
		ASSERT_TRUE(target);
		FramebufferDesc fbDesc;
		fbDesc.width = kShadowTarget;
		fbDesc.height = kShadowTarget;
		fbDesc.layers = 1;
		fbDesc.renderPass = pass;
		FramebufferAttachmentDesc att;
		att.texture = target;
		att.layer = UINT32_MAX;
		fbDesc.colorAttachments = { att };
		const auto fb = rhi().createFramebuffer(fbDesc);
		ASSERT_TRUE(fb);

		const ShadowCase cases[] = {
			{ "linear ref 0.25", ShadowLookup::LinearCompare, 0.25f },
			{ "linear ref 0.5", ShadowLookup::LinearCompare, 0.5f },
			{ "linear ref 0.75", ShadowLookup::LinearCompare, 0.75f },
			{ "linear ref equal 9/16", ShadowLookup::LinearCompare, 0.5625f },
			{ "nearest ref 0.5", ShadowLookup::NearestCompare, 0.5f },
			{ "nearest ref equal 9/16", ShadowLookup::NearestCompare, 0.5625f },
			{ "raw depth nearest", ShadowLookup::RawDepth, 0.f },
		};

		const uint32_t pixelCount = kShadowTarget * kShadowTarget;
		for (const auto& c : cases) {
			SCOPED_TRACE(c.name);

			const float params[8] = { kShadowScale[0], kShadowScale[1], kShadowOffset[0], kShadowOffset[1],
				static_cast<float>(kShadowTarget), static_cast<float>(kShadowTarget), c.ref, 0.f };
			BufferDesc paramsDesc;
			paramsDesc.size = sizeof(params);
			paramsDesc.usage = BufferUsage::Uniform;
			paramsDesc.cpuAccess = CpuAccess::Write;
			paramsDesc.debugName = "rhi_test_shadow_compare";
			const auto paramsBuffer = rhi().createBuffer(paramsDesc, params);
			ASSERT_TRUE(paramsBuffer);

			const bool raw = c.lookup == ShadowLookup::RawDepth;
			const auto sampler = raw ? nearestRaw
				: c.lookup == ShadowLookup::LinearCompare ? linearCompare : nearestCompare;
			const auto pipeline = raw ? rawPipeline : comparePipeline;

			const auto set = rhi().allocateDescriptorSet(layout);
			ASSERT_TRUE(set);
			WriteDescriptor image{};
			image.set = set;
			image.binding = 0;
			image.images.push_back({ depth, sampler });
			WriteDescriptor uniform{};
			uniform.set = set;
			uniform.binding = 1;
			uniform.buffers.push_back({ paramsBuffer, 0, 0 });
			rhi().updateDescriptorSets({ image, uniform });

			rhi().beginFrame();
			const auto cmd = rhi().beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = kShadowTarget;
			begin.height = kShadowTarget;
			rhi().cmdBeginRenderPass(cmd, begin);
			rhi().cmdBindPipeline(cmd, pipeline);
			rhi().cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi().cmdDraw(cmd, 3, 1, 0, 0);
			rhi().cmdEndRenderPass(cmd);
			rhi().endCommandList(cmd);
			rhi().submit(cmd);
			rhi().present();
			rhi().waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			const bool readOk = rhi().readbackTexture(target, pixels, w, h);
			rhi().freeDescriptorSet(set);
			rhi().destroyBuffer(paramsBuffer);
			ASSERT_TRUE(readOk) << "readback failed";
			ASSERT_EQ(pixels.size(), static_cast<size_t>(pixelCount) * 4u);

			// File each readback row under the row index the shader wrote into alpha.
			ShadowImage got;
			got.narrow.assign(pixelCount, 0);
			got.wide.assign(pixelCount, 0);
			std::vector<bool> filled(kShadowTarget, false);
			for (uint32_t row = 0; row < kShadowTarget; ++row) {
				const uint8_t reported = pixels[static_cast<size_t>(row) * kShadowTarget * 4u + 3u];
				ASSERT_LT(reported, kShadowTarget) << "readback row " << row << " reports row " << int(reported)
					<< ": the draw did not cover the target";
				ASSERT_FALSE(filled[reported]) << "two readback rows report row " << int(reported);
				filled[reported] = true;
				for (uint32_t x = 0; x < kShadowTarget; ++x) {
					const size_t src = (static_cast<size_t>(row) * kShadowTarget + x) * 4u;
					const size_t dst = static_cast<size_t>(reported) * kShadowTarget + x;
					got.narrow[dst] = pixels[src];
					got.wide[dst] = static_cast<uint16_t>(pixels[src + 1] * 256u + pixels[src + 2]);
				}
			}

			ShadowImage cpu;
			cpu.narrow.assign(pixelCount, 0);
			cpu.wide.assign(pixelCount, 0);
			for (uint32_t y = 0; y < kShadowTarget; ++y) {
				for (uint32_t x = 0; x < kShadowTarget; ++x) {
					float u = 0.f;
					float v = 0.f;
					shadowUv(x, y, u, v);
					const double value = cpuShadow(c, u, v);
					const size_t i = static_cast<size_t>(y) * kShadowTarget + x;
					cpu.narrow[i] = static_cast<uint8_t>(std::floor(value * 255.0 + 0.5));
					cpu.wide[i] = static_cast<uint16_t>(std::floor(value * 65535.0 + 0.5));
				}
			}

			// Worst 8-bit and 16-bit difference, where the 8-bit one is worst, and how many pixels
			// differ in 8 bits at all.
			struct Diff {
				int narrow = 0;
				int wide = 0;
				uint32_t x = 0;
				uint32_t y = 0;
				uint32_t count = 0;
			};
			const auto measure = [&](const ShadowImage& other) {
				Diff d;
				for (uint32_t y = 0; y < kShadowTarget; ++y) {
					for (uint32_t x = 0; x < kShadowTarget; ++x) {
						const size_t i = static_cast<size_t>(y) * kShadowTarget + x;
						const int n = std::abs(int(got.narrow[i]) - int(other.narrow[i]));
						d.wide = std::max(d.wide, std::abs(int(got.wide[i]) - int(other.wide[i])));
						d.count += n != 0 ? 1u : 0u;
						if (n > d.narrow) {
							d.narrow = n;
							d.x = x;
							d.y = y;
						}
					}
				}
				return d;
			};
			const auto describe = [&](uint32_t x, uint32_t y) {
				float u = 0.f;
				float v = 0.f;
				shadowUv(x, y, u, v);
				std::ostringstream out;
				out << "pixel (" << x << ", " << y << ") uv (" << u << ", " << v << ") texel-space ("
					<< u * kShadowTex - 0.5f << ", " << v * kShadowTex - 0.5f << ")";
				return out.str();
			};
			const auto valueAt = [](const ShadowImage& img, uint32_t x, uint32_t y) {
				const size_t i = static_cast<size_t>(y) * kShadowTarget + x;
				std::ostringstream out;
				out << int(img.narrow[i]) << "/" << img.wide[i];
				return out.str();
			};

			const Diff vsCpu = measure(cpu);
			std::cout << "[shadow] " << backend << " " << c.name << ": vs CPU max 8-bit " << vsCpu.narrow
				<< " (" << vsCpu.count << " px), max 16-bit " << vsCpu.wide << ", worst at "
				<< describe(vsCpu.x, vsCpu.y) << " got " << valueAt(got, vsCpu.x, vsCpu.y)
				<< " cpu " << valueAt(cpu, vsCpu.x, vsCpu.y) << "\n";

			auto& seen = shadowSeen();
			const auto other = seen.find(c.name);
			if (other == seen.end()) {
				seen.emplace(c.name, got);
				continue;
			}
			const Diff vsOther = measure(other->second);
			std::cout << "[shadow] " << backend << " " << c.name << ": vs other backend max 8-bit " << vsOther.narrow
				<< " (" << vsOther.count << " px), max 16-bit " << vsOther.wide << ", worst at "
				<< describe(vsOther.x, vsOther.y) << " this " << valueAt(got, vsOther.x, vsOther.y) << " other "
				<< valueAt(other->second, vsOther.x, vsOther.y) << " cpu " << valueAt(cpu, vsOther.x, vsOther.y) << "\n";
			EXPECT_EQ(vsOther.narrow, 0)
				<< c.name << ": shadow lookups differ between backends by up to " << vsOther.narrow << "/255 on "
				<< vsOther.count << " pixels, worst at " << describe(vsOther.x, vsOther.y) << ", this backend "
				<< valueAt(got, vsOther.x, vsOther.y) << ", other backend " << valueAt(other->second, vsOther.x, vsOther.y)
				<< ", CPU " << valueAt(cpu, vsOther.x, vsOther.y) << " (8-bit/16-bit)";
		}

		rhi().destroyFramebuffer(fb);
		rhi().destroyTexture(target);
		rhi().destroyGraphicsPipeline(rawPipeline);
		rhi().destroyGraphicsPipeline(comparePipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroyRenderPass(pass);
		rhi().destroySampler(nearestRaw);
		rhi().destroySampler(nearestCompare);
		rhi().destroySampler(linearCompare);
		for (const auto layerFb : layerFbs) {
			rhi().destroyFramebuffer(layerFb);
		}
		rhi().destroyGraphicsPipeline(depthPipeline);
		rhi().destroyRenderPass(depthPass);
		rhi().destroyTexture(depth);
	}

	namespace {

		constexpr uint32_t kLodTex = 64;
		constexpr uint32_t kLodMips = 7;
		constexpr uint32_t kLodTarget = 128;

		// Per-bit weights 128, 64, ... 4: every weight is even, so a 2x2 box average of a sum of them
		// is again such a sum plus whole constants, down to 1x1. Generated mips are then exact bytes on
		// both backends (see GeneratedMipLevelsMatchA2x2BoxFilter), and a difference in a lookup below
		// cannot come from the mip contents.
		uint8_t lodBits(uint32_t i) {
			uint32_t sum = 0;
			for (uint32_t k = 0; k < 6; ++k) {
				sum += ((i >> k) & 1u) * (128u >> k);
			}
			return static_cast<uint8_t>(sum);
		}

		std::vector<uint8_t> lodSource() {
			std::vector<uint8_t> bytes;
			for (uint32_t y = 0; y < kLodTex; ++y) {
				for (uint32_t x = 0; x < kLodTex; ++x) {
					bytes.insert(bytes.end(), { lodBits(x), lodBits(y), lodBits(x ^ y), 255 });
				}
			}
			return bytes;
		}

		enum class LodOutput { Color, FloatBits };

		struct LodCase {
			const char* name;
			bool engineSampler; // the albedo sampler, else LinearMipmapLinear
			float select;       // rhi_test_implicit_lod.fs select.x
			LodOutput output;
		};

		std::map<std::string, std::vector<uint8_t>>& lodSeen() {
			static std::map<std::string, std::vector<uint8_t>> seen;
			return seen;
		}

		float lodFloatAt(const std::vector<uint8_t>& pixels, size_t pixel) {
			const uint32_t bits = pixels[pixel * 4u] | (pixels[pixel * 4u + 1] << 8u)
				| (pixels[pixel * 4u + 2] << 16u) | (static_cast<uint32_t>(pixels[pixel * 4u + 3]) << 24u);
			float value = 0.f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}
	}

	// Implicit-LOD sampling and the screen-space derivatives behind it, compared across backends. The
	// frame comparison shows G-buffer albedo differing by 1/255 on ~36k pixels and by up to 41 on a
	// few, on textured characters and along the floor grid lines (grid.fs uses fwidth). g_buffer.fs
	// samples albedo with texture(), so the LOD comes from derivatives, and the Vulkan backend reaches
	// OpenGL's +Y-up image with an un-flipped viewport and inverted winding, which could flip dFdy.
	//
	// A perspective floor quad samples a mipmapped high-frequency texture two ways: through the
	// albedo sampler exactly as TextureHandler::loadTexture makes it (Linear/Linear Repeat,
	// maxAnisotropy 1 — which, having no mip filter, pins lookups to level 0), and through a
	// LinearMipmapLinear sampler, where the implicit LOD matters. Colour cases are texture(),
	// textureGrad with the fragment's own derivatives, and textureLod at textureQueryLod().x: if
	// texture() differs but the other two agree with each other, the LOD computation differs; if
	// textureLod differs too, the trilinear blend does. Float cases write the exact bits of uv, its
	// derivatives, fwidth and the queried LOD, and count pixels where one backend holds the exact
	// negation of the other, which is what a flipped dFdy would look like.
	//
	// Readback rows are compared as returned: both backends hand rows back top down
	// (TextureFormatMatrixRoundTripsThroughCreateAndReadback), and a vertical misalignment would show
	// as a mismatch in every case, including uv. The assert is exact on every case.
	TEST_P(RhiBackendTest, ImplicitLodAndDerivativesMatchAcrossBackendsOnAPerspectiveQuad) {
		const char* backend = graphicsBackendName(GetParam());
		const auto source = lodSource();

		TextureDesc texDesc;
		texDesc.width = kLodTex;
		texDesc.height = kLodTex;
		texDesc.mipLevels = kLodMips;
		texDesc.format = TextureFormat::RGBA8;
		texDesc.dimension = TextureDimension::Tex2D;
		texDesc.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
		const auto texture = rhi().createTexture(texDesc, source.data());
		ASSERT_TRUE(texture);

		const auto makeSampler = [&](FilterMode minFilter) {
			SamplerDesc desc;
			desc.minFilter = minFilter;
			desc.magFilter = FilterMode::Linear;
			desc.addressU = AddressMode::Repeat;
			desc.addressV = AddressMode::Repeat;
			desc.addressW = AddressMode::Repeat;
			desc.maxAnisotropy = 1.f;
			return rhi().createSampler(desc);
		};
		const auto albedoSampler = makeSampler(FilterMode::Linear);
		const auto trilinearSampler = makeSampler(FilterMode::LinearMipmapLinear);
		ASSERT_TRUE(albedoSampler);
		ASSERT_TRUE(trilinearSampler);

		// Transparent black: pixels off the quad read back as zero on both backends, colour or bits.
		const auto pass = makeLodPass(rhi(), { 0.f, 0.f, 0.f, 0.f });
		ASSERT_TRUE(pass);

		DescriptorSetLayoutDesc layoutDesc;
		layoutDesc.bindings.push_back({ 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment });
		layoutDesc.bindings.push_back({ 1, DescriptorType::UniformBuffer, ShaderStage::Fragment });
		const auto layout = rhi().createDescriptorSetLayout(layoutDesc);
		ASSERT_TRUE(layout);

		GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.renderPass = pass;
		pipelineDesc.topology = PrimitiveTopology::Triangles;
		pipelineDesc.cullMode = CullMode::None;
		pipelineDesc.frontFace = FrontFace::CounterClockWise;
		pipelineDesc.fillMode = PolygonFillMode::Fill;
		pipelineDesc.depthStencilState = { false, false, CompareOp::Always };
		pipelineDesc.blend = { false };
		pipelineDesc.descriptorSetLayouts = { layout };
		pipelineDesc.shaders.vertex = rhi().loadShader(ShaderStage::Vertex, "shaders/rhi_test_perspective_quad.vs");
		pipelineDesc.shaders.fragment = rhi().loadShader(ShaderStage::Fragment, "shaders/rhi_test_implicit_lod.fs");
		const auto pipeline = rhi().createGraphicsPipeline(pipelineDesc);
		ASSERT_TRUE(pipeline) << "pipeline creation failed";

		TextureDesc targetDesc;
		targetDesc.width = kLodTarget;
		targetDesc.height = kLodTarget;
		targetDesc.format = TextureFormat::RGBA8;
		targetDesc.dimension = TextureDimension::Tex2D;
		targetDesc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled | TextureUsage::TransferSrc;
		const auto target = rhi().createTexture(targetDesc);
		ASSERT_TRUE(target);
		FramebufferDesc fbDesc;
		fbDesc.width = kLodTarget;
		fbDesc.height = kLodTarget;
		fbDesc.layers = 1;
		fbDesc.renderPass = pass;
		FramebufferAttachmentDesc att;
		att.texture = target;
		att.layer = UINT32_MAX;
		fbDesc.colorAttachments = { att };
		const auto fb = rhi().createFramebuffer(fbDesc);
		ASSERT_TRUE(fb);

		const LodCase cases[] = {
			{ "albedo sampler texture", true, 0.f, LodOutput::Color },
			{ "trilinear texture", false, 0.f, LodOutput::Color },
			{ "trilinear textureGrad", false, 1.f, LodOutput::Color },
			{ "trilinear textureLod at queried lod", false, 2.f, LodOutput::Color },
			{ "uv.x", false, 10.f, LodOutput::FloatBits },
			{ "uv.y", false, 11.f, LodOutput::FloatBits },
			{ "dFdx(uv).x", false, 12.f, LodOutput::FloatBits },
			{ "dFdx(uv).y", false, 13.f, LodOutput::FloatBits },
			{ "dFdy(uv).x", false, 14.f, LodOutput::FloatBits },
			{ "dFdy(uv).y", false, 15.f, LodOutput::FloatBits },
			{ "fwidth(uv).x", false, 16.f, LodOutput::FloatBits },
			{ "fwidth(uv).y", false, 17.f, LodOutput::FloatBits },
			{ "trilinear textureQueryLod.x", false, 18.f, LodOutput::FloatBits },
			{ "trilinear textureQueryLod.y", false, 19.f, LodOutput::FloatBits },
			{ "albedo sampler textureQueryLod.x", true, 18.f, LodOutput::FloatBits },
		};

		const size_t pixelCount = static_cast<size_t>(kLodTarget) * kLodTarget;
		for (const auto& c : cases) {
			SCOPED_TRACE(c.name);

			const float params[4] = { c.select, 0.f, 0.f, 0.f };
			BufferDesc paramsDesc;
			paramsDesc.size = sizeof(params);
			paramsDesc.usage = BufferUsage::Uniform;
			paramsDesc.cpuAccess = CpuAccess::Write;
			paramsDesc.debugName = "rhi_test_implicit_lod";
			const auto paramsBuffer = rhi().createBuffer(paramsDesc, params);
			ASSERT_TRUE(paramsBuffer);

			const auto set = rhi().allocateDescriptorSet(layout);
			ASSERT_TRUE(set);
			WriteDescriptor image{};
			image.set = set;
			image.binding = 0;
			image.images.push_back({ texture, c.engineSampler ? albedoSampler : trilinearSampler });
			WriteDescriptor uniform{};
			uniform.set = set;
			uniform.binding = 1;
			uniform.buffers.push_back({ paramsBuffer, 0, 0 });
			rhi().updateDescriptorSets({ image, uniform });

			rhi().beginFrame();
			const auto cmd = rhi().beginCommandList(CommandListType::Graphics);
			RenderPassBeginInfo begin;
			begin.renderPass = pass;
			begin.framebuffer = fb;
			begin.width = kLodTarget;
			begin.height = kLodTarget;
			rhi().cmdBeginRenderPass(cmd, begin);
			rhi().cmdBindPipeline(cmd, pipeline);
			rhi().cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1);
			rhi().cmdDraw(cmd, 6, 1, 0, 0);
			rhi().cmdEndRenderPass(cmd);
			rhi().endCommandList(cmd);
			rhi().submit(cmd);
			rhi().present();
			rhi().waitIdle();

			std::vector<uint8_t> pixels;
			int w = 0;
			int h = 0;
			const bool readOk = rhi().readbackTexture(target, pixels, w, h);
			rhi().freeDescriptorSet(set);
			rhi().destroyBuffer(paramsBuffer);
			ASSERT_TRUE(readOk) << "readback failed";
			ASSERT_EQ(pixels.size(), pixelCount * 4u);

			// How much of the target the quad covered, so an empty draw cannot pass as a match.
			size_t covered = 0;
			for (size_t i = 0; i < pixelCount; ++i) {
				covered += (pixels[i * 4u] | pixels[i * 4u + 1] | pixels[i * 4u + 2] | pixels[i * 4u + 3]) != 0 ? 1u : 0u;
			}

			auto& seen = lodSeen();
			const auto other = seen.find(c.name);
			if (other == seen.end()) {
				std::cout << "[implicit lod] " << backend << " " << c.name << ": " << covered << " of " << pixelCount
					<< " pixels non-zero\n";
				// Colour only: a float case may legitimately be 0.0 everywhere, as the albedo sampler's
				// queried LOD is, and zero bits read as uncovered.
				if (c.output == LodOutput::Color) {
					EXPECT_GT(covered, pixelCount / 8) << "the quad did not cover the target";
				}
				seen.emplace(c.name, pixels);
				continue;
			}
			const auto& theirs = other->second;

			size_t differing = 0;
			size_t worstPixel = 0;
			std::ostringstream detail;
			if (c.output == LodOutput::Color) {
				int worst = 0;
				for (size_t i = 0; i < pixelCount; ++i) {
					int d = 0;
					for (size_t ch = 0; ch < 4; ++ch) {
						d = std::max(d, std::abs(int(pixels[i * 4u + ch]) - int(theirs[i * 4u + ch])));
					}
					differing += d != 0 ? 1u : 0u;
					if (d > worst) {
						worst = d;
						worstPixel = i;
					}
				}
				const auto rgba = [&](const std::vector<uint8_t>& img) {
					std::ostringstream out;
					out << "(" << int(img[worstPixel * 4u]) << ", " << int(img[worstPixel * 4u + 1]) << ", "
						<< int(img[worstPixel * 4u + 2]) << ", " << int(img[worstPixel * 4u + 3]) << ")";
					return out.str();
				};
				detail << "max channel diff " << worst << " this " << rgba(pixels) << " other " << rgba(theirs);
			}
			else {
				double worst = 0.0;
				double worstRelative = 0.0;
				size_t negated = 0;
				for (size_t i = 0; i < pixelCount; ++i) {
					const float mine = lodFloatAt(pixels, i);
					const float yours = lodFloatAt(theirs, i);
					if (std::memcmp(&pixels[i * 4u], &theirs[i * 4u], 4) == 0) {
						continue;
					}
					++differing;
					negated += (mine == -yours && mine != 0.f) ? 1u : 0u;
					const double d = std::abs(static_cast<double>(mine) - static_cast<double>(yours));
					if (d > worst) {
						worst = d;
						worstRelative = d / std::max(std::abs(static_cast<double>(yours)), 1e-30);
						worstPixel = i;
					}
				}
				detail << std::setprecision(9) << "exact negations " << negated << ", max abs diff " << worst
					<< " (relative " << worstRelative << ") this " << lodFloatAt(pixels, worstPixel) << " other "
					<< lodFloatAt(theirs, worstPixel);
			}
			const size_t worstX = worstPixel % kLodTarget;
			const size_t worstY = worstPixel / kLodTarget;
			std::cout << "[implicit lod] " << backend << " " << c.name << ": vs other backend " << differing << " of "
				<< covered << " covered pixels differ, " << detail.str() << " at readback pixel (" << worstX << ", "
				<< worstY << ")\n";
			EXPECT_EQ(differing, 0u) << c.name << ": " << differing << " pixels differ between backends, "
				<< detail.str() << " at readback pixel (" << worstX << ", " << worstY << ")";
		}

		rhi().destroyFramebuffer(fb);
		rhi().destroyTexture(target);
		rhi().destroyGraphicsPipeline(pipeline);
		rhi().destroyDescriptorSetLayout(layout);
		rhi().destroyRenderPass(pass);
		rhi().destroySampler(trilinearSampler);
		rhi().destroySampler(albedoSampler);
		rhi().destroyTexture(texture);
	}
}

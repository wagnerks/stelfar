# Render issues

Known problems in the GPU-driven renderer, most urgent first.

## 1. Debug code in `bin/shaders/animateBones.cs` (working copy)
- [x] The `dbg` matrix is written to `outBones[entityId * MAX_BONES + MAX_BONES - 1 - boneId]`, which overwrites other bones of the same character. Remove it before committing.

## 2. The bones buffer is addressed by entity id
- [x] `RenderSystem::createSceneBuffers` reserves `kMaxArmatureBones` (256) matrices, 16 KB, for every entity slot, skeleton or not. At 4096 entities that is 64 MB, and the capacity doubles: 65k entities take 1 GB, 130k take 2 GB, even with ten animated characters. This hits the storage buffer size limit (2–4 GB on desktop, much less on mobile).
- Fix: a pooled range per skinned entity, sized by its bone count. The offset goes into the instance record and skinning reads `bones[boneOffset + boneId]`. Same incremental scheme as `GpuScene`. This also removes limit 3.

## 3. At most 256 bones per armature
- [x] Bones past `kMaxArmatureBones` are silently not animated (`AnimationComputePass` clamps `boneCount`, the CPU upload clamps too). There is no warning at load.

## 4. Skinned bounds are a static inflated AABB
- [x] The box is the bind pose plus a margin. A pose that reaches past it (a wide swing, a fall) gets the part that sticks out culled by frustum culling now, and by occlusion culling once it exists.
- Fix: compute the AABB per frame in `animateBones.cs` from bone world positions. Add a per-bone radius (the farthest vertex weighted to the bone in bind pose, computed once at load) scaled by the bone's world axes, and write the result into a bounds buffer the cull reads. Costs a few percent of a 0.1–0.2 ms pass and 24 bytes per character.
- Tried (cec243f, reverted in 1fe15ae): one pose box per entity cost 0.5–0.8 ms of GPU time on the character scene, measured with old and new runtimes alternated in rotating order. The compute was free; the loss was culling. Every submesh took the whole body's box instead of its own inflated one, so small submeshes (eyes, hair, clothing) passed far more views, the shadow cascades above all. For the vampire the pose box was 135×130×91 against a 135×106×34 mesh box.
- Done, as a box per submesh: at load every bone gets the distance of its farthest weighted vertex (`Armature::boneRadius`) and a bit per skinned mesh it moves (`boneBoundsMask`); `animateBones.cs` grows one box per skinned mesh from the posed joints into the matrices the bone pool keeps before the entity's bones (`RenderSystem::BoneRange::bounds`), and the mesh table's `skinned` field carries `2 + slot` so the cull knows which box to read. A pose uploaded from the CPU writes an empty box and keeps the inflated fallback. Tighter boxes now cull better than they cost: 7.98 to 7.77 ms of GPU time (rotated A/B, 6 rounds), against 0.5-0.8 ms lost by the whole-body box.
- Watch out: the box must be built from the joint matrix read back out of the `worlds` buffer, not from the local `world` variable. A second consumer of that variable made the OpenGL compiler evaluate the bone chain differently, `precise` and all, and skinned vertices landed a few ULPs off the Vulkan ones.

## 5. No occlusion culling
- [x] Nothing writes `OcclusionComponent::occluded`: the flag is always false. `AnimationComputePass` reads it, while `GeometryPass` and `CascadedShadowPass` only include the header.
- Fix: Hi-Z in `gpu_cull.cs`, camera views only. Use two-pass culling (cull with the last frame's pyramid, draw, build a pyramid from this frame, retest what was culled and draw what became visible) so moving occluders and fast camera turns do not pop. Delete the old `OcclusionComponent`.
- Done: `GpuScene::cullVisibleLastFrame` / `buildDepthPyramid` / `cullOccluded`, used by `GeometryPass` and, per cascade, by `CascadedShadowPass` -- a caster the light cannot see stands in another caster's shadow, so its own shadow adds nothing. The pyramid lives in a storage buffer (`hiz.cs`, the RHI has no storage images) and reads either a depth texture or one layer of the cascade array; the per-slot `lastVisible` flags need no CPU upkeep (a stale flag only picks the phase), and the second phase packs at the end of each batch's range so both phases share one visible buffer. Point lights are not occlusion culled.
- The default scene hides almost nothing behind anything (131 of 1284 instances visible either way), so there it costs about 0.05 ms and saves nothing. `--rooms` spawns the scene it is meant for -- walled rooms with a roof and characters inside -- and from the street 103 of 3042 instances survive against 394 without the test, the cascades cull from 938 down to 13 by cascade, and the frame's GPU time goes from 5.48 to 5.02 ms (rotated A/B, 6 rounds).

## 6. Animation still walks every character on the CPU each frame
- [x] `AnimationComputePass::render` iterates all animated entities, advances `mCurrentTime`, packs `AnimPush`, uploads it and dispatches once per clip.
- Fix: persistent per-entity records on the GPU (clip, start time, speed, loop, pause), updated on events only. The shader derives the time from one global clock. Put all clips in shared key buffers so one dispatch covers everyone. Anything on the CPU that reads `mCurrentTime` must use the same formula, and the start times need periodic rebasing to keep float precision.
- Done: every character has a record on the GPU (`AnimationComputePass::GpuRecord`) rewritten only on `ANIMATION_UPDATED`, `ARMATURE_UPDATED` or destroy, and the shader takes the time from `Engine::animationClock()` (`AnimationComponent::time()` is the same formula on the CPU). Clips share key buffers and one dispatch covers all records. The records are written in the new `RenderPass::prepare`, before the frame's first command list. Start times are relative to an epoch moved every 512 s. A start set off the main thread (`AnimationComponent::play`) is fixed on the first frame that draws it, so loader timing does not change the pose. Whoever edits the component sends `ANIMATION_UPDATED`. The occluded skip went with the per-frame walk; occlusion (item 5) has to bring its own.
- [x] One dispatch is capped at 65535 characters per clip (the minimum `maxComputeWorkGroupCount`). Now 16 records per invocation group over one dispatch for all clips.

## 7. `AnimationComputePass` leaks buffers on growth
- [x] When a clip's instance count grows, `instances` and `worlds` are replaced by new buffers and the old ones are never destroyed or retired. Clip buffers in `mClips` are never freed either, and the cache is keyed by `Animation*`.

## 8. The outline list is rebuilt from scratch
- [x] `GpuScene::rebuildOutline` repacks the whole list (`pack()`) instead of updating it through `markEntity` like the other lists. Usually only a few entities, but it is the last non-incremental list.

## 9. Portability (mobile, consoles)
- [ ] Hard requirements with no fallback: `multiDrawIndirect`, `drawIndirectFirstInstance`, storage buffers in vertex shaders (`vertexPipelineStoresAndAtomics`), sampler arrays with dynamic indexing, and compute.
  - Older and low-end Mali/Adreno lack some of these. Fallback: draw the indirect commands one by one in the RHI when the feature is missing.
  - The OpenGL backend needs 4.6, so there is no GLES path.
  - iOS only works through MoltenVK, where multi-draw-indirect is emulated. A native Metal backend would be better.
- Consoles need their own RHI backend (DX12, PS5) and shader translation (GLSL → SPIR-V → HLSL). The GPU-driven design itself maps directly.

## 10. Engine users cannot add their own render passes
- [x] The pass list is a compile-time array, `RENDER_PASSES` in `systemsModule/SystemsPriority.h`. `RenderSystem` builds its passes from it once, and the order is the array index. There is no public API to add a pass from game code or a plugin, or to put one before or after a named pass. `setPriority` is stored and never used for ordering.
- Already reusable: the `RenderPass` interface, `FrameBlackboard` (pass outputs shared by name), `RenderData`, and `GpuScene`'s public cull and draw calls.
- Fix: `RenderSystem::addPass(name, factory, after = "<pass name>")` on top of the default list. Blackboard names are plain strings, so a typo silently yields an empty handle; public names should be constants and a missing resource should warn.
- Since step 6 of the engine split the list is `Render::PassList` (`renderModule/PassList.h`): an application builds it from the engine's own `Render::defaultPasses()` and its passes, put in before or after a named pass (a name not in the list is an error, said, and changes nothing), replaced, disabled or removed, each with its own run predicate, and hands it to `RenderSystem(PassList)`; `RenderSystem::addRenderPass(PassEntry, after)` puts one in at run time. `SystemsPriority.h` is gone.

## 11. `GpuScene` compacts and re-uploads every frame in scenes with many rarely used meshes
- [x] `GpuScene::update` checks holes on every frame (`GpuScene.cpp:161`), and `compact()` itself leaves `n/4 + 4` spare slots per batch (`:254`). With more than about 1024 batches averaging under ~5 instances each (a typical imported static level), the check fails right after compacting. GBuffer and Shadow then repack and re-upload their whole instance and batch buffers every frame. Batch slots emptied by `growGroup` keep their capacity and add to it.
- Fix: count holes against the spare that compact leaves, or check only after a frame that changed something.

## 12. Glass is sorted by the entity origin, not by its bounds
- [x] `gpu_glass.cs:118` uses `m[3]`. Every submesh of one entity gets the same key, and so does glass in imported models whose pivots sit at the origin. Ties fall back to the slot index, so blending order is wrong and flips after swap-remove.
- Fix: sort by the transformed centre of the mesh bounds (already computed a few lines above).

## 13. The cull runs one invocation per batch and loops over its instances
- [x] `gpu_cull.cs:109`. One mesh with 100k instances (foliage, debris) is a single GPU thread running a 100k loop, repeated for every cascade and the camera. No parallelism, and it risks a driver timeout.
- Fix: dispatch per instance and write through a per-batch offset, or split big batches.
- Measured: the serial loop was the most expensive part of the frame's GPU time. One cull of 1284 instances took 0.42 ms at 1024 instances per batch, 0.055 ms at 64, 0.02 ms at 16. `kMaxBatchInstances` is 16 now: with occlusion culling on, the default scene went from 10.24 to 8.89 ms of GPU time on Vulkan and from 12.73 to 10.49 ms a frame on OpenGL (rotated A/B, 6 and 4 rounds).

## 14. The material table only grows
- [x] `GpuMaterialTable.cpp:34`: entries are keyed by rounded parameter values and never removed. Dragging a roughness or tint slider in the editor adds an entry per step, and every step also calls `markEntity`.

## 15. The 8 glass texture slots are never freed
- [x] `GpuMaterialTable.cpp:48`: slots are taken per texture handle for the whole session. Once 8 are used, new glass draws untextured until restart, and a destroyed texture stays bound in its slot (`GlassPass.cpp:246`).

## 16. One dirty range per list
- [x] `GpuScene.cpp:377`: a remove in an early batch plus a batch growing at the end in the same update re-uploads nearly the whole instance buffer. Happens on edits, not every frame.

## 17. A new mesh re-uploads the whole mesh table
- [x] `GpuScene.cpp:191`: while meshes stream in, every frame that adds one copies and uploads all of them.

## 18. `TasksManager` listener lists are not thread-safe
- [x] `TasksManager.cpp:4-10`: `addWorker` appends to `workers[type]` on the main thread while `notify` iterates it on worker threads. `TransformSystem` is created first (`ECSHandler.cpp:56`) and can already send `TRAHSFORM_RELOADED` while `AABBSystem` and `RenderSystem` subscribe. A reallocation mid-loop reads freed memory, which is a possible crash at startup.

## 19. An entity can stay invisible when its mesh arrives before its transform
- [x] `GpuScene::addEntity` (`GpuScene.cpp:269`) silently skips an entity without `TransformMatComp`, and `TRAHSFORM_RELOADED` does not re-queue it (`RenderSystem.cpp:485`). Scene load through `fillTree` sends `MESH_UPDATED` before the transform worker adds the component. The entity is not drawn until its mesh or material changes. An entity left at the default transform never calls a setter and depends on the constructor's notify, which can be lost.
- Fix: `TRAHSFORM_RELOADED` for an entity without refs calls `markEntity`.

## 20. Data race on `TransformComponent`
- [x] The camera matrix was read on the main thread while the transform worker writes `mTransform` (`TransformComponent.cpp:94`); a pin prevents moves, not writes, so a moving camera could get a half-written view for a frame. RenderSystem now reads the camera from `TransformMatComp`, whose replacement waits for pins.
- [x] Setters, `reloadTransform` and a new `copyTransform` take a lock striped by entity (outside the component, which stays trivially copyable); cross-thread readers (RenderSystem, AABBSystem, Gizmo) use `copyTransform`, and spawning seeds the render copy from its own values instead of reloading the component beside the worker.

## 21. Descriptor caches compare handle ids, and Vulkan reuses ids
- [x] `CascadedShadowPass.cpp:336`, `PointLightPass.cpp:234`, `GpuScene.cpp:551/614` skip the set update when the ids are unchanged. `VulkanRHI::recycleResource` (`rhi/VulkanRHI.h:216`) reuses freed ids with no generation. A pass skipped while a buffer is replaced twice can see the same id and keep a set pointing at a destroyed buffer (undefined behaviour or device lost). Frequent outline buffer re-creation makes reuse faster.
- Fix: a generation counter in handles, or invalidate caches on retire.

## 22. Possible out-of-bounds transform read for one frame
- [x] `RenderSystem.cpp:456-481`: the dirty-list swap and `GpuScene::update` are not atomic against the transform worker. An id above `mGpuCapacity` that gets `TransformMatComp` after the swap is added to the scene in the same frame, so the cull reads past the transforms buffer. Fixed by the next frame's growth.

## 23. Shared mesh buffers never free anything
- [ ] `MeshVaoRegistry.cpp:57-79, 113-153`: allocation only appends, with no free or reuse. Offsets are `uint32` and wrap silently. Every growth re-uploads all meshes from their CPU copies, so all geometry stays in RAM forever. On Vulkan a growth mid-frame turns into one blocking submit per mesh.
- [x] Offsets past `uint32` are refused with an error instead of wrapping onto the start of the buffers.
- [x] A mid-frame growth would now show up in the log (see 30); meshes are uploaded by SYNC tasks outside frame recording.
- [ ] Freeing needs an asset lifecycle the engine does not have yet: models are never unloaded, so there is nothing to free. When they are, free ranges per mesh like the bone pool, and copy buffer to buffer on growth instead of keeping every mesh's CPU copy.

## 24. Retired buffers and the frames-in-flight constant
- [x] Retired buffers are freed only in `uploadDirtyGpuData`: never at shutdown (`~RenderSystem`, `RenderSystem.cpp:626`), and they pile up during long loads.
- [x] `VulkanRHI.cpp:22` defines an unused `kMaxFramesInFlight = 1`, shadowed by the class constant 2. `kMaxGpuFrames = 2` (`RenderSystem.h:284`) and the `min(2, …)` clamps silently assume 2. At 3, per-frame slots would be shared between frames in flight.

## 25. `GlassPass` creates a framebuffer every frame and never frees them
- [x] `GlassPass.cpp:124-140`: `ensureTargets` makes a new framebuffer whenever the Final or GBufferDepth texture id changes, and never destroys the old one. Both ids alternate every frame: Lighting ping-pongs its output (`LightingPass.cpp:322,338`), and on Vulkan the depth buffer alternates per frame slot (`GeometryPass.cpp:234,242`). That leaks one FBO/VkFramebuffer per frame, always on Vulkan and on GL with SSR/Sheen/SSS/ContactGI off.
- Fix: cache one framebuffer per (final, depth) pair.

## 26. `DebugPass` destroys a buffer the previous frame still uses (Vulkan)
- [x] `DebugPass.cpp:40`: `growDebugVBO` calls `destroyBuffer` directly, and `VulkanRHI::destroyBuffer` frees it immediately. Frame N-1 was submitted without waiting and still binds it, which means use-after-free or device lost when debug geometry outgrows the buffer. The RHI has no deferred destroy; only `RenderSystem::retireBuffer` works around it.
- Fix: deferred destroy in the Vulkan backend itself (free after `framesInFlight` frames), so no caller has to remember.

## 27. Shared CPU-writable buffers are overwritten while the previous frame reads them (Vulkan)
- [x] `VulkanRHI::updateBuffer` on a mapped buffer is a plain memcpy, and a frame only waits on its own slot's fence. RenderSystem and GpuScene keep one buffer per frame slot, but these do not: `CascadedShadowPass.cpp:298` (cascade matrices), `PointLightPass.cpp:227`, `LightingPass.cpp:245,293,302,320`, `GlassPass.cpp:257`, `AnimationComputePass.cpp:279` (instances). Frame N-1 can render with frame N's cascade matrices (shadows swim while the camera moves), or skin with the wrong entity ids. GL is unaffected.
- Fix: per-slot buffers for these, or have the RHI rotate per-frame copies of `CpuAccess::Write` buffers.

## 28. The Vulkan descriptor pool has a fixed size and fails silently
- [x] `VulkanRHIResources.cpp:248-258`: 2048 sets and 2048 descriptors per type, never grown. GeometryPass keeps a set per texture combination forever (`GeometryPass.cpp:409`, 6 images each, so ~340 combinations exhaust images), and shadow mask sets are never freed. When the pool is full, `allocateDescriptorSet` returns an empty handle without a log and the bind is skipped silently, so the draw uses the previous group's textures. `detachBoundSet` then writes a bound set in place, which invalidates the command buffer.
- Fix: a list of pools that grows, and a log on failure.

## 29. Sets rewritten every frame (Vulkan)
- [x] Writing a set that the previous frame still binds allocates a new set and copies it (`VulkanRHIResources.cpp:999-1046`). This happens every frame in `GlassPass.cpp:182-254` (12 writes, no change check), `GeometryPass.cpp:291-311` and `:344`, LightingPass `mTexSet`/`mUboSet` (inputs alternate per slot), and `AnimationComputePass.cpp:280` (per clip). Correct, but costly, and it feeds item 28.
- Fix: one set per frame slot where inputs alternate, and change checks.
- [x] Vulkan now skips a write that repeats what the binding holds (compared by handle id, offset and range), so passes without their own change check no longer copy sets. Measured on the character scene: 11 copies a frame before, 9 after.
- [x] The remaining 9 are post passes whose inputs really alternate every frame (Lighting's ping-pong output, the per-slot G-buffer and camera UBO). Each has one set per frame slot now -- Lighting, Glass, the Geometry outline sets, SSAO, Sheen, SSS, SSR, TAA, ContactGI, DoF and Final -- and the frame copies no sets at all (9 before).
- Fixing it turned up a bug: SSAO wrote the G-buffer textures into its sets once at init, so on Vulkan, with two frames in flight, every other frame read the other slot's G-buffer. With the sets per slot the backends agree far more closely than before: the final image went from 2.52% of pixels differing to 0.024%, TAA from 1.66% to 0.014%. The OpenGL capture is unchanged, the Vulkan one is what moved.

## 30. Vulkan buffer writes during recording stall the frame
- [x] After uploads are flushed at `beginCommandList`, `updateBuffer` to a device-local buffer and `cmdUpdateBuffer` run `executeImmediate`: submit and wait on a fence (`VulkanRHIResources.cpp:131-143`, `:1362`). `GpuMaterialTable::upload()` is called from inside passes (`GeometryPass.cpp:263`), so a new material makes the GPU wait mid-frame. Copies queued earlier run at submit, after the immediate one, so the order of two writes to the same range is inverted compared with GL (no current caller does that).

## 31. Allocations per draw and per frame
- [x] GL `cmdBindDescriptorSets` deep-copies the bindings' vectors on every bind (`OpenGLRHI.cpp:1420`), the command list starts from zero capacity every frame, and push constants allocate. Vulkan allocates a vector per bind (`VulkanRHIPipelines.cpp:1389`), per render pass (`:1236`) and 4 per `updateDescriptorSets`. Passes allocate too: GlassPass `writes`, AnimationComputePass `unordered_map`, PointLightPass `matrices`/`spheres`/`candidates`, `GpuScene::cullSpheres` `shapes`.

## 32. `GlassPass` writes bindings its layout does not have
- [x] `GlassPass.cpp:197-203, 232-238` write bindings 12 and 14, which the layout (`:77-87`) lacks. Both backends log "set has no binding" twice per frame, and the log spam hides real errors.

## 33. Smaller backend differences
- [x] `RenderSystem.cpp:573` binds the frame set with an empty pipeline. Vulkan drops it and GL applies it, so GL's persistent binding can hide a missing bind that only breaks on Vulkan.
- [x] Not verified: Vulkan `runOnWorkerThread` has no lock, and `TextureHandler.cpp:75` calls `createTexture` on the calling thread. If textures load off the main thread, the resource arrays and the upload command pool are used from two threads.

## 34. Housekeeping
- [x] `bin/compare/**` outputs are tracked in git, so every `compare-backends.ps1` run dirties the tree. Ignore them.
- [x] `scripts/compare-backends.ps1` (editor repo) is untracked.

## 35. TAA smeared anything that animated
- [x] TAA reprojected its history with the camera matrices alone, so a pixel that moved because the *model* moved sampled the history of whatever stood there before: a skinned character dragged a trail of itself, worst on arms and weapons.
- Fix: the G-buffer grew a fourth target with the per-pixel motion vector (`g_buffer.vs` carries the clip position under this frame's and the previous frame's unjittered PV, the fragment shaders write the difference in UV), and TAA reprojects by it. The previous transforms and bone matrices live in two buffers alongside the current ones (frame set bindings 8 and 9); `animateBones.cs` copies a bone into the previous buffer before overwriting it, so the pair always describes the same frame step.
- Cost: one more RGBA16F target and two buffers the size of the transform and bone tables. Alternated against the
  revision before it on the character scene (3 rounds each, Vulkan): 7.42 ms frame / 6.97 ms gpu before, 7.67 / 7.22 after.
  The laptop heats up through a round set, so call it +0.2 to +0.3 ms.
- Two things this cost a day: the visible geometry draws with `g_buffer_masked.fs`, not `g_buffer.fs`, and only the latter had the write at first, so the target stayed at its clear value while every bit of plumbing checked out. And the `SharedMatrices` uniform block was copy-pasted into 13 shaders; adding two matrices to two of them made OpenGL refuse to link the grid ("struct type mismatch between shaders") and silently drop the floor. The block body is a single macro in `sfe.glsl` now.

## 36. Material textures had no mip chain
- [x] `TextureHandler.cpp` created every material texture with `mipLevels = 1` and `maxAnisotropy = 1`, so a character a hundred metres away sampled one texel of a 4K map per pixel. It sparkled, and TAA averaged the sparkle into flat grey -- worst at a glancing camera angle, where the pixel footprint is stretched.
- [x] `sfeMaskedAlbedo` made it worse: it read `textureLod(..., 0.0)` and, of five taps, kept the *least saturated* one. At a distance those taps land far apart in the texture, so it picked a different one every frame and preferred the greyest of them.
- Fix: a full mip chain and 8x anisotropy on material textures; the masked albedo, the cutout mask and the cutout dither all work at the mip the pixel actually samples (`textureQueryLod`), and the dither key is anchored to that mip's texel grid so a sub-pixel TAA jitter no longer reshuffles the stipple. TAA also stopped raising the weight of the current frame with local contrast -- that was there to hide the ghosting on detailed models before the motion vectors of item 35 existed, and it kept 28% of every jittered frame on them forever, which reads as a crawl.
- [x] The backends stopped matching at the horizon once the chains existed (final image 0.01% of pixels differing before, 6.6% after). `updateTexture` takes a mip level now and writes that level only, and `TextureHandler::buildMipChain` builds the chain on the CPU with a rounded 2x2 box filter, so both backends sample the same texels instead of whatever `glGenerateMipmap` and a blit chain each produce. The colour and the cutout mask are read with plain `texture()` rather than an explicit lod: `textureQueryLod` is the driver's own and differs a little, which was half the remaining divergence. `RhiTextureTests.UploadedMipLevelsAreTheOnesSampledBack` pins the upload path on both backends and `EngineTest.TextureMipChainHalvesAndAveragesTwoByTwo` pins the filter.
- [x] Anisotropy is 16x (clamped to what the device allows -- Vulkan asked for the raw number before, which is invalid usage above the limit). Without it a surface at a glancing angle takes the mip for the axis it is stretched along and loses the other one, which is what a low resolution texture looks like. It is also the one piece of filtering the two backends implement differently, and the price is the frame comparison: 0.8% of pixels differing (mean 0.03) with it off, 6.7% (mean 0.27) with it on, against 0.01% before mips existed at all. Of that 0.8% floor, the pixels are ones sitting on the alpha test threshold where a filtered mask lands either side of it; every other G-buffer target is identical. Run the comparison with anisotropy at 1 to tell a real divergence from the filter.

## 37. Distant models had no simplified geometry
- [x] `Model.cpp` built exactly one level (`mLODs.resize(1)`), so `LODSystem` -- which has computed a level per entity by distance all along -- had nothing to switch between. A character a hundred metres away was drawn from all 15k of its triangles, most of them smaller than a pixel. Geometry that small has no stable answer for a pixel, and no temporal filter can average a signal that never repeats: that is the boiling silhouette that no amount of anti-aliasing fixed.
- Fix: meshoptimizer (submodule) builds three coarser index sets per mesh at load, halving the triangles each time. Index only, over the mesh's own vertices, so bone weights are untouched and a skinned mesh simplifies like any other. A character's UV seams duplicate vertices and read to the simplifier as the edge of the surface, which stalls it after a level or two, so a level that cannot reach its target falls back to `meshopt_simplifySloppy`. The levels are consecutive mesh ids in one allocation; the scene registers each instance once under every level and `gpu_cull.cs` keeps the one whose screen size band matches, so the choice is per instance, on the GPU, and the levels not in use come out with an instance count of zero. Every view chooses from the player's camera, cascades included.
- [x] The first version of this produced levels that were visibly broken -- folded arms, smeared textures -- which is what the checkerboard of grey models at middle distance was. The meshes arrive from the importer with a vertex per triangle corner (45063 vertices for 45063 indices on the character), so they have no topology at all: every edge reads as the border of the surface and the simplifier refuses to collapse anything. It fell back to the sloppy pass, which ignores topology and wrecks the UVs. `weldMesh` merges vertices identical in every attribute first -- 45063 down to 9131 on that mesh -- and then `meshopt_simplifyWithAttributes`, weighted on UV and normal, hits every target exactly (45063 / 22527 / 11259 / 5625 indices) with a maximum error of 1% of the model's size. Each level now looks like the one above it.
- Measured on the character scene, Vulkan (3 alternating rounds each). Unwelded, full detail: 11.23 ms frame / 6.48 ms gpu. Welded, full detail: 6.23 / 5.81 -- the weld alone is most of it, since a fifth of the vertices are shaded and skinned. Welded with the levels: 5.22 / 4.83.

## 38. The post pass sharpened the aliasing back in
- [x] `post.fs` added a third of an unsharp mask (`hdr += (hdr - blur) * 0.32`) to the HDR values, after TAA. An edge between a bright and a dark sample overshoots at both ends; the tonemap turns the overshoot into a coloured fringe, and every pixel of aliasing TAA had just averaged away came back amplified. On a crowd of distant characters that reads as the whole image being crunchy, and it is the part that looked like bad anti-aliasing while the anti-aliasing was working.
- Fix: contrast adaptive sharpening after the tonemap instead, clamped to the range the four neighbours already cover and falling off where the neighbourhood is near black or near white. Strength is a graphics setting (`sharpness`, default 0.5) rather than a constant in the shader.

## 39. Motion vectors pointed from the world origin
- [x] `prevTransforms` was maintained by hand: `uploadTransforms` wrote the value it was about to replace. An entity is only uploaded when it changes, so an entity that never moved since it spawned kept whatever its slot was allocated with -- the identity matrix -- as its previous transform forever, and one that moved and then stopped kept the step it took last. The motion vector for a character standing still therefore ran from the world origin to where it actually is, TAA reprojected its history to the other side of the screen, and the models blended with the floor behind them: washed out, grey, worst on the ones furthest from the origin. Captures freeze the animation clock, which is why every still frame looked correct while the running scene did not.
- Fix: `cmdCopyBuffer` in the RHI (new), and the frame copies the whole transform buffer into `prevTransforms` after the last pass. Correct for every entity whether it moved or not, and it drops the CPU-side shadow copy of every transform that the old scheme needed. The copy is 64 bytes per entity once a frame and does not show up in the frame time.
- `RhiComputeTests.CopyBufferMovesTheRangeItWasGivenAndNothingElse` pins the offsets and that the copy touches nothing outside its range, on both backends.

## 40. Shadow casters a nearer cascade already covers -- tried, does not pay
- A shadow travels along the light and nowhere else, so in light space a caster whose footprint lies inside cascade N-1's side planes can only darken ground inside them, and that ground reads its shadow from the nearer cascade. Cascade N could skip it, which is the hole in the middle of every cascade past the first.
- Built it: four exclusion planes per view in `CullParams`, a containment test in `gpu_cull.cs`, and `CascadedShadowPass` handing each cascade the previous one's side planes. Two things bound what it can win. The lighting shader cross-fades between neighbouring cascades over a band near each split, so a caster near the boundary has to stay in both or it loses half its shadow where they mix -- the exclusion region has to shrink by that band. And the cascades are fitted per frame, so a low sun can place them side by side rather than one inside the other; the exclusion then has to be dropped for that cascade.
- Measured on both scenes, Vulkan, 3 alternating rounds each: character scene 4.53 ms frame / 4.06 ms gpu without it against 4.63 / 4.09 with. Rooms 4.26 / 3.56 against 4.31 / 3.65. Even with the shrink set to zero -- the most it could ever exclude, artefacts and all -- rooms gave 4.00 / 3.52 against 4.08 / 3.44. Nothing outside the noise either way.
- Why there is nothing there: the cascades already cull to their own volume, drop what the depth pyramid hides, and draw one LOD level coarser than the view. What is left inside the inner footprint is a small share of the casters, and the per-instance test costs about what it saves. Reverted; this note is here so it is not rebuilt from scratch.

## 41. Cascade boundaries ran as straight bands across the ground
- [x] Each cascade is fitted to a circle around its slice of the view (`CascadeShadowComponent::getLightSpaceMatrix`), which is what keeps the fit still while the camera turns. But `deferred_shading.fs` chose the cascade by distance along the view direction, which cuts the ground into slabs across the view -- so the step in resolution between two cascades ran as a razor-straight line over the floor, the one shape the eye picks out of a scene that has none.
- Fix: `selectCascade` picks the first cascade whose own fitted circle holds the fragment (`length(uv * 2 - 1) <= 1` on its shadow map), and the cross-fade runs over the outer fifth of that circle. On the ground the boundary is now an arc of that circle in perspective instead of a line. The choice lived in five copies -- the lighting path and four debug terms -- which is one function now, and the view-depth split lookup it replaces is gone.
- Cost: up to one matrix transform per cascade per pixel instead of one plus a fallback. Measured 5.02 ms frame / 4.48 ms gpu before, 5.05 / 4.57 after (3 alternating rounds, Vulkan).
- [x] The last cascade was still cut by straight lines, for two reasons. It has no next cascade to cross-fade into, so its shadow stopped wherever the square map ended: now it fades out over the rim of its own circle like every other step. And its far depth plane was fitted to the corners of the view slice, but the circle reaches past the slice sideways and so does the ground under it -- ground inside the circle but further along the light lay behind the far plane and lost its shadow along the line where that plane meets the floor. The far side is now padded by the circle's diameter (`CascadeShadowComponent::getLightSpaceMatrix`). Cascade depth maps stay identical between the backends; near shadows show no new acne.

## 42. Thin walls showed their inside on the coarser levels
- [x] A tin bucket, a frisbee, a barrel: sheet modelled with a thickness, two layers under a millimetre apart. `MeshLod` simplifies each layer on its own, and already the first level moves a vertex further than that, so the outer sheet dipped behind the inner one in patches; drawn with no culling, the inner sheet's back showed through it as dark triangles. Over the camp's models, 254 parts showed it on a level drawn 32 px across or more, in 184 of the litter models.
- Fix: a mesh whose back side is never seen (`markOneSided`, at load, kept in the model cache) is drawn with its back faces culled, by `GeometryPass` and by the game's litter pass; where the layers cross, the side facing the eye shows. One-sided is closed and facing out, which holds in any pose, or rigid and, drawn at full detail from 14 directions, with back faces under 1% of what is seen -- a bucket's handle wire is open at its ends, and the slit between its bottom and its wall shows 0.75%. That is 172 of the 254 parts. The rest are sheets and nets, and layers kept as parts of their own (an enamel bucket's blue outside and white inside), each of which alone shows its back. Shadows are drawn as before.
- Checked by `MeshLodTests`, by the camp views before and after (every difference within that of two runs of the same build) and by buckets on the ground drawn at their first and second levels (lod pixels 2 and 8), with no inside showing.

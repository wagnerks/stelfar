#include "rhi/SpirvReflect.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace SFE::Render {
	namespace {
		constexpr uint32_t kMagic = 0x07230203u;
		constexpr size_t kHeaderWords = 5;

		// The opcodes, decorations and storage classes read here, as the SPIR-V specification numbers them.
		enum Op : uint32_t {
			OpEntryPoint = 15,
			OpTypeImage = 25,
			OpTypeSampler = 26,
			OpTypeSampledImage = 27,
			OpTypeArray = 28,
			OpTypeRuntimeArray = 29,
			OpTypeStruct = 30,
			OpTypePointer = 32,
			OpConstant = 43,
			OpFunction = 54,
			OpFunctionEnd = 56,
			OpFunctionCall = 57,
			OpVariable = 59,
			OpImageTexelPointer = 60,
			OpLoad = 61,
			OpStore = 62,
			OpCopyMemory = 63,
			OpCopyMemorySized = 64,
			OpAccessChain = 65,
			OpInBoundsAccessChain = 66,
			OpPtrAccessChain = 67,
			OpArrayLength = 68,
			OpInBoundsPtrAccessChain = 70,
			OpDecorate = 71,
			OpCopyObject = 83,
			OpAtomicLoad = 227,
			OpAtomicStore = 228,
			OpAtomicXor = 242,
		};
		enum Decoration : uint32_t { DecorationBlock = 2, DecorationBufferBlock = 3, DecorationBinding = 33, DecorationDescriptorSet = 34 };
		enum StorageClass : uint32_t { StorageUniformConstant = 0, StorageUniform = 2, StorageStorageBuffer = 12 };

		struct FunctionUses {
			std::vector<uint32_t> named; // the ids its instructions may name a resource by
			std::vector<uint32_t> calls; // the functions it calls
		};

		const char* kindName(const SpirvResourceKind kind) {
			switch (kind) {
			case SpirvResourceKind::Texture: return "texture";
			case SpirvResourceKind::UniformBuffer: return "uniform buffer";
			case SpirvResourceKind::StorageBuffer: return "storage buffer";
			default: return "resource";
			}
		}

		const char* typeName(const DescriptorType type) {
			switch (type) {
			case DescriptorType::UniformBuffer: return "uniform buffer";
			case DescriptorType::StorageBuffer: return "storage buffer";
			case DescriptorType::CombinedImageSampler: return "texture";
			case DescriptorType::UniformBufferDynamic: return "dynamic uniform buffer";
			case DescriptorType::StorageBufferDynamic: return "dynamic storage buffer";
			default: return "descriptor";
			}
		}

		const char* stageName(const ShaderStage stage) {
			switch (stage) {
			case ShaderStage::Vertex: return "vertex";
			case ShaderStage::Fragment: return "fragment";
			case ShaderStage::Geometry: return "geometry";
			case ShaderStage::TessControl: return "tessellation control";
			case ShaderStage::TessEval: return "tessellation evaluation";
			case ShaderStage::Compute: return "compute";
			default: return "shader";
			}
		}

		// Whether a layout's type gives what the shader declares; a kind the RHI has no type for is not judged.
		bool typeGives(const DescriptorType type, const SpirvResourceKind kind) {
			switch (kind) {
			case SpirvResourceKind::Texture: return type == DescriptorType::CombinedImageSampler;
			case SpirvResourceKind::UniformBuffer: return type == DescriptorType::UniformBuffer || type == DescriptorType::UniformBufferDynamic;
			case SpirvResourceKind::StorageBuffer: return type == DescriptorType::StorageBuffer || type == DescriptorType::StorageBufferDynamic;
			default: return true;
			}
		}

		// A layout's stage mask as Vulkan reads it: none is every graphics stage (toVkShaderStage).
		bool stageGiven(const ShaderStage mask, const ShaderStage stage) {
			const auto bits = static_cast<uint8_t>(mask);
			if (bits == 0) {
				return stage != ShaderStage::Compute;
			}
			return (bits & static_cast<uint8_t>(stage)) != 0;
		}
	}

	SpirvReflection reflectSpirv(const uint32_t* words, const size_t count) {
		if (!words || count < kHeaderWords || words[0] != kMagic) {
			return {};
		}
		struct Variable {
			uint32_t id = 0;
			uint32_t type = 0;
			uint32_t storage = 0;
		};
		std::vector<Variable> variables;
		std::unordered_map<uint32_t, uint32_t> sets;
		std::unordered_map<uint32_t, uint32_t> bindings;
		std::unordered_set<uint32_t> blocks;
		std::unordered_set<uint32_t> bufferBlocks;
		std::unordered_map<uint32_t, uint32_t> pointees;                       // pointer type -> what it points at
		std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;    // array type -> element, length constant
		std::unordered_map<uint32_t, uint32_t> runtimeArrays;                  // runtime array type -> element
		std::unordered_set<uint32_t> opaque;                                   // image, sampler and sampled image types
		std::unordered_map<uint32_t, uint32_t> constants;
		std::vector<uint32_t> entryPoints;
		std::unordered_map<uint32_t, FunctionUses> functions;
		FunctionUses* current = nullptr; // the function being read; a node's address stays put as the map grows

		for (size_t at = kHeaderWords; at < count;) {
			const uint32_t length = words[at] >> 16;
			const uint32_t op = words[at] & 0xFFFFu;
			if (length == 0 || at + length > count) {
				return {};
			}
			const uint32_t* w = words + at;
			const auto name = [&](const uint32_t index) {
				if (current && index < length) {
					current->named.push_back(w[index]);
				}
			};
			switch (op) {
			case OpEntryPoint:
				if (length >= 3) {
					entryPoints.push_back(w[2]);
				}
				break;
			case OpDecorate:
				if (length >= 3) {
					if (w[2] == DecorationBlock) {
						blocks.insert(w[1]);
					}
					else if (w[2] == DecorationBufferBlock) {
						bufferBlocks.insert(w[1]);
					}
					else if (w[2] == DecorationBinding && length >= 4) {
						bindings[w[1]] = w[3];
					}
					else if (w[2] == DecorationDescriptorSet && length >= 4) {
						sets[w[1]] = w[3];
					}
				}
				break;
			case OpTypeImage:
			case OpTypeSampler:
			case OpTypeSampledImage:
				if (length >= 2) {
					opaque.insert(w[1]);
				}
				break;
			case OpTypeArray:
				if (length >= 4) {
					arrays[w[1]] = { w[2], w[3] };
				}
				break;
			case OpTypeRuntimeArray:
				if (length >= 3) {
					runtimeArrays[w[1]] = w[2];
				}
				break;
			case OpTypePointer:
				if (length >= 4) {
					pointees[w[1]] = w[3];
				}
				break;
			case OpConstant:
				if (length >= 4) {
					constants[w[2]] = w[3];
				}
				break;
			case OpFunction:
				if (length >= 3) {
					current = &functions[w[2]];
				}
				break;
			case OpFunctionEnd:
				current = nullptr;
				break;
			case OpVariable:
				// Only the module's own: a function's variables are its locals.
				if (!current && length >= 4) {
					variables.push_back({ w[2], w[1], w[3] });
				}
				break;
			case OpFunctionCall:
				if (current && length >= 4) {
					current->calls.push_back(w[3]);
					// A sampler is handed to a function by its pointer.
					for (uint32_t k = 4; k < length; ++k) {
						name(k);
					}
				}
				break;
			case OpLoad:
			case OpImageTexelPointer:
			case OpAccessChain:
			case OpInBoundsAccessChain:
			case OpPtrAccessChain:
			case OpInBoundsPtrAccessChain:
			case OpArrayLength:
			case OpCopyObject:
				name(3);
				break;
			case OpStore:
			case OpAtomicStore:
				name(1);
				break;
			case OpCopyMemory:
			case OpCopyMemorySized:
				name(1);
				name(2);
				break;
			default:
				if (op >= OpAtomicLoad && op <= OpAtomicXor) {
					name(3);
				}
				break;
			}
			at += length;
		}

		// What the entry points reach over their calls, at any depth.
		std::unordered_set<uint32_t> reached;
		std::vector<uint32_t> pending = entryPoints;
		std::unordered_set<uint32_t> named;
		while (!pending.empty()) {
			const uint32_t function = pending.back();
			pending.pop_back();
			if (!reached.insert(function).second) {
				continue;
			}
			const auto it = functions.find(function);
			if (it == functions.end()) {
				continue;
			}
			named.insert(it->second.named.begin(), it->second.named.end());
			pending.insert(pending.end(), it->second.calls.begin(), it->second.calls.end());
		}

		SpirvReflection reflection;
		reflection.ok = true;
		for (const auto& variable : variables) {
			const auto binding = bindings.find(variable.id);
			if (binding == bindings.end()) {
				continue;
			}
			SpirvResource resource;
			const auto set = sets.find(variable.id);
			resource.set = set != sets.end() ? set->second : 0u;
			resource.binding = binding->second;
			resource.used = named.contains(variable.id);

			uint32_t type = 0;
			if (const auto pointee = pointees.find(variable.type); pointee != pointees.end()) {
				type = pointee->second;
			}
			// A descriptor array: its length from its constant, 0 for a runtime one.
			if (const auto array = arrays.find(type); array != arrays.end()) {
				const auto length = constants.find(array->second.second);
				resource.count = length != constants.end() ? length->second : 1u;
				type = array->second.first;
			}
			else if (const auto runtime = runtimeArrays.find(type); runtime != runtimeArrays.end()) {
				resource.count = 0;
				type = runtime->second;
			}

			if (variable.storage == StorageUniformConstant) {
				resource.kind = opaque.contains(type) ? SpirvResourceKind::Texture : SpirvResourceKind::Other;
			}
			else if (variable.storage == StorageUniform) {
				resource.kind = blocks.contains(type) ? SpirvResourceKind::UniformBuffer
					: bufferBlocks.contains(type) ? SpirvResourceKind::StorageBuffer : SpirvResourceKind::Other;
			}
			else if (variable.storage == StorageStorageBuffer) {
				resource.kind = SpirvResourceKind::StorageBuffer;
			}
			else {
				continue; // not a descriptor: an input, an output or push constants decorated by mistake
			}
			reflection.resources.push_back(resource);
		}
		return reflection;
	}

	std::vector<std::string> layoutMismatches(const std::vector<SpirvStage>& stages, const std::vector<const DescriptorSetLayoutDesc*>& sets) {
		std::vector<std::string> lines;
		for (const auto& stage : stages) {
			if (!stage.resources) {
				continue;
			}
			const std::string shader = stage.name ? stage.name : "a shader";
			for (const auto& resource : *stage.resources) {
				if (!resource.used) {
					continue;
				}
				const std::string what = shader + ": set " + std::to_string(resource.set) + " binding " + std::to_string(resource.binding)
					+ " (" + kindName(resource.kind) + ")";
				const DescriptorSetLayoutBinding* given = nullptr;
				if (resource.set < sets.size() && sets[resource.set]) {
					for (const auto& binding : sets[resource.set]->bindings) {
						if (binding.binding == resource.binding) {
							given = &binding;
							break;
						}
					}
				}
				if (!given) {
					lines.push_back(what + " is not in its layout");
					continue;
				}
				if (!typeGives(given->type, resource.kind)) {
					lines.push_back(what + " is a " + typeName(given->type) + " in its layout");
				}
				if (!stageGiven(given->stageMask, stage.stage)) {
					lines.push_back(what + " is not given to the " + stageName(stage.stage) + " stage");
				}
				if (resource.count > 1 && resource.count > given->count) {
					lines.push_back(what + " is an array of " + std::to_string(resource.count) + ", its layout has " + std::to_string(given->count));
				}
			}
		}
		return lines;
	}

	std::vector<uint32_t> LayoutCompat::idsOf(const std::vector<const DescriptorSetLayoutDesc*>& sets, const uint32_t pushSize, const ShaderStage pushStages) {
		// Without a push range its stages make no range, so they do not count.
		const uint64_t stages = pushSize > 0 ? static_cast<uint8_t>(pushStages) : 0u;
		std::vector<uint64_t> key{ (static_cast<uint64_t>(pushSize) << 32) | stages };
		std::vector<uint32_t> ids;
		ids.reserve(sets.size());
		for (const auto* set : sets) {
			std::vector<DescriptorSetLayoutBinding> bindings;
			if (set) {
				bindings = set->bindings;
			}
			std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
			// Each layout led by its count of bindings, with its top bit set, so no two prefixes read alike.
			key.push_back((uint64_t{ 1 } << 63) | bindings.size());
			for (const auto& binding : bindings) {
				key.push_back((static_cast<uint64_t>(binding.binding) << 32) | binding.count);
				key.push_back((static_cast<uint64_t>(binding.type) << 8) | static_cast<uint8_t>(binding.stageMask));
			}
			const auto known = std::find(mKnown.begin(), mKnown.end(), key);
			if (known != mKnown.end()) {
				ids.push_back(static_cast<uint32_t>(known - mKnown.begin()));
			}
			else {
				ids.push_back(static_cast<uint32_t>(mKnown.size()));
				mKnown.push_back(key);
			}
		}
		return ids;
	}

	void BoundSets::bind(const std::vector<uint32_t>& compat, const uint32_t firstSet, const uint32_t* sets, const uint32_t count) {
		if (!sets || count == 0 || firstSet >= kMaxSets) {
			return;
		}
		const uint32_t last = (std::min)(firstSet + count - 1, kMaxSets - 1);
		const auto compatAt = [&](const uint32_t index) { return index < compat.size() ? compat[index] : kNone; };
		// Read before the bind changes anything.
		const bool keepHigher = mBound[last].set != kNone && last < compat.size() && mBound[last].compat == compat[last];
		for (uint32_t j = 0; j < firstSet; ++j) {
			if (j >= compat.size() || mBound[j].compat != compat[j]) {
				mBound[j] = {};
			}
		}
		if (!keepHigher) {
			for (uint32_t j = last + 1; j < kMaxSets; ++j) {
				mBound[j] = {};
			}
		}
		for (uint32_t index = firstSet; index <= last; ++index) {
			mBound[index] = { sets[index - firstSet], compatAt(index) };
		}
	}

	std::vector<std::string> BoundSets::missing(const std::vector<uint32_t>& compat, const std::vector<SpirvResource>& used,
		const std::array<uint32_t, kMaxSets>& known, const std::function<bool(uint32_t set, uint32_t binding)>& written) const
	{
		std::vector<std::pair<uint32_t, uint32_t>> wanted;
		wanted.reserve(used.size());
		for (const auto& resource : used) {
			if (resource.used) {
				wanted.emplace_back(resource.set, resource.binding);
			}
		}
		std::sort(wanted.begin(), wanted.end());
		wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

		std::vector<std::string> lines;
		for (size_t i = 0; i < wanted.size();) {
			const uint32_t set = wanted[i].first;
			size_t end = i;
			while (end < wanted.size() && wanted[end].first == set) {
				++end;
			}
			// A set past the pipeline's layout is the layout check's to name.
			if (set < compat.size() && set < kMaxSets) {
				const auto& bound = mBound[set];
				if (bound.set == kNone || bound.compat != compat[set]) {
					lines.push_back("set " + std::to_string(set) + " unbound");
				}
				else if (bound.set != known[set]) {
					for (size_t k = i; k < end; ++k) {
						if (!written || !written(bound.set, wanted[k].second)) {
							lines.push_back("set " + std::to_string(set) + " binding " + std::to_string(wanted[k].second) + " never written");
						}
					}
				}
			}
			i = end;
		}
		return lines;
	}

	uint32_t BoundSets::setAt(const uint32_t index) const {
		return index < kMaxSets ? mBound[index].set : kNone;
	}
}

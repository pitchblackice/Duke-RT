#pragma once

#include "nri_smoke_contracts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

inline uint32_t NRIMakeSmokeSourceId(std::string_view domain, std::string_view scope,
	std::string_view name, uint32_t instance = 0u)
{
	uint32_t hash = 2166136261u;
	auto append = [&hash](std::string_view value)
	{
		for (const unsigned char byte : value)
		{
			const unsigned char folded = byte >= 'A' && byte <= 'Z' ?
				static_cast<unsigned char>(byte + ('a' - 'A')) : byte;
			hash = (hash ^ folded) * 16777619u;
		}
		hash = (hash ^ 0xffu) * 16777619u;
	};
	append(domain);
	append(scope);
	append(name);
	for (uint32_t shift = 0u; shift < 32u; shift += 8u)
		hash = (hash ^ ((instance >> shift) & 0xffu)) * 16777619u;
	return hash != 0u ? hash : 1u;
}

constexpr uint32_t NRISmokeSourceClassMask = 0xffu;

inline uint32_t NRIPackSmokeSourceMetadata(NRISmokeInjectionSourceClass sourceClass,
	uint32_t authoredPriority = 0u)
{
	return (static_cast<uint32_t>(sourceClass) & NRISmokeSourceClassMask) |
		((authoredPriority & 0xffu) << 8u);
}

inline NRISmokeInjectionSourceClass NRIGetSmokeSourceClass(uint32_t sourceMetadata)
{
	return static_cast<NRISmokeInjectionSourceClass>(sourceMetadata & NRISmokeSourceClassMask);
}

inline uint32_t NRIGetSmokeSourceAuthoredPriority(uint32_t sourceMetadata)
{
	return (sourceMetadata >> 8u) & 0xffu;
}

inline bool NRIIsInteractiveSmokeSource(uint32_t sourceMetadata)
{
	return NRIGetSmokeSourceClass(sourceMetadata) != NRISmokeInjectionSourceClass::AmbientMap;
}

struct NRISmokeAdmissionSnapshot
{
	uint32_t gathered = 0;
	uint32_t uploaded = 0;
	uint32_t boundedDeferred = 0;
	uint32_t coalesced = 0;
	uint32_t expired = 0;
	uint32_t rejected = 0;
	uint32_t sourceCount = 0;
	uint32_t interactiveGathered = 0;
	uint32_t interactiveUploaded = 0;
	uint64_t estimatedBrickWorkGathered = 0;
	uint64_t estimatedBrickWorkUploaded = 0;

	bool Closes() const
	{
		return gathered == uploaded + boundedDeferred + coalesced + expired + rejected;
	}
};

// The CPU cap is a command transport limit. Estimate sparse-grid footprint so
// deficit service does not treat a broad map rectangle as equivalent to a
// point pulse while GPU allocation still performs exact source-fair brick work.
inline uint32_t NRIEstimateSmokeCommandBrickWork(const NRISmokeInjectionCommandGpu& command,
	float cellSize)
{
	const float brickWidth = std::max(cellSize, 0.0001f) * 8.0f;
	auto length3 = [](const float value[3])
	{
		return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
	};
	const float radius = std::max(command.spawnRadius * std::max(command.radiusScale, 0.0f), cellSize);
	const float extent = radius + length3(command.halfAxisU) + length3(command.halfAxisV);
	const uint32_t axis = std::clamp((uint32_t)std::ceil((extent * 2.0f) / brickWidth) + 1u, 1u, 16u);
	return std::min(axis * axis * axis, 4096u);
}

class NRISmokeAdmissionScheduler
{
public:
	NRISmokeAdmissionSnapshot SelectGridCommands(const std::vector<NRISmokeInjectionCommandGpu>& gathered,
		uint32_t capacity, uint32_t rendererFrame, float cellSize,
		std::vector<NRISmokeInjectionCommandGpu>& selected)
	{
		struct Source
		{
			uint32_t id = 0;
			uint32_t metadata = 0;
			std::vector<uint32_t> commands;
			uint32_t cursor = 0;
		};
		NRISmokeAdmissionSnapshot result = {};
		result.gathered = (uint32_t)gathered.size();
		selected.clear();
		selected.reserve(std::min<uint32_t>(capacity, result.gathered));
		std::vector<Source> sources;
		std::unordered_map<uint64_t, uint32_t> sourceLookup;
		for (uint32_t index = 0; index < result.gathered; ++index)
		{
			const auto& command = gathered[index];
			const uint64_t key = (uint64_t(command.sourceMetadata) << 32u) | command.sourceId;
			auto [it, inserted] = sourceLookup.emplace(key, (uint32_t)sources.size());
			if (inserted)
				sources.push_back({ command.sourceId, command.sourceMetadata });
			sources[it->second].commands.push_back(index);
			const uint32_t cost = NRIEstimateSmokeCommandBrickWork(command, cellSize);
			result.estimatedBrickWorkGathered += cost;
			if (NRIIsInteractiveSmokeSource(command.sourceMetadata))
				result.interactiveGathered++;
		}
		std::sort(sources.begin(), sources.end(), [](const Source& left, const Source& right)
		{
			const bool leftInteractive = NRIIsInteractiveSmokeSource(left.metadata);
			const bool rightInteractive = NRIIsInteractiveSmokeSource(right.metadata);
			if (leftInteractive != rightInteractive) return leftInteractive > rightInteractive;
			const uint32_t leftPriority = NRIGetSmokeSourceAuthoredPriority(left.metadata);
			const uint32_t rightPriority = NRIGetSmokeSourceAuthoredPriority(right.metadata);
			if (leftPriority != rightPriority) return leftPriority > rightPriority;
			if (left.id != right.id) return left.id < right.id;
			return left.metadata < right.metadata;
		});
		result.sourceCount = (uint32_t)sources.size();
		if (capacity == 0u || sources.empty())
		{
			result.rejected = result.gathered;
			return result;
		}

		auto emit = [&](Source& source)
		{
			const auto& command = gathered[source.commands[source.cursor++]];
			selected.push_back(command);
			result.estimatedBrickWorkUploaded += NRIEstimateSmokeCommandBrickWork(command, cellSize);
			if (NRIIsInteractiveSmokeSource(command.sourceMetadata))
				result.interactiveUploaded++;
		};
		auto service = [&](bool interactiveOnly, uint32_t limit)
		{
			if (sources.empty()) return;
			const uint32_t start = (rendererFrame + (interactiveOnly ? 0u : 1u)) % (uint32_t)sources.size();
			bool progressed = true;
			while (selected.size() < limit && progressed)
			{
				progressed = false;
				for (uint32_t ordinal = 0; ordinal < sources.size() && selected.size() < limit; ++ordinal)
				{
					Source& source = sources[(start + ordinal) % sources.size()];
					if ((interactiveOnly && !NRIIsInteractiveSmokeSource(source.metadata)) ||
						source.cursor >= source.commands.size())
						continue;
					const auto& command = gathered[source.commands[source.cursor]];
					const uint32_t cost = NRIEstimateSmokeCommandBrickWork(command, cellSize);
					int64_t& deficit = mDeficit[(uint64_t(source.metadata) << 32u) | source.id];
					deficit = std::min<int64_t>(deficit + 64, 8192);
					if (deficit < cost)
						continue;
					deficit -= cost;
					emit(source);
					progressed = true;
				}
			}
		};

		// Preserve the historical 192-command map ceiling's 64-command tail as
		// an explicit interactive service guarantee, then let every source borrow
		// the remainder through the same cost-aware rounds.
		service(true, std::min<uint32_t>(capacity, 64u));
		service(false, capacity);
		// A very costly first command must not be starved merely because this
		// frame did not contain enough deficit rounds. Finish with one stable
		// source round before admitting second commands from any source.
		if (selected.size() < capacity)
		{
			const uint32_t start = rendererFrame % (uint32_t)sources.size();
			for (uint32_t ordinal = 0; ordinal < sources.size() && selected.size() < capacity; ++ordinal)
			{
				Source& source = sources[(start + ordinal) % sources.size()];
				if (source.cursor < source.commands.size()) emit(source);
			}
		}

		std::stable_sort(selected.begin(), selected.end(), [](const auto& left, const auto& right)
		{
			if (left.sourceId != right.sourceId) return left.sourceId < right.sourceId;
			if (left.sourceMetadata != right.sourceMetadata) return left.sourceMetadata < right.sourceMetadata;
			return left.serial < right.serial;
		});
		uint32_t sourceSlot = UINT32_MAX;
		uint32_t previousId = 0u;
		uint32_t previousMetadata = 0u;
		for (auto& command : selected)
		{
			if (sourceSlot == UINT32_MAX || command.sourceId != previousId || command.sourceMetadata != previousMetadata)
			{
				sourceSlot++;
				previousId = command.sourceId;
				previousMetadata = command.sourceMetadata;
			}
			command.sourceSlot = sourceSlot;
		}
		result.uploaded = (uint32_t)selected.size();
		result.rejected = result.gathered - result.uploaded;
		return result;
	}

	void Reset() { mDeficit.clear(); }

private:
	std::unordered_map<uint64_t, int64_t> mDeficit;
};

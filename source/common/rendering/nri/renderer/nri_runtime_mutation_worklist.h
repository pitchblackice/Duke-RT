#pragma once

#include <cstdint>
#include <vector>

class NRIRuntimeMutationWorklist
{
public:
	void Reset();
	void BeginFrame(uint32_t chunkCount);
	bool MarkCandidate(uint32_t chunkIndex, uint32_t sourceMask);

	uint32_t GetSourceMask(uint32_t chunkIndex) const;
	const std::vector<uint32_t>& GetSourceMasks() const { return m_sourceMasks; }
	const std::vector<uint32_t>& GetCandidates() const { return m_candidates; }

private:
	std::vector<uint32_t> m_sourceMasks;
	std::vector<uint32_t> m_candidates;
};

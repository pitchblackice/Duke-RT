#include "nri_runtime_mutation_worklist.h"

void NRIRuntimeMutationWorklist::Reset()
{
	m_sourceMasks.clear();
	m_candidates.clear();
}

void NRIRuntimeMutationWorklist::BeginFrame(uint32_t chunkCount)
{
	if (m_sourceMasks.size() != chunkCount)
	{
		m_sourceMasks.assign(chunkCount, 0u);
		m_candidates.clear();
		m_candidates.reserve(chunkCount);
		return;
	}

	for (uint32_t chunkIndex : m_candidates)
	{
		m_sourceMasks[chunkIndex] = 0u;
	}
	m_candidates.clear();
}

bool NRIRuntimeMutationWorklist::MarkCandidate(uint32_t chunkIndex, uint32_t sourceMask)
{
	if (chunkIndex >= m_sourceMasks.size() || sourceMask == 0u)
	{
		return false;
	}

	const bool added = m_sourceMasks[chunkIndex] == 0u;
	if (added)
	{
		m_candidates.push_back(chunkIndex);
	}
	m_sourceMasks[chunkIndex] |= sourceMask;
	return added;
}

uint32_t NRIRuntimeMutationWorklist::GetSourceMask(uint32_t chunkIndex) const
{
	return chunkIndex < m_sourceMasks.size() ? m_sourceMasks[chunkIndex] : 0u;
}

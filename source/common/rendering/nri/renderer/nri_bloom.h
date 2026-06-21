#pragma once

class NRIPassDispatchContext;

struct NRIBloomDispatchDesc
{
};

bool DispatchBloom(NRIPassDispatchContext& context, const NRIBloomDispatchDesc& desc);

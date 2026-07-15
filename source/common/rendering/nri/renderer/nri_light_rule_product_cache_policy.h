#pragma once

#include <cstdint>
#include <utility>
#include <vector>

struct NRILightRuleProductCacheIdentity
{
	bool CanReuseResolvedProducts(uint32_t resolvedGeneration) const
	{
		return resolvedProductsValid &&
			resolvedGeneration != 0 &&
			resolvedProductsGeneration == resolvedGeneration;
	}

	bool CanReuseStaticMapProducts(uint32_t resolvedGeneration, uint64_t mapBuildSerial) const
	{
		return staticMapProductsValid &&
			resolvedGeneration != 0 &&
			mapBuildSerial != 0 &&
			staticMapProductsGeneration == resolvedGeneration &&
			staticMapBuildSerial == mapBuildSerial;
	}

	void CommitResolvedProducts(uint32_t resolvedGeneration)
	{
		resolvedProductsValid = resolvedGeneration != 0;
		resolvedProductsGeneration = resolvedProductsValid ? resolvedGeneration : 0;
	}

	void CommitStaticMapProducts(uint32_t resolvedGeneration, uint64_t mapBuildSerial)
	{
		staticMapProductsValid = resolvedGeneration != 0 && mapBuildSerial != 0;
		staticMapProductsGeneration = staticMapProductsValid ? resolvedGeneration : 0;
		staticMapBuildSerial = staticMapProductsValid ? mapBuildSerial : 0;
	}

	void InvalidateResolvedProducts()
	{
		resolvedProductsValid = false;
		resolvedProductsGeneration = 0;
	}

	void InvalidateStaticMapProducts()
	{
		staticMapProductsValid = false;
		staticMapProductsGeneration = 0;
		staticMapBuildSerial = 0;
	}

	void Reset()
	{
		InvalidateResolvedProducts();
		InvalidateStaticMapProducts();
	}

	bool resolvedProductsValid = false;
	bool staticMapProductsValid = false;
	uint32_t resolvedProductsGeneration = 0;
	uint32_t staticMapProductsGeneration = 0;
	uint64_t staticMapBuildSerial = 0;
};

namespace nri_light_rule_product_cache
{
	template<typename SourceRange, typename Product, typename TryBuildProduct>
	void AppendAcceptedProductsInSourceOrder(
		const SourceRange& source,
		std::vector<Product>& products,
		TryBuildProduct&& tryBuildProduct)
	{
		for (const auto& sourceRule : source)
		{
			Product product = {};
			if (tryBuildProduct(sourceRule, product))
			{
				products.push_back(std::move(product));
			}
		}
	}
}

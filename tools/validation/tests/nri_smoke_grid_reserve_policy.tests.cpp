#include "nri_smoke_grid_reserve_policy.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
		std::exit(1);
	}
}
}

int main()
{
	Require(NRISmokeGridFirstUseCoreCapacity(64u) == 8u,
		"minimum-capacity grid must retain an eight-brick soft core");
	Require(NRISmokeGridFirstUseCoreCapacity(512u) == 32u,
		"default grid must use the documented one-sixteenth soft core");
	Require(NRISmokeGridFirstUseCoreCapacity(4096u) == 256u,
		"large grid must scale the soft core without a hard quota");
	Require(!NRISmokeGridAmbientBorrowsCore(0u, 33u, 512u),
		"ambient allocation outside the core must remain ordinary");
	Require(NRISmokeGridAmbientBorrowsCore(0u, 32u, 512u),
		"ambient allocation consuming the soft core must be marked borrowed");
	Require(!NRISmokeGridAmbientBorrowsCore(2u, 1u, 512u),
		"interactive first use must never be marked as borrowed ambient capacity");
	Require(NRISmokeGridIsFirstUseClass(1u) && NRISmokeGridIsFirstUseClass(2u) &&
		NRISmokeGridIsFirstUseClass(3u) && !NRISmokeGridIsFirstUseClass(0u),
		"all non-ambient source classes must be eligible for first-use recovery");
	Require(NRISmokeGridBorrowedDormantCandidate(NRI_SMOKE_GRID_POLICY_BRICK_RESIDENT,
		NRI_SMOKE_GRID_POLICY_BRICK_BORROWED | NRI_SMOKE_GRID_POLICY_BRICK_HALO, 1u),
		"prior-frame borrowed optically empty topology must be replaceable");
	Require(!NRISmokeGridBorrowedDormantCandidate(NRI_SMOKE_GRID_POLICY_BRICK_RESIDENT,
		NRI_SMOKE_GRID_POLICY_BRICK_BORROWED | NRI_SMOKE_GRID_POLICY_BRICK_CONTENT, 1u),
		"borrowed visible density must never be replaceable");
	Require(!NRISmokeGridBorrowedDormantCandidate(NRI_SMOKE_GRID_POLICY_BRICK_RESIDENT,
		NRI_SMOKE_GRID_POLICY_BRICK_BORROWED | NRI_SMOKE_GRID_POLICY_BRICK_HALO, 0u),
		"same-frame or unclassified borrowed topology must not be replaceable");

	std::cout << "Smoke grid borrowable first-use reserve policy tests passed.\n";
	return 0;
}

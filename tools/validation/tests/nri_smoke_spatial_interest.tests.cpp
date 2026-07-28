#include "nri_smoke_spatial_interest.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

NRISmokeSpatialBrickObservation Brick(int32_t x, uint32_t generation = 1u,
	NRISmokeSpatialAuthority authority = NRISmokeSpatialAuthority::Fine)
{
	NRISmokeSpatialBrickObservation result = {};
	result.coordinate.x = x;
	result.generation = generation;
	result.authority = authority;
	result.occupied = true;
	result.opticalMass = static_cast<float>(x + 100);
	return result;
}

NRISmokeSpatialInterestRegion Region(float minimum, float maximum, uint32_t reason)
{
	NRISmokeSpatialInterestRegion result = {};
	result.boundsMin[0] = minimum;
	result.boundsMin[1] = result.boundsMin[2] = -1.0f;
	result.boundsMax[0] = maximum;
	result.boundsMax[1] = result.boundsMax[2] = 65.0f;
	result.reasons = reason;
	return result;
}

NRISmokeSpatialInterestConfig SmallConfig()
{
	NRISmokeSpatialInterestConfig config = {};
	config.hotEnterDistance = 10.0f;
	config.hotLeaveDistance = 20.0f;
	config.warmEnterDistance = 30.0f;
	config.warmLeaveDistance = 40.0f;
	config.maximumPrefetchDistance = 20.0f;
	config.cameraCutDistance = 100.0f;
	config.recentPositiveFrames = 2u;
	config.cameraCutGraceFrames = 2u;
	config.discoveryGraceFrames = 0u;
	config.minimumDormantFrames = 2u;
	return config;
}
}

int main()
{
	NRISmokeSpatialInterestOwner owner;
	NRISmokeSpatialInterestFrameInput input = {};
	input.epoch = 3u;
	input.brickWorldSize = 8.0f;
	input.conservativeInterestComplete = true;
	input.demotionQuantity = 2u;
	input.promotionQuantity = 2u;
	input.bricks = { Brick(20), Brick(21), Brick(22) };
	const auto config = SmallConfig();

	const auto& discovered = owner.Update(input, config);
	Require(discovered.warm == 3u && discovered.demotions.empty(),
		"newly observed bricks must receive one conservative discovery frame");
	input.rendererFrame = 1u;
	const auto& dormant = owner.Update(input, config);
	Require(dormant.dormant == 3u && dormant.demotions.empty(),
		"dormancy must not become archive eligible before its leave grace");
	input.rendererFrame = 3u;
	const auto& capped = owner.Update(input, config);
	Require(capped.eligibleDemotions == 3u && capped.demotions.size() == 2u &&
		capped.projectedRecoveredBricks == 3u && capped.selectedRecoveredBricks == 2u,
		"fixed list cap must not hide total eligible or projected capacity");
	Require(capped.demotions[0].coordinate.x == 20 && capped.demotions[1].coordinate.x == 21,
		"equal-age demotion ordering must be stable and prefer lower optical mass");

	std::reverse(input.bricks.begin(), input.bricks.end());
	input.rendererFrame = 4u;
	const auto& reordered = owner.Update(input, config);
	Require(reordered.demotions[0].coordinate.x == 20 && reordered.demotions[1].coordinate.x == 21,
		"caller observation order must not affect worklist order");

	input.conservativeInterestComplete = false;
	input.rendererFrame = 5u;
	const auto& incomplete = owner.Update(input, config);
	Require(!incomplete.demotionEvidenceValid && incomplete.warm == 3u && incomplete.demotions.empty(),
		"incomplete positive-interest evidence must never authorize demotion");
	input.conservativeInterestComplete = true;
	input.runtimePortalUncertain = true;
	input.rendererFrame = 6u;
	const auto& uncertainPortal = owner.Update(input, config);
	Require(!uncertainPortal.demotionEvidenceValid && uncertainPortal.warm == 3u,
		"unresolved runtime portals must preserve potentially reachable smoke");

	owner.Reset(4u);
	input = {};
	input.epoch = 4u;
	input.brickWorldSize = 8.0f;
	input.conservativeInterestComplete = true;
	input.bricks = { Brick(0), Brick(10), Brick(11, 1u, NRISmokeSpatialAuthority::Coarse) };
	input.positiveRegions = {
		Region(0.0f, 8.0f, NRISmokeSpatialReason_Portal),
		Region(80.0f, 96.0f, NRISmokeSpatialReason_Mirror),
	};
	input.promotionQuantity = 1u;
	const auto& positive = owner.Update(input, config);
	Require(positive.hot == 3u && positive.promotions.size() == 1u &&
		(positive.promotions[0].reasons & NRISmokeSpatialReason_Mirror) != 0u,
		"portal and mirror regions must be positive evidence and coarse smoke must enter promotion work");

	owner.Reset(5u);
	input = {};
	input.epoch = 5u;
	input.brickWorldSize = 8.0f;
	input.conservativeInterestComplete = true;
	input.bricks = { Brick(0), Brick(100) };
	input.rendererFrame = 1u;
	input.cameraPosition[0] = 804.0f;
	input.previousCameraPosition[0] = 4.0f;
	input.hasPreviousCamera = true;
	const auto& cut = owner.Update(input, config);
	Require(cut.cameraCut && cut.hot == 1u && cut.warm == 1u,
		"camera cuts must keep the old neighborhood warm while the destination becomes hot");
	input.rendererFrame = 2u;
	input.previousCameraPosition[0] = 780.0f;
	const auto& fastTurn = owner.Update(input, config);
	Require(!fastTurn.cameraCut && fastTurn.hot == 1u && fastTurn.warm == 1u,
		"fast non-cut motion must not discard the previous camera-cut neighborhood during grace");

	owner.Reset(6u);
	input = {};
	input.epoch = 6u;
	input.brickWorldSize = 8.0f;
	input.conservativeInterestComplete = true;
	input.bricks = { Brick(50, 1u, NRISmokeSpatialAuthority::Coarse),
		Brick(51, 1u, NRISmokeSpatialAuthority::Coarse) };
	input.positiveRegions = { Region(400.0f, 416.0f, NRISmokeSpatialReason_MainView) };
	input.promotionQuantity = 1u;
	const auto& promotionCap = owner.Update(input, config);
	Require(promotionCap.eligiblePromotions == 2u && promotionCap.promotions.size() == 1u,
		"promotion list must expose overflow behind its fixed quantity");

	input.bricks.push_back(Brick(50, 2u));
	input.rendererFrame = 1u;
	const auto& duplicate = owner.Update(input, config);
	Require(duplicate.duplicateObservations == 1u && duplicate.observed == 2u,
		"duplicate coordinates must be detected and highest generation selected deterministically");

	owner.Reset(7u);
	input = {};
	input.epoch = 7u;
	input.brickWorldSize = 8.0f;
	input.conservativeInterestComplete = true;
	input.cameraPosition[0] = 1000.0f;
	auto upperRoom = Brick(0);
	upperRoom.coordinate.z = 10;
	input.bricks = { Brick(0), upperRoom };
	input.positiveRegions = { Region(0.0f, 8.0f, NRISmokeSpatialReason_MainView) };
	owner.Update(input, config);
	input.rendererFrame = 1u;
	const auto& stackedRooms = owner.Update(input, config);
	Require(stackedRooms.hot == 1u && stackedRooms.dormant == 1u,
		"room-over-room interest must remain vertically bounded instead of promoting a stacked brick");

	std::cout << "Smoke spatial-interest classification tests passed.\n";
	return 0;
}

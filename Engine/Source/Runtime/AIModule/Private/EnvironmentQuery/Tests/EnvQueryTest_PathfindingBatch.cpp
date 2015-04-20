// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "AIModulePrivate.h"
#include "AI/Navigation/NavigationSystem.h"
#include "AI/Navigation/NavAgentInterface.h"
#include "AI/Navigation/RecastNavMesh.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_PathfindingBatch.h"

#if WITH_RECAST
#include "AI/Navigation/PImplRecastNavMesh.h"
#endif

#define LOCTEXT_NAMESPACE "EnvQueryGenerator"

UEnvQueryTest_PathfindingBatch::UEnvQueryTest_PathfindingBatch(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	ScanRangeMultiplier.DefaultValue = 1.5f;
}

#if WITH_RECAST
namespace NodePoolHelpers
{
	static bool HasPath(const FRecastDebugPathfindingData& NodePool, const FNavigationProjectionWork* TestPt)
	{
		FRecastDebugPathfindingNode SearchKey(TestPt->OutLocation.NodeRef);
		const FRecastDebugPathfindingNode* MyNode = NodePool.Nodes.Find(SearchKey);
		return MyNode != nullptr;
	}

	static float GetPathLength(const FRecastDebugPathfindingData& NodePool, const FNavigationProjectionWork* TestPt, const dtQueryFilter* Filter)
	{
		FRecastDebugPathfindingNode SearchKey(TestPt->OutLocation.NodeRef);
		const FRecastDebugPathfindingNode* MyNode = NodePool.Nodes.Find(SearchKey);
		if (MyNode)
		{
			float LastSegmentLength = FVector::Dist(MyNode->NodePos, TestPt->OutLocation.Location);
			return MyNode->Length + LastSegmentLength;
		}

		return BIG_NUMBER;
	}

	static float GetPathCost(const FRecastDebugPathfindingData& NodePool, const FNavigationProjectionWork* TestPt, const dtQueryFilter* Filter)
	{
		FRecastDebugPathfindingNode SearchKey(TestPt->OutLocation.NodeRef);
		const FRecastDebugPathfindingNode* MyNode = NodePool.Nodes.Find(SearchKey);
		if (MyNode)
		{
			return MyNode->TotalCost;
		}

		return BIG_NUMBER;
	}

	typedef float(*PathParamFunc)(const FRecastDebugPathfindingData&, const FNavigationProjectionWork*, const dtQueryFilter*);
}

void UEnvQueryTest_PathfindingBatch::RunTest(FEnvQueryInstance& QueryInstance) const
{
	UObject* DataOwner = QueryInstance.Owner.Get();
	BoolValue.BindData(DataOwner, QueryInstance.QueryID);
	PathFromContext.BindData(DataOwner, QueryInstance.QueryID);
	SkipUnreachable.BindData(DataOwner, QueryInstance.QueryID);
	FloatValueMin.BindData(DataOwner, QueryInstance.QueryID);
	FloatValueMax.BindData(DataOwner, QueryInstance.QueryID);
	ScanRangeMultiplier.BindData(DataOwner, QueryInstance.QueryID);

	bool bWantsPath = BoolValue.GetValue();
	bool bPathToItem = PathFromContext.GetValue();
	bool bDiscardFailed = SkipUnreachable.GetValue();
	float MinThresholdValue = FloatValueMin.GetValue();
	float MaxThresholdValue = FloatValueMax.GetValue();
	float RangeMultiplierValue = ScanRangeMultiplier.GetValue();

	UNavigationSystem* NavSys = QueryInstance.World->GetNavigationSystem();
	if (NavSys == nullptr)
	{
		return;
	}
	ANavigationData* NavData = FindNavigationData(*NavSys, QueryInstance.Owner.Get());
	ARecastNavMesh* NavMeshData = Cast<ARecastNavMesh>(NavData);
	if (NavMeshData == nullptr)
	{
		return;
	}

	TArray<FVector> ContextLocations;
	if (!QueryInstance.PrepareContext(Context, ContextLocations))
	{
		return;
	}

	TArray<FNavigationProjectionWork> TestPoints;
	TArray<float> CollectDistanceSq;
	CollectDistanceSq.Init(0.0f, ContextLocations.Num());

	TSharedPtr<FNavigationQueryFilter> NavigationFilter = FilterClass != nullptr
		? UNavigationQueryFilter::GetQueryFilter(*NavMeshData, FilterClass)->GetCopy()
		: NavMeshData->GetDefaultQueryFilter()->GetCopy();
	NavigationFilter->SetBacktrackingEnabled(!bPathToItem);
	const dtQueryFilter* NavQueryFilter = ((const FRecastQueryFilter*)NavigationFilter->GetImplementation())->GetAsDetourQueryFilter();

	{
		// scope for perf timers

		// can't use FEnvQueryInstance::ItemIterator yet, since it has built in scoring functionality
		for (int32 ItemIdx = 0; ItemIdx < QueryInstance.Items.Num(); ItemIdx++)
		{
			if (QueryInstance.Items[ItemIdx].IsValid())
			{
				const FVector ItemLocation = GetItemLocation(QueryInstance, ItemIdx);
				TestPoints.Add(FNavigationProjectionWork(ItemLocation));

				for (int32 ContextIdx = 0; ContextIdx < ContextLocations.Num(); ContextIdx++)
				{
					const float TestDistanceSq = FVector::DistSquared(ItemLocation, ContextLocations[ContextIdx]);
					CollectDistanceSq[ContextIdx] = FMath::Max(CollectDistanceSq[ContextIdx], TestDistanceSq);
				}
			}
		}

		NavMeshData->BatchProjectPoints(TestPoints, NavMeshData->GetDefaultQueryExtent(), NavigationFilter);
	}

	TArray<FRecastDebugPathfindingData> NodePoolData;
	NodePoolData.SetNum(ContextLocations.Num());

	{
		// scope for perf timer

		TArray<NavNodeRef> Polys;
		for (int32 ContextIdx = 0; ContextIdx < ContextLocations.Num(); ContextIdx++)
		{
			const float MaxPathDistance = FMath::Sqrt(CollectDistanceSq[ContextIdx]) * RangeMultiplierValue;

			Polys.Reset();
			NodePoolData[ContextIdx].Flags = ERecastDebugPathfindingFlags::PathLength;

			NavMeshData->GetPolysWithinPathingDistance(ContextLocations[ContextIdx], MaxPathDistance, Polys, NavigationFilter, nullptr, &NodePoolData[ContextIdx]);
		}
	}

	FNavigationProjectionWork* ProjectedPtr = TestPoints.GetTypedData();
	if (GetWorkOnFloatValues())
	{
		NodePoolHelpers::PathParamFunc Func[] = { nullptr, NodePoolHelpers::GetPathCost, NodePoolHelpers::GetPathLength };
		FEnvQueryInstance::ItemIterator It(this, QueryInstance);
		for (It.IgnoreTimeLimit(); It; ++It, ProjectedPtr++)
		{
			for (int32 ContextIndex = 0; ContextIndex < ContextLocations.Num(); ContextIndex++)
			{
				const float PathValue = Func[TestMode](NodePoolData[ContextIndex], ProjectedPtr, NavQueryFilter);
				It.SetScore(TestPurpose, FilterType, PathValue, MinThresholdValue, MaxThresholdValue);

				if (bDiscardFailed && PathValue >= BIG_NUMBER)
				{
					It.DiscardItem();
				}
			}
		}
	}
	else
	{
		FEnvQueryInstance::ItemIterator It(this, QueryInstance);
		for (It.IgnoreTimeLimit(); It; ++It, ProjectedPtr++)
		{
			for (int32 ContextIndex = 0; ContextIndex < ContextLocations.Num(); ContextIndex++)
			{
				const bool bFoundPath = NodePoolHelpers::HasPath(NodePoolData[ContextIndex], ProjectedPtr);
				It.SetScore(TestPurpose, FilterType, bFoundPath, bWantsPath);
			}
		}
	}
}

#else

void UEnvQueryTest_PathfindingBatch::RunTest(FEnvQueryInstance& QueryInstance) const
{
	// can't do anything without navmesh
}

#endif

FText UEnvQueryTest_PathfindingBatch::GetDescriptionTitle() const
{
	return FText::Format(LOCTEXT("PathfindingBatch","{0} [batch]"), Super::GetDescriptionTitle());
}

#undef LOCTEXT_NAMESPACE

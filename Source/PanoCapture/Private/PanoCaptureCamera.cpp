// Copyright Jad Deeb. All Rights Reserved.

#include "PanoCaptureCamera.h"
#include "PanoCaptureModule.h"
#include "PanoCapturePoint.h"
#include "PanoExposure.h"
#include "PanoSceneExport.h"
#include "PanoStitcher.h"
#include "PanoViewerServer.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Components/ArrowComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "CollisionShape.h"
#include "ContentStreaming.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "PanoCaptureCamera"

namespace
{
	struct FFaceDef
	{
		const TCHAR* Name;
		FRotator Rotation;
	};

	const FFaceDef GFaceDefs[] =
	{
		{ TEXT("Face_Front"), FRotator(0.f, 0.f, 0.f) },
		{ TEXT("Face_Right"), FRotator(0.f, 90.f, 0.f) },
		{ TEXT("Face_Back"),  FRotator(0.f, 180.f, 0.f) },
		{ TEXT("Face_Left"),  FRotator(0.f, -90.f, 0.f) },
		{ TEXT("Face_Up"),    FRotator(90.f, 0.f, 0.f) },
		{ TEXT("Face_Down"),  FRotator(-90.f, 0.f, 0.f) },
	};

	// Tag on points created by Auto Place Points, so a new run can replace them without touching hand-placed ones.
	const FName AutoPointTag(TEXT("PanoAutoPoint"));

	// Stitched images waiting to be written. Each 8K panorama plus its faces is roughly 300 MB of CPU memory.
	constexpr int32 MaxPendingWrites = 2;

	// Base color needs no lighting to converge; a few frames let streaming and Nanite catch up.
	constexpr int32 FloorPlanFrames = 8;

	// How far below a capture point to look for the floor.
	constexpr float FloorTraceDistance = 1000.f;
	constexpr float DefaultEyeHeight = 150.f;

#if WITH_EDITOR
	/** Undo/redo for editor actions, through UEngine so this runtime module doesn't link against UnrealEd. */
	struct FEditorTransaction
	{
		bool bActive = false;

		FEditorTransaction(const UWorld* World, const FText& Description)
		{
			if (GEngine && GIsEditor && !GIsTransacting && World && !World->IsGameWorld() && GEngine->CanTransact())
			{
				bActive = GEngine->BeginTransaction(TEXT("PanoCapture"), Description, nullptr) != INDEX_NONE;
			}
		}

		~FEditorTransaction()
		{
			if (bActive)
			{
				GEngine->EndTransaction();
			}
		}
	};
#endif

	void ConvertToColor(const TArray<FFloat16Color>& In, TArray<FColor>& Out, bool bLinearToSRGB)
	{
		Out.SetNumUninitialized(In.Num());
		ParallelFor(In.Num() / 4096 + 1, [&](int32 Chunk)
		{
			const int32 End = FMath::Min((Chunk + 1) * 4096, In.Num());
			for (int32 Index = Chunk * 4096; Index < End; ++Index)
			{
				const FLinearColor Color(In[Index]);
				Out[Index] = bLinearToSRGB
					? FLinearColor(Color.R, Color.G, Color.B, 1.f).ToFColorSRGB()
					: FLinearColor(Color.R, Color.G, Color.B, 1.f).ToFColor(false);
			}
		});
	}
}

APanoCaptureCamera::APanoCaptureCamera()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	LensRoot = CreateDefaultSubobject<USceneComponent>(TEXT("LensRoot"));
	LensRoot->SetupAttachment(SceneRoot);

	for (const FFaceDef& Def : GFaceDefs)
	{
		USceneCaptureComponent2D* Face = CreateDefaultSubobject<USceneCaptureComponent2D>(Def.Name);
		Face->SetupAttachment(LensRoot);
		Face->SetRelativeRotation(Def.Rotation);
		Face->FOVAngle = 90.f;
		Face->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		Face->bCaptureEveryFrame = false;
		Face->bCaptureOnMovement = false;
		// Keeps temporal history (Lumen, TSR, VSM caches) between warm-up frames.
		Face->bAlwaysPersistRenderingState = true;
		Face->ShowFlags.SetMotionBlur(false);
		Face->ShowFlags.SetVignette(false);
		Face->ShowFlags.SetDepthOfField(false);
		Face->ShowFlags.SetLocalExposure(false);
		Faces.Add(Face);
	}

	PlanCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("PlanCapture"));
	PlanCapture->SetupAttachment(SceneRoot);
	PlanCapture->ProjectionType = ECameraProjectionMode::Orthographic;
	PlanCapture->bAutoCalculateOrthoPlanes = false;
	// Albedo only: a plan should read like a map, not depend on where the sun is.
	PlanCapture->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
	PlanCapture->bCaptureEveryFrame = false;
	PlanCapture->bCaptureOnMovement = false;
	PlanCapture->bAlwaysPersistRenderingState = true;

#if WITH_EDITORONLY_DATA
	Arrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	if (Arrow)
	{
		Arrow->SetupAttachment(LensRoot);
		Arrow->ArrowColor = FColor(255, 160, 0);
	}
#endif
}

void APanoCaptureCamera::CaptureHere()
{
	FCaptureJob Job;
	Job.Id = FDateTime::Now().ToString(TEXT("Pano_%Y%m%d_%H%M%S"));
	Job.Location = GetActorLocation();
	Job.Heading = bUsePointYaw ? GetActorRotation().Yaw : 0.f;
	Job.FloorZ = FindFloorZ(Job.Location);

	TArray<FCaptureJob> NewJobs;
	NewJobs.Add(MoveTemp(Job));
	StartCapture(MoveTemp(NewJobs), false);
}

void APanoCaptureCamera::CaptureTour()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	TArray<APanoCapturePoint*> Points;
	for (TActorIterator<APanoCapturePoint> It(World); It; ++It)
	{
		if (It->bIncludeInTour)
		{
			Points.Add(*It);
		}
	}

	OrderForCapture(Points);

	TArray<FCaptureJob> NewJobs;
	TSet<FString> UsedIds;
	for (APanoCapturePoint* Point : Points)
	{
		FString Id = Point->GetResolvedId();
		for (int32 Suffix = 2; UsedIds.Contains(Id); ++Suffix)
		{
			Id = FString::Printf(TEXT("%s_%d"), *Point->GetResolvedId(), Suffix);
		}
		UsedIds.Add(Id);

		FCaptureJob& Job = NewJobs.AddDefaulted_GetRef();
		Job.Id = Id;
		Job.Location = Point->GetActorLocation();
		Job.Heading = bUsePointYaw ? Point->GetActorRotation().Yaw : 0.f;
		Job.FloorZ = FindFloorZ(Job.Location);
		Job.Floor = Point->FloorIndex;
		Job.Point = Point;
	}

	if (ExposureMode == EPanoExposureMode::Tour && NewJobs.Num() > 1)
	{
		// First sweep meters every point (and writes its depth); the second captures color at one shared exposure.
		TArray<FCaptureJob> MeterJobs = NewJobs;
		for (FCaptureJob& Job : MeterJobs)
		{
			Job.bMeterOnly = true;
		}
		for (FCaptureJob& Job : NewJobs)
		{
			Job.bUseTourBias = true;
		}
		MeterJobs.Append(MoveTemp(NewJobs));
		NewJobs = MoveTemp(MeterJobs);
	}

	if (bCaptureFloorPlans)
	{
		AddFloorPlanJobs(NewJobs);
	}

	StartCapture(MoveTemp(NewJobs), true);
}

void APanoCaptureCamera::CancelCapture()
{
	if (IsCapturing())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("Pano capture cancelled."));
		StopCapture(true);
	}
}

void APanoCaptureCamera::OpenViewer()
{
	const FString Folder = GetOutputFolder();
	if (!FPaths::FileExists(Folder / TEXT("tour.json")))
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("No tour in %s yet. Run Capture Tour first."), *Folder);
		return;
	}

	CopyViewer();
	const FString Url = PanoViewerServer::GetViewerUrl(Folder);
	if (!Url.IsEmpty())
	{
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
}

FString APanoCaptureCamera::GetOutputFolder() const
{
	FString Base;
	if (OutputDirectory.IsEmpty())
	{
		Base = FPaths::ProjectSavedDir() / TEXT("PanoCaptures");
	}
	else if (FPaths::IsRelative(OutputDirectory))
	{
		Base = FPaths::ProjectSavedDir() / OutputDirectory;
	}
	else
	{
		Base = OutputDirectory;
	}

	const FString Folder = Base / FPaths::MakeValidFileName(GetResolvedTourName(), TEXT('_'));
	return FPaths::ConvertRelativePathToFull(Folder);
}

FString APanoCaptureCamera::GetResolvedTourName() const
{
	if (!TourName.IsEmpty())
	{
		return TourName;
	}
	if (const UWorld* World = GetWorld())
	{
		// Strip the PIE prefix so play-in-editor captures land next to editor ones.
		return UWorld::RemovePIEPrefix(World->GetMapName());
	}
	return TEXT("Tour");
}

void APanoCaptureCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopCapture(true);
	Super::EndPlay(EndPlayReason);
}

void APanoCaptureCamera::Destroyed()
{
	// Editor-world actors never get EndPlay, so deleting the camera mid-capture is handled here.
	StopCapture(true);
	Super::Destroyed();
}

void APanoCaptureCamera::BeginDestroy()
{
	// Closing the map mid-capture reaches neither EndPlay nor Destroyed. Only non-UObject state is touched here.
	ReleaseCaptureState(true);
	Super::BeginDestroy();
}

#if WITH_EDITOR
void APanoCaptureCamera::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FProperty* Property = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty : PropertyChangedEvent.Property;
	if (IsCapturing() && Property && Property->GetOwnerClass() == APanoCaptureCamera::StaticClass())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("%s changed during a capture; the running capture keeps the settings it started with."), *Property->GetName());
	}
}
#endif

float APanoCaptureCamera::FindFloorZ(const FVector& From) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return From.Z - DefaultEyeHeight;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PanoFloorTrace), /*bTraceComplex*/ true, this);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, From, From - FVector(0.f, 0.f, FloorTraceDistance), ECC_Visibility, Params))
	{
		return Hit.ImpactPoint.Z;
	}

	UE_LOG(LogPanoCapture, Warning, TEXT("No floor found below %s; assuming %.0f cm eye height."), *From.ToCompactString(), DefaultEyeHeight);
	return From.Z - DefaultEyeHeight;
}

void APanoCaptureCamera::OrderForCapture(TArray<APanoCapturePoint*>& Points) const
{
	// Explicit Sort Order still wins; within each group, walk a short path (always the nearest remaining point next).
	Points.Sort([](const APanoCapturePoint& A, const APanoCapturePoint& B)
	{
		return A.SortOrder != B.SortOrder ? A.SortOrder < B.SortOrder : A.GetResolvedId() < B.GetResolvedId();
	});

	TArray<APanoCapturePoint*> Ordered;
	Ordered.Reserve(Points.Num());
	FVector Cursor = GetActorLocation();
	for (int32 Start = 0; Start < Points.Num();)
	{
		int32 End = Start;
		while (End < Points.Num() && Points[End]->SortOrder == Points[Start]->SortOrder)
		{
			++End;
		}

		TArray<APanoCapturePoint*> Group(Points.GetData() + Start, End - Start);
		while (!Group.IsEmpty())
		{
			int32 Best = 0;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < Group.Num(); ++Index)
			{
				const double Distance = FVector::DistSquared(Cursor, Group[Index]->GetActorLocation());
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = Index;
				}
			}
			Cursor = Group[Best]->GetActorLocation();
			Ordered.Add(Group[Best]);
			Group.RemoveAt(Best);
		}
		Start = End;
	}
	Points = MoveTemp(Ordered);
}

void APanoCaptureCamera::OverrideBackgroundThrottle(bool bOverride)
{
#if WITH_EDITOR
	if (bOverride && !bThrottleOverridden)
	{
		// Found by name so the runtime module doesn't need to link against UnrealEd.
		UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.EditorPerformanceSettings"));
		ThrottleProperty = SettingsClass ? FindFProperty<FBoolProperty>(SettingsClass, TEXT("bThrottleCPUWhenNotForeground")) : nullptr;
		if (!ThrottleProperty)
		{
			return;
		}
		ThrottleValue = ThrottleProperty->ContainerPtrToValuePtr<void>(SettingsClass->GetDefaultObject());
		bSavedThrottle = ThrottleProperty->GetPropertyValue(ThrottleValue);
		ThrottleProperty->SetPropertyValue(ThrottleValue, false);
		bThrottleOverridden = true;
	}
	else if (!bOverride && bThrottleOverridden)
	{
		// Restored through the pointers found above: this also runs from BeginDestroy, where FindObject is not allowed.
		ThrottleProperty->SetPropertyValue(ThrottleValue, bSavedThrottle);
		bThrottleOverridden = false;
		ThrottleProperty = nullptr;
		ThrottleValue = nullptr;
	}
#endif
}

void APanoCaptureCamera::ClearAutoPoints()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
#if WITH_EDITOR
	const FEditorTransaction Transaction(World, LOCTEXT("ClearAutoPointsTransaction", "Clear Auto Pano Points"));
#endif

	TArray<APanoCapturePoint*> ToRemove;
	for (TActorIterator<APanoCapturePoint> It(World); It; ++It)
	{
		if (It->ActorHasTag(AutoPointTag))
		{
			ToRemove.Add(*It);
		}
	}

	int32 Removed = 0;
	for (APanoCapturePoint* Point : ToRemove)
	{
#if WITH_EDITOR
		// Recorded for undo (actor and level), like deleting it in the viewport.
		const bool bDestroyed = World->IsGameWorld() ? Point->Destroy() : World->EditorDestroyActor(Point, /*bShouldModifyLevel*/ true);
#else
		const bool bDestroyed = Point->Destroy();
#endif
		Removed += bDestroyed ? 1 : 0;
	}
	if (Removed > 0)
	{
		UE_LOG(LogPanoCapture, Log, TEXT("Removed %d auto-placed capture points."), Removed);
	}
}

void APanoCaptureCamera::AutoPlacePoints()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (IsCapturing())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("Wait for the capture to finish before placing points."));
		return;
	}
#if WITH_EDITOR
	// One undo step for clearing the old auto points and placing the new ones, so hand-adjusted points can be brought back.
	const FEditorTransaction Transaction(World, LOCTEXT("AutoPlaceTransaction", "Auto Place Pano Points"));
#endif
	ClearAutoPoints();

	struct FSeed
	{
		FVector Location;
		float FloorZ;
		int32 Floor;
	};

	// Hand-placed points seed the fill (and set the lens height); without any, start under this camera.
	TArray<FSeed> Seeds;
	TArray<float> LensHeights;
	for (TActorIterator<APanoCapturePoint> It(World); It; ++It)
	{
		const FVector Location = It->GetActorLocation();
		const float FloorZ = FindFloorZ(Location);
		Seeds.Add({ Location, FloorZ, It->FloorIndex });
		LensHeights.Add(Location.Z - FloorZ);
	}
	if (Seeds.IsEmpty())
	{
		Seeds.Add({ GetActorLocation(), FindFloorZ(GetActorLocation()), 0 });
	}
	LensHeights.Sort();
	const float Lens = AutoLensHeight > 0.f ? AutoLensHeight : (LensHeights.IsEmpty() ? 150.f : LensHeights[LensHeights.Num() / 2]);

	constexpr float Cell = 50.f;          // flood-fill resolution
	constexpr float MaxStep = 40.f;       // floor height change allowed from the seed's floor (no stairs or tabletops)
	constexpr int32 MaxCells = 60000;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PanoAutoPlace), /*bTraceComplex*/ false, this);
	const float CapsuleHalfHeight = FMath::Max((Lens + 10.f - 40.f) * 0.5f, AutoClearance);

	auto Blocked = [&](const FVector& From, const FVector& To)
	{
		return World->LineTraceTestByChannel(From, To, ECC_Visibility, Params);
	};
	auto Overlaps = [&](const FVector& FloorPoint, float Radius)
	{
		// A standing person: from knee height to just above the lens.
		const FVector Center = FloorPoint + FVector(0.f, 0.f, 40.f + CapsuleHalfHeight);
		return World->OverlapAnyTestByChannel(Center, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeCapsule(Radius, FMath::Max(CapsuleHalfHeight, Radius)), Params);
	};

	struct FCandidate
	{
		FVector FloorPoint;
		int32 Floor;
		int32 Openness;      // 0-2: how much free space around (prefer room centers over corners)
		float PathDistance;  // walking distance from the seed, so points fill outward
	};
	TArray<FCandidate> Candidates;
	TSet<FIntVector> Visited;

	for (const FSeed& Seed : Seeds)
	{
		struct FNode
		{
			FIntPoint Grid;
			FVector FloorPoint;
			float PathDistance;
		};
		TArray<FNode> Queue;
		const FIntPoint StartGrid(FMath::RoundToInt(Seed.Location.X / Cell), FMath::RoundToInt(Seed.Location.Y / Cell));
		Queue.Add({ StartGrid, FVector(StartGrid.X * Cell, StartGrid.Y * Cell, Seed.FloorZ), 0.f });
		Visited.Add(FIntVector(StartGrid.X, StartGrid.Y, Seed.Floor));

		for (int32 Head = 0; Head < Queue.Num() && Visited.Num() < MaxCells; ++Head)
		{
			const FNode Node = Queue[Head];
			static const FIntPoint Steps[] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			for (const FIntPoint& Step : Steps)
			{
				const FIntPoint Grid = Node.Grid + Step;
				const FIntVector Key(Grid.X, Grid.Y, Seed.Floor);
				if (Visited.Contains(Key))
				{
					continue;
				}

				const FVector2D XY(Grid.X * Cell, Grid.Y * Cell);
				if (FVector2D::Distance(XY, FVector2D(Seed.Location)) > AutoSearchRadius)
				{
					continue;
				}

				// Floor under this cell, looking down from below tabletop height so furniture tops don't count.
				FHitResult Hit;
				const FVector Down(XY, Node.FloorPoint.Z + 100.f);
				if (!World->LineTraceSingleByChannel(Hit, Down, Down - FVector(0.f, 0.f, 200.f), ECC_Visibility, Params)
					|| Hit.ImpactNormal.Z < 0.7f || FMath::Abs(Hit.ImpactPoint.Z - Seed.FloorZ) > MaxStep)
				{
					Visited.Add(Key);
					continue;
				}
				const FVector FloorPoint(XY, Hit.ImpactPoint.Z);

				// Walls stop the fill (chest height); furniture too (knee height). Doorways let it through.
				if (Blocked(Node.FloorPoint + FVector(0.f, 0.f, Lens), FloorPoint + FVector(0.f, 0.f, Lens))
					|| Blocked(Node.FloorPoint + FVector(0.f, 0.f, 50.f), FloorPoint + FVector(0.f, 0.f, 50.f)))
				{
					continue; // not visited: it may still be reachable from another side
				}

				Visited.Add(Key);
				const float PathDistance = Node.PathDistance + Cell;
				Queue.Add({ Grid, FloorPoint, PathDistance });

				if (Overlaps(FloorPoint, AutoClearance))
				{
					continue; // walkable to pass through, but too tight to stand a camera
				}
				const int32 Openness = Overlaps(FloorPoint, 100.f) ? 0 : (Overlaps(FloorPoint, 160.f) ? 1 : 2);
				Candidates.Add({ FloorPoint, Seed.Floor, Openness, PathDistance });
			}
		}
	}

	// Poisson-style pick: fill outward from the seeds, keeping Auto Spacing between points. Open spots go first,
	// then tighter ones fill the gaps (corridors, small rooms).
	Candidates.StableSort([](const FCandidate& A, const FCandidate& B) { return A.PathDistance < B.PathDistance; });
	TArray<TPair<FVector, int32>> Taken;
	for (const FSeed& Seed : Seeds)
	{
		Taken.Add({ FVector(Seed.Location.X, Seed.Location.Y, Seed.FloorZ), Seed.Floor });
	}

	TArray<FCandidate> Chosen;
	const float MinDistanceSquared = FMath::Square(AutoSpacing);
	for (int32 MinOpenness = 2; MinOpenness >= 0 && Chosen.Num() < AutoMaxPoints; --MinOpenness)
	{
		for (const FCandidate& Candidate : Candidates)
		{
			if (Chosen.Num() >= AutoMaxPoints)
			{
				break;
			}
			if (Candidate.Openness < MinOpenness)
			{
				continue;
			}
			const bool bTooClose = Taken.ContainsByPredicate([&](const TPair<FVector, int32>& Other)
			{
				return Other.Value == Candidate.Floor && FVector::DistSquaredXY(Other.Key, Candidate.FloorPoint) < MinDistanceSquared;
			});
			if (!bTooClose)
			{
				Taken.Add({ Candidate.FloorPoint, Candidate.Floor });
				Chosen.Add(Candidate);
			}
		}
	}

	if (ULevel* Level = World->GetCurrentLevel(); Level && !Chosen.IsEmpty())
	{
		Level->Modify();
	}
	for (int32 Index = 0; Index < Chosen.Num(); ++Index)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transactional;
		APanoCapturePoint* Point = World->SpawnActor<APanoCapturePoint>(Chosen[Index].FloorPoint + FVector(0.f, 0.f, Lens), FRotator::ZeroRotator, SpawnParams);
		if (!Point)
		{
			continue;
		}
		Point->Tags.Add(AutoPointTag);
		Point->FloorIndex = Chosen[Index].Floor;
#if WITH_EDITOR
		Point->SetActorLabel(FString::Printf(TEXT("AutoPoint_%02d"), Index + 1));
		Point->SetFolderPath(TEXT("PanoCapture/Auto"));
#endif
	}

	UE_LOG(LogPanoCapture, Log, TEXT("Auto Place: %d walkable cells, %d candidates, placed %d points (spacing %.0f cm, lens %.0f cm)."),
		Visited.Num(), Candidates.Num(), Chosen.Num(), AutoSpacing, Lens);

	if (GIsEditor && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("AutoPlaced", "Placed {0} capture points"), Chosen.Num()));
		Info.ExpireDuration = 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void APanoCaptureCamera::AddFloorPlanJobs(TArray<FCaptureJob>& InOutJobs) const
{
	struct FFloorInfo
	{
		TArray<float> FloorZs;
		FBox2D Bounds = FBox2D(ForceInit);
	};

	TSortedMap<int32, FFloorInfo> Floors;
	for (const FCaptureJob& Job : InOutJobs)
	{
		if (Job.Type == EJobType::Pano && !Job.bMeterOnly)
		{
			FFloorInfo& Floor = Floors.FindOrAdd(Job.Floor);
			Floor.FloorZs.Add(Job.FloorZ);
			Floor.Bounds += FVector2D(Job.Location);
		}
	}

	for (TPair<int32, FFloorInfo>& Pair : Floors)
	{
		FFloorInfo& Floor = Pair.Value;
		Floor.FloorZs.Sort();

		FCaptureJob& Job = InOutJobs.AddDefaulted_GetRef();
		Job.Type = EJobType::FloorPlan;
		Job.Id = FString::Printf(TEXT("floor_%d"), Pair.Key);
		Job.Floor = Pair.Key;
		// Median, so one point standing on a stair doesn't move the whole floor.
		Job.FloorZ = Floor.FloorZs[Floor.FloorZs.Num() / 2];
		Job.PlanBounds = Floor.Bounds.ExpandBy(FloorPlanMargin);
	}
}

void APanoCaptureCamera::StartCapture(TArray<FCaptureJob>&& InJobs, bool bInWriteManifest)
{
	if (IsCapturing())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("A pano capture is already running on %s."), *GetName());
		return;
	}
	if (InJobs.IsEmpty())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("Nothing to capture. Place Pano Capture Points in the level first."));
		return;
	}
	if (!GetWorld())
	{
		return;
	}

	// Image encoders are created on worker threads, so the module has to be loaded here first.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	ActiveOutputFolder = GetOutputFolder();
	if (!IFileManager::Get().MakeDirectory(*ActiveOutputFolder, true))
	{
		UE_LOG(LogPanoCapture, Error, TEXT("Could not create output folder %s"), *ActiveOutputFolder);
		return;
	}

	Active.OutputWidth = OutputWidth;
	Active.ImageFormat = ImageFormat;
	Active.JpegQuality = JpegQuality;
	Active.bWritePreviews = bWritePreviews;
	Active.PreviewWidth = PreviewWidth;
	Active.bCaptureDepth = bCaptureDepth;
	Active.DepthWidth = DepthWidth;
	Active.MaxDepth = MaxDepth;
	Active.WarmupFrames = WarmupFrames;
	Active.SettleFrames = SettleFrames;
	Active.Supersampling = Supersampling;
	Active.FaceOverscanDegrees = FaceOverscanDegrees;
	Active.ExposureMode = ExposureMode;
	Active.ExposureBias = ExposureBias;
	Active.bExportSceneMesh = bExportSceneMesh;
	Active.SceneMeshMaxTriangles = SceneMeshMaxTriangles;
	Active.FloorPlanCutHeight = FloorPlanCutHeight;
	Active.FloorPlanMargin = FloorPlanMargin;
	Active.FloorPlanResolution = FloorPlanResolution;
	Active.TourName = GetResolvedTourName();
	ActivePostProcess = PostProcessSettings;
	ActiveHiddenActors = HiddenActors;
	ActiveHiddenActors.AddUnique(this);

	// Writes left over from a cancelled capture that have finished can go; TickCapture waits for the rest.
	OrphanedWrites.RemoveAll([](const TFuture<bool>& Write) { return Write.IsReady(); });

	Jobs = MoveTemp(InJobs);
	PendingWrites.Reset();
	CurrentJob = 0;
	FramesAtCurrentJob = 0;
	bMeteredCurrentJob = false;
	SavedCount = 0;
	SavedPanoCount = 0;
	bTourBiasReady = false;
	SceneMeshTriangles = 0;
	PanoJobCount = Jobs.FilterByPredicate([](const FCaptureJob& Job) { return Job.Type == EJobType::Pano && !Job.bMeterOnly; }).Num();
	LastSavedPath.Reset();
	bWriteManifest = bInWriteManifest;

	PrepareFaces();
	ShowNotification();

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &APanoCaptureCamera::TickCapture));
	OverrideBackgroundThrottle(true);

	UE_LOG(LogPanoCapture, Log, TEXT("Capturing %d panorama(s) at %dx%d (faces %dpx, %.2fx supersampling, metering at %dpx) to %s"),
		PanoJobCount, Active.PanoWidth(), Active.PanoWidth() / 2, FaceSize, Active.Supersampling, MeterFaceSize, *ActiveOutputFolder);
}

void APanoCaptureCamera::ReleaseCaptureState(bool bRemoveTicker)
{
	if (bRemoveTicker && TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	TickerHandle.Reset();
	OverrideBackgroundThrottle(false);

	// Only still open if the capture was cut short; a normal finish closes it first.
	FinishNotification(false, LOCTEXT("Cancelled", "Pano capture cancelled"), FString());
}

void APanoCaptureCamera::StopCapture(bool bRemoveTicker)
{
	ReleaseCaptureState(bRemoveTicker);

	// In-flight saves own their pixel data, so they can finish on their own. They are kept so the next capture
	// can wait for them rather than race them on the same file names.
	for (FPendingWrite& Write : PendingWrites)
	{
		if (!Write.Result.IsReady())
		{
			OrphanedWrites.Add(MoveTemp(Write.Result));
		}
	}
	PendingWrites.Reset();
	Jobs.Reset();
	ActivePostProcess = FPostProcessSettings();
	ActiveHiddenActors.Reset();

	if (LensRoot)
	{
		LensRoot->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
	}
	if (PlanCapture)
	{
		PlanCapture->HiddenComponents.Reset();
		PlanCapture->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
	}
}

void APanoCaptureCamera::PrepareFaces()
{
	const float Fov = 90.f + Active.FaceOverscanDegrees;
	TanHalfFov = FMath::Tan(FMath::DegreesToRadians(Fov * 0.5f));
	// Match the panorama's pixel density at the center of each face, times the supersampling factor.
	const int32 NativeFaceSize = FMath::Clamp(FMath::CeilToInt(Active.OutputWidth / 4.f * TanHalfFov), 64, 8192);
	FaceSize = FMath::Clamp(FMath::CeilToInt(NativeFaceSize * FMath::Max(Active.Supersampling, 1.f)), 64, 8192);

	// When the tour meters in its own sweep, the HDR pass only needs depth and average brightness, so it renders at
	// the depth panorama's density: much faster, and it metered within ~1% of a full-size pass in testing. Otherwise
	// it must match FaceSize so the rendering history (Lumen, TSR) carries over into the color frames.
	const bool bSeparateMeterSweep = Jobs.ContainsByPredicate([](const FCaptureJob& Job) { return Job.bMeterOnly; });
	const int32 DepthFaceSize = FMath::CeilToInt(Active.DepthWidth / 4.f * TanHalfFov);
	MeterFaceSize = bSeparateMeterSweep ? FMath::Clamp(DepthFaceSize, 256, FMath::Min(NativeFaceSize, FaceSize)) : FaceSize;

	auto EnsureTarget = [this](TObjectPtr<UTextureRenderTarget2D>& Target, ETextureRenderTargetFormat Format, int32 Size)
	{
		if (!Target)
		{
			Target = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
			Target->RenderTargetFormat = Format;
			Target->ClearColor = FLinearColor::Black;
		}
		if (Target->SizeX != Size || Target->SizeY != Size)
		{
			Target->InitAutoFormat(Size, Size);
			Target->UpdateResourceImmediate(true);
		}
	};

	FaceTargets.SetNum(Faces.Num());
	MeterTargets.SetNum(Active.NeedsHdrPass() ? Faces.Num() : 0);
	for (int32 Index = 0; Index < Faces.Num(); ++Index)
	{
		EnsureTarget(FaceTargets[Index], RTF_RGBA8, FaceSize);
		if (MeterTargets.IsValidIndex(Index))
		{
			EnsureTarget(MeterTargets[Index], RTF_RGBA16f, MeterFaceSize);
		}

		USceneCaptureComponent2D* Face = Faces[Index];
		Face->FOVAngle = Fov;
		Face->PostProcessBlendWeight = 1.f;
		Face->HiddenActors = ActiveHiddenActors;
	}
}

void APanoCaptureCamera::SetFacesMetering(bool bMetering, float Bias)
{
	const FPostProcessSettings FacePostProcess = BuildFacePostProcess(Bias);
	for (int32 Index = 0; Index < Faces.Num(); ++Index)
	{
		USceneCaptureComponent2D* Face = Faces[Index];
		Face->CaptureSource = bMetering ? ESceneCaptureSource::SCS_SceneColorSceneDepth : ESceneCaptureSource::SCS_FinalColorLDR;
		Face->TextureTarget = bMetering ? MeterTargets[Index] : FaceTargets[Index];
		Face->PostProcessSettings = FacePostProcess;
	}
}

FPostProcessSettings APanoCaptureCamera::BuildFacePostProcess(float Bias) const
{
	FPostProcessSettings Settings = ActivePostProcess;

	Settings.bOverride_VignetteIntensity = true;
	Settings.VignetteIntensity = 0.f;
	Settings.bOverride_FilmGrainIntensity = true;
	Settings.FilmGrainIntensity = 0.f;
	Settings.bOverride_SceneFringeIntensity = true;
	Settings.SceneFringeIntensity = 0.f;
	Settings.bOverride_LensFlareIntensity = true;
	Settings.LensFlareIntensity = 0.f;
	Settings.bOverride_MotionBlurAmount = true;
	Settings.MotionBlurAmount = 0.f;

	// Every face gets the same fixed exposure, otherwise each would adapt on its own and the seams would show.
	Settings.bOverride_AutoExposureMethod = true;
	Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Settings.AutoExposureApplyPhysicalCameraExposure = 0;
	Settings.bOverride_AutoExposureBias = true;
	Settings.AutoExposureBias = Bias;

	return Settings;
}

bool APanoCaptureCamera::TickCapture(float DeltaSeconds)
{
	if (!OrphanedWrites.IsEmpty())
	{
		// A cancelled capture is still writing; starting now could overwrite new files with its old ones.
		OrphanedWrites.RemoveAll([](const TFuture<bool>& Write) { return Write.IsReady(); });
		if (!OrphanedWrites.IsEmpty())
		{
			return true;
		}
	}

	if (Jobs.IsValidIndex(CurrentJob))
	{
		FCaptureJob& Job = Jobs[CurrentJob];
		if (Job.Type == EJobType::Pano)
		{
			TickPanoJob(Job);
		}
		else
		{
			TickFloorPlanJob(Job);
		}
	}

	PollPendingWrites();
	if (!IsCapturing())
	{
		// A listener cancelled the capture.
		return false;
	}

	if (CurrentJob >= Jobs.Num() && PendingWrites.IsEmpty())
	{
		if (bWriteManifest)
		{
			if (Active.bExportSceneMesh)
			{
				ExportSceneMesh();
			}
			WriteManifest();
			CopyViewer();
		}

		const bool bAllSaved = SavedCount == Jobs.Num();
		if (!bWriteManifest && bAllSaved)
		{
			FinishNotification(true, LOCTEXT("PanoSaved", "Panorama saved"), LastSavedPath);
		}
		else if (bAllSaved)
		{
			FinishNotification(true, FText::Format(LOCTEXT("TourSaved", "Tour saved ({0} panoramas)"), PanoJobCount), ActiveOutputFolder / TEXT("tour.json"));
		}
		else
		{
			FinishNotification(false, FText::Format(LOCTEXT("SomeFailed", "Saved {0} of {1} images, see the Output Log"), SavedCount, Jobs.Num()),
				SavedCount > 0 ? LastSavedPath : FString());
		}

		const FString Folder = ActiveOutputFolder;
		StopCapture(false);
		UE_LOG(LogPanoCapture, Log, TEXT("Pano capture finished: %s"), *Folder);
		OnCaptureFinished.Broadcast(Folder);
		return false;
	}

	return true;
}

void APanoCaptureCamera::TickPanoJob(FCaptureJob& Job)
{
	// Tour exposure, second sweep: exposure and depth are already known, so render the final look straight away.
	const bool bHdrPass = Active.NeedsHdrPass() && !Job.bUseTourBias;

	if (FramesAtCurrentJob == 0 && Job.bUseTourBias)
	{
		LensRoot->SetWorldLocationAndRotation(Job.Location, FRotator(0.f, Job.Heading, 0.f));
		Job.FaceBias = ComputeTourBias();
		PendingDepthFaces.Reset();
		SetFacesMetering(false, Job.FaceBias);
	}
	else if (FramesAtCurrentJob == 0)
	{
		// Always level: pitch and roll would tilt the horizon in the viewer.
		LensRoot->SetWorldLocationAndRotation(Job.Location, FRotator(0.f, Job.Heading, 0.f));
		bMeteredCurrentJob = false;
		Job.FaceBias = Active.ExposureBias;
		PendingDepthFaces.Reset();
		// Warm up in linear HDR (depth in alpha) at exposure 1 so the scene can be metered, then switch to the final look.
		SetFacesMetering(bHdrPass, bHdrPass ? PanoExposure::GetMeteringBias() : Active.ExposureBias);
	}

	IStreamingManager::Get().AddViewLocation(Job.Location);
	for (USceneCaptureComponent2D* Face : Faces)
	{
		Face->CaptureScene();
	}
	++FramesAtCurrentJob;

	if (bHdrPass && !bMeteredCurrentJob && FramesAtCurrentJob >= Active.WarmupFrames)
	{
		if (Job.bMeterOnly && PendingWrites.Num() >= MaxPendingWrites)
		{
			return; // wait for a free write slot before reading back
		}
		ReadHdrPass(Job);
		bMeteredCurrentJob = true;
		if (Job.bMeterOnly)
		{
			SaveDepthOnly(Job);
			++CurrentJob;
			FramesAtCurrentJob = 0;
			return;
		}
		SetFacesMetering(false, Job.FaceBias);
	}

	const int32 FramesNeeded = Active.WarmupFrames + (bHdrPass ? Active.SettleFrames : 0);
	if (FramesAtCurrentJob >= FramesNeeded && PendingWrites.Num() < MaxPendingWrites)
	{
		ReadBackAndSavePano(Job);
		++CurrentJob;
		FramesAtCurrentJob = 0;
	}
}

void APanoCaptureCamera::ReadHdrPass(FCaptureJob& Job)
{
	TArray<TArray<FFloat16Color>> HdrFaces;
	HdrFaces.SetNum(MeterTargets.Num());
	for (int32 Index = 0; Index < MeterTargets.Num(); ++Index)
	{
		FTextureRenderTargetResource* Resource = MeterTargets[Index]->GameThread_GetRenderTargetResource();
		if (!Resource || !Resource->ReadFloat16Pixels(HdrFaces[Index]) || HdrFaces[Index].Num() != MeterFaceSize * MeterFaceSize)
		{
			UE_LOG(LogPanoCapture, Warning, TEXT("Could not read the HDR pass for face %d of %s; using Exposure Bias and no depth."), Index, *Job.Id);
			Job.FaceBias = Active.ExposureBias;
			Job.bMetered = false;
			return;
		}
	}

	if (Active.ExposureMode != EPanoExposureMode::Manual)
	{
		const FPanoExposureSettings Settings = PanoExposure::GetSettingsAt(GetWorld(), Job.Location);
		const float AverageLuminance = PanoExposure::MeterAverageLuminance(HdrFaces, MeterFaceSize, TanHalfFov, Settings);
		Job.FaceBias = PanoExposure::ComputeManualBias(Settings, AverageLuminance, Active.ExposureBias);
		Job.bMetered = true;

		UE_LOG(LogPanoCapture, Log, TEXT("%s: average luminance %.4f, level bias %.2f%s, exposure bias %.2f EV"),
			*Job.Id, AverageLuminance, Settings.Bias, Settings.bManual ? TEXT(" (manual)") : TEXT(""), Job.FaceBias);
	}

	if (Active.bCaptureDepth)
	{
		// Nearest-sample the depth down to what the depth panorama needs; blending depth would invent surfaces at edges.
		const int32 DepthFaceSize = FMath::Clamp(FMath::CeilToInt(Active.DepthWidth / 4.f * TanHalfFov), 16, MeterFaceSize);
		PendingDepthFaces.SetNum(Faces.Num());
		for (int32 Index = 0; Index < Faces.Num(); ++Index)
		{
			const FQuat Rotation = Faces[Index]->GetRelativeRotation().Quaternion();
			FPanoCubeFace& DepthFace = PendingDepthFaces[Index];
			DepthFace.Forward = FVector3f(Rotation.GetForwardVector());
			DepthFace.Right = FVector3f(Rotation.GetRightVector());
			DepthFace.Up = FVector3f(Rotation.GetUpVector());
			DepthFace.Size = DepthFaceSize;
			DepthFace.Depth.SetNumUninitialized(DepthFaceSize * DepthFaceSize);

			const TArray<FFloat16Color>& Hdr = HdrFaces[Index];
			for (int32 Y = 0; Y < DepthFaceSize; ++Y)
			{
				const int32 SrcY = FMath::Min((int32)((Y + 0.5f) * MeterFaceSize / DepthFaceSize), MeterFaceSize - 1);
				for (int32 X = 0; X < DepthFaceSize; ++X)
				{
					const int32 SrcX = FMath::Min((int32)((X + 0.5f) * MeterFaceSize / DepthFaceSize), MeterFaceSize - 1);
					DepthFace.Depth[Y * DepthFaceSize + X] = Hdr[SrcY * MeterFaceSize + SrcX].A.GetFloat();
				}
			}
		}

		// Straight down should read the lens height: a quick sanity check in the log.
		const FPanoCubeFace& Down = PendingDepthFaces.Last();
		UE_LOG(LogPanoCapture, Log, TEXT("%s: depth straight down %.1f cm (lens %.1f cm above the traced floor)"),
			*Job.Id, Down.Depth[(DepthFaceSize / 2) * DepthFaceSize + DepthFaceSize / 2], Job.Location.Z - Job.FloorZ);
	}
}

void APanoCaptureCamera::SaveDepthOnly(const FCaptureJob& Job)
{
	UpdateNotification();
	if (PendingDepthFaces.IsEmpty())
	{
		// Nothing to write (depth off, or the read failed); still count the job as done.
		Jobs[CurrentJob].bSaved = true;
		++SavedCount;
		return;
	}

	const FString DepthPath = ActiveOutputFolder / Job.Id + TEXT("_depth.png");
	const int32 DepthW = Active.DepthPanoWidth();
	const float MaxDistance = Active.MaxDepth;
	const float Tan = TanHalfFov;

	FPendingWrite& Write = PendingWrites.AddDefaulted_GetRef();
	Write.JobIndex = CurrentJob;
	Write.Path = DepthPath;
	Write.Result = Async(EAsyncExecution::ThreadPool, [DepthFaces = MoveTemp(PendingDepthFaces), DepthPath, DepthW, MaxDistance, Tan]()
	{
		TArray<float> Distance;
		PanoStitcher::CubeDepthToEquirect(DepthFaces, Tan, DepthW, Distance);
		TArray<FColor> Encoded;
		PanoStitcher::EncodeDepth(Distance, MaxDistance, Encoded);
		return FImageUtils::SaveImageByExtension(*DepthPath, FImageView(Encoded.GetData(), DepthW, DepthW / 2, EGammaSpace::Linear));
	});
	PendingDepthFaces.Reset();
}

float APanoCaptureCamera::ComputeTourBias()
{
	if (!bTourBiasReady)
	{
		// Average in EV (log space), like averaging the scene brightness of every point. Spots whose read failed only
		// carry the user bias, not a measurement, so they would pull the average towards it.
		double Sum = 0.0;
		int32 Count = 0;
		for (const FCaptureJob& Job : Jobs)
		{
			if (Job.bMeterOnly && Job.Type == EJobType::Pano && Job.bMetered)
			{
				Sum += Job.FaceBias;
				++Count;
			}
		}
		TourBias = Count > 0 ? float(Sum / Count) : Active.ExposureBias;
		bTourBiasReady = true;
		UE_LOG(LogPanoCapture, Log, TEXT("Tour exposure: %.2f EV (average of %d points)"), TourBias, Count);
	}
	return TourBias;
}

void APanoCaptureCamera::ExportSceneMesh()
{
	FBox Bounds(ForceInit);
	for (const FCaptureJob& Job : Jobs)
	{
		if (Job.Type == EJobType::Pano && !Job.bMeterOnly)
		{
			Bounds += Job.Location;
			Bounds += FVector(Job.Location.X, Job.Location.Y, Job.FloorZ);
		}
	}
	if (!Bounds.IsValid)
	{
		return;
	}
	// Room for the walls and furniture around the outer points, and a floor above and below.
	Bounds = Bounds.ExpandBy(FVector(Active.FloorPlanMargin + 1000.f, Active.FloorPlanMargin + 1000.f, 600.f));

	TArray<const AActor*> Ignore;
	for (const AActor* Actor : ActiveHiddenActors)
	{
		Ignore.Add(Actor);
	}

	const PanoSceneExport::FResult Result = PanoSceneExport::ExportStaticGeometry(GetWorld(), Bounds, Ignore,
		Active.SceneMeshMaxTriangles, /*MaxComponentSize*/ 50000.f, ActiveOutputFolder / TEXT("scene.glb"));
	SceneMeshTriangles = Result.bSuccess ? Result.Triangles : 0;
}

void APanoCaptureCamera::ReadBackAndSavePano(const FCaptureJob& Job)
{
	TArray<FPanoCubeFace> CubeFaces;
	CubeFaces.SetNum(Faces.Num());
	for (int32 Index = 0; Index < Faces.Num(); ++Index)
	{
		const FQuat Rotation = Faces[Index]->GetRelativeRotation().Quaternion();
		FPanoCubeFace& CubeFace = CubeFaces[Index];
		CubeFace.Forward = FVector3f(Rotation.GetForwardVector());
		CubeFace.Right = FVector3f(Rotation.GetRightVector());
		CubeFace.Up = FVector3f(Rotation.GetUpVector());
		CubeFace.Size = FaceSize;

		FTextureRenderTargetResource* Resource = FaceTargets[Index]->GameThread_GetRenderTargetResource();
		if (!Resource || !Resource->ReadPixels(CubeFace.Pixels) || CubeFace.Pixels.Num() != FaceSize * FaceSize)
		{
			UE_LOG(LogPanoCapture, Error, TEXT("Could not read back face %d for %s"), Index, *Job.Id);
			return;
		}
	}

	const FString Path = ActiveOutputFolder / Job.Id + Active.ImageExtension();
	const FString PreviewPath = Active.bWritePreviews ? ActiveOutputFolder / Job.Id + TEXT("_preview.jpg") : FString();
	const FString MediumPath = Active.WritesMedium() ? ActiveOutputFolder / Job.Id + TEXT("_4k.jpg") : FString();
	const int32 SamplesPerAxis = Active.Supersampling > 1.2f ? 2 : 1;
	const FString DepthPath = ActiveOutputFolder / Job.Id + TEXT("_depth.png");
	const int32 DepthW = Active.DepthPanoWidth();
	const float MaxDistance = Active.MaxDepth;
	TArray<FPanoCubeFace> DepthFaces = MoveTemp(PendingDepthFaces);
	PendingDepthFaces.Reset();
	const int32 Width = Active.PanoWidth();
	const int32 PreviewW = FMath::Min(Active.PreviewWidth, Width) & ~1;
	const float Tan = TanHalfFov;
	const int32 Quality = Active.ImageFormat == EPanoImageFormat::JPG ? Active.JpegQuality : 0;

	FPendingWrite& Write = PendingWrites.AddDefaulted_GetRef();
	Write.JobIndex = CurrentJob;
	Write.Path = Path;
	Write.Result = Async(EAsyncExecution::ThreadPool, [CubeFaces = MoveTemp(CubeFaces), DepthFaces = MoveTemp(DepthFaces), Path, PreviewPath, MediumPath, DepthPath, Width, PreviewW, DepthW, MaxDistance, Tan, Quality, SamplesPerAxis]() mutable
	{
		TArray<FColor> Pano;
		PanoStitcher::CubeToEquirect(CubeFaces, Tan, Width, Pano, SamplesPerAxis);
		CubeFaces.Empty(); // the faces are the biggest allocation; free them before encoding
		if (!FImageUtils::SaveImageByExtension(*Path, FImageView(Pano.GetData(), Width, Width / 2, EGammaSpace::sRGB), Quality))
		{
			return false;
		}

		if (!PreviewPath.IsEmpty())
		{
			TArray<FColor> Preview;
			FImageUtils::ImageResize(Width, Width / 2, Pano, PreviewW, PreviewW / 2, Preview, /*bResizeSRGBinLinearSpace*/ false);
			FImageUtils::SaveImageByExtension(*PreviewPath, FImageView(Preview.GetData(), PreviewW, PreviewW / 2, EGammaSpace::sRGB), 85);
		}

		if (!MediumPath.IsEmpty())
		{
			TArray<FColor> Medium;
			FImageUtils::ImageResize(Width, Width / 2, Pano, 4096, 2048, Medium, /*bResizeSRGBinLinearSpace*/ false);
			FImageUtils::SaveImageByExtension(*MediumPath, FImageView(Medium.GetData(), 4096, 2048, EGammaSpace::sRGB), FMath::Max(Quality, 88));
		}

		if (!DepthFaces.IsEmpty())
		{
			TArray<float> Distance;
			PanoStitcher::CubeDepthToEquirect(DepthFaces, Tan, DepthW, Distance);
			TArray<FColor> Encoded;
			PanoStitcher::EncodeDepth(Distance, MaxDistance, Encoded);
			// Linear gamma: these bytes are numbers, not colors.
			if (!FImageUtils::SaveImageByExtension(*DepthPath, FImageView(Encoded.GetData(), DepthW, DepthW / 2, EGammaSpace::Linear)))
			{
				UE_LOG(LogPanoCapture, Error, TEXT("Failed to write %s"), *DepthPath);
			}
		}
		return true;
	});
}

void APanoCaptureCamera::TickFloorPlanJob(FCaptureJob& Job)
{
	if (FramesAtCurrentJob == 0)
	{
		PrepareFloorPlan(Job);
	}

	IStreamingManager::Get().AddViewLocation(PlanCapture->GetComponentLocation());
	PlanCapture->CaptureScene();
	++FramesAtCurrentJob;

	if (FramesAtCurrentJob >= FloorPlanFrames && PendingWrites.Num() < MaxPendingWrites)
	{
		ReadBackAndSaveFloorPlan(Job);
		++CurrentJob;
		FramesAtCurrentJob = 0;
	}
}

void APanoCaptureCamera::PrepareFloorPlan(const FCaptureJob& Job)
{
	const FVector2D Size = Job.PlanBounds.GetSize();
	const FVector2D Center = Job.PlanBounds.GetCenter();
	const float CutZ = Job.FloorZ + Active.FloorPlanCutHeight;

	// Looking straight down with yaw 0, image right is world +Y and image up is world +X.
	const float ImageWorldWidth = Size.Y;
	const float ImageWorldHeight = Size.X;
	const float Scale = Active.FloorPlanResolution / FMath::Max(ImageWorldWidth, ImageWorldHeight);
	const int32 PixelWidth = FMath::Max(16, FMath::RoundToInt(ImageWorldWidth * Scale));
	const int32 PixelHeight = FMath::Max(16, FMath::RoundToInt(ImageWorldHeight * Scale));

	if (!PlanTarget)
	{
		PlanTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		PlanTarget->RenderTargetFormat = RTF_RGBA16f;
		PlanTarget->ClearColor = FLinearColor::Black;
	}
	PlanTarget->InitAutoFormat(PixelWidth, PixelHeight);
	PlanTarget->UpdateResourceImmediate(true);

	PlanCapture->TextureTarget = PlanTarget;
	PlanCapture->OrthoWidth = ImageWorldWidth;
	PlanCapture->SetWorldLocationAndRotation(FVector(Center.X, Center.Y, CutZ), FRotator(-90.f, 0.f, 0.f));

	PlanCapture->HiddenActors = ActiveHiddenActors;

	// Cut the building open: leave out everything that starts above the cut height (ceilings, upper floors, roofs).
	// Whole components only, so a component that spans the cut (a house modeled as one mesh) stays, roof included.
	PlanCapture->HiddenComponents.Reset();
	const FBox PlanBox(FVector(Job.PlanBounds.Min, -UE_BIG_NUMBER), FVector(Job.PlanBounds.Max, UE_BIG_NUMBER));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		It->ForEachComponent<UPrimitiveComponent>(false, [&](UPrimitiveComponent* Primitive)
		{
			if (Primitive->IsRegistered() && Primitive->Bounds.GetBox().Min.Z > CutZ && Primitive->Bounds.GetBox().Intersect(PlanBox))
			{
				PlanCapture->HiddenComponents.Add(Primitive);
			}
		});
	}

	UE_LOG(LogPanoCapture, Log, TEXT("Floor %d plan: %dx%d px, %.0f x %.0f cm, floor Z %.0f, hiding %d components above Z %.0f"),
		Job.Floor, PixelWidth, PixelHeight, ImageWorldWidth, ImageWorldHeight, Job.FloorZ, PlanCapture->HiddenComponents.Num(), CutZ);
}

void APanoCaptureCamera::ReadBackAndSaveFloorPlan(const FCaptureJob& Job)
{
	TArray<FFloat16Color> Linear;
	FTextureRenderTargetResource* Resource = PlanTarget->GameThread_GetRenderTargetResource();
	if (!Resource || !Resource->ReadFloat16Pixels(Linear))
	{
		UE_LOG(LogPanoCapture, Error, TEXT("Could not read back the plan for floor %d"), Job.Floor);
		return;
	}

	const int32 Width = PlanTarget->SizeX;
	const int32 Height = PlanTarget->SizeY;
	const FString Path = ActiveOutputFolder / Job.Id + TEXT(".png");

	FPendingWrite& Write = PendingWrites.AddDefaulted_GetRef();
	Write.JobIndex = CurrentJob;
	Write.Path = Path;
	Write.Result = Async(EAsyncExecution::ThreadPool, [Linear = MoveTemp(Linear), Path, Width, Height]()
	{
		TArray<FColor> Pixels;
		ConvertToColor(Linear, Pixels, /*bLinearToSRGB*/ true);
		return FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), Width, Height, EGammaSpace::sRGB));
	});
}

void APanoCaptureCamera::PollPendingWrites()
{
	for (int32 Index = 0; Index < PendingWrites.Num();)
	{
		if (!PendingWrites[Index].Result.IsReady())
		{
			++Index;
			continue;
		}

		FPendingWrite Write = MoveTemp(PendingWrites[Index]);
		PendingWrites.RemoveAt(Index);

		if (!Write.Result.Get())
		{
			UE_LOG(LogPanoCapture, Error, TEXT("Failed to write %s"), *Write.Path);
			continue;
		}

		FCaptureJob& Job = Jobs[Write.JobIndex];
		Job.bSaved = true;
		++SavedCount;
		UE_LOG(LogPanoCapture, Log, TEXT("Saved %s"), *Write.Path);

		if (Job.Type == EJobType::Pano && !Job.bMeterOnly)
		{
			++SavedPanoCount;
			LastSavedPath = Write.Path;
			UpdateNotification();
			OnPanoCaptured.Broadcast(Job.Id, Write.Path);
			if (!IsCapturing())
			{
				return;
			}
		}
	}
}

void APanoCaptureCamera::WriteManifest() const
{
	TMap<const APanoCapturePoint*, FString> IdsByPoint;
	for (const FCaptureJob& Job : Jobs)
	{
		if (Job.Type == EJobType::Pano && !Job.bMeterOnly && Job.bSaved && Job.Point.IsValid())
		{
			IdsByPoint.Add(Job.Point.Get(), Job.Id);
		}
	}

	auto MakeVector = [](const FVector& V)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), V.X);
		Object->SetNumberField(TEXT("y"), V.Y);
		Object->SetNumberField(TEXT("z"), V.Z);
		return Object;
	};

	TArray<TSharedPtr<FJsonValue>> PointValues;
	TArray<TSharedPtr<FJsonValue>> FloorValues;
	for (const FCaptureJob& Job : Jobs)
	{
		if (!Job.bSaved || Job.bMeterOnly)
		{
			continue;
		}

		if (Job.Type == EJobType::FloorPlan)
		{
			TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
			Plan->SetStringField(TEXT("image"), Job.Id + TEXT(".png"));
			Plan->SetNumberField(TEXT("minX"), Job.PlanBounds.Min.X);
			Plan->SetNumberField(TEXT("maxX"), Job.PlanBounds.Max.X);
			Plan->SetNumberField(TEXT("minY"), Job.PlanBounds.Min.Y);
			Plan->SetNumberField(TEXT("maxY"), Job.PlanBounds.Max.Y);

			TSharedRef<FJsonObject> Floor = MakeShared<FJsonObject>();
			Floor->SetNumberField(TEXT("index"), Job.Floor);
			Floor->SetStringField(TEXT("name"), FString::Printf(TEXT("Floor %d"), Job.Floor + 1));
			Floor->SetNumberField(TEXT("z"), Job.FloorZ);
			Floor->SetObjectField(TEXT("plan"), Plan);
			FloorValues.Add(MakeShared<FJsonValueObject>(Floor));
			continue;
		}

		TSharedRef<FJsonObject> PointObject = MakeShared<FJsonObject>();
		PointObject->SetStringField(TEXT("id"), Job.Id);
		PointObject->SetStringField(TEXT("image"), Job.Id + Active.ImageExtension());
		if (Active.bWritePreviews)
		{
			PointObject->SetStringField(TEXT("preview"), Job.Id + TEXT("_preview.jpg"));
			if (Active.WritesMedium())
			{
				PointObject->SetStringField(TEXT("medium"), Job.Id + TEXT("_4k.jpg"));
			}
		}
		if (Active.bCaptureDepth)
		{
			PointObject->SetStringField(TEXT("depth"), Job.Id + TEXT("_depth.png"));
		}
		PointObject->SetObjectField(TEXT("position"), MakeVector(Job.Location));
		PointObject->SetNumberField(TEXT("floorZ"), Job.FloorZ);
		PointObject->SetNumberField(TEXT("heading"), Job.Heading);
		PointObject->SetNumberField(TEXT("floor"), Job.Floor);
		PointObject->SetNumberField(TEXT("exposureBias"), Job.FaceBias);

		TArray<TSharedPtr<FJsonValue>> NeighborValues;
		if (const APanoCapturePoint* Point = Job.Point.Get())
		{
			for (const APanoCapturePoint* Neighbor : Point->Neighbors)
			{
				if (const FString* NeighborId = IdsByPoint.Find(Neighbor))
				{
					NeighborValues.Add(MakeShared<FJsonValueString>(*NeighborId));
				}
			}
		}
		PointObject->SetArrayField(TEXT("neighbors"), NeighborValues);

		PointValues.Add(MakeShared<FJsonValueObject>(PointObject));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 2);
	Root->SetStringField(TEXT("name"), Active.TourName);
	Root->SetStringField(TEXT("projection"), TEXT("equirectangular"));
	Root->SetNumberField(TEXT("width"), Active.PanoWidth());
	Root->SetNumberField(TEXT("height"), Active.PanoWidth() / 2);
	Root->SetStringField(TEXT("coordinateSystem"), TEXT("Unreal: X forward, Y right, Z up, centimeters. heading is degrees around Z; 0 means the image center faces +X. Plan images have +Y to the right and +X up."));
	if (Active.bCaptureDepth)
	{
		TSharedRef<FJsonObject> Depth = MakeShared<FJsonObject>();
		Depth->SetNumberField(TEXT("width"), Active.DepthPanoWidth());
		Depth->SetNumberField(TEXT("height"), Active.DepthPanoWidth() / 2);
		Depth->SetStringField(TEXT("encoding"), TEXT("rgb24-mm"));
		Depth->SetStringField(TEXT("description"), TEXT("Distance from the lens along each ray in millimeters: R * 65536 + G * 256 + B. 0 means no surface."));
		Root->SetObjectField(TEXT("depth"), Depth);
	}
	if (SceneMeshTriangles > 0)
	{
		TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
		Mesh->SetStringField(TEXT("file"), TEXT("scene.glb"));
		Mesh->SetNumberField(TEXT("triangles"), SceneMeshTriangles);
		Mesh->SetStringField(TEXT("description"), TEXT("Static geometry around the tour, positions only, glTF space (meters, Y up): Unreal (x, y, z) cm -> (y, z, -x) / 100."));
		Root->SetObjectField(TEXT("mesh"), Mesh);
	}
	if (Active.ExposureMode == EPanoExposureMode::Tour && bTourBiasReady)
	{
		Root->SetNumberField(TEXT("exposureBias"), TourBias);
	}
	Root->SetArrayField(TEXT("floors"), FloorValues);
	Root->SetArrayField(TEXT("points"), PointValues);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);

	const FString ManifestPath = ActiveOutputFolder / TEXT("tour.json");
	if (FFileHelper::SaveStringToFile(Json, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogPanoCapture, Log, TEXT("Wrote %s (%d points, %d floors)"), *ManifestPath, PointValues.Num(), FloorValues.Num());
	}
	else
	{
		UE_LOG(LogPanoCapture, Error, TEXT("Failed to write %s"), *ManifestPath);
	}
}

void APanoCaptureCamera::CopyViewer() const
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PanoCapture"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString SourceDir = Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Viewer");
	const FString DestDir = ActiveOutputFolder.IsEmpty() ? GetOutputFolder() : ActiveOutputFolder;
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*"), /*Files*/ true, /*Directories*/ false);
	for (const FString& File : Files)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *(SourceDir + TEXT("/")));
		IFileManager::Get().Copy(*(DestDir / Relative), *File, /*Replace*/ true);
	}
}

void APanoCaptureCamera::ShowNotification()
{
	// Toasts are for the editor only; a packaged configurator shouldn't pop Slate notifications over the game.
	if (!GIsEditor || !FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(FText::GetEmpty());
	Info.bFireAndForget = false;
	Info.bUseThrobber = true;
	Info.bUseSuccessFailIcons = true;
	Info.ExpireDuration = 10.f;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("CancelButton", "Cancel"),
		LOCTEXT("CancelTooltip", "Stop capturing"),
		FSimpleDelegate::CreateUObject(this, &APanoCaptureCamera::CancelCapture),
		SNotificationItem::CS_Pending));

	if (bWriteManifest)
	{
		const FString Folder = ActiveOutputFolder;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("OpenViewerButton", "Open viewer"),
			LOCTEXT("OpenViewerTooltip", "Open the tour in your browser"),
			FSimpleDelegate::CreateLambda([Folder]()
			{
				const FString Url = PanoViewerServer::GetViewerUrl(Folder);
				if (!Url.IsEmpty())
				{
					FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
				}
			}),
			SNotificationItem::CS_Success));
	}

	Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Pending);
		UpdateNotification();
	}
}

void APanoCaptureCamera::UpdateNotification()
{
	if (!Notification.IsValid())
	{
		return;
	}

	Notification->SetText(bWriteManifest
		? (Jobs.IsValidIndex(CurrentJob) && Jobs[CurrentJob].bMeterOnly
			? FText::Format(LOCTEXT("MeteringTour", "Metering exposure: {0} of {1}"), FMath::Min(CurrentJob + 1, PanoJobCount), PanoJobCount)
			: FText::Format(LOCTEXT("CapturingTour", "Capturing tour: {0} of {1}"), FMath::Min(SavedPanoCount, PanoJobCount), PanoJobCount))
		: LOCTEXT("CapturingPano", "Capturing panorama..."));
}

void APanoCaptureCamera::FinishNotification(bool bSuccess, const FText& Text, const FString& LinkPath)
{
	if (!Notification.IsValid())
	{
		return;
	}
	if (!FSlateApplication::IsInitialized())
	{
		// Editor shutting down with a capture running (reached from BeginDestroy): the toast is going away anyway.
		Notification.Reset();
		return;
	}

	Notification->SetText(Text);
	if (!LinkPath.IsEmpty())
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(LinkPath);
		// Opens Explorer with the file selected, like the high resolution screenshot toast.
		Notification->SetHyperlink(
			FSimpleDelegate::CreateLambda([FullPath]() { FPlatformProcess::ExploreFolder(*FullPath); }),
			FText::FromString(FullPath));
	}
	Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	Notification->ExpireAndFadeout();
	Notification.Reset();
}

#undef LOCTEXT_NAMESPACE

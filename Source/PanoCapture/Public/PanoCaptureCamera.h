// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Engine/Scene.h"
#include "PanoStitcher.h"
#include "PanoCaptureCamera.generated.h"

class APanoCapturePoint;
class FBoolProperty;
class SNotificationItem;
class UArrowComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

UENUM(BlueprintType)
enum class EPanoImageFormat : uint8
{
	JPG,
	PNG
};

UENUM(BlueprintType)
enum class EPanoExposureMode : uint8
{
	/**
	 * Meter every point first, then capture them all with one exposure (the average), so walking between
	 * points never changes brightness. Costs one extra warm-up per point. A single Capture Here behaves like Auto.
	 */
	Tour,
	/** Meter each spot the way the viewport's auto exposure would (including post process volumes), then use that one exposure for all six faces. */
	Auto,
	/** Same fixed exposure everywhere: 2^ExposureBias. Useful for keeping every room of a tour identical. */
	Manual
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPanoCaptured, const FString&, PointId, const FString&, ImagePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPanoCaptureFinished, const FString&, OutputFolder);

/**
 * Renders 360 equirectangular panoramas and virtual tours. Six scene captures (one per cube face, with a little
 * overscan to hide seams) are rendered for a number of warm-up frames so Lumen, shadows and streaming can settle,
 * then stitched into a 2:1 image on a worker thread.
 *
 * A tour captures every Pano Capture Point, a top-down floor plan per floor, and writes tour.json plus a web viewer.
 * Works in the editor (buttons in the details panel) and at runtime from Blueprint.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Pano 360 Camera"))
class PANOCAPTURE_API APanoCaptureCamera : public AActor
{
	GENERATED_BODY()

public:
	APanoCaptureCamera();

	/** Width of the panorama in pixels. Height is half of this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", meta = (ClampMin = 512, ClampMax = 16384))
	int32 OutputWidth = 8192;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	EPanoImageFormat ImageFormat = EPanoImageFormat::JPG;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", meta = (ClampMin = 1, ClampMax = 100, EditCondition = "ImageFormat == EPanoImageFormat::JPG"))
	int32 JpegQuality = 92;

	/**
	 * Also write smaller copies: <id>_preview.jpg (Preview Width, for the dollhouse and quick loads) and <id>_4k.jpg
	 * (for walking between points), so the viewer only needs the full image for the spot you are standing on.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	bool bWritePreviews = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", meta = (ClampMin = 256, ClampMax = 4096, EditCondition = "bWritePreviews"))
	int32 PreviewWidth = 2048;

	/**
	 * Also write a depth panorama (<id>_depth.png, distance per pixel) so the viewer can build 3D:
	 * the dollhouse view, walk transitions through real geometry, and a cursor that sticks to walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	bool bCaptureDepth = true;

	/** Width of the depth panorama. The viewer turns it into a mesh, so it doesn't need the color resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", meta = (ClampMin = 256, ClampMax = 4096, EditCondition = "bCaptureDepth"))
	int32 DepthWidth = 2048;

	/** Surfaces farther than this are stored as "no surface" (sky). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", AdvancedDisplay, meta = (EditCondition = "bCaptureDepth", Units = "cm"))
	float MaxDepth = 100000.f;

	/** Where images and tour.json go. Relative paths are under the project's Saved folder. Empty means Saved/PanoCaptures. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	FString OutputDirectory;

	/** Subfolder and display name for this set of captures. Empty uses the map name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	FString TourName;

	/** Frames rendered at each spot before metering and saving, so Lumen, virtual shadow maps and texture streaming can settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Quality", meta = (ClampMin = 1, ClampMax = 1024))
	int32 WarmupFrames = 32;

	/** Frames rendered after the exposure is set, so anti-aliasing history catches up. Only used with Auto exposure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Quality", AdvancedDisplay, meta = (ClampMin = 1, ClampMax = 256))
	int32 SettleFrames = 12;

	/**
	 * Renders the cube faces this much larger than the panorama needs, then filters them down (2x2 samples per pixel).
	 * Capture time and GPU memory grow with the square of this. Caution: in UE 5.6, scene captures much above the
	 * native size (faces over ~3000 px) lost their Lumen reflections in testing, so check the result before relying on it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Quality", AdvancedDisplay, meta = (ClampMin = 1, ClampMax = 2, UIMin = 1, UIMax = 2))
	float Supersampling = 1.f;

	/** Extra field of view per cube face, in degrees, cropped away when stitching. Hides screen-space artifacts (reflections, AO, bloom) at the seams. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Quality", meta = (ClampMin = 0, ClampMax = 45))
	float FaceOverscanDegrees = 10.f;

	/** Turn the panorama with each capture point's (or this camera's) yaw. Off means every panorama faces world +X, which keeps a tour consistent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Quality")
	bool bUsePointYaw = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Look")
	EPanoExposureMode ExposureMode = EPanoExposureMode::Tour;

	/** Exposure compensation in EV. In Auto mode this is added on top of what the viewport would use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Look", meta = (UIMin = -10, UIMax = 10))
	float ExposureBias = 0.f;

	/**
	 * Applied to every face on top of the level's post process volumes. Effects that would show seams
	 * (vignette, grain, chromatic aberration, lens flares, motion blur, depth of field, local exposure) are always turned off,
	 * and exposure is controlled by Exposure Mode.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Look")
	FPostProcessSettings PostProcessSettings;

	/** Actors left out of the captures. This camera is always hidden. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Look")
	TArray<TObjectPtr<AActor>> HiddenActors;

	/**
	 * Export the static geometry around the tour (scene.glb, shape only). The viewer projects the panoramas onto it,
	 * which gives Matterport-style smooth transitions and a clean dollhouse with flat walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output")
	bool bExportSceneMesh = true;

	/** Per mesh, the first LOD at or under this many triangles is exported. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Output", AdvancedDisplay, meta = (EditCondition = "bExportSceneMesh", ClampMin = 100))
	int32 SceneMeshMaxTriangles = 20000;

	/** Capture a top-down plan of each floor during a tour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Floor Plan")
	bool bCaptureFloorPlans = true;

	/**
	 * Anything that starts higher than this above the floor (ceilings, upper floors, roofs) is left out of the plan.
	 * Whole components are hidden, so a building modeled as a single mesh keeps its roof: split roofs and ceilings into their own meshes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Floor Plan", meta = (EditCondition = "bCaptureFloorPlans", Units = "cm"))
	float FloorPlanCutHeight = 200.f;

	/** Space around the capture points included in the plan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Floor Plan", meta = (EditCondition = "bCaptureFloorPlans", Units = "cm"))
	float FloorPlanMargin = 500.f;

	/** Size of the plan image's longer side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Floor Plan", meta = (EditCondition = "bCaptureFloorPlans", ClampMin = 256, ClampMax = 8192))
	int32 FloorPlanResolution = 2048;

	/** Distance between automatically placed points. Matterport-style tours use 1.5-2.5 m indoors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Auto Place", meta = (Units = "cm", ClampMin = 100))
	float AutoSpacing = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Auto Place", meta = (ClampMin = 1, ClampMax = 500))
	int32 AutoMaxPoints = 40;

	/** How far from the existing points (or this camera, when there are none) to look for walkable floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Auto Place", meta = (Units = "cm", ClampMin = 100))
	float AutoSearchRadius = 2500.f;

	/** Keep points at least this far from walls and furniture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Auto Place", meta = (Units = "cm", ClampMin = 10))
	float AutoClearance = 50.f;

	/** Lens height above the floor. 0 matches the existing points (150 cm when there are none). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano|Auto Place", meta = (Units = "cm", ClampMin = 0))
	float AutoLensHeight = 0.f;

	/**
	 * Fill the walkable floor around the existing points with capture points at Auto Spacing. Walls stop the fill and
	 * doorways let it through; spots on furniture, under tables or too close to obstacles are skipped. Replaces
	 * points placed by an earlier run; hand-placed points are kept.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano|Auto Place")
	void AutoPlacePoints();

	/** Remove the points Auto Place Points created. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano|Auto Place")
	void ClearAutoPoints();

	/** Fires after each panorama is written to disk. */
	UPROPERTY(BlueprintAssignable, Category = "Pano")
	FOnPanoCaptured OnPanoCaptured;

	/** Fires once every queued image is on disk (and tour.json for a tour). */
	UPROPERTY(BlueprintAssignable, Category = "Pano")
	FOnPanoCaptureFinished OnCaptureFinished;

	/** Capture one panorama from this camera's location. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano")
	void CaptureHere();

	/** Capture every Pano Capture Point in the level and the floor plans, then write tour.json and the web viewer. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano")
	void CaptureTour();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano")
	void CancelCapture();

	/** Open the last captured tour in the browser, served from the editor on localhost. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Pano")
	void OpenViewer();

	UFUNCTION(BlueprintPure, Category = "Pano")
	bool IsCapturing() const { return TickerHandle.IsValid(); }

	/** Folder the next capture writes to. */
	UFUNCTION(BlueprintPure, Category = "Pano")
	FString GetOutputFolder() const;

	/** Tour Name, or the map name when it is empty. */
	UFUNCTION(BlueprintPure, Category = "Pano")
	FString GetResolvedTourName() const;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/**
	 * Settings a capture depends on, copied when it starts so edits (details panel or Blueprint) can't change it halfway.
	 * The ones holding object references are copied into ActivePostProcess and ActiveHiddenActors instead.
	 */
	struct FCaptureSettings
	{
		int32 OutputWidth = 0;
		EPanoImageFormat ImageFormat = EPanoImageFormat::JPG;
		int32 JpegQuality = 92;
		bool bWritePreviews = true;
		int32 PreviewWidth = 2048;
		bool bCaptureDepth = true;
		int32 DepthWidth = 2048;
		float MaxDepth = 100000.f;
		int32 WarmupFrames = 32;
		int32 SettleFrames = 12;
		float Supersampling = 1.f;
		float FaceOverscanDegrees = 10.f;
		EPanoExposureMode ExposureMode = EPanoExposureMode::Tour;
		float ExposureBias = 0.f;
		bool bExportSceneMesh = true;
		int32 SceneMeshMaxTriangles = 20000;
		float FloorPlanCutHeight = 200.f;
		float FloorPlanMargin = 500.f;
		int32 FloorPlanResolution = 2048;
		FString TourName;

		/** Width and height are kept even so the equirect is exactly 2:1. */
		int32 PanoWidth() const { return OutputWidth & ~1; }
		int32 DepthPanoWidth() const { return DepthWidth & ~1; }
		/** The medium (4K walking) copy only exists when the full image is larger. */
		bool WritesMedium() const { return bWritePreviews && OutputWidth > 4096; }
		/** An HDR warm-up pass is needed to meter exposure or to read depth. */
		bool NeedsHdrPass() const { return ExposureMode != EPanoExposureMode::Manual || bCaptureDepth; }
		const TCHAR* ImageExtension() const { return ImageFormat == EPanoImageFormat::PNG ? TEXT(".png") : TEXT(".jpg"); }
	};

	enum class EJobType : uint8
	{
		Pano,
		FloorPlan
	};

	struct FCaptureJob
	{
		EJobType Type = EJobType::Pano;
		FString Id;
		FVector Location = FVector::ZeroVector;
		float Heading = 0.f;
		float FloorZ = 0.f;
		int32 Floor = 0;
		TWeakObjectPtr<APanoCapturePoint> Point;
		/** Floor plan area in world XY. */
		FBox2D PlanBounds = FBox2D(ForceInit);
		/** Exposure the faces were rendered with (AutoExposureBias, manual). */
		float FaceBias = 0.f;
		/** FaceBias came from metering this spot (false: the read failed and it is just Exposure Bias). */
		bool bMetered = false;
		/** Tour exposure, first sweep: only meter (and write depth), no color. */
		bool bMeterOnly = false;
		/** Tour exposure, second sweep: color at the shared tour exposure. */
		bool bUseTourBias = false;
		bool bSaved = false;
	};

	struct FPendingWrite
	{
		int32 JobIndex = INDEX_NONE;
		FString Path;
		TFuture<bool> Result;
	};

	void StartCapture(TArray<FCaptureJob>&& InJobs, bool bInWriteManifest);
	void StopCapture(bool bRemoveTicker);
	/** The part of StopCapture that touches no UObjects, so it is also safe from BeginDestroy. */
	void ReleaseCaptureState(bool bRemoveTicker);
	bool TickCapture(float DeltaSeconds);
	void TickPanoJob(FCaptureJob& Job);
	void TickFloorPlanJob(FCaptureJob& Job);
	void PollPendingWrites();

	void PrepareFaces();
	void SetFacesMetering(bool bMetering, float Bias);
	/** Reads the HDR warm-up pass: meters exposure and keeps a downsampled copy of the depth. */
	void ReadHdrPass(FCaptureJob& Job);
	void ReadBackAndSavePano(const FCaptureJob& Job);
	void SaveDepthOnly(const FCaptureJob& Job);
	float ComputeTourBias();
	void ExportSceneMesh();
	void PrepareFloorPlan(const FCaptureJob& Job);
	void ReadBackAndSaveFloorPlan(const FCaptureJob& Job);

	float FindFloorZ(const FVector& From) const;
	/** Capture order: nearest next, so Lumen's surface cache, shadow pages and streaming stay warm between points. */
	void OrderForCapture(TArray<APanoCapturePoint*>& Points) const;
	/** Editors throttle to a few fps in the background; a capture needs every frame it can get. */
	void OverrideBackgroundThrottle(bool bOverride);
	void AddFloorPlanJobs(TArray<FCaptureJob>& InOutJobs) const;
	void WriteManifest() const;
	void CopyViewer() const;
	FPostProcessSettings BuildFacePostProcess(float Bias) const;

	/** Editor toast in the bottom-right corner, like the high resolution screenshot one. */
	void ShowNotification();
	void UpdateNotification();
	void FinishNotification(bool bSuccess, const FText& Text, const FString& LinkPath);

	UPROPERTY(VisibleAnywhere, Category = "Pano")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Moved to each capture spot. The faces are attached to it. */
	UPROPERTY(VisibleAnywhere, Category = "Pano")
	TObjectPtr<USceneComponent> LensRoot;

	UPROPERTY(VisibleAnywhere, Category = "Pano")
	TArray<TObjectPtr<USceneCaptureComponent2D>> Faces;

	/** Top-down orthographic capture for floor plans. */
	UPROPERTY(VisibleAnywhere, Category = "Pano")
	TObjectPtr<USceneCaptureComponent2D> PlanCapture;

	/** 8-bit targets for the final (tonemapped) faces. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> FaceTargets;

	/** Float targets for metering the HDR scene. Same size as the face targets so rendering history carries over. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> MeterTargets;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> PlanTarget;

	/** Post Process Settings and Hidden Actors as they were when the running capture started (see FCaptureSettings). */
	UPROPERTY(Transient)
	FPostProcessSettings ActivePostProcess;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> ActiveHiddenActors;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> Arrow;
#endif

	/** Depth from the current spot's HDR pass, waiting to be stitched with its color. */
	TArray<FPanoCubeFace> PendingDepthFaces;

	/** Copy of the settings for the running capture. */
	FCaptureSettings Active;
	TArray<FCaptureJob> Jobs;
	TArray<FPendingWrite> PendingWrites;
	/** Writes still running from a cancelled capture. A new capture waits for them, since it may write the same files. */
	TArray<TFuture<bool>> OrphanedWrites;
	int32 CurrentJob = 0;
	int32 FramesAtCurrentJob = 0;
	bool bMeteredCurrentJob = false;
	int32 FaceSize = 0;
	/** HDR/depth pass resolution. Lower than FaceSize for the tour metering sweep, which only needs depth and brightness. */
	int32 MeterFaceSize = 0;
	float TanHalfFov = 1.f;
	bool bWriteManifest = false;
	FString ActiveOutputFolder;
	FString LastSavedPath;
	int32 SavedCount = 0;
	int32 PanoJobCount = 0;
	int32 SavedPanoCount = 0;
	float TourBias = 0.f;
	bool bTourBiasReady = false;
	int32 SceneMeshTriangles = 0;
	FTSTicker::FDelegateHandle TickerHandle;
	TSharedPtr<SNotificationItem> Notification;
	bool bThrottleOverridden = false;
	bool bSavedThrottle = false;
	/** Where the overridden setting lives (in a class default object, which is never collected), so it can be restored without FindObject. */
	FBoolProperty* ThrottleProperty = nullptr;
	void* ThrottleValue = nullptr;
};

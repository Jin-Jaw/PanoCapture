// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PanoCapturePoint.generated.h"

class UArrowComponent;
class UBillboardComponent;

/**
 * A spot in the level where a panorama is captured (a "sweep" in Matterport terms).
 * The actor location is the lens position, so place it at eye height (around 150 cm above the floor).
 */
UCLASS(BlueprintType, meta = (DisplayName = "Pano Capture Point"))
class PANOCAPTURE_API APanoCapturePoint : public AActor
{
	GENERATED_BODY()

public:
	APanoCapturePoint();

	/** Id used for the image filename and in tour.json. Uses the actor label when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano")
	FString PointId;

	/** Floor this point belongs to, written to tour.json for floor switching in the viewer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano")
	int32 FloorIndex = 0;

	/** Room this point stands in ("Kitchen"). The viewer labels each room in the dollhouse and floor plan, at the middle of its points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano")
	FString RoomName;

	/** Points the viewer can move to from here. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Pano")
	TArray<TObjectPtr<APanoCapturePoint>> Neighbors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano")
	bool bIncludeInTour = true;

	/** Capture and tour order, lowest first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano")
	int32 SortOrder = 0;

	/** PointId, or the actor label when PointId is empty, made safe for use as a filename. */
	UFUNCTION(BlueprintPure, Category = "Pano")
	FString GetResolvedId() const;

private:
	UPROPERTY(VisibleAnywhere, Category = "Pano")
	TObjectPtr<USceneComponent> SceneRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> Arrow;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};

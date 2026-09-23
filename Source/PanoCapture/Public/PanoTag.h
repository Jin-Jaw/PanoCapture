// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PanoTag.generated.h"

class UArrowComponent;
class UBillboardComponent;

/**
 * An info tag in the tour (Matterport calls them Mattertags): a colored disc on a thin stem that opens a card with a
 * title, a description, an image or video, and a link. Place it on the surface it describes; the stem points along
 * the actor's up axis (Z), so rotate it to stick out of a wall.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Pano Tag"))
class PANOCAPTURE_API APanoTag : public AActor
{
	GENERATED_BODY()

public:
	APanoTag();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	FString Title;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag", meta = (MultiLine = true))
	FString Description;

	/**
	 * Shown at the top of the card: a YouTube or Vimeo link, a video file (.mp4, .webm) or an image. A relative path is
	 * a file in the tour folder, which the web build copies along.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	FString MediaUrl;

	/** Opened in a new tab from the card's header. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	FString LinkUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	FString LinkLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	FColor Color = FColor(31, 138, 153);

	/** Distance from the surface to the disc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag", meta = (Units = "cm", ClampMin = 0, ClampMax = 300))
	float StemLength = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pano Tag")
	bool bIncludeInTour = true;

	/** Id in tour.json: the actor label (or name), made safe for use in a URL. */
	UFUNCTION(BlueprintPure, Category = "Pano Tag")
	FString GetResolvedId() const;

	/** Direction the stem sticks out, world space. */
	FVector GetStemDirection() const { return GetActorUpVector(); }

private:
	UPROPERTY(VisibleAnywhere, Category = "Pano Tag")
	TObjectPtr<USceneComponent> SceneRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> Stem;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};

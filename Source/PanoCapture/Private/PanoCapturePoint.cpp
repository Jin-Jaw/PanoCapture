// Copyright Jad Deeb. All Rights Reserved.

#include "PanoCapturePoint.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Misc/Paths.h"

APanoCapturePoint::APanoCapturePoint()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

#if WITH_EDITORONLY_DATA
	Arrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	if (Arrow)
	{
		Arrow->SetupAttachment(SceneRoot);
		Arrow->ArrowColor = FColor(0, 200, 255);
		Arrow->ArrowSize = 0.75f;
	}

	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		Sprite->SetupAttachment(SceneRoot);
	}
#endif
}

FString APanoCapturePoint::GetResolvedId() const
{
	FString Id = PointId;
	if (Id.IsEmpty())
	{
#if WITH_EDITOR
		Id = GetActorLabel();
#else
		Id = GetName();
#endif
	}
	return FPaths::MakeValidFileName(Id, TEXT('_'));
}

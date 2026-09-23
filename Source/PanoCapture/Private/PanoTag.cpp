// Copyright Jad Deeb. All Rights Reserved.

#include "PanoTag.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Misc/Paths.h"

APanoTag::APanoTag()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

#if WITH_EDITORONLY_DATA
	// Shows the stem in the editor: it points along the actor's up axis, like the tag in the viewer.
	Stem = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("Stem"));
	if (Stem)
	{
		Stem->SetupAttachment(SceneRoot);
		Stem->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
		Stem->ArrowColor = Color;
		Stem->ArrowSize = 0.5f;
	}

	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		Sprite->SetupAttachment(SceneRoot);
		Sprite->SetRelativeLocation(FVector(0.f, 0.f, 40.f));
	}
#endif
}

FString APanoTag::GetResolvedId() const
{
#if WITH_EDITOR
	const FString Label = GetActorLabel();
#else
	const FString Label = GetName();
#endif
	return FPaths::MakeValidFileName(Label, TEXT('_')).Replace(TEXT(" "), TEXT("_"));
}

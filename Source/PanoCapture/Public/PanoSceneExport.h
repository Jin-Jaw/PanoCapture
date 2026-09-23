// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

namespace PanoSceneExport
{
	struct FResult
	{
		bool bSuccess = false;
		int32 Components = 0;
		int32 Triangles = 0;
		/** Components whose geometry isn't readable on the CPU (skipped). */
		int32 Skipped = 0;
	};

	/**
	 * Writes the static mesh geometry inside Bounds as a positions-only binary glTF. The viewer projects the
	 * panoramas onto it, so it needs shape, not materials. Coordinates are glTF's: meters, Y up, right-handed,
	 * mapped from Unreal as (Y, Z, -X).
	 *
	 * @param MaxTrianglesPerMesh  Uses the first LOD at or under this many triangles (or the last LOD).
	 * @param MaxComponentSize     Skips components bigger than this (sky spheres, huge landscapes), in cm.
	 */
	PANOCAPTURE_API FResult ExportStaticGeometry(UWorld* World, const FBox& Bounds, const TArray<const AActor*>& IgnoreActors,
		int32 MaxTrianglesPerMesh, float MaxComponentSize, const FString& Path);
}

// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One rendered face of the capture cube, with its basis in lens-local space. */
struct FPanoCubeFace
{
	FVector3f Forward = FVector3f::ForwardVector;
	FVector3f Right = FVector3f::RightVector;
	FVector3f Up = FVector3f::UpVector;
	TArray<FColor> Pixels;
	/** Planar scene depth (distance along Forward, cm). Used instead of Pixels for depth stitching. */
	TArray<float> Depth;
	int32 Size = 0;
};

namespace PanoStitcher
{
	/**
	 * Resamples the cube faces into a 2:1 equirectangular image. The image center looks down lens-local +X,
	 * and +Y (right) is to the right of center, which is the layout web pano viewers expect.
	 * @param TanHalfFov      tan(FOV / 2) the faces were rendered with (1 for an exact 90 degree cube, more with overscan).
	 * @param SamplesPerAxis  Sub-samples per output pixel along each axis (1-4). Use 2+ when the faces are supersampled.
	 */
	PANOCAPTURE_API void CubeToEquirect(TConstArrayView<FPanoCubeFace> Faces, float TanHalfFov, int32 Width, TArray<FColor>& OutPixels, int32 SamplesPerAxis = 1);

	/**
	 * Same layout as CubeToEquirect, for depth: returns the distance from the lens along each ray (cm).
	 * Bilinear where the four nearest texels agree within 8%, nearest across depth edges so they stay sharp
	 * instead of blending foreground into background.
	 */
	PANOCAPTURE_API void CubeDepthToEquirect(TConstArrayView<FPanoCubeFace> Faces, float TanHalfFov, int32 Width, TArray<float>& OutDistance);

	/**
	 * Packs distances into 24-bit RGB millimeters (R high byte), so browsers can read them from an 8-bit PNG.
	 * 0 means no surface (sky, or farther than MaxDistance).
	 */
	PANOCAPTURE_API void EncodeDepth(TConstArrayView<float> Distance, float MaxDistance, TArray<FColor>& OutPixels);
}

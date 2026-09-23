// Copyright Jad Deeb. All Rights Reserved.

#include "PanoStitcher.h"
#include "Async/ParallelFor.h"

namespace
{
	/** Bilinear lookup that adds into R, G, B (so several sub-samples can be averaged before rounding). */
	FORCEINLINE void AccumulateBilinear(const FPanoCubeFace& Face, float U, float V, float& R, float& G, float& B)
	{
		const int32 Max = Face.Size - 1;
		const float FloorU = FMath::FloorToFloat(U);
		const float FloorV = FMath::FloorToFloat(V);
		const float Fx = U - FloorU;
		const float Fy = V - FloorV;
		const int32 X0 = FMath::Clamp((int32)FloorU, 0, Max);
		const int32 Y0 = FMath::Clamp((int32)FloorV, 0, Max);
		const int32 X1 = FMath::Min(X0 + 1, Max);
		const int32 Y1 = FMath::Min(Y0 + 1, Max);

		const FColor& C00 = Face.Pixels[Y0 * Face.Size + X0];
		const FColor& C10 = Face.Pixels[Y0 * Face.Size + X1];
		const FColor& C01 = Face.Pixels[Y1 * Face.Size + X0];
		const FColor& C11 = Face.Pixels[Y1 * Face.Size + X1];

		const float W00 = (1.f - Fx) * (1.f - Fy);
		const float W10 = Fx * (1.f - Fy);
		const float W01 = (1.f - Fx) * Fy;
		const float W11 = Fx * Fy;
		R += C00.R * W00 + C10.R * W10 + C01.R * W01 + C11.R * W11;
		G += C00.G * W00 + C10.G * W10 + C01.G * W01 + C11.G * W11;
		B += C00.B * W00 + C10.B * W10 + C01.B * W01 + C11.B * W11;
	}
}

namespace
{
	/** Visits every equirect pixel with the face whose forward axis is closest to its ray (that face always contains it). */
	template <typename FunctionType>
	void ForEachEquirectPixel(TConstArrayView<FPanoCubeFace> Faces, int32 Width, FunctionType&& Function)
	{
		const int32 Height = Width / 2;

		TArray<float> CosLon, SinLon;
		CosLon.SetNumUninitialized(Width);
		SinLon.SetNumUninitialized(Width);
		for (int32 X = 0; X < Width; ++X)
		{
			const float Lon = (X + 0.5f) / Width * UE_TWO_PI - UE_PI;
			FMath::SinCos(&SinLon[X], &CosLon[X], Lon);
		}

		ParallelFor(Height, [&](int32 Y)
		{
			const float Lat = UE_HALF_PI - (Y + 0.5f) / Height * UE_PI;
			float SinLat, CosLat;
			FMath::SinCos(&SinLat, &CosLat, Lat);

			for (int32 X = 0; X < Width; ++X)
			{
				const FVector3f Dir(CosLat * CosLon[X], CosLat * SinLon[X], SinLat);

				int32 Best = 0;
				float BestZ = -2.f;
				for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
				{
					const float Z = Dir | Faces[FaceIndex].Forward;
					if (Z > BestZ)
					{
						BestZ = Z;
						Best = FaceIndex;
					}
				}

				Function((int64)Y * Width + X, Faces[Best], Dir, BestZ);
			}
		});
	}
}

void PanoStitcher::CubeDepthToEquirect(TConstArrayView<FPanoCubeFace> Faces, float TanHalfFov, int32 Width, TArray<float>& OutDistance)
{
	check(Faces.Num() > 0 && Width > 0 && TanHalfFov > 0.f);
	OutDistance.SetNumUninitialized(Width * (Width / 2));

	const float InvTanHalfFov = 1.f / TanHalfFov;
	ForEachEquirectPixel(Faces, Width, [&](int64 Index, const FPanoCubeFace& Face, const FVector3f& Dir, float Z)
	{
		// Continuous texel coordinates, with texel centers on integers.
		const float Scale = InvTanHalfFov / Z;
		const float U = ((Dir | Face.Right) * Scale * 0.5f + 0.5f) * Face.Size - 0.5f;
		const float V = (0.5f - (Dir | Face.Up) * Scale * 0.5f) * Face.Size - 0.5f;
		const int32 Max = Face.Size - 1;
		const float FloorU = FMath::FloorToFloat(U);
		const float FloorV = FMath::FloorToFloat(V);
		const float Fx = U - FloorU;
		const float Fy = V - FloorV;
		const int32 X0 = FMath::Clamp((int32)FloorU, 0, Max);
		const int32 Y0 = FMath::Clamp((int32)FloorV, 0, Max);
		const int32 X1 = FMath::Min(X0 + 1, Max);
		const int32 Y1 = FMath::Min(Y0 + 1, Max);

		const float A = Face.Depth[Y0 * Face.Size + X0];
		const float B = Face.Depth[Y0 * Face.Size + X1];
		const float C = Face.Depth[Y1 * Face.Size + X0];
		const float D = Face.Depth[Y1 * Face.Size + X1];

		// Same rule as the viewer's depthAtUV: blend where the four texels agree (one smooth surface), otherwise take
		// the nearest, since blending across an edge would invent a surface floating between foreground and background.
		const float Lo = FMath::Min(FMath::Min(A, B), FMath::Min(C, D));
		const float Hi = FMath::Max(FMath::Max(A, B), FMath::Max(C, D));
		float Planar;
		if (Lo <= 0.f || !FMath::IsFinite(Hi) || Hi > Lo * 1.08f)
		{
			Planar = Fy < 0.5f ? (Fx < 0.5f ? A : B) : (Fx < 0.5f ? C : D);
		}
		else
		{
			Planar = FMath::Lerp(FMath::Lerp(A, B, Fx), FMath::Lerp(C, D, Fx), Fy);
		}

		// Scene depth is planar (along the face's forward axis); this pixel's own ray makes it a distance.
		OutDistance[Index] = Planar / Z;
	});
}

void PanoStitcher::EncodeDepth(TConstArrayView<float> Distance, float MaxDistance, TArray<FColor>& OutPixels)
{
	OutPixels.SetNumUninitialized(Distance.Num());
	ParallelFor(Distance.Num() / 4096 + 1, [&](int32 Chunk)
	{
		const int32 End = FMath::Min((Chunk + 1) * 4096, Distance.Num());
		for (int32 Index = Chunk * 4096; Index < End; ++Index)
		{
			const float Cm = Distance[Index];
			const uint32 Mm = (FMath::IsFinite(Cm) && Cm > 0.f && Cm < MaxDistance)
				? (uint32)FMath::Clamp(FMath::RoundToInt(Cm * 10.f), 1, 0xFFFFFF)
				: 0u;
			OutPixels[Index] = FColor((Mm >> 16) & 0xFF, (Mm >> 8) & 0xFF, Mm & 0xFF, 255);
		}
	});
}

void PanoStitcher::CubeToEquirect(TConstArrayView<FPanoCubeFace> Faces, float TanHalfFov, int32 Width, TArray<FColor>& OutPixels, int32 SamplesPerAxis)
{
	check(Faces.Num() > 0 && Width > 0 && TanHalfFov > 0.f);

	const int32 Height = Width / 2;
	const int32 N = FMath::Clamp(SamplesPerAxis, 1, 4);
	const float InvSamples = 1.f / (N * N);
	OutPixels.SetNumUninitialized(Width * Height);

	// N x N sub-samples per output pixel (a box filter over the pixel's footprint). With faces rendered
	// larger than needed this is real supersampling: sharper than one bilinear tap, and no stair-stepping.
	TArray<float> CosLon, SinLon;
	CosLon.SetNumUninitialized(Width * N);
	SinLon.SetNumUninitialized(Width * N);
	for (int32 X = 0; X < Width; ++X)
	{
		for (int32 Sx = 0; Sx < N; ++Sx)
		{
			const float Lon = (X + (Sx + 0.5f) / N) / Width * UE_TWO_PI - UE_PI;
			FMath::SinCos(&SinLon[X * N + Sx], &CosLon[X * N + Sx], Lon);
		}
	}

	const float InvTanHalfFov = 1.f / TanHalfFov;

	ParallelFor(Height, [&](int32 Y)
	{
		float SinLat[4], CosLat[4];
		for (int32 Sy = 0; Sy < N; ++Sy)
		{
			const float Lat = UE_HALF_PI - (Y + (Sy + 0.5f) / N) / Height * UE_PI;
			FMath::SinCos(&SinLat[Sy], &CosLat[Sy], Lat);
		}

		FColor* Row = OutPixels.GetData() + (int64)Y * Width;
		for (int32 X = 0; X < Width; ++X)
		{
			float R = 0.f, G = 0.f, B = 0.f;
			for (int32 Sy = 0; Sy < N; ++Sy)
			{
				for (int32 Sx = 0; Sx < N; ++Sx)
				{
					const int32 Column = X * N + Sx;
					const FVector3f Dir(CosLat[Sy] * CosLon[Column], CosLat[Sy] * SinLon[Column], SinLat[Sy]);

					// The face whose forward axis is closest to the ray always contains it.
					int32 Best = 0;
					float BestZ = -2.f;
					for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
					{
						const float Z = Dir | Faces[FaceIndex].Forward;
						if (Z > BestZ)
						{
							BestZ = Z;
							Best = FaceIndex;
						}
					}

					const FPanoCubeFace& Face = Faces[Best];
					const float Scale = InvTanHalfFov / BestZ;
					const float U = ((Dir | Face.Right) * Scale * 0.5f + 0.5f) * Face.Size - 0.5f;
					const float V = (0.5f - (Dir | Face.Up) * Scale * 0.5f) * Face.Size - 0.5f;
					AccumulateBilinear(Face, U, V, R, G, B);
				}
			}

			Row[X] = FColor(
				(uint8)FMath::Clamp(FMath::RoundToInt(R * InvSamples), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(G * InvSamples), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt(B * InvSamples), 0, 255),
				255);
		}
	});
}

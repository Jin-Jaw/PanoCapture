// Copyright Jad Deeb. All Rights Reserved.

#include "PanoSceneExport.h"
#include "PanoCaptureModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "StaticMeshResources.h"

namespace
{
	void AppendPadded(TArray<uint8>& Out, const void* Data, int32 Size, uint8 Pad)
	{
		Out.Append(static_cast<const uint8*>(Data), Size);
		while (Out.Num() % 4 != 0)
		{
			Out.Add(Pad);
		}
	}

	void AppendUint32(TArray<uint8>& Out, uint32 Value)
	{
		Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	}

	bool WriteGlb(const TArray<FVector3f>& Positions, const TArray<uint32>& Indices, const FString& Path)
	{
		FVector3f Min(TNumericLimits<float>::Max()), Max(-TNumericLimits<float>::Max());
		for (const FVector3f& P : Positions)
		{
			Min = Min.ComponentMin(P);
			Max = Max.ComponentMax(P);
		}

		const int32 PositionBytes = Positions.Num() * sizeof(FVector3f);
		const int32 IndexBytes = Indices.Num() * sizeof(uint32);

		const FString Json = FString::Printf(TEXT(
			"{\"asset\":{\"version\":\"2.0\",\"generator\":\"PanoCapture\"},"
			"\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":\"PanoProxy\"}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
			"\"buffers\":[{\"byteLength\":%d}],"
			"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%d,\"target\":34962},"
			"{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d,\"target\":34963}],"
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[%f,%f,%f],\"max\":[%f,%f,%f]},"
			"{\"bufferView\":1,\"componentType\":5125,\"count\":%d,\"type\":\"SCALAR\"}]}"),
			PositionBytes + IndexBytes, PositionBytes, PositionBytes, IndexBytes,
			Positions.Num(), Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z, Indices.Num());

		TArray<uint8> JsonChunk;
		const FTCHARToUTF8 Utf8(*Json);
		AppendPadded(JsonChunk, Utf8.Get(), Utf8.Length(), ' ');

		TArray<uint8> BinChunk;
		BinChunk.Append(reinterpret_cast<const uint8*>(Positions.GetData()), PositionBytes);
		AppendPadded(BinChunk, Indices.GetData(), IndexBytes, 0);

		TArray<uint8> Glb;
		AppendUint32(Glb, 0x46546C67); // "glTF"
		AppendUint32(Glb, 2);
		AppendUint32(Glb, 12 + 8 + JsonChunk.Num() + 8 + BinChunk.Num());
		AppendUint32(Glb, JsonChunk.Num());
		AppendUint32(Glb, 0x4E4F534A); // "JSON"
		Glb.Append(JsonChunk);
		AppendUint32(Glb, BinChunk.Num());
		AppendUint32(Glb, 0x004E4942); // "BIN\0"
		Glb.Append(BinChunk);

		return FFileHelper::SaveArrayToFile(Glb, *Path);
	}
}

PanoSceneExport::FResult PanoSceneExport::ExportStaticGeometry(UWorld* World, const FBox& Bounds, const TArray<const AActor*>& IgnoreActors,
	int32 MaxTrianglesPerMesh, float MaxComponentSize, const FString& Path)
{
	FResult Result;
	if (!World)
	{
		return Result;
	}

	TArray<FVector3f> Positions;
	TArray<uint32> Indices;
	TArray<uint32> LodIndices;
	TArray<FTransform> Transforms;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (IgnoreActors.Contains(Actor) || Actor->IsHidden())
		{
			continue;
		}

		Actor->ForEachComponent<UStaticMeshComponent>(false, [&](UStaticMeshComponent* Component)
		{
			if (!Component->IsRegistered() || !Component->IsVisible() || Component->bHiddenInGame)
			{
				return;
			}

			UStaticMesh* Mesh = Component->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || RenderData->LODResources.IsEmpty())
			{
				return;
			}

			const FBox ComponentBox = Component->Bounds.GetBox();
			if (!ComponentBox.Intersect(Bounds) || ComponentBox.GetSize().GetMax() > MaxComponentSize)
			{
				return;
			}

			// Coarsest detail that still keeps the shape: the first LOD under the budget.
			int32 LodIndex = 0;
			while (LodIndex < RenderData->LODResources.Num() - 1 && RenderData->LODResources[LodIndex].GetNumTriangles() > MaxTrianglesPerMesh)
			{
				++LodIndex;
			}
			const FStaticMeshLODResources& Lod = RenderData->LODResources[LodIndex];
			const FPositionVertexBuffer& VertexPositions = Lod.VertexBuffers.PositionVertexBuffer;
			LodIndices.Reset();
			Lod.IndexBuffer.GetCopy(LodIndices);
			if (VertexPositions.GetNumVertices() == 0 || !VertexPositions.GetVertexData() || LodIndices.IsEmpty())
			{
				++Result.Skipped;
				return;
			}

			Transforms.Reset();
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
			{
				const FBox MeshBox = Mesh->GetBounds().GetBox();
				for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
				{
					FTransform Transform;
					if (Instanced->GetInstanceTransform(Instance, Transform, /*bWorldSpace*/ true) && MeshBox.TransformBy(Transform).Intersect(Bounds))
					{
						Transforms.Add(Transform);
					}
				}
			}
			else
			{
				Transforms.Add(Component->GetComponentTransform());
			}

			for (const FTransform& Transform : Transforms)
			{
				const uint32 Base = Positions.Num();
				for (uint32 Vertex = 0; Vertex < VertexPositions.GetNumVertices(); ++Vertex)
				{
					const FVector P = Transform.TransformPosition(FVector(VertexPositions.VertexPosition(Vertex)));
					Positions.Add(FVector3f(P.Y, P.Z, -P.X) * 0.01f);
				}

				// Unreal -> glTF flips handedness, which flips winding; a mirrored transform flips it back.
				const bool bKeepWinding = Transform.GetDeterminant() < 0;
				for (int32 Index = 0; Index + 2 < LodIndices.Num(); Index += 3)
				{
					Indices.Add(Base + LodIndices[Index]);
					Indices.Add(Base + LodIndices[Index + (bKeepWinding ? 1 : 2)]);
					Indices.Add(Base + LodIndices[Index + (bKeepWinding ? 2 : 1)]);
				}
				++Result.Components;
			}
		});
	}

	Result.Triangles = Indices.Num() / 3;
	if (Indices.IsEmpty())
	{
		UE_LOG(LogPanoCapture, Warning, TEXT("No static geometry to export near the capture points."));
		return Result;
	}

	Result.bSuccess = WriteGlb(Positions, Indices, Path);
	UE_LOG(LogPanoCapture, Log, TEXT("Exported %d triangles from %d meshes to %s (%d skipped: no CPU-readable geometry)"),
		Result.Triangles, Result.Components, *Path, Result.Skipped);
	return Result;
}

// Copyright Jad Deeb. All Rights Reserved.

#include "PanoExposure.h"
#include "PanoCaptureModule.h"
#include "ColorManagement/ColorSpace.h"
#include "Curves/CurveFloat.h"
#include "Engine/Scene.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/Interface_PostProcessVolume.h"

// Mirrors the engine's exposure setup in UE 5.6: FSceneView::StartFinalPostprocessSettings and OverridePostProcessSettings
// (SceneView.cpp), DoPostProcessVolume (World.cpp), GetEyeAdaptationParameters (PostProcessEyeAdaptation.cpp) and the
// histogram (PostProcessHistogram.usf, PostProcessHistogramCommon.ush, PostProcessEyeAdaptation.usf).

namespace
{
	int32 GetCVarInt(const TCHAR* Name, int32 Default)
	{
		const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
		return CVar ? CVar->GetInt() : Default;
	}

	float GetCVarFloat(const TCHAR* Name, float Default)
	{
		const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
		return CVar ? CVar->GetFloat() : Default;
	}

	bool IsExtendedLuminanceRange()
	{
		return GetCVarInt(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"), 0) != 0;
	}

	// Mirrors the engine's LuminanceMaxFromLensAttenuation(): 1 with the default lens attenuation of 0.78.
	float LuminanceMax()
	{
		if (!IsExtendedLuminanceRange())
		{
			return 1.f;
		}
		const float LensAttenuation = GetCVarFloat(TEXT("r.EyeAdaptation.LensAttenuation"), 0.78f);
		return 0.78f / FMath::Max(LensAttenuation, 0.01f);
	}

	float PhysicalCameraEV100(const FPostProcessSettings& Settings)
	{
		return FMath::Log2(FMath::Square(Settings.DepthOfFieldFstop) * Settings.CameraShutterSpeed * 100.f / FMath::Max(1.f, Settings.CameraISO));
	}

	// r.AutoExposure.LuminanceMethod: 0 uniform thirds (the engine default), 1 NTSC, 2 the working color space (Rec.709 for sRGB).
	FVector3f LuminanceWeights()
	{
		switch (GetCVarInt(TEXT("r.AutoExposure.LuminanceMethod"), 0))
		{
		case 1:
			return FVector3f(0.3f, 0.59f, 0.11f);
		case 2:
		{
			const FLinearColor Factors = UE::Color::FColorSpace::GetWorking().GetLuminanceFactors();
			return FVector3f(Factors.R, Factors.G, Factors.B);
		}
		default:
			return FVector3f(1.f / 3.f);
		}
	}

	void WarnOnce(bool& bWarned, const TCHAR* Message)
	{
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogPanoCapture, Warning, TEXT("%s"), Message);
		}
	}
}

FPanoExposureSettings PanoExposure::GetSettingsAt(const UWorld* World, const FVector& Location)
{
	const bool bExtendedRange = IsExtendedLuminanceRange();

	// Defaults come from the project settings (r.DefaultFeature.AutoExposure.Bias and the luminance range).
	FPostProcessSettings Blended;
	if (GetCVarInt(TEXT("r.DefaultFeature.AutoExposure"), 1) == 0)
	{
		// Auto exposure off in the project settings: brightness limits of 1, which volumes can still override.
		Blended.AutoExposureMinBrightness = 1.f;
		Blended.AutoExposureMaxBrightness = 1.f;
		if (bExtendedRange)
		{
			Blended.AutoExposureMinBrightness = Blended.AutoExposureMaxBrightness = FMath::Log2(1.f / 1.2f);
		}
	}
	else
	{
		const int32 Method = GetCVarInt(TEXT("r.DefaultFeature.AutoExposure.Method"), 0);
		if (Method >= 0 && Method < AEM_MAX)
		{
			Blended.AutoExposureMethod = (EAutoExposureMethod)Method;
		}
	}

	bool bHasMeterMask = false;
	if (World)
	{
		struct FVolumeEntry
		{
			FPostProcessVolumeProperties Properties;
			float Weight;
		};

		TArray<FVolumeEntry> Volumes;
		for (IInterface_PostProcessVolume* Volume : World->PostProcessVolumes)
		{
			if (!Volume)
			{
				continue;
			}

			const FPostProcessVolumeProperties Properties = Volume->GetProperties();
			if (!Properties.bIsEnabled || !Properties.Settings)
			{
				continue;
			}

			float Weight = FMath::Clamp(Properties.BlendWeight, 0.f, 1.f);
			if (!Properties.bIsUnbound)
			{
				float Distance = 0.f;
				Volume->EncompassesPoint(Location, 0.f, &Distance);
				if (Distance < 0.f || Distance > Properties.BlendRadius)
				{
					continue;
				}
				if (Properties.BlendRadius >= 1.f)
				{
					Weight *= 1.f - Distance / Properties.BlendRadius;
				}
			}

			if (Weight > 0.f)
			{
				Volumes.Add({ Properties, Weight });
			}
		}

		Volumes.StableSort([](const FVolumeEntry& A, const FVolumeEntry& B) { return A.Properties.Priority < B.Properties.Priority; });

		for (const FVolumeEntry& Entry : Volumes)
		{
			const FPostProcessSettings& Src = *Entry.Properties.Settings;
			const float W = Entry.Weight;

			// Enums and assets can't be blended: any weight above 0 sets them (SET_PP / IF_PP in the engine).
			if (Src.bOverride_AutoExposureMethod)
			{
				Blended.AutoExposureMethod = Src.AutoExposureMethod;
			}
			if (Src.bOverride_AutoExposureApplyPhysicalCameraExposure)
			{
				Blended.AutoExposureApplyPhysicalCameraExposure = Src.AutoExposureApplyPhysicalCameraExposure;
			}
			if (Src.bOverride_AutoExposureBiasCurve && Src.AutoExposureBiasCurve)
			{
				Blended.AutoExposureBiasCurve = Src.AutoExposureBiasCurve;
			}
			if (Src.bOverride_AutoExposureMeterMask && Src.AutoExposureMeterMask)
			{
				bHasMeterMask = true;
			}

#define PANO_BLEND(Field) if (Src.bOverride_##Field) { Blended.Field = FMath::Lerp(Blended.Field, Src.Field, W); }
			PANO_BLEND(AutoExposureBias)
			PANO_BLEND(AutoExposureMinBrightness)
			PANO_BLEND(AutoExposureMaxBrightness)
			PANO_BLEND(AutoExposureLowPercent)
			PANO_BLEND(AutoExposureHighPercent)
			PANO_BLEND(HistogramLogMin)
			PANO_BLEND(HistogramLogMax)
			PANO_BLEND(DepthOfFieldFstop)
			PANO_BLEND(CameraShutterSpeed)
			PANO_BLEND(CameraISO)
#undef PANO_BLEND
		}
	}

	// GetAutoExposureMethod(): scalability and the debug override can change the method the volumes asked for.
	EAutoExposureMethod Method = Blended.AutoExposureMethod;
	if (Method != AEM_Manual && GetCVarInt(TEXT("r.EyeAdaptationQuality"), 2) == 1)
	{
		Method = AEM_Basic;
	}
	switch (GetCVarInt(TEXT("r.EyeAdaptation.MethodOverride"), -1))
	{
	case 1: Method = AEM_Histogram; break;
	case 2: Method = AEM_Basic; break;
	case 3: Method = AEM_Manual; break;
	default: break;
	}

	static bool bWarnedBasic = false;
	static bool bWarnedMask = false;
	if (Method == AEM_Basic)
	{
		WarnOnce(bWarnedBasic, TEXT("This level uses Basic auto exposure; panoramas are metered with the histogram method, so brightness may differ a little from the viewport."));
	}
	if (bHasMeterMask && Method != AEM_Manual)
	{
		WarnOnce(bWarnedMask, TEXT("The exposure Meter Mask is ignored: it weights the screen, and a panorama has no screen."));
	}

	const float LumMax = LuminanceMax();

	FPanoExposureSettings Result;
	Result.bManual = Method == AEM_Manual;
	// r.EyeAdaptationQuality 0 clears the EyeAdaptation show flag, which locks the viewport to exposure 1.
	Result.bEyeAdaptationOff = GetCVarInt(TEXT("r.EyeAdaptationQuality"), 2) <= 0;
	Result.Bias = Blended.AutoExposureBias;
	Result.BiasCurve = Blended.AutoExposureBiasCurve;
	Result.HighPercent = FMath::Clamp(Blended.AutoExposureHighPercent, 1.f, 99.f) * 0.01f;
	Result.LowPercent = FMath::Min(FMath::Clamp(Blended.AutoExposureLowPercent, 1.f, 99.f) * 0.01f, Result.HighPercent);
	Result.ManualEV100 = Blended.AutoExposureApplyPhysicalCameraExposure ? PhysicalCameraEV100(Blended) : 0.f;

	// With the extended range the histogram limits are EV100; the histogram itself works in log2 luminance.
	Result.HistogramLogMax = bExtendedRange ? Blended.HistogramLogMax + FMath::Log2(LumMax) : Blended.HistogramLogMax;
	Result.HistogramLogMin = FMath::Min(bExtendedRange ? Blended.HistogramLogMin + FMath::Log2(LumMax) : Blended.HistogramLogMin, Result.HistogramLogMax - 1.f);

	if (bExtendedRange)
	{
		Result.MinEV100 = Blended.AutoExposureMinBrightness;
		Result.MaxEV100 = Blended.AutoExposureMaxBrightness;
	}
	else
	{
		// Without the extended range the brightness limits are linear luminance.
		Result.MinEV100 = FMath::Log2(FMath::Max(Blended.AutoExposureMinBrightness, 1e-6f));
		Result.MaxEV100 = FMath::Log2(FMath::Max(Blended.AutoExposureMaxBrightness, 1e-6f));
	}
	return Result;
}

float PanoExposure::MeterAverageLuminance(TConstArrayView<TArray<FFloat16Color>> Faces, int32 FaceSize, float TanHalfFov, const FPanoExposureSettings& Settings)
{
	constexpr int32 NumBuckets = 64; // HISTOGRAM_SIZE
	// Every 4th pixel in each direction is plenty for an average.
	constexpr int32 Stride = 4;

	const float LogMin = Settings.HistogramLogMin;
	const float LogMax = FMath::Max(Settings.HistogramLogMax, LogMin + 1e-3f);
	const float LuminanceMin = FMath::Exp2(LogMin);
	const FVector3f Weights = LuminanceWeights();
	// The engine drops the black bucket by default (0), so pixels at or below the histogram's floor don't count.
	const float BlackBucketInfluence = GetCVarFloat(TEXT("r.EyeAdaptation.BlackHistogramBucketInfluence"), 0.f);

	double Histogram[NumBuckets] = {};

	for (const TArray<FFloat16Color>& Pixels : Faces)
	{
		if (Pixels.Num() != FaceSize * FaceSize)
		{
			continue;
		}

		for (int32 Y = 0; Y < FaceSize; Y += Stride)
		{
			// Tangent-plane coordinates; |t| > 1 is overscan that belongs to a neighboring face.
			const float Ty = ((Y + 0.5f) / FaceSize * 2.f - 1.f) * TanHalfFov;
			if (FMath::Abs(Ty) > 1.f)
			{
				continue;
			}

			for (int32 X = 0; X < FaceSize; X += Stride)
			{
				const float Tx = ((X + 0.5f) / FaceSize * 2.f - 1.f) * TanHalfFov;
				if (FMath::Abs(Tx) > 1.f)
				{
					continue;
				}

				// Solid angle of a pixel on a cube face, so the sphere is weighted evenly.
				const float SolidAngle = FMath::Pow(1.f + Tx * Tx + Ty * Ty, -1.5f);
				const FFloat16Color& Pixel = Pixels[Y * FaceSize + X];
				const float Luminance = FMath::Max(Weights.X * Pixel.R.GetFloat() + Weights.Y * Pixel.G.GetFloat() + Weights.Z * Pixel.B.GetFloat(), LuminanceMin);
				const float Position = FMath::Clamp((FMath::Log2(Luminance) - LogMin) / (LogMax - LogMin), 0.f, 1.f);

				// Split between the two buckets that straddle the position, like the engine's histogram pass.
				const float Bucket = Position * (NumBuckets - 1);
				const int32 Bucket0 = FMath::Min((int32)Bucket, NumBuckets - 1);
				const int32 Bucket1 = FMath::Min(Bucket0 + 1, NumBuckets - 1);
				const float Weight1 = Bucket - Bucket0;
				const float Weight0 = (1.f - Weight1) * (Bucket0 == 0 ? BlackBucketInfluence : 1.f);
				Histogram[Bucket0] += Weight0 * SolidAngle;
				Histogram[Bucket1] += Weight1 * SolidAngle;
			}
		}
	}

	double Sum = 0.0;
	for (double Bucket : Histogram)
	{
		Sum += Bucket;
	}

	// Same outlier rejection as the engine's ComputeAverageLuminanceWithoutOutlier().
	double MinFractionSum = Sum * Settings.LowPercent;
	double MaxFractionSum = Sum * Settings.HighPercent;
	double LogSum = 0.0;
	double WeightSum = 0.0;
	for (int32 Index = 0; Index < NumBuckets; ++Index)
	{
		double Value = Histogram[Index];

		const double Sub = FMath::Min(Value, MinFractionSum);
		Value -= Sub;
		MinFractionSum -= Sub;
		MaxFractionSum -= Sub;

		Value = FMath::Min(Value, MaxFractionSum);
		MaxFractionSum -= Value;

		const double LogLuminance = LogMin + (LogMax - LogMin) * Index / double(NumBuckets - 1);
		LogSum += LogLuminance * Value;
		WeightSum += Value;
	}

	// An empty histogram (everything black) averages to log 0, i.e. 1, as in the engine.
	return FMath::Exp2(float(LogSum / FMath::Max(WeightSum, 1e-4)));
}

float PanoExposure::ComputeManualBias(const FPanoExposureSettings& Settings, float AverageLuminance, float UserBias)
{
	// The faces use non-physical manual exposure: exposure = 2^FaceBias / LuminanceMax.
	const float LumMax = LuminanceMax();
	if (Settings.bEyeAdaptationOff)
	{
		// The viewport renders at exposure 1, ignoring every exposure setting.
		return FMath::Log2(LumMax) + UserBias;
	}
	if (Settings.bManual)
	{
		// Level is manual: exposure = 2^Bias / (LuminanceMax * 2^EV100).
		return Settings.Bias - Settings.ManualEV100 + UserBias;
	}

	// Level is auto: exposure = 2^(Bias + curve) * 0.18 / clamp(average, 0.18 * min white, 0.18 * max white).
	float CurveBias = 0.f;
	if (Settings.BiasCurve && AverageLuminance > 0.f)
	{
		// GetAutoExposureCompensationFromCurve(): looked up at the average's EV100, shifted from middle grey to white.
		CurveBias = Settings.BiasCurve->GetFloatValue(FMath::Log2(AverageLuminance / LumMax) + FMath::Log2(1.f / 0.18f));
	}
	const float MaxAverage = 0.18f * LumMax * FMath::Exp2(Settings.MaxEV100);
	const float MinAverage = FMath::Min(0.18f * LumMax * FMath::Exp2(Settings.MinEV100), MaxAverage);
	const float Target = FMath::Clamp(AverageLuminance, MinAverage, MaxAverage);
	return Settings.Bias + CurveBias + FMath::Log2(0.18f / Target) + FMath::Log2(LumMax) + UserBias;
}

float PanoExposure::GetMeteringBias()
{
	return FMath::Log2(LuminanceMax());
}

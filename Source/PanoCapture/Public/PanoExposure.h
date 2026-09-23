// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UCurveFloat;
class UWorld;

/** The exposure settings the viewport would use at a location: project defaults blended with the level's post process volumes. */
struct FPanoExposureSettings
{
	bool bManual = false;
	/** r.EyeAdaptationQuality 0 (low post process scalability) turns exposure off: the viewport shows exposure 1 whatever the settings. */
	bool bEyeAdaptationOff = false;
	/** Exposure compensation, in EV. */
	float Bias = 0.f;
	/** Extra compensation (EV) looked up by scene brightness, like the engine's Exposure Compensation Curve. Auto only. */
	const UCurveFloat* BiasCurve = nullptr;
	/** Auto exposure clamp, in EV100. */
	float MinEV100 = -10.f;
	float MaxEV100 = 20.f;
	/** Histogram outlier rejection, as fractions (0.1 = 10%). */
	float LowPercent = 0.1f;
	float HighPercent = 0.9f;
	/** Histogram range in log2 luminance (already converted from EV100 with the extended range). Darker pixels go to the bottom bucket. */
	float HistogramLogMin = -8.f;
	float HistogramLogMax = 4.f;
	/** Physical camera EV100 for manual exposure. 0 when the physical camera is not applied. */
	float ManualEV100 = 0.f;
};

namespace PanoExposure
{
	PANOCAPTURE_API FPanoExposureSettings GetSettingsAt(const UWorld* World, const FVector& Location);

	/**
	 * Average scene luminance over the full sphere, built like the engine's histogram auto exposure: same buckets, range,
	 * luminance weights (r.AutoExposure.LuminanceMethod), black bucket handling and outlier rejection.
	 * Faces hold linear HDR scene color rendered at exposure 1, laid out like the pano faces (tan(FOV/2) = TanHalfFov).
	 */
	PANOCAPTURE_API float MeterAverageLuminance(TConstArrayView<TArray<FFloat16Color>> Faces, int32 FaceSize, float TanHalfFov, const FPanoExposureSettings& Settings);

	/**
	 * Returns the AutoExposureBias to use with non-physical manual exposure (where exposure = 2^bias) to match
	 * what the engine's auto exposure would settle on for AverageLuminance.
	 */
	PANOCAPTURE_API float ComputeManualBias(const FPanoExposureSettings& Settings, float AverageLuminance, float UserBias);

	/** AutoExposureBias that gives an exposure of exactly 1 with non-physical manual exposure, for metering renders. */
	PANOCAPTURE_API float GetMeteringBias();
}

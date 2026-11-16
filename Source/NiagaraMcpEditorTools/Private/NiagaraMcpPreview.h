#pragma once

#include "CoreMinimal.h"
#include "NiagaraMcpEditorTools.h"
#include "ToolsetRegistry/ToolsetImage.h"

class AActor;
class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;

/**
 * Off-screen previews so an agent can look at what it built. The subject is added to a private preview world
 * (FPreviewScene), simulated there and rendered alone (show-only list, black background, fixed exposure) by a
 * scene capture. The editor level is never touched and nothing is saved.
 */
namespace NiagaraMcp::Preview
{
	struct FCameraSettings
	{
		/** Camera yaw around the subject in degrees; 0 looks along +X (side view). */
		float Yaw = 0.0f;
		/** Camera pitch in degrees; negative looks down. */
		float Pitch = -15.0f;
		/** Distance from the subject's bounds center; 0 frames the bounds automatically. */
		float Distance = 0.0f;
		float FieldOfView = 50.0f;
		/** Black (default), Grey or White (a lit backdrop plane). Dark/translucent smoke disappears on black; additive looks brighter. */
		FString Background = TEXT("Black");
	};

	/** Seconds the preview tools wait for Niagara script / shader compilation before giving up with a retry hint. */
	constexpr double DefaultMaxWaitSeconds = 90.0;

	/** Simulation times are clamped to 0..MaxSimulationSeconds (a longer time would tick the game thread for minutes). */
	constexpr float MaxSimulationSeconds = 600.0f;

	/** Waits for the system's script compilation (engine wait; script compiles take seconds, shader waits are bounded). */
	bool WaitForSystemCompile(UNiagaraSystem& System, double MaxSeconds, FString& OutError);

	/** Waits (bounded) for outstanding shader compile jobs. False with OutError (job count + retry hint) on timeout. */
	bool WaitForShaderJobs(double MaxSeconds, FString& OutError);

	/**
	 * Simulates ONE run of the component through the given times (seconds since activation), visiting them in time
	 * order: reset once, then advance by the difference between consecutive times. Every sample of one call therefore
	 * comes from the same simulation (random spawns don't change between frames) and the cost is the largest time,
	 * not the sum. Visit gets the caller's index of each time.
	 */
	void SimulateInOrder(UNiagaraComponent& Component, const TArray<float>& Times, float TickDelta, TFunctionRef<void(int32 Index, float Seconds)> Visit);

	// ------------------------------------------------------------------------------------------ Quality level

	/**
	 * Parses a Niagara quality level: Current / None / Default (= INDEX_NONE, the editor's level), Low, Medium, High,
	 * Epic, Cinematic (or the project's own quality level names) or a number 0-4. False with OutError otherwise.
	 */
	bool ParseQualityLevel(const FString& Text, int32& OutLevel, FString& OutError);

	/** Display name of a quality level ("Epic"); INDEX_NONE gives the current level's name. */
	FString QualityLevelName(int32 Level);

	/**
	 * Applies a Niagara quality level override (FNiagaraPlatformSet::SetNiagaraQualityLevelOverride) for its lifetime
	 * and restores the previous state (a previous override, or none) on destruction. INDEX_NONE does nothing.
	 * Changing the level re-evaluates scalability of every loaded system once on apply and once on restore.
	 */
	class FScopedQualityLevel
	{
	public:
		explicit FScopedQualityLevel(int32 Level);
		~FScopedQualityLevel();
		FScopedQualityLevel(const FScopedQualityLevel&) = delete;
		FScopedQualityLevel& operator=(const FScopedQualityLevel&) = delete;

	private:
		bool bApplied = false;
		int32 PreviousOverride = INDEX_NONE;
	};

	// ------------------------------------------------------------------------------------------ Niagara shots

	/** GPU cost view modes for CaptureNiagaraShots. */
	enum class ECostView : uint8
	{
		None,
		ShaderComplexity,
	};

	struct FShotRequest
	{
		/** Seconds since activation, caller order (visited in time order in one continuous run). Must not be empty. */
		TArray<float> Times;
		/** Camera yaws; every time is shot from each yaw. Must not be empty (Camera.Yaw is ignored). */
		TArray<float> Yaws;
		int32 FrameSize = 256;
		FCameraSettings Camera;
		/** Quality level override for the run (INDEX_NONE = current). */
		int32 QualityLevel = INDEX_NONE;
		/** Use Framing instead of the union of the effect's bounds (re-render with identical framing). */
		bool bFixedFraming = false;
		FBoxSphereBounds Framing = FBoxSphereBounds(FVector::ZeroVector, FVector(200.0f), 200.0f);
		ECostView CostView = ECostView::None;
	};

	struct FShot
	{
		int32 TimeIndex = 0;
		int32 YawIndex = 0;
		float Time = 0.0f;
		float Yaw = 0.0f;
		/** FrameSize * FrameSize pixels, opaque. */
		TArray<FColor> Pixels;
		/** Particle counts at Time from the same run. */
		FNiagaraMcpParticleSample Counts;
	};

	struct FShotResult
	{
		/** Time-major in caller order: Shots[TimeIndex * Yaws.Num() + YawIndex]. */
		TArray<FShot> Shots;
		/** The empty background (effect hidden) per yaw; "covered" pixels differ from it. */
		TArray<TArray<FColor>> Backgrounds;
		/**
		 * Per yaw: the coverage threshold (0-255 per channel) to use against Backgrounds. The renderer adds a little
		 * temporal noise to empty frames (up to ~13/255 near black), so it is measured from two background shots and
		 * the threshold is max(CoverageThreshold, noise + 2).
		 */
		TArray<int32> Thresholds;
		FBoxSphereBounds Framing;
		int32 FrameSize = 0;
		/** Name of the quality level the run used. */
		FString QualityLevelName;
	};

	/**
	 * Simulates the system ONCE through Request.Times and renders every time from every yaw (plus one empty
	 * background frame per yaw) off-screen. Returns false with OutError on failure (invalid system, unknown
	 * background, shader compile timeout, no rendering).
	 */
	bool CaptureNiagaraShots(UNiagaraSystem& System, const FShotRequest& Request, FShotResult& OutResult, FString& OutError);

	/**
	 * Simulates the system to each time (seconds since activation) and renders one frame per time into a grid
	 * (left to right, top to bottom, up to 4 per row). Returns false with OutError on failure.
	 */
	bool CaptureNiagaraFrames(UNiagaraSystem& System, const TArray<float>& Times, int32 FrameSize, const FCameraSettings& Camera, FToolsetImage& OutImage, FString& OutError);

	/** Simulates the system off-screen and records particle counts per emitter at each time (optionally at a quality level). */
	bool SimulateParticleCounts(UNiagaraSystem& System, const TArray<float>& Times, TArray<FNiagaraMcpParticleSample>& OutSamples, FString& OutError, int32 QualityLevel = INDEX_NONE);

	/**
	 * Renders a material on a preview mesh ("Plane", "Sphere", "Cube" or a static mesh path). With FrameCount > 1
	 * the frames are rendered at preview-world times 0, FrameInterval, 2 * FrameInterval, ... (seconds; drives Time,
	 * Panner, flipbooks) into a grid of up to 4 frames per row. A single frame is rendered at time 0.
	 */
	bool CaptureMaterial(UMaterialInterface& Material, const FString& Mesh, int32 Size, const FCameraSettings& Camera, int32 FrameCount, float FrameInterval, FToolsetImage& OutImage, FString& OutError);

	// ------------------------------------------------------------------------------------------ CPU image helpers

	/** A pixel is "covered" when any channel differs from the background pixel by more than this (0-255). */
	constexpr int32 CoverageThreshold = 6;

	inline bool IsCovered(const FColor& Pixel, const FColor& Background, int32 Threshold = CoverageThreshold)
	{
		return FMath::Abs(Pixel.R - Background.R) > Threshold || FMath::Abs(Pixel.G - Background.G) > Threshold || FMath::Abs(Pixel.B - Background.B) > Threshold;
	}

	/** Luminance (Rec. 709 weights on the sRGB values), 0-1. */
	inline float Luminance(const FColor& Pixel)
	{
		return (0.2126f * Pixel.R + 0.7152f * Pixel.G + 0.0722f * Pixel.B) / 255.0f;
	}

	/** Fills the pixel statistics of Out (Coverage .. bEmpty; not Hint, Time, counts or tile position). */
	void ComputeFrameStats(const TArray<FColor>& Pixels, const TArray<FColor>& Background, int32 Size, FNiagaraMcpFrameStats& Out, int32 Threshold = CoverageThreshold);

	/**
	 * Loads an image file (PNG, JPG, BMP, TGA, EXR, ...) and fits it into a Size x Size opaque tile (aspect kept,
	 * centered on black). False with OutError for a missing or non-image file.
	 */
	bool LoadImageTile(const FString& Path, int32 Size, TArray<FColor>& OutPixels, FIntPoint& OutSourceSize, FString& OutError);

	/** Parses "0, 90, 180" (also "[0, 90]"); unset text (None / Auto / "") gives { Fallback }. At most 8 yaws. */
	bool ParseYawList(const FString& Text, float Fallback, TArray<float>& OutYaws, FString& OutError);

	/** A grid of square tiles composed on the CPU. */
	struct FSheet
	{
		int32 Columns = 1;
		int32 Rows = 1;
		int32 TileSize = 0;
		TArray<FColor> Pixels;

		void Init(int32 InColumns, int32 InRows, int32 InTileSize, FColor Fill = FColor::Black);
		int32 Width() const { return Columns * TileSize; }
		int32 Height() const { return Rows * TileSize; }
		void Paste(int32 Column, int32 Row, const TArray<FColor>& Tile);
		bool Encode(FToolsetImage& OutImage) const;
	};
}

/** The v0.5 vision tools built on the preview helpers (implemented in NiagaraMcpEditorTools_Vision.cpp). */
namespace NiagaraMcp::Vision
{
	struct FPreviewArgs
	{
		TArray<float> Times;
		int32 FrameSize = 320;
		Preview::FCameraSettings Camera;
		FString CameraYaws = TEXT("None");
		FString QualityLevel = TEXT("Current");
		FString ReferenceImage = TEXT("None");
	};

	/** CapturePreview: contact sheet + per-tile stats + caption. False with OutError on failure. */
	bool CapturePreview(UNiagaraSystem& System, const FPreviewArgs& Args, FNiagaraMcpPreviewResult& OutResult, FString& OutError);
}

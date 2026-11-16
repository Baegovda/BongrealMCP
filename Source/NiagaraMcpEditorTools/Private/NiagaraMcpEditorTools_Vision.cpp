// v0.5 VISION: per-tile image statistics, multi-yaw / quality level / reference previews (CapturePreview), visual
// baselines (ComparePreview), GPU cost view modes (CaptureCostView), flipbook baking (BakeFlipbook) and texture
// channel views (UAssetMcpEditorTools::ViewTexture). Everything renders off-screen in a private preview world.

#include "AssetMcpEditorTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraMcpEditorToolsInternal.h"
#include "NiagaraMcpPreview.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

using namespace NiagaraMcp;

/**
 * File-local helpers in a named namespace (called qualified), so unity builds that pull in other files'
 * "using namespace" directives cannot make them ambiguous.
 */
namespace NiagaraMcpVision
{
	using NiagaraMcp::Preview::FShot;
	using NiagaraMcp::Preview::FShotRequest;
	using NiagaraMcp::Preview::FShotResult;
	using NiagaraMcp::Preview::FSheet;

	static const TArray<float> DefaultTimes = { 0.25f, 0.5f, 1.0f, 2.0f };
	constexpr int32 MaxTimes = 12;
	constexpr int32 MaxSheetSize = 8192;

	/** Where every shot (time-major) and the optional reference tile go in the sheet. */
	struct FLayout
	{
		int32 Columns = 1;
		int32 Rows = 1;
		bool bReference = false;
		FIntPoint ReferenceCell = FIntPoint(0, 0);
		TArray<FIntPoint> Cells;

		int32 TileIndex(const FIntPoint& Cell) const { return Cell.Y * Columns + Cell.X; }

		FString Describe() const
		{
			return FString::Printf(TEXT("%d row%s x %d column%s"), Rows, Rows == 1 ? TEXT("") : TEXT("s"), Columns, Columns == 1 ? TEXT("") : TEXT("s"));
		}
	};

	/**
	 * One yaw: tiles left to right, top to bottom in the order of Times (up to 4 per row), reference first.
	 * Several yaws: one row per time, one column per yaw; the reference gets a row of its own on top.
	 */
	FLayout MakeLayout(int32 NumTimes, int32 NumYaws, bool bReference)
	{
		FLayout Layout;
		Layout.bReference = bReference;
		if (NumYaws <= 1)
		{
			const int32 Count = NumTimes + (bReference ? 1 : 0);
			Layout.Columns = FMath::Clamp(Count, 1, 4);
			Layout.Rows = FMath::DivideAndRoundUp(Count, Layout.Columns);
			const int32 First = bReference ? 1 : 0;
			for (int32 Index = 0; Index < NumTimes; ++Index)
			{
				Layout.Cells.Add(FIntPoint((First + Index) % Layout.Columns, (First + Index) / Layout.Columns));
			}
		}
		else
		{
			Layout.Columns = NumYaws;
			Layout.Rows = NumTimes + (bReference ? 1 : 0);
			const int32 FirstRow = bReference ? 1 : 0;
			for (int32 Time = 0; Time < NumTimes; ++Time)
			{
				for (int32 Yaw = 0; Yaw < NumYaws; ++Yaw)
				{
					Layout.Cells.Add(FIntPoint(Yaw, FirstRow + Time));
				}
			}
		}
		return Layout;
	}

	/** Fills a shot request from tool arguments. False (error reported) on invalid input. */
	bool BuildRequest(const TArray<float>& InTimes, int32 FrameSize, float CameraYaw, float CameraPitch, float CameraDistance, const FString& Background,
		const FString& CameraYaws, const FString& QualityLevel, int32 ExtraTiles, FShotRequest& OutRequest)
	{
		OutRequest.Times = InTimes.Num() ? InTimes : DefaultTimes;
		if (OutRequest.Times.Num() > MaxTimes)
		{
			ReportError(FString::Printf(TEXT("At most %d times per call (got %d)."), MaxTimes, OutRequest.Times.Num()));
			return false;
		}
		for (float Time : OutRequest.Times)
		{
			if (!(Time >= 0.0f && Time <= 600.0f))
			{
				ReportError(FString::Printf(TEXT("Times must be 0-600 seconds (got %s)."), *FString::SanitizeFloat(Time)));
				return false;
			}
		}
		if (FrameSize < 64 || FrameSize > 1024)
		{
			ReportError(FString::Printf(TEXT("FrameSize must be 64-1024 (got %d)."), FrameSize));
			return false;
		}
		FString Error;
		if (!Preview::ParseYawList(CameraYaws, CameraYaw, OutRequest.Yaws, Error) || !Preview::ParseQualityLevel(QualityLevel, OutRequest.QualityLevel, Error))
		{
			ReportError(Error);
			return false;
		}
		const FLayout Layout = MakeLayout(OutRequest.Times.Num(), OutRequest.Yaws.Num(), ExtraTiles > 0);
		if (Layout.Columns * FrameSize > MaxSheetSize || Layout.Rows * FrameSize > MaxSheetSize)
		{
			ReportError(FString::Printf(TEXT("The sheet would be %dx%d px (max %d). Use a smaller FrameSize, fewer Times or fewer CameraYaws."),
				Layout.Columns * FrameSize, Layout.Rows * FrameSize, MaxSheetSize));
			return false;
		}
		OutRequest.FrameSize = FrameSize;
		OutRequest.Camera.Yaw = OutRequest.Yaws[0];
		OutRequest.Camera.Pitch = CameraPitch;
		OutRequest.Camera.Distance = CameraDistance;
		OutRequest.Camera.Background = IsUnset(Background) ? FString(TEXT("Black")) : Background;
		return true;
	}

	/** Emitter name -> has at least one enabled renderer (enabled emitters with editable data only). */
	TMap<FName, bool> EmitterRendererState(const UNiagaraSystem& System)
	{
		TMap<FName, bool> State;
		for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr || !Handle.GetIsEnabled())
			{
				continue;
			}
			bool bAny = false;
			Data->ForEachEnabledRenderer([&bAny](UNiagaraRendererProperties*) { bAny = true; });
			State.Add(Handle.GetName(), bAny);
		}
		return State;
	}

	FString Fmt(double Value, int32 Decimals = 1)
	{
		FString Text = FString::Printf(TEXT("%.*f"), Decimals, Value);
		if (Text.Contains(TEXT(".")))
		{
			Text.TrimEndInline();
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			Text.RemoveFromEnd(TEXT("."));
		}
		return Text;
	}

	/** A diagnosis for a suspicious tile; empty when it looks fine. */
	FString MakeHint(const FNiagaraMcpFrameStats& Frame, const TMap<FName, bool>& Renderers, const FString& Background)
	{
		if (Frame.bEmpty)
		{
			if (Frame.Particles == 0)
			{
				return TEXT("0 particles: spawn/lifetime problem (nothing alive at this time: check the spawn modules (rate / burst timing), Lifetime, emitter enabled / scalability and loop settings; GetParticleCounts shows the timeline)");
			}
			TArray<FString> NoRenderer;
			for (const FNiagaraMcpEmitterCount& Count : Frame.Emitters)
			{
				const bool* bHasRenderer = Renderers.Find(Count.Emitter);
				if (Count.Particles > 0 && bHasRenderer && !*bHasRenderer)
				{
					NoRenderer.Add(Count.Emitter.ToString());
				}
			}
			if (NoRenderer.Num() > 0)
			{
				return FString::Printf(TEXT("%d particles but nothing drawn: emitter %s has no enabled renderer"), Frame.Particles, *FString::Join(NoRenderer, TEXT(", ")));
			}
			FString Hint = FString::Printf(TEXT("%d particles but nothing drawn: material, alpha, size, facing or bounds (check the renderer material, Color alpha, SpriteSize / MeshScale, sprite facing / alignment, fixed bounds)"), Frame.Particles);
			if (Background.IsEmpty() || Background.Equals(TEXT("Black"), ESearchCase::IgnoreCase))
			{
				Hint += TEXT("; dark or translucent smoke is invisible on black: retry with Background Grey");
			}
			return Hint;
		}
		TArray<FString> Hints;
		if (Frame.Coverage > 85.0f)
		{
			Hints.Add(TEXT("fills almost the whole frame: the camera may be inside the effect or the bounds are huge (set CameraDistance)"));
		}
		if (Frame.ClippedShare > 40.0f && Frame.Coverage > 0.5f)
		{
			Hints.Add(FString::Printf(TEXT("%s%% of the drawn pixels are clipped at 255: overexposed, lower the color / emissive intensity"), *Fmt(Frame.ClippedShare, 0)));
		}
		if (Frame.PeakLuminance < 0.06f)
		{
			Hints.Add(TEXT("very dark, barely visible on this background (try Background Grey or a brighter color)"));
		}
		TArray<FString> Bounds;
		Frame.ScreenBounds.ParseIntoArray(Bounds, TEXT(","), true);
		if (Frame.Coverage <= 85.0f && Bounds.Num() == 4
			&& (FCString::Atof(*Bounds[0]) <= 0.0f || FCString::Atof(*Bounds[1]) <= 0.0f || FCString::Atof(*Bounds[2]) >= 1.0f || FCString::Atof(*Bounds[3]) >= 1.0f))
		{
			Hints.Add(TEXT("cut off at the frame edge: particles leave the effect's bounds (fixed bounds too small?) or set CameraDistance"));
		}
		return FString::Join(Hints, TEXT("; "));
	}

	FString DescribeFrame(const FNiagaraMcpFrameStats& Frame)
	{
		FString Line = FString::Printf(TEXT("Tile %d (row %d, col %d) t=%ss yaw %s: %s"), Frame.Tile, Frame.Row, Frame.Column, *Fmt(Frame.Time, 2), *Fmt(Frame.CameraYaw, 0),
			*FString::Printf(TEXT("%d particles"), Frame.Particles));
		if (Frame.Emitters.Num() > 1)
		{
			TArray<FString> Parts;
			for (const FNiagaraMcpEmitterCount& Count : Frame.Emitters)
			{
				Parts.Add(FString::Printf(TEXT("%s %d"), *Count.Emitter.ToString(), Count.Particles));
			}
			Line += FString::Printf(TEXT(" (%s)"), *FString::Join(Parts, TEXT(", ")));
		}
		if (Frame.bEmpty)
		{
			Line += TEXT("; EMPTY (nothing drawn)");
		}
		else
		{
			Line += FString::Printf(TEXT("; coverage %s%%, luminance mean %s peak %s, clipped %s%%, color %s, screen bounds %s"),
				*Fmt(Frame.Coverage, 1), *Fmt(Frame.MeanLuminance, 2), *Fmt(Frame.PeakLuminance, 2), *Fmt(Frame.ClippedShare, 1), *Frame.MeanColor, *Frame.ScreenBounds);
		}
		if (!Frame.Hint.IsEmpty())
		{
			Line += TEXT(". HINT: ") + Frame.Hint;
		}
		return Line + TEXT(".");
	}

	FString DescribeSetup(const UNiagaraSystem& System, const FShotRequest& Request, const FShotResult& Shots, const FLayout& Layout)
	{
		FString Order = Request.Yaws.Num() > 1
			? FString(TEXT("each row is one time in the order of Times, each column one camera yaw"))
			: FString(TEXT("left to right, top to bottom in the order of Times"));
		return FString::Printf(TEXT("%s: %d tiles in %s, %d px each (%s). Framing radius %s cm around (%s), camera pitch %s, distance %s, background %s, quality level %s; all tiles from ONE continuous simulation run."),
			*System.GetName(), Layout.Cells.Num() + (Layout.bReference ? 1 : 0), *Layout.Describe(), Shots.FrameSize, *Order,
			*Fmt(Shots.Framing.SphereRadius, 0), *FString::Printf(TEXT("%.0f, %.0f, %.0f"), Shots.Framing.Origin.X, Shots.Framing.Origin.Y, Shots.Framing.Origin.Z),
			*Fmt(Request.Camera.Pitch, 0), Request.Camera.Distance > 0.0f ? *FString::Printf(TEXT("%s cm"), *Fmt(Request.Camera.Distance, 0)) : TEXT("auto"),
			*Request.Camera.Background, *Shots.QualityLevelName);
	}

	int32 ThresholdFor(const FShotResult& Shots, int32 YawIndex)
	{
		return Shots.Thresholds.IsValidIndex(YawIndex) ? Shots.Thresholds[YawIndex] : Preview::CoverageThreshold;
	}

	FNiagaraMcpFrameStats MakeFrameStats(const FShot& Shot, const FShotResult& Shots, const FLayout& Layout, int32 ShotIndex)
	{
		FNiagaraMcpFrameStats Frame;
		const FIntPoint Cell = Layout.Cells[ShotIndex];
		Frame.Tile = Layout.TileIndex(Cell);
		Frame.Row = Cell.Y;
		Frame.Column = Cell.X;
		Frame.Time = Shot.Time;
		Frame.CameraYaw = Shot.Yaw;
		Frame.Particles = Shot.Counts.TotalParticles;
		Frame.Emitters = Shot.Counts.Emitters;
		Preview::ComputeFrameStats(Shot.Pixels, Shots.Backgrounds.IsValidIndex(Shot.YawIndex) ? Shots.Backgrounds[Shot.YawIndex] : TArray<FColor>(), Shots.FrameSize, Frame, ThresholdFor(Shots, Shot.YawIndex));
		return Frame;
	}

	// ---------------------------------------------------------------------------------------------- Baselines

	struct FBaseline
	{
		FString SystemPath;
		FShotRequest Request;
		TArray<TArray<FColor>> Pixels;
		TArray<TArray<FColor>> Backgrounds;
		TArray<float> Coverage;
		TArray<int32> Particles;
		uint64 LastUsed = 0;
	};

	constexpr int32 MaxBaselines = 8;
	TMap<FString, FBaseline> GBaselines;
	uint64 GBaselineClock = 0;

	void StoreBaseline(const FString& Key, FBaseline&& Baseline)
	{
		Baseline.LastUsed = ++GBaselineClock;
		GBaselines.Add(Key, MoveTemp(Baseline));
		// Bounded by count AND memory (a baseline keeps raw pixels): at most 256 MB in total.
		auto TotalBytes = []()
		{
			int64 Bytes = 0;
			for (const TPair<FString, FBaseline>& Pair : GBaselines)
			{
				for (const TArray<FColor>& Tile : Pair.Value.Pixels) { Bytes += Tile.Num() * sizeof(FColor); }
				for (const TArray<FColor>& Tile : Pair.Value.Backgrounds) { Bytes += Tile.Num() * sizeof(FColor); }
			}
			return Bytes;
		};
		while (GBaselines.Num() > MaxBaselines || (GBaselines.Num() > 1 && TotalBytes() > 256ll * 1024 * 1024))
		{
			FString Oldest;
			uint64 OldestUse = MAX_uint64;
			for (const TPair<FString, FBaseline>& Pair : GBaselines)
			{
				if (Pair.Value.LastUsed < OldestUse)
				{
					OldestUse = Pair.Value.LastUsed;
					Oldest = Pair.Key;
				}
			}
			GBaselines.Remove(Oldest);
		}
	}

	constexpr int32 ChangeThreshold = 12;

	bool PixelChanged(const FColor& A, const FColor& B, int32 Threshold)
	{
		return FMath::Abs(A.R - B.R) > Threshold || FMath::Abs(A.G - B.G) > Threshold || FMath::Abs(A.B - B.B) > Threshold;
	}

	// ---------------------------------------------------------------------------------------------- Cost view

	/** Inverts the view mode's color ramp: rendered color -> cost index 0-1. */
	struct FCostScale
	{
		TArray<FColor> Colors;
		TArray<float> Values;
		float BadFrom = 0.28f;
		float ExtremeFrom = 0.67f;
		TMap<uint32, TPair<float, int32>> Cache;

		/** Nearest ramp entry: (value, squared RGB distance). */
		TPair<float, int32> Lookup(const FColor& Pixel)
		{
			const uint32 Key = (uint32(Pixel.R) << 16) | (uint32(Pixel.G) << 8) | Pixel.B;
			if (const TPair<float, int32>* Found = Cache.Find(Key))
			{
				return *Found;
			}
			int32 Best = MAX_int32;
			float Value = 0.0f;
			for (int32 Index = 0; Index < Colors.Num(); ++Index)
			{
				const int32 DR = Pixel.R - Colors[Index].R, DG = Pixel.G - Colors[Index].G, DB = Pixel.B - Colors[Index].B;
				const int32 Distance = DR * DR + DG * DG + DB * DB;
				if (Distance < Best)
				{
					Best = Distance;
					Value = Values[Index];
				}
			}
			return Cache.Add(Key, TPair<float, int32>(Value, Best));
		}
	};

	FColor EncodeRampColor(const FLinearColor& Color, bool bSRGB)
	{
		return bSRGB ? Color.ToFColor(true) : FLinearColor(Color.R, Color.G, Color.B, 1.0f).QuantizeRound();
	}

	/** Replicates ColorizeComplexity (ShaderComplexityApplyPixelShader.usf, ramp mode) on the CPU. */
	FLinearColor ShaderComplexityRamp(const TArray<FLinearColor>& Colors, float Complexity)
	{
		const int32 Count = Colors.Num();
		if (Count < 2)
		{
			return FLinearColor::Black;
		}
		auto At = [&Colors, Count](int32 Index) { return Colors[FMath::Clamp(Index, 0, Count - 1)]; };
		Complexity = FMath::Clamp(Complexity, 0.0f, 0.999f);
		float Expanded = FMath::Min(static_cast<float>(Count) - 2.0f, Complexity * 18.0f);
		FLinearColor Low, High;
		if (Expanded < 6.0f)
		{
			const int32 Index = FMath::FloorToInt(Expanded);
			Low = At(Index);
			High = At(Index + 1);
		}
		else
		{
			Expanded = (Complexity - 0.33333333f) * (Count <= 8 ? 1.5f : 3.0f);
			if (Expanded <= 1.0f)
			{
				Low = At(6);
				High = At(7);
			}
			else
			{
				Low = At(7);
				High = At(8);
			}
		}
		return FMath::Lerp(Low, High, FMath::Frac(Expanded));
	}

	FCostScale MakeCostScale(bool bSRGB)
	{
		FCostScale Scale;
		const TArray<FLinearColor>& Colors = GEngine->ShaderComplexityColors;
		for (int32 Step = 0; Step <= 999; ++Step)
		{
			const float Complexity = Step / 1000.0f;
			Scale.Colors.Add(EncodeRampColor(ShaderComplexityRamp(Colors, Complexity), bSRGB));
			Scale.Values.Add(Complexity);
		}
		Scale.BadFrom = 5.0f / 18.0f;      // dark red (color 5) onwards
		Scale.ExtremeFrom = 2.0f / 3.0f;   // pink -> white
		return Scale;
	}

	/**
	 * Only ShaderComplexity: QuadOverdraw (DVSM_QuadComplexity) in a scene capture never finished in 5.8 tests (the
	 * capture stalled while the editor's memory grew past 20 GB), so it is rejected with an explanation.
	 */
	bool ParseCostMode(const FString& Text, FString& OutError)
	{
		const FString Value = Text.TrimStartAndEnd();
		if (Value.IsEmpty() || IsUnset(Value) || LooseNameEquals(Value, TEXT("ShaderComplexity")) || LooseNameEquals(Value, TEXT("Complexity")))
		{
			return true;
		}
		if (LooseNameEquals(Value, TEXT("QuadOverdraw")) || LooseNameEquals(Value, TEXT("Overdraw")))
		{
			OutError = TEXT("Mode QuadOverdraw is not available off-screen (the scene capture cannot render it). Use ShaderComplexity; its red / white areas also show translucent overdraw.");
			return false;
		}
		OutError = FString::Printf(TEXT("Unknown Mode '%s'. Use ShaderComplexity."), *Text);
		return false;
	}

	// ---------------------------------------------------------------------------------------------- Textures

	bool IsFloatFormat(ERawImageFormat::Type Format)
	{
		return Format == ERawImageFormat::RGBA16F || Format == ERawImageFormat::RGBA32F || Format == ERawImageFormat::R16F || Format == ERawImageFormat::R32F;
	}
}

// ================================================================================================== CapturePreview

bool NiagaraMcp::Vision::CapturePreview(UNiagaraSystem& System, const FPreviewArgs& Args, FNiagaraMcpPreviewResult& OutResult, FString& OutError)
{
	using namespace NiagaraMcpVision;
	OutResult = FNiagaraMcpPreviewResult();

	// Validate everything before rendering.
	FShotRequest Request;
	Request.Times = Args.Times.Num() ? Args.Times : DefaultTimes;
	if (Request.Times.Num() > MaxTimes)
	{
		OutError = FString::Printf(TEXT("At most %d frames per preview (got %d)."), MaxTimes, Request.Times.Num());
		return false;
	}
	if (Args.FrameSize < 64 || Args.FrameSize > 1024)
	{
		OutError = FString::Printf(TEXT("FrameSize must be 64-1024 (got %d)."), Args.FrameSize);
		return false;
	}
	if (!Preview::ParseYawList(Args.CameraYaws, Args.Camera.Yaw, Request.Yaws, OutError) || !Preview::ParseQualityLevel(Args.QualityLevel, Request.QualityLevel, OutError))
	{
		return false;
	}
	const bool bReference = !IsUnset(Args.ReferenceImage);
	TArray<FColor> ReferencePixels;
	FIntPoint ReferenceSize(0, 0);
	if (bReference && !Preview::LoadImageTile(Args.ReferenceImage, Args.FrameSize, ReferencePixels, ReferenceSize, OutError))
	{
		return false;
	}
	const FLayout Layout = MakeLayout(Request.Times.Num(), Request.Yaws.Num(), bReference);
	if (Layout.Columns * Args.FrameSize > MaxSheetSize || Layout.Rows * Args.FrameSize > MaxSheetSize)
	{
		OutError = FString::Printf(TEXT("The sheet would be %dx%d px (max %d). Use a smaller FrameSize, fewer Times or fewer CameraYaws."), Layout.Columns * Args.FrameSize, Layout.Rows * Args.FrameSize, MaxSheetSize);
		return false;
	}
	Request.FrameSize = Args.FrameSize;
	Request.Camera = Args.Camera;

	FShotResult Shots;
	if (!Preview::CaptureNiagaraShots(System, Request, Shots, OutError))
	{
		return false;
	}

	FSheet Sheet;
	Sheet.Init(Layout.Columns, Layout.Rows, Shots.FrameSize);
	TArray<FString> Lines;
	Lines.Add(DescribeSetup(System, Request, Shots, Layout));
	if (bReference)
	{
		Sheet.Paste(Layout.ReferenceCell.X, Layout.ReferenceCell.Y, ReferencePixels);
		Lines.Add(FString::Printf(TEXT("Tile 0 (row 0, col 0): REFERENCE image %s (%dx%d), not rendered."), *Args.ReferenceImage.TrimStartAndEnd(), ReferenceSize.X, ReferenceSize.Y));
	}
	const TMap<FName, bool> Renderers = EmitterRendererState(System);
	int32 EmptyTiles = 0;
	for (int32 Index = 0; Index < Shots.Shots.Num(); ++Index)
	{
		const FShot& Shot = Shots.Shots[Index];
		FNiagaraMcpFrameStats Frame = MakeFrameStats(Shot, Shots, Layout, Index);
		Frame.Hint = MakeHint(Frame, Renderers, Request.Camera.Background);
		EmptyTiles += Frame.bEmpty ? 1 : 0;
		Sheet.Paste(Layout.Cells[Index].X, Layout.Cells[Index].Y, Shot.Pixels);
		Lines.Add(DescribeFrame(Frame));
		OutResult.Frames.Add(MoveTemp(Frame));
	}
	if (EmptyTiles == Shots.Shots.Num())
	{
		Lines.Add(TEXT("Every tile is empty: see the hints above."));
	}
	if (!Sheet.Encode(OutResult.Image))
	{
		OutError = TEXT("Encoding the preview image failed.");
		return false;
	}
	OutResult.Caption = FString::Join(Lines, TEXT("\n"));
	OutResult.FramingRadius = Shots.Framing.SphereRadius;
	OutResult.Columns = Layout.Columns;
	OutResult.Rows = Layout.Rows;
	OutResult.QualityLevel = Shots.QualityLevelName;
	return true;
}

// ================================================================================================== ComparePreview

FNiagaraMcpPreviewDiff UNiagaraMcpEditorTools::ComparePreview(UNiagaraSystem* System, const FString& BaselineName, const TArray<float>& Times, int32 FrameSize, float CameraYaw, float CameraPitch, float CameraDistance, const FString& Background, const FString& CameraYaws, const FString& QualityLevel, bool bResetBaseline)
{
	using namespace NiagaraMcpVision;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	const FString Name = BaselineName.TrimStartAndEnd();
	if (Name.IsEmpty() || IsUnset(Name))
	{
		ReportError(TEXT("BaselineName is required, e.g. \"before_color\"."));
		return {};
	}
	const FString Key = Name.ToLower();
	FBaseline* Existing = bResetBaseline ? nullptr : GBaselines.Find(Key);

	FShotRequest Request;
	if (Existing)
	{
		Request = Existing->Request;
	}
	else if (!BuildRequest(Times, FrameSize, CameraYaw, CameraPitch, CameraDistance, Background, CameraYaws, QualityLevel, 0, Request))
	{
		return {};
	}
	System->RequestCompile(false); // catch up on uncompiled edits (no-op when in sync)

	FShotResult Shots;
	FString Error;
	if (!Preview::CaptureNiagaraShots(*System, Request, Shots, Error))
	{
		ReportError(Error);
		return {};
	}
	const FLayout Layout = MakeLayout(Request.Times.Num(), Request.Yaws.Num(), false);
	FNiagaraMcpPreviewDiff Result;
	Result.BaselineName = Name;
	FSheet Sheet;
	Sheet.Init(Layout.Columns, Layout.Rows, Shots.FrameSize);
	TArray<FString> Lines;

	if (Existing == nullptr)
	{
		// New baseline: store the pixels with the exact camera and framing.
		FBaseline Baseline;
		Baseline.SystemPath = System->GetPathName();
		Baseline.Request = Request;
		Baseline.Request.bFixedFraming = true;
		Baseline.Request.Framing = Shots.Framing;
		Baseline.Backgrounds = Shots.Backgrounds;
		for (int32 Index = 0; Index < Shots.Shots.Num(); ++Index)
		{
			const FShot& Shot = Shots.Shots[Index];
			const FNiagaraMcpFrameStats Frame = MakeFrameStats(Shot, Shots, Layout, Index);
			Baseline.Pixels.Add(Shot.Pixels);
			Baseline.Coverage.Add(Frame.Coverage);
			Baseline.Particles.Add(Shot.Counts.TotalParticles);
			Sheet.Paste(Layout.Cells[Index].X, Layout.Cells[Index].Y, Shot.Pixels);
		}
		StoreBaseline(Key, MoveTemp(Baseline));
		Result.bBaselineCreated = true;
		Lines.Add(FString::Printf(TEXT("Baseline '%s' stored for %s: %d tiles in %s (%d px), framing radius %s cm, quality %s. Edit the system, then call ComparePreview with the same BaselineName to see what changed."),
			*Name, *System->GetName(), Shots.Shots.Num(), *Layout.Describe(), Shots.FrameSize, *Fmt(Shots.Framing.SphereRadius, 0), *Shots.QualityLevelName));
	}
	else
	{
		Existing->LastUsed = ++GBaselineClock;
		Lines.Add(FString::Printf(TEXT("Compared %s with baseline '%s'%s: %d tiles in %s (%d px), same camera and framing as the baseline. Changed pixels are tinted red."),
			*System->GetName(), *Name, Existing->SystemPath == System->GetPathName() ? TEXT("") : *FString::Printf(TEXT(" (rendered from %s)"), *Existing->SystemPath),
			Shots.Shots.Num(), *Layout.Describe(), Shots.FrameSize));
		const int32 Size = Shots.FrameSize;
		for (int32 Index = 0; Index < Shots.Shots.Num(); ++Index)
		{
			const FShot& Shot = Shots.Shots[Index];
			const TArray<FColor>& Before = Existing->Pixels.IsValidIndex(Index) ? Existing->Pixels[Index] : Shot.Pixels;
			const FNiagaraMcpFrameStats Frame = MakeFrameStats(Shot, Shots, Layout, Index);
			FNiagaraMcpTileDiff& Diff = Result.Tiles.AddDefaulted_GetRef();
			Diff.Tile = Frame.Tile;
			Diff.Time = Shot.Time;
			Diff.CameraYaw = Shot.Yaw;
			Diff.Coverage = Frame.Coverage;
			Diff.Particles = Shot.Counts.TotalParticles;
			Diff.BaselineCoverage = Existing->Coverage.IsValidIndex(Index) ? Existing->Coverage[Index] : 0.0f;
			Diff.BaselineParticles = Existing->Particles.IsValidIndex(Index) ? Existing->Particles[Index] : 0;

			// Pixels that differ by less than the renderer's own frame-to-frame noise do not count as changed.
			const int32 Threshold = FMath::Max(ChangeThreshold, ThresholdFor(Shots, Shot.YawIndex));
			TArray<FColor> Tinted = Shot.Pixels;
			int64 Changed = 0;
			double SumR = 0.0, SumG = 0.0, SumB = 0.0;
			for (int32 Pixel = 0; Pixel < Tinted.Num() && Pixel < Before.Num(); ++Pixel)
			{
				const FColor& New = Shot.Pixels[Pixel];
				const FColor& Old = Before[Pixel];
				SumR += New.R - Old.R;
				SumG += New.G - Old.G;
				SumB += New.B - Old.B;
				if (PixelChanged(New, Old, Threshold))
				{
					++Changed;
					Tinted[Pixel] = FColor(static_cast<uint8>(FMath::Min(255, 140 + New.R * 45 / 100)), static_cast<uint8>(New.G * 35 / 100), static_cast<uint8>(New.B * 35 / 100), 255);
				}
			}
			// 8x8 blocks: average colors, less sensitive to particles that only moved a little.
			constexpr int32 Block = 8;
			const int32 Blocks = FMath::DivideAndRoundUp(Size, Block);
			int32 ChangedBlocks = 0;
			for (int32 BY = 0; BY < Blocks; ++BY)
			{
				for (int32 BX = 0; BX < Blocks; ++BX)
				{
					int64 New[3] = { 0, 0, 0 }, Old[3] = { 0, 0, 0 };
					int32 Count = 0;
					for (int32 Y = BY * Block; Y < FMath::Min(Size, (BY + 1) * Block); ++Y)
					{
						for (int32 X = BX * Block; X < FMath::Min(Size, (BX + 1) * Block); ++X)
						{
							const int32 Pixel = Y * Size + X;
							if (!Before.IsValidIndex(Pixel) || !Shot.Pixels.IsValidIndex(Pixel))
							{
								continue;
							}
							New[0] += Shot.Pixels[Pixel].R; New[1] += Shot.Pixels[Pixel].G; New[2] += Shot.Pixels[Pixel].B;
							Old[0] += Before[Pixel].R; Old[1] += Before[Pixel].G; Old[2] += Before[Pixel].B;
							++Count;
						}
					}
					if (Count > 0 && (FMath::Abs(New[0] - Old[0]) > Threshold * Count || FMath::Abs(New[1] - Old[1]) > Threshold * Count || FMath::Abs(New[2] - Old[2]) > Threshold * Count))
					{
						++ChangedBlocks;
					}
				}
			}
			const double Total = FMath::Max(1, Shot.Pixels.Num());
			Diff.ChangedPercent = static_cast<float>(100.0 * Changed / Total);
			Diff.BlockChangedPercent = static_cast<float>(100.0 * ChangedBlocks / FMath::Max(1, Blocks * Blocks));
			Diff.MeanColorDelta = FString::Printf(TEXT("%s, %s, %s"), *Fmt(SumR / Total, 1), *Fmt(SumG / Total, 1), *Fmt(SumB / Total, 1));
			Result.MaxChangedPercent = FMath::Max(Result.MaxChangedPercent, Diff.ChangedPercent);
			Sheet.Paste(Layout.Cells[Index].X, Layout.Cells[Index].Y, Tinted);
			Lines.Add(FString::Printf(TEXT("Tile %d t=%ss yaw %s: %s%% pixels changed (%s%% of 8x8 blocks), mean color delta %s, coverage %s%% -> %s%%, particles %d -> %d."),
				Diff.Tile, *Fmt(Diff.Time, 2), *Fmt(Diff.CameraYaw, 0), *Fmt(Diff.ChangedPercent, 2), *Fmt(Diff.BlockChangedPercent, 1), *Diff.MeanColorDelta,
				*Fmt(Diff.BaselineCoverage, 1), *Fmt(Diff.Coverage, 1), Diff.BaselineParticles, Diff.Particles));
		}
		Lines.Add(Result.MaxChangedPercent < 0.5f ? FString(TEXT("No visible change.")) : FString::Printf(TEXT("Largest change: %s%% of a tile's pixels."), *Fmt(Result.MaxChangedPercent, 1)));
	}
	if (!Sheet.Encode(Result.Image))
	{
		ReportError(TEXT("Encoding the comparison image failed."));
		return {};
	}
	Result.Caption = FString::Join(Lines, TEXT("\n"));
	return Result;
}

// ================================================================================================== CaptureCostView

FNiagaraMcpCostResult UNiagaraMcpEditorTools::CaptureCostView(UNiagaraSystem* System, const TArray<float>& Times, const FString& Mode, int32 FrameSize, float CameraYaw, float CameraPitch, float CameraDistance, const FString& CameraYaws, const FString& QualityLevel)
{
	using namespace NiagaraMcpVision;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	FString ModeError;
	if (!ParseCostMode(Mode, ModeError))
	{
		ReportError(ModeError);
		return {};
	}
	const FString ModeName = TEXT("ShaderComplexity");
	if (GEngine == nullptr || GEngine->ShaderComplexityColors.Num() < 2)
	{
		ReportError(TEXT("The engine's complexity color ramps are not configured."));
		return {};
	}
	FShotRequest Request;
	if (!BuildRequest(Times, FrameSize, CameraYaw, CameraPitch, CameraDistance, TEXT("Black"), CameraYaws, QualityLevel, 0, Request))
	{
		return {};
	}
	Request.CostView = Preview::ECostView::ShaderComplexity;
	System->RequestCompile(false);

	FShotResult Shots;
	FString Error;
	if (!Preview::CaptureNiagaraShots(*System, Request, Shots, Error))
	{
		ReportError(Error);
		return {};
	}

	// Pick the ramp encoding (sRGB or linear output) that fits the covered pixels best (empty space is a checkerboard).
	FCostScale SRGBScale = MakeCostScale(true);
	FCostScale LinearScale = MakeCostScale(false);
	int64 SRGBError = 0, LinearError = 0;
	for (const FShot& Shot : Shots.Shots)
	{
		const TArray<FColor>& Back = Shots.Backgrounds.IsValidIndex(Shot.YawIndex) ? Shots.Backgrounds[Shot.YawIndex] : Shot.Pixels;
		for (int32 Pixel = 0; Pixel < Shot.Pixels.Num(); Pixel += 7)
		{
			if (Back.IsValidIndex(Pixel) && Preview::IsCovered(Shot.Pixels[Pixel], Back[Pixel], ThresholdFor(Shots, Shot.YawIndex)))
			{
				SRGBError += SRGBScale.Lookup(Shot.Pixels[Pixel]).Value;
				LinearError += LinearScale.Lookup(Shot.Pixels[Pixel]).Value;
			}
		}
	}
	FCostScale& Scale = SRGBError <= LinearError ? SRGBScale : LinearScale;

	const FLayout Layout = MakeLayout(Request.Times.Num(), Request.Yaws.Num(), false);
	FNiagaraMcpCostResult Result;
	Result.Mode = ModeName;
	FSheet Sheet;
	Sheet.Init(Layout.Columns, Layout.Rows, Shots.FrameSize);
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%s %s view: %d tiles in %s (%d px), framing radius %s cm, quality %s. Covered = pixels the effect touches; bad = red band or worse (cost index >= %s); extreme = %s band (>= %s). Empty space shows as a grey / black checkerboard."),
		*System->GetName(), *ModeName, Shots.Shots.Num(), *Layout.Describe(), Shots.FrameSize, *Fmt(Shots.Framing.SphereRadius, 0), *Shots.QualityLevelName,
		*Fmt(Scale.BadFrom, 2), TEXT("pink / white"), *Fmt(Scale.ExtremeFrom, 2)));
	bool bRampMismatch = false;
	for (int32 Index = 0; Index < Shots.Shots.Num(); ++Index)
	{
		const FShot& Shot = Shots.Shots[Index];
		const TArray<FColor>& Back = Shots.Backgrounds.IsValidIndex(Shot.YawIndex) ? Shots.Backgrounds[Shot.YawIndex] : Shot.Pixels;
		int64 Covered = 0, Bad = 0, Extreme = 0, FarFromRamp = 0;
		double SumCost = 0.0;
		for (int32 Pixel = 0; Pixel < Shot.Pixels.Num(); ++Pixel)
		{
			if (!Back.IsValidIndex(Pixel) || !Preview::IsCovered(Shot.Pixels[Pixel], Back[Pixel], ThresholdFor(Shots, Shot.YawIndex)))
			{
				continue;
			}
			++Covered;
			const TPair<float, int32> Cost = Scale.Lookup(Shot.Pixels[Pixel]);
			SumCost += Cost.Key;
			Bad += Cost.Key >= Scale.BadFrom - KINDA_SMALL_NUMBER ? 1 : 0;
			Extreme += Cost.Key >= Scale.ExtremeFrom - KINDA_SMALL_NUMBER ? 1 : 0;
			FarFromRamp += Cost.Value > 40 * 40 ? 1 : 0;
		}
		FNiagaraMcpCostFrame& Frame = Result.Frames.AddDefaulted_GetRef();
		Frame.Tile = Layout.TileIndex(Layout.Cells[Index]);
		Frame.Time = Shot.Time;
		Frame.CameraYaw = Shot.Yaw;
		Frame.Particles = Shot.Counts.TotalParticles;
		Frame.Coverage = static_cast<float>(100.0 * Covered / FMath::Max(1, Shot.Pixels.Num()));
		if (Covered > 0)
		{
			Frame.BadShare = static_cast<float>(100.0 * Bad / Covered);
			Frame.ExtremeShare = static_cast<float>(100.0 * Extreme / Covered);
			Frame.MeanComplexity = static_cast<float>(SumCost / Covered);
			bRampMismatch |= FarFromRamp * 2 > Covered;
		}
		Sheet.Paste(Layout.Cells[Index].X, Layout.Cells[Index].Y, Shot.Pixels);
		Lines.Add(FString::Printf(TEXT("Tile %d t=%ss yaw %s: %d particles; coverage %s%%, bad %s%%, extreme %s%%, mean cost index %s."),
			Frame.Tile, *Fmt(Frame.Time, 2), *Fmt(Frame.CameraYaw, 0), Frame.Particles, *Fmt(Frame.Coverage, 1), *Fmt(Frame.BadShare, 1), *Fmt(Frame.ExtremeShare, 1), *Fmt(Frame.MeanComplexity, 3)));
	}
	if (bRampMismatch)
	{
		Lines.Add(TEXT("Warning: many covered pixels do not match the engine's color ramp, so the numbers are approximate (custom ShaderComplexityColors?)."));
	}
	if (!Sheet.Encode(Result.Image))
	{
		ReportError(TEXT("Encoding the cost view image failed."));
		return {};
	}
	Result.Caption = FString::Join(Lines, TEXT("\n"));
	return Result;
}

// ================================================================================================== BakeFlipbook

FNiagaraMcpBakeResult UNiagaraMcpEditorTools::BakeFlipbook(UNiagaraSystem* System, const FString& NewTextureAssetPath, int32 Columns, int32 Rows, float StartTime, float EndTime, int32 FrameSize, const FString& Background, float CameraYaw, float CameraPitch, bool bSave)
{
	using namespace NiagaraMcpVision;
	if (!NiagaraMcpTools::CheckSystemArg(System))
	{
		return {};
	}
	// Validate everything before rendering or creating anything.
	if (Columns < 1 || Columns > 16 || Rows < 1 || Rows > 16)
	{
		ReportError(FString::Printf(TEXT("Columns and Rows must be 1-16 (got %d x %d)."), Columns, Rows));
		return {};
	}
	if (FrameSize < 32 || FrameSize > 1024)
	{
		ReportError(FString::Printf(TEXT("FrameSize must be 32-1024 (got %d)."), FrameSize));
		return {};
	}
	if (Columns * FrameSize > 8192 || Rows * FrameSize > 8192)
	{
		ReportError(FString::Printf(TEXT("The atlas would be %dx%d px; the maximum is 8192. Use a smaller FrameSize or fewer frames."), Columns * FrameSize, Rows * FrameSize));
		return {};
	}
	if (!(StartTime >= 0.0f && EndTime > StartTime && EndTime <= 600.0f))
	{
		ReportError(FString::Printf(TEXT("Need 0 <= StartTime < EndTime <= 600 (got %s .. %s)."), *FString::SanitizeFloat(StartTime), *FString::SanitizeFloat(EndTime)));
		return {};
	}
	const FString BackgroundName = IsUnset(Background) ? FString(TEXT("Black")) : Background.TrimStartAndEnd();
	const bool bBlack = BackgroundName.Equals(TEXT("Black"), ESearchCase::IgnoreCase);
	if (!bBlack && !BackgroundName.Equals(TEXT("Grey"), ESearchCase::IgnoreCase) && !BackgroundName.Equals(TEXT("Gray"), ESearchCase::IgnoreCase) && !BackgroundName.Equals(TEXT("White"), ESearchCase::IgnoreCase))
	{
		ReportError(FString::Printf(TEXT("Unknown Background '%s'. Use Black, Grey or White."), *Background));
		return {};
	}
	FString PackageName, AssetName;
	if (!SplitNewAssetPath(NewTextureAssetPath, PackageName, AssetName))
	{
		return {};
	}

	const int32 FrameCount = Columns * Rows;
	FShotRequest Request;
	for (int32 Index = 0; Index < FrameCount; ++Index)
	{
		Request.Times.Add(StartTime + (EndTime - StartTime) * Index / FrameCount);
	}
	Request.Yaws = { CameraYaw };
	Request.FrameSize = FrameSize;
	Request.Camera.Yaw = CameraYaw;
	Request.Camera.Pitch = CameraPitch;
	Request.Camera.Background = BackgroundName;
	System->RequestCompile(false);

	FShotResult Shots;
	FString Error;
	if (!Preview::CaptureNiagaraShots(*System, Request, Shots, Error))
	{
		ReportError(Error);
		return {};
	}

	// Pack the frames (frame i at column i % Columns, row i / Columns), alpha from brightness on black.
	const int32 Width = Columns * FrameSize;
	const int32 Height = Rows * FrameSize;
	TArray<FColor> Atlas;
	Atlas.Init(FColor(0, 0, 0, bBlack ? 0 : 255), Width * Height);
	FNiagaraMcpBakeResult Result;
	const int32 NoiseFloor = ThresholdFor(Shots, 0);
	for (int32 Index = 0; Index < Shots.Shots.Num(); ++Index)
	{
		const FShot& Shot = Shots.Shots[Index];
		FNiagaraMcpFrameStats Frame;
		Preview::ComputeFrameStats(Shot.Pixels, Shots.Backgrounds.IsValidIndex(0) ? Shots.Backgrounds[0] : TArray<FColor>(), FrameSize, Frame, ThresholdFor(Shots, 0));
		Result.FrameTimes.Add(Shot.Time);
		Result.FrameCoverage.Add(Frame.Coverage);
		const int32 OffsetX = (Index % Columns) * FrameSize;
		const int32 OffsetY = (Index / Columns) * FrameSize;
		for (int32 Y = 0; Y < FrameSize; ++Y)
		{
			for (int32 X = 0; X < FrameSize; ++X)
			{
				FColor Pixel = Shot.Pixels[Y * FrameSize + X];
				if (bBlack)
				{
					// The tonemapper dithers black by one quantization step, which would turn the empty background into a 5 %
					// alpha haze: anything at or below the measured noise floor is fully transparent.
					const uint8 Brightness = FMath::Max3(Pixel.R, Pixel.G, Pixel.B);
					Pixel = Brightness <= NoiseFloor ? FColor(0, 0, 0, 0) : FColor(Pixel.R, Pixel.G, Pixel.B, Brightness);
				}
				else
				{
					Pixel.A = 255;
				}
				Atlas[(OffsetY + Y) * Width + OffsetX + X] = Pixel;
			}
		}
	}

	FString CreatedName;
	UPackage* Package = CreateNewAssetPackage(NewTextureAssetPath, CreatedName);
	if (Package == nullptr)
	{
		return {};
	}
	// No transaction: undoing a recorded NewObject leaves a broken asset in memory.
	UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(*CreatedName), RF_Public | RF_Standalone | RF_Transactional);
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Atlas.GetData()));
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->LODGroup = TEXTUREGROUP_Effects;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();

	Result.AssetPath = Texture->GetPathName();
	Result.Width = Width;
	Result.Height = Height;
	Result.Columns = Columns;
	Result.Rows = Rows;
	Result.FrameSize = FrameSize;
	Result.AlphaMode = bBlack
		? FString::Printf(TEXT("Brightness: A = brightest of R, G, B over black (black and the render noise floor, <= %d / 255, are transparent; RGB is premultiplied - use additive or premultiplied alpha blending)"), NoiseFloor)
		: FString::Printf(TEXT("Opaque (rendered over %s)"), *BackgroundName);
	Result.Usage = FString::Printf(TEXT("Sprite renderer: Material with this texture in a SubUV / flipbook sampler, SubImageSize = (%d, %d); animate Particles.SubImageIndex from 0 to %d (SubUV Animation module, e.g. Linear over the particle's life). Frame i shows simulation time %s + i * %s s."),
		Columns, Rows, FrameCount - 1, *Fmt(StartTime, 3), *Fmt((EndTime - StartTime) / FrameCount, 4));
	Result.bSaved = bSave && SaveAssetSilently(Texture);
	return Result;
}

// ================================================================================================== ViewTexture

FToolsetImage UAssetMcpEditorTools::ViewTexture(UTexture2D* Texture, const FString& Channels, int32 GridColumns, int32 GridRows, int32 Size)
{
	using namespace NiagaraMcpVision;
	if (Texture == nullptr)
	{
		ReportError(TEXT("Texture is null or could not be loaded. Pass a texture path such as /Game/FX/T_Smoke.T_Smoke."));
		return {};
	}
	if (Size < 64 || Size > 1024)
	{
		ReportError(FString::Printf(TEXT("Size must be 64-1024 (got %d)."), Size));
		return {};
	}
	if (GridColumns < 0 || GridRows < 0 || GridColumns > 64 || GridRows > 64)
	{
		ReportError(FString::Printf(TEXT("GridColumns and GridRows must be 0-64 (got %d x %d)."), GridColumns, GridRows));
		return {};
	}
	enum class ETile : uint8 { Color, Checker, R, G, B, A };
	TArray<ETile> Tiles;
	int32 SheetColumns = 1;
	const FString Mode = IsUnset(Channels) ? FString(TEXT("All")) : Channels.TrimStartAndEnd();
	if (Mode.Equals(TEXT("All"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("RGBA"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::Color, ETile::Checker, ETile::R, ETile::G, ETile::B, ETile::A };
		SheetColumns = 3;
	}
	else if (Mode.Equals(TEXT("RGB"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::Color, ETile::R, ETile::G, ETile::B };
		SheetColumns = 2;
	}
	else if (Mode.Equals(TEXT("R"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("Red"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::R };
	}
	else if (Mode.Equals(TEXT("G"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("Green"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::G };
	}
	else if (Mode.Equals(TEXT("B"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("Blue"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::B };
	}
	else if (Mode.Equals(TEXT("A"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase))
	{
		Tiles = { ETile::A };
	}
	else
	{
		ReportError(FString::Printf(TEXT("Unknown Channels '%s'. Use All, RGB, R, G, B or A."), *Channels));
		return {};
	}
	if (!Texture->Source.IsValid() || Texture->Source.GetNumBlocks() != 1)
	{
		ReportError(FString::Printf(TEXT("'%s' has no plain 2D source image (UDIM / virtual textures without source data are not supported)."), *Texture->GetPathName()));
		return {};
	}

	// Source pixels (independent of compression): 8 / 16-bit integer sources show their stored values, float (HDR)
	// sources are converted from linear to sRGB for display.
	FImage Source;
	if (!Texture->Source.GetMipImage(Source, 0, 0, 0) || Source.GetWidth() <= 0 || Source.GetHeight() <= 0)
	{
		ReportError(FString::Printf(TEXT("Could not read the source image of '%s'."), *Texture->GetPathName()));
		return {};
	}
	// Pow22 (textures with legacy gamma) can be read but never written: ImageCore asserts on a Pow22 destination and
	// takes the whole editor down, so show those as sRGB (visually the same curve).
	const EGammaSpace DisplayGamma = IsFloatFormat(Source.Format) || Source.GammaSpace == EGammaSpace::Pow22 ? EGammaSpace::sRGB : Source.GammaSpace;
	FImage Display;
	Source.CopyTo(Display, ERawImageFormat::BGRA8, DisplayGamma);

	const float Scale = static_cast<float>(Size) / static_cast<float>(FMath::Max(Display.GetWidth(), Display.GetHeight()));
	const int32 TileWidth = FMath::Clamp(FMath::RoundToInt(Display.GetWidth() * Scale), 1, Size);
	const int32 TileHeight = FMath::Clamp(FMath::RoundToInt(Display.GetHeight() * Scale), 1, Size);
	FImage Resized;
	const bool bUpscale = TileWidth >= Display.GetWidth();
	FImageCore::ResizeImageAllocDest(Display, Resized, TileWidth, TileHeight, ERawImageFormat::BGRA8, DisplayGamma,
		bUpscale ? FImageCore::EResizeImageFilter::PointSample : FImageCore::EResizeImageFilter::Box);
	const TArrayView64<FColor> Pixels = Resized.AsBGRA8();

	const int32 OffsetX = (Size - TileWidth) / 2;
	const int32 OffsetY = (Size - TileHeight) / 2;
	const FColor Letterbox(30, 30, 30, 255);
	const FColor GridColor(255, 220, 0, 255);
	FSheet Sheet;
	Sheet.Init(SheetColumns, FMath::DivideAndRoundUp(Tiles.Num(), SheetColumns), Size, Letterbox);
	for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
	{
		TArray<FColor> Tile;
		Tile.Init(Letterbox, Size * Size);
		for (int32 Y = 0; Y < TileHeight; ++Y)
		{
			for (int32 X = 0; X < TileWidth; ++X)
			{
				const FColor Pixel = Pixels[static_cast<int64>(Y) * TileWidth + X];
				FColor Out;
				switch (Tiles[TileIndex])
				{
				case ETile::Color: Out = FColor(Pixel.R, Pixel.G, Pixel.B, 255); break;
				case ETile::R: Out = FColor(Pixel.R, Pixel.R, Pixel.R, 255); break;
				case ETile::G: Out = FColor(Pixel.G, Pixel.G, Pixel.G, 255); break;
				case ETile::B: Out = FColor(Pixel.B, Pixel.B, Pixel.B, 255); break;
				case ETile::A: Out = FColor(Pixel.A, Pixel.A, Pixel.A, 255); break;
				case ETile::Checker:
				default:
				{
					const uint8 Check = ((X / 8 + Y / 8) % 2) ? 200 : 120;
					auto Blend = [&Pixel, Check](uint8 Channel) { return static_cast<uint8>((Channel * Pixel.A + Check * (255 - Pixel.A) + 127) / 255); };
					Out = FColor(Blend(Pixel.R), Blend(Pixel.G), Blend(Pixel.B), 255);
					break;
				}
				}
				Tile[(OffsetY + Y) * Size + OffsetX + X] = Out;
			}
		}
		// Flipbook grid: lines between the cells.
		for (int32 Column = 1; Column < GridColumns; ++Column)
		{
			const int32 X = OffsetX + FMath::Clamp(FMath::RoundToInt(static_cast<float>(TileWidth) * Column / GridColumns), 0, TileWidth - 1);
			for (int32 Y = OffsetY; Y < OffsetY + TileHeight; ++Y)
			{
				Tile[Y * Size + X] = GridColor;
			}
		}
		for (int32 Row = 1; Row < GridRows; ++Row)
		{
			const int32 Y = OffsetY + FMath::Clamp(FMath::RoundToInt(static_cast<float>(TileHeight) * Row / GridRows), 0, TileHeight - 1);
			for (int32 X = OffsetX; X < OffsetX + TileWidth; ++X)
			{
				Tile[Y * Size + X] = GridColor;
			}
		}
		Sheet.Paste(TileIndex % SheetColumns, TileIndex / SheetColumns, Tile);
	}
	FToolsetImage Image;
	if (!Sheet.Encode(Image))
	{
		ReportError(TEXT("Encoding the texture view failed."));
		return {};
	}
	return Image;
}

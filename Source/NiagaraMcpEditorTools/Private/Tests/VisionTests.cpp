#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetMcpEditorTools.h"
#include "Engine/Texture2D.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "ModelContextProtocolToolResults.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraMcpEditorTools.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

/**
 * v0.5 vision tools: per-tile stats and hints, multiple yaws, quality level previews, reference tiles, texture channel
 * views, visual baselines, GPU cost view modes and flipbook baking. Every check reads real pixels or numbers.
 */
namespace NiagaraMcpVisionTests
{
	struct FDecoded
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;

		FColor At(int32 X, int32 Y) const
		{
			return Pixels.IsValidIndex(Y * Width + X) ? Pixels[Y * Width + X] : FColor(1, 2, 3, 0);
		}
	};

	bool Decode(const FToolsetImage& Image, FDecoded& Out)
	{
		TArray<uint8> Bytes;
		if (Image.Data.IsEmpty() || !FBase64::Decode(Image.Data, Bytes))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<uint8> Raw;
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			return false;
		}
		Out.Width = static_cast<int32>(Wrapper->GetWidth());
		Out.Height = static_cast<int32>(Wrapper->GetHeight());
		Out.Pixels.SetNum(Out.Width * Out.Height);
		FMemory::Memcpy(Out.Pixels.GetData(), Raw.GetData(), FMath::Min<int64>(Raw.Num(), Out.Pixels.Num() * sizeof(FColor)));
		return true;
	}

	bool Near(const FColor& A, const FColor& B, int32 Tolerance)
	{
		return FMath::Abs(A.R - B.R) <= Tolerance && FMath::Abs(A.G - B.G) <= Tolerance && FMath::Abs(A.B - B.B) <= Tolerance;
	}

	FString ColorText(const FColor& C)
	{
		return FString::Printf(TEXT("%d,%d,%d"), C.R, C.G, C.B);
	}

	FString Folder(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("/Game/NiagaraMcpTests/Vision/%s_%s"), Prefix, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	/** A system with one Minimal emitter: constant spawn rate, long lifetime (visible from ~0.1 s on). */
	UNiagaraSystem* MakeSystem(const FString& Path, FName Emitter, bool bDeterministic = false)
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *UNiagaraMcpEditorTools::CreateSystem(Path, false).Path);
		if (System == nullptr)
		{
			return nullptr;
		}
		UNiagaraMcpEditorTools::AddEmitter(System, Emitter, TEXT("Minimal"), false);
		UNiagaraMcpEditorTools::AddModule(System, Emitter, TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
		UNiagaraMcpEditorTools::SetModuleInput(System, Emitter, TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("60"), TEXT("Any"), false);
		UNiagaraMcpEditorTools::SetModuleInput(System, Emitter, TEXT("Initialize Particle"), TEXT("Lifetime"), TEXT("5"), TEXT("Any"), false);
		UNiagaraMcpEditorTools::AddModule(System, Emitter, TEXT("ParticleSpawn"), TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"), -1, false);
		if (bDeterministic)
		{
			UNiagaraMcpEditorTools::SetEmitterProperty(System, Emitter, TEXT("bDeterminism"), TEXT("true"), false);
		}
		UNiagaraMcpEditorTools::CompileSystem(System, true, false);
		return System;
	}

	/** Writes a tool image to Saved/NiagaraMcpTests/Vision for a human look. */
	void SaveForInspection(const FToolsetImage& Image, const FString& Name)
	{
		TArray<uint8> Bytes;
		if (!Image.Data.IsEmpty() && FBase64::Decode(Image.Data, Bytes))
		{
			FFileHelper::SaveArrayToFile(Bytes, *(FPaths::ProjectSavedDir() / TEXT("NiagaraMcpTests/Vision") / Name));
		}
	}

	int32 EmitterCount(const TArray<FNiagaraMcpEmitterCount>& Emitters, FName Emitter)
	{
		const FNiagaraMcpEmitterCount* Count = Emitters.FindByPredicate([Emitter](const FNiagaraMcpEmitterCount& C) { return C.Emitter == Emitter; });
		return Count ? Count->Particles : -1;
	}
}

// ------------------------------------------------------------------------------------------------ Preview stats

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionPreviewTest, "NiagaraMcp.Vision.PreviewStatsYawsReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionPreviewTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	const FString Root = Folder(TEXT("Preview"));
	UNiagaraSystem* Visible = MakeSystem(Root + TEXT("/NS_Visible"), TEXT("Puff"));
	UNiagaraSystem* Hidden = MakeSystem(Root + TEXT("/NS_Hidden"), TEXT("Ghost"));
	if (!TestNotNull(TEXT("Visible system"), Visible) || !TestNotNull(TEXT("Hidden system"), Hidden))
	{
		return false;
	}

	// Stats: t=0 has no particles yet (nothing simulated), t=0.5 is visible.
	const FNiagaraMcpPreviewResult Stats = UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.0f, 0.5f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), TEXT("None"));
	AddInfo(TEXT("Caption:\n") + Stats.Caption);
	SaveForInspection(Stats.Image, TEXT("stats.png"));
	if (TestEqual(TEXT("Two frames"), Stats.Frames.Num(), 2))
	{
		const FNiagaraMcpFrameStats& Empty = Stats.Frames[0];
		const FNiagaraMcpFrameStats& Drawn = Stats.Frames[1];
		TestTrue(TEXT("t=0: 0 particles, empty, spawn/lifetime hint"), Empty.Particles == 0 && Empty.bEmpty && Empty.Coverage == 0.0f && Empty.Hint.Contains(TEXT("spawn/lifetime")));
		TestTrue(FString::Printf(TEXT("t=0.5: %d particles, coverage %.2f%%, color %s"), Drawn.Particles, Drawn.Coverage, *Drawn.MeanColor),
			Drawn.Particles > 10 && !Drawn.bEmpty && Drawn.Coverage > 0.05f && !Drawn.MeanColor.IsEmpty() && !Drawn.ScreenBounds.IsEmpty());
		TestTrue(TEXT("Luminance in range"), Drawn.PeakLuminance >= Drawn.MeanLuminance && Drawn.PeakLuminance > 0.0f && Drawn.PeakLuminance <= 1.0f);
		TestTrue(TEXT("Per emitter counts"), Drawn.Emitters.Num() == 1 && Drawn.Emitters[0].Emitter == TEXT("Puff") && Drawn.Emitters[0].Particles == Drawn.Particles);
		TestTrue(TEXT("Tiles in order"), Empty.Tile == 0 && Drawn.Tile == 1 && Drawn.Column == 1 && Drawn.Row == 0);
	}
	TestTrue(TEXT("Caption names the framing radius"), Stats.Caption.Contains(TEXT("Framing radius")) && Stats.FramingRadius > 0.0f);
	TestTrue(TEXT("Layout 2 x 1"), Stats.Columns == 2 && Stats.Rows == 1);

	// Renderer disabled: particles but nothing drawn.
	for (const FNiagaraEmitterHandle& Handle : Hidden->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
			{
				Renderer->SetIsEnabled(false);
			}
		}
	}
	Hidden->RequestCompile(false);
	Hidden->WaitForCompilationComplete();
	const FNiagaraMcpPreviewResult NoRenderer = UNiagaraMcpEditorTools::CapturePreview(Hidden, { 0.5f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), TEXT("None"));
	SaveForInspection(NoRenderer.Image, TEXT("no_renderer.png"));
	if (TestEqual(TEXT("One frame"), NoRenderer.Frames.Num(), 1))
	{
		const FNiagaraMcpFrameStats& Frame = NoRenderer.Frames[0];
		AddInfo(FString::Printf(TEXT("Disabled renderer: %d particles, coverage %.3f, hint: %s"), Frame.Particles, Frame.Coverage, *Frame.Hint));
		TestTrue(TEXT("Disabled renderer: particles, coverage 0, 'nothing drawn' hint naming the emitter"),
			Frame.Particles > 10 && Frame.bEmpty && Frame.Coverage == 0.0f && Frame.Hint.Contains(TEXT("nothing drawn")) && Frame.Hint.Contains(TEXT("Ghost")));
	}

	// Two yaws: 2 tiles per time, rows = times.
	const FNiagaraMcpPreviewResult Yaws = UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.5f, 1.0f }, 96, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("0, 90"), TEXT("Current"), TEXT("None"));
	SaveForInspection(Yaws.Image, TEXT("yaws.png"));
	FDecoded YawImage;
	TestTrue(TEXT("Two yaws x two times = 4 tiles"), Yaws.Frames.Num() == 4 && Yaws.Columns == 2 && Yaws.Rows == 2);
	TestTrue(TEXT("Yaw sheet is 192 x 192"), Decode(Yaws.Image, YawImage) && YawImage.Width == 192 && YawImage.Height == 192);
	if (Yaws.Frames.Num() == 4)
	{
		TestTrue(TEXT("Time-major layout with yaws per column"), FMath::IsNearlyEqual(Yaws.Frames[1].CameraYaw, 90.0f) && Yaws.Frames[1].Row == 0 && Yaws.Frames[1].Column == 1
			&& FMath::IsNearlyEqual(Yaws.Frames[2].Time, 1.0f) && Yaws.Frames[2].Row == 1 && Yaws.Frames[2].Column == 0);
		TestTrue(TEXT("Same particles for both yaws of one time (one run)"), Yaws.Frames[0].Particles == Yaws.Frames[1].Particles && Yaws.Frames[2].Particles > Yaws.Frames[0].Particles);
	}
	AddExpectedError(TEXT("CameraYaws"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Bad yaw list rejected"), UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.5f }, 96, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("0, side"), TEXT("Current"), TEXT("None")).Image.Data.IsEmpty());

	// Reference tile: a known image file becomes tile 0.
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NiagaraMcpTests"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString RefPath = Dir / TEXT("vision_reference.png");
	FImage Reference(64, 32, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	for (FColor& Pixel : Reference.AsBGRA8())
	{
		Pixel = FColor(200, 50, 100, 255);
	}
	TestTrue(TEXT("Reference written"), FImageUtils::SaveImageByExtension(*RefPath, Reference));
	const FNiagaraMcpPreviewResult WithReference = UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.5f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), RefPath);
	SaveForInspection(WithReference.Image, TEXT("reference.png"));
	FDecoded RefImage;
	if (TestTrue(TEXT("Reference sheet decodes"), Decode(WithReference.Image, RefImage) && RefImage.Width == 256 && RefImage.Height == 128))
	{
		// 64x32 fitted into 128: 128x64 centered vertically (rows 32-95); outside is black.
		TestTrue(FString::Printf(TEXT("Reference pixels match the file (%s)"), *ColorText(RefImage.At(64, 64))), Near(RefImage.At(64, 64), FColor(200, 50, 100), 2) && Near(RefImage.At(10, 40), FColor(200, 50, 100), 2));
		TestTrue(TEXT("Letterbox above the reference is black"), Near(RefImage.At(64, 5), FColor::Black, 1));
	}
	TestTrue(TEXT("Rendered frame follows the reference"), WithReference.Frames.Num() == 1 && WithReference.Frames[0].Tile == 1 && WithReference.Caption.Contains(TEXT("REFERENCE")));

	AddExpectedError(TEXT("does not exist"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Missing reference rejected"), UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.5f }, 96, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), Dir / TEXT("missing_ref.png")).Image.Data.IsEmpty());
	const FString TextPath = Dir / TEXT("not_an_image.png");
	FFileHelper::SaveStringToFile(TEXT("this is not a png"), *TextPath);
	AddExpectedError(TEXT("not a readable image"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Non-image reference rejected"), UNiagaraMcpEditorTools::CapturePreview(Visible, { 0.5f }, 96, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), TextPath).Image.Data.IsEmpty());
	return true;
}

// ------------------------------------------------------------------------------------------------ Quality level

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionQualityTest, "NiagaraMcp.Vision.QualityLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionQualityTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	UNiagaraSystem* System = MakeSystem(Folder(TEXT("Quality")) + TEXT("/NS_Scalable"), TEXT("Always"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	UNiagaraMcpEditorTools::AddEmitter(System, TEXT("EpicOnly"), TEXT("Minimal"), false);
	UNiagaraMcpEditorTools::AddModule(System, TEXT("EpicOnly"), TEXT("EmitterUpdate"), TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("EpicOnly"), TEXT("Spawn Rate"), TEXT("SpawnRate"), TEXT("60"), TEXT("Any"), false);
	// Quality level mask bit 3 = Epic only.
	const FNiagaraMcpPropertyResult Mask = UNiagaraMcpEditorTools::SetEmitterProperty(System, TEXT("EpicOnly"), TEXT("Platforms.QualityLevelMask"), TEXT("8"), false);
	TestEqual(TEXT("Platform set limited to Epic"), Mask.Value, FString(TEXT("8")));
	UNiagaraMcpEditorTools::CompileSystem(System, true, false);

	const int32 LevelBefore = FNiagaraPlatformSet::GetQualityLevel();
	const TArray<FNiagaraMcpParticleSample> Low = UNiagaraMcpEditorTools::GetParticleCounts(System, { 0.5f }, TEXT("Low"));
	const TArray<FNiagaraMcpParticleSample> Epic = UNiagaraMcpEditorTools::GetParticleCounts(System, { 0.5f }, TEXT("Epic"));
	const int32 LowEpicOnly = Low.Num() ? EmitterCount(Low[0].Emitters, TEXT("EpicOnly")) : -1;
	const int32 EpicEpicOnly = Epic.Num() ? EmitterCount(Epic[0].Emitters, TEXT("EpicOnly")) : -1;
	const int32 LowAlways = Low.Num() ? EmitterCount(Low[0].Emitters, TEXT("Always")) : -1;
	AddInfo(FString::Printf(TEXT("EpicOnly particles: Low %d, Epic %d; Always at Low %d"), LowEpicOnly, EpicEpicOnly, LowAlways));
	TestTrue(TEXT("Epic-only emitter has particles at Epic"), EpicEpicOnly > 10);
	TestTrue(TEXT("Epic-only emitter has none at Low (absent or 0)"), LowEpicOnly <= 0);
	TestTrue(TEXT("Unrestricted emitter runs at Low"), LowAlways > 10);
	TestEqual(TEXT("Quality level restored after the calls"), FNiagaraPlatformSet::GetQualityLevel(), LevelBefore);

	const FNiagaraMcpPreviewResult LowPreview = UNiagaraMcpEditorTools::CapturePreview(System, { 0.5f }, 96, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("0"), TEXT("None"));
	TestTrue(FString::Printf(TEXT("Preview reports the quality level (%s)"), *LowPreview.QualityLevel), LowPreview.QualityLevel == TEXT("Low") && LowPreview.Caption.Contains(TEXT("quality level Low")));
	TestTrue(TEXT("Low preview counts no Epic-only particles"), LowPreview.Frames.Num() == 1 && EmitterCount(LowPreview.Frames[0].Emitters, TEXT("EpicOnly")) <= 0);
	TestEqual(TEXT("Quality level restored after the preview"), FNiagaraPlatformSet::GetQualityLevel(), LevelBefore);

	AddExpectedError(TEXT("Unknown QualityLevel"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("Unknown quality level rejected"), UNiagaraMcpEditorTools::GetParticleCounts(System, { 0.5f }, TEXT("Ultra")).Num(), 0);
	return true;
}

// ------------------------------------------------------------------------------------------------ ViewTexture

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionTextureTest, "NiagaraMcp.Vision.ViewTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionTextureTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	// 8-bit source: every pixel (200, 100, 50, 25).
	UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<FColor> Source;
	Source.Init(FColor(200, 100, 50, 25), 4 * 4);
	Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Source.GetData()));

	SaveForInspection(UAssetMcpEditorTools::ViewTexture(Texture, TEXT("All"), 2, 2, 64), TEXT("texture_all.png"));
	FDecoded All;
	if (TestTrue(TEXT("All channels: 3 x 2 tiles of 64 px"), Decode(UAssetMcpEditorTools::ViewTexture(Texture, TEXT("All"), 2, 2, 64), All) && All.Width == 192 && All.Height == 128))
	{
		TestTrue(FString::Printf(TEXT("Color tile (%s)"), *ColorText(All.At(16, 16))), Near(All.At(16, 16), FColor(200, 100, 50), 1));
		TestTrue(FString::Printf(TEXT("R tile (%s)"), *ColorText(All.At(128 + 16, 16))), Near(All.At(128 + 16, 16), FColor(200, 200, 200), 1));
		TestTrue(FString::Printf(TEXT("G tile (%s)"), *ColorText(All.At(16, 64 + 16))), Near(All.At(16, 64 + 16), FColor(100, 100, 100), 1));
		TestTrue(FString::Printf(TEXT("B tile (%s)"), *ColorText(All.At(64 + 16, 64 + 16))), Near(All.At(64 + 16, 64 + 16), FColor(50, 50, 50), 1));
		TestTrue(FString::Printf(TEXT("A tile (%s)"), *ColorText(All.At(128 + 16, 64 + 16))), Near(All.At(128 + 16, 64 + 16), FColor(25, 25, 25), 1));
		// Alpha 25 over the dark checker square (120): 200*25/255 + 120*230/255 = 128.
		TestTrue(FString::Printf(TEXT("RGBA over checker (%s)"), *ColorText(All.At(64 + 16, 16))), Near(All.At(64 + 16, 16), FColor(128, 118, 113), 2));
		TestTrue(FString::Printf(TEXT("Flipbook grid line (%s)"), *ColorText(All.At(32, 10))), Near(All.At(32, 10), FColor(255, 220, 0), 1) && Near(All.At(10, 32), FColor(255, 220, 0), 1));
	}
	FDecoded Single;
	TestTrue(TEXT("Single channel: one 64 px tile"), Decode(UAssetMcpEditorTools::ViewTexture(Texture, TEXT("G"), 0, 0, 64), Single) && Single.Width == 64 && Single.Height == 64 && Near(Single.At(32, 32), FColor(100, 100, 100), 1));

	// Float (HDR) source: linear 0.5 / 0.25 / 1.0 shown as sRGB 188 / 137 / 255.
	UTexture2D* HdrTexture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<FFloat16Color> HdrSource;
	HdrSource.Init(FFloat16Color(FLinearColor(0.5f, 0.25f, 1.0f, 1.0f)), 2 * 2);
	HdrTexture->Source.Init(2, 2, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(HdrSource.GetData()));
	FDecoded Hdr;
	if (TestTrue(TEXT("RGB view: 2 x 2 tiles"), Decode(UAssetMcpEditorTools::ViewTexture(HdrTexture, TEXT("RGB"), 0, 0, 64), Hdr) && Hdr.Width == 128 && Hdr.Height == 128))
	{
		TestTrue(FString::Printf(TEXT("Float color converted to sRGB (%s)"), *ColorText(Hdr.At(32, 32))), Near(Hdr.At(32, 32), FColor(188, 137, 255), 2));
		TestTrue(FString::Printf(TEXT("Float R tile (%s)"), *ColorText(Hdr.At(64 + 32, 32))), Near(Hdr.At(64 + 32, 32), FColor(188, 188, 188), 2));
	}

	// Non-square textures keep their aspect.
	UTexture2D* Wide = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<FColor> WideSource;
	WideSource.Init(FColor(255, 0, 0, 255), 8 * 2);
	Wide->Source.Init(8, 2, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(WideSource.GetData()));
	FDecoded WideImage;
	if (TestTrue(TEXT("Wide texture view"), Decode(UAssetMcpEditorTools::ViewTexture(Wide, TEXT("R"), 0, 0, 64), WideImage)))
	{
		TestTrue(TEXT("Wide texture letterboxed (center red, top grey)"), Near(WideImage.At(32, 32), FColor(255, 255, 255), 1) && Near(WideImage.At(32, 2), FColor(30, 30, 30), 1));
	}

	AddExpectedError(TEXT("Unknown Channels"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Unknown channel rejected"), UAssetMcpEditorTools::ViewTexture(Texture, TEXT("Purple"), 0, 0, 64).Data.IsEmpty());
	return true;
}

// ------------------------------------------------------------------------------------------------ ComparePreview

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionCompareTest, "NiagaraMcp.Vision.ComparePreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionCompareTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	UNiagaraSystem* System = MakeSystem(Folder(TEXT("Compare")) + TEXT("/NS_Diff"), TEXT("Puff"), true);
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	const FString Name = FString::Printf(TEXT("vision_test_%s"), *FDateTime::Now().ToString(TEXT("%H%M%S%s")));
	const FNiagaraMcpPreviewDiff First = UNiagaraMcpEditorTools::ComparePreview(System, Name, { 0.5f, 1.0f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), false);
	TestTrue(TEXT("First call stores the baseline"), First.bBaselineCreated && First.Tiles.Num() == 0 && !First.Image.Data.IsEmpty());

	// Arguments after the name are ignored once the baseline exists (same camera and framing).
	const FNiagaraMcpPreviewDiff Same = UNiagaraMcpEditorTools::ComparePreview(System, Name, { 3.0f }, 64, 45.0f, -60.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), false);
	AddInfo(TEXT("Unchanged:\n") + Same.Caption);
	TestTrue(TEXT("Unchanged system: same tiles"), !Same.bBaselineCreated && Same.Tiles.Num() == 2);
	TestTrue(FString::Printf(TEXT("Unchanged system: ~0%% changed (%.3f%%)"), Same.MaxChangedPercent), Same.MaxChangedPercent < 0.5f);
	TestTrue(TEXT("Unchanged coverage > 0"), Same.Tiles.Num() == 2 && Same.Tiles[0].Coverage > 0.05f && FMath::IsNearlyEqual(Same.Tiles[0].Coverage, Same.Tiles[0].BaselineCoverage, 0.5f));

	// Change the particle color: white -> green.
	const FNiagaraMcpModuleInfo ColorModule = UNiagaraMcpEditorTools::AddModule(System, TEXT("Puff"), TEXT("ParticleUpdate"), TEXT("/Niagara/Modules/Update/Color/Color.Color"), -1, false);
	UNiagaraMcpEditorTools::SetModuleInput(System, TEXT("Puff"), ColorModule.ModuleId, TEXT("Color"), TEXT("0, 1, 0, 1"), TEXT("Any"), false);
	UNiagaraMcpEditorTools::CompileSystem(System, true, false);
	const FNiagaraMcpPreviewDiff Changed = UNiagaraMcpEditorTools::ComparePreview(System, Name, {}, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), false);
	AddInfo(TEXT("Changed:\n") + Changed.Caption);
	TestTrue(FString::Printf(TEXT("Color change detected (%.2f%%)"), Changed.MaxChangedPercent), Changed.MaxChangedPercent > 0.05f && Changed.Tiles.Num() == 2 && Changed.Tiles[0].ChangedPercent > 0.0f);
	TestTrue(FString::Printf(TEXT("Red went down (%s)"), Changed.Tiles.Num() ? *Changed.Tiles[0].MeanColorDelta : TEXT("-")), Changed.Tiles.Num() && FCString::Atof(*Changed.Tiles[0].MeanColorDelta) < 0.0f);
	SaveForInspection(Changed.Image, TEXT("compare_changed.png"));
	FDecoded Tinted;
	TestTrue(TEXT("Diff image has the baseline layout (2 x 128 px)"), Decode(Changed.Image, Tinted) && Tinted.Width == 256 && Tinted.Height == 128);

	const FNiagaraMcpPreviewDiff Reset = UNiagaraMcpEditorTools::ComparePreview(System, Name, { 0.5f }, 64, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), true);
	TestTrue(TEXT("bResetBaseline stores a new baseline"), Reset.bBaselineCreated);
	return true;
}

// ------------------------------------------------------------------------------------------------ Cost view + bake

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionCostBakeTest, "NiagaraMcp.Vision.CostViewAndBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionCostBakeTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	const FString Root = Folder(TEXT("Cost"));
	UNiagaraSystem* System = MakeSystem(Root + TEXT("/NS_Cost"), TEXT("Puff"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	const FNiagaraMcpCostResult Complexity = UNiagaraMcpEditorTools::CaptureCostView(System, { 0.5f, 1.5f }, TEXT("ShaderComplexity"), 128, 0.0f, -15.0f, 0.0f, TEXT("None"), TEXT("Current"));
	AddInfo(TEXT("ShaderComplexity:\n") + Complexity.Caption);
	if (TestEqual(TEXT("Complexity frames"), Complexity.Frames.Num(), 2))
	{
		const FNiagaraMcpCostFrame& Frame = Complexity.Frames[1];
		TestTrue(FString::Printf(TEXT("Complexity numbers (coverage %.2f, mean %.3f, bad %.1f)"), Frame.Coverage, Frame.MeanComplexity, Frame.BadShare),
			Frame.Coverage > 0.05f && Frame.MeanComplexity > 0.0f && Frame.BadShare >= 0.0f && Frame.BadShare <= 100.0f && Frame.Particles > 10);
	}
	SaveForInspection(Complexity.Image, TEXT("shader_complexity.png"));
	FDecoded ComplexityImage;
	if (TestTrue(TEXT("Complexity image"), Decode(Complexity.Image, ComplexityImage) && ComplexityImage.Width == 256))
	{
		// Empty space (no cost) is drawn as a grey / black checkerboard by the complexity pass, not black like the normal view.
		const FColor Corner = ComplexityImage.At(2, 2);
		TestTrue(FString::Printf(TEXT("Cost view mode applied (corner %s is checker grey)"), *ColorText(Corner)), Corner.R > 90 && Corner.R == Corner.G && Corner.G == Corner.B);
		TestFalse(TEXT("Covered pixels fit the engine's complexity ramp"), Complexity.Caption.Contains(TEXT("Warning")));
	}
	// The normal render of the same frame has a black corner: the numbers above come from the cost view.
	const FNiagaraMcpPreviewResult Normal = UNiagaraMcpEditorTools::CapturePreview(System, { 1.5f }, 128, 0.0f, -15.0f, 0.0f, TEXT("Black"), TEXT("None"), TEXT("Current"), TEXT("None"));
	FDecoded NormalImage;
	TestTrue(TEXT("Normal render corner is (near) black"), Decode(Normal.Image, NormalImage) && Near(NormalImage.At(2, 2), FColor::Black, 16)); // the renderer adds up to ~13/255 of noise to empty space
	AddExpectedError(TEXT("QuadOverdraw is not available"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("QuadOverdraw rejected with an explanation"), UNiagaraMcpEditorTools::CaptureCostView(System, { 1.0f }, TEXT("QuadOverdraw"), 128, 0.0f, -15.0f, 0.0f, TEXT("None"), TEXT("Current")).Image.Data.IsEmpty());
	AddExpectedError(TEXT("Unknown Mode"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Unknown mode rejected"), UNiagaraMcpEditorTools::CaptureCostView(System, { 1.0f }, TEXT("Wireframe"), 128, 0.0f, -15.0f, 0.0f, TEXT("None"), TEXT("Current")).Image.Data.IsEmpty());

	// Flipbook bake: 4 x 2 frames of 64 px.
	const FNiagaraMcpBakeResult Bake = UNiagaraMcpEditorTools::BakeFlipbook(System, Root + TEXT("/T_Puff_SubUV"), 4, 2, 0.1f, 1.7f, 64, TEXT("Black"), 0.0f, 0.0f, false);
	TestTrue(TEXT("Bake layout"), Bake.Width == 256 && Bake.Height == 128 && Bake.Columns == 4 && Bake.Rows == 2 && Bake.FrameTimes.Num() == 8 && Bake.FrameCoverage.Num() == 8);
	TestTrue(TEXT("Frame times step by 0.2 s"), Bake.FrameTimes.Num() == 8 && FMath::IsNearlyEqual(Bake.FrameTimes[0], 0.1f, 0.001f) && FMath::IsNearlyEqual(Bake.FrameTimes[7], 1.5f, 0.001f));
	UTexture2D* Baked = LoadObject<UTexture2D>(nullptr, *Bake.AssetPath);
	if (TestNotNull(TEXT("Baked texture"), Baked))
	{
		TestTrue(TEXT("Baked source is 256 x 128"), Baked->Source.GetSizeX() == 256 && Baked->Source.GetSizeY() == 128);
		SaveForInspection(UAssetMcpEditorTools::ViewTexture(Baked, TEXT("All"), 4, 2, 256), TEXT("bake_view.png"));
		FImage Pixels;
		if (TestTrue(TEXT("Baked source readable"), Baked->Source.GetMipImage(Pixels, 0, 0, 0)))
		{
			FImage Bgra;
			Pixels.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			int32 Opaque = 0;
			for (const FColor& Pixel : Bgra.AsBGRA8())
			{
				Opaque += Pixel.A > 0 ? 1 : 0;
			}
			TestTrue(FString::Printf(TEXT("Alpha from brightness: transparent corner, %d drawn pixels"), Opaque), Bgra.AsBGRA8()[0].A == 0 && Opaque > 20);
		}
	}
	int32 Drawn = 0;
	for (float Coverage : Bake.FrameCoverage)
	{
		Drawn += Coverage > 0.0f ? 1 : 0;
	}
	TestTrue(FString::Printf(TEXT("Frames have content (%d of 8)"), Drawn), Drawn >= 7);
	AddExpectedError(TEXT("Columns and Rows"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("Bad grid rejected"), UNiagaraMcpEditorTools::BakeFlipbook(System, Root + TEXT("/T_Bad"), 0, 2, 0.0f, 1.0f, 64, TEXT("Black"), 0.0f, 0.0f, false).AssetPath.IsEmpty());
	return true;
}

// ------------------------------------------------------------------------------------------------ MCP image proxies

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraMcpVisionProxyTest, "NiagaraMcp.Vision.ImageProxies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraMcpVisionProxyTest::RunTest(const FString& Parameters)
{
	using namespace NiagaraMcpVisionTests;
	IModelContextProtocolModule* Module = IModelContextProtocolModule::Get();
	if (Module == nullptr)
	{
		AddWarning(TEXT("Unreal MCP is not loaded; the image proxies cannot be tested."));
		return true;
	}
	UNiagaraSystem* System = MakeSystem(Folder(TEXT("Proxy")) + TEXT("/NS_Proxy"), TEXT("Puff"));
	if (!TestNotNull(TEXT("System"), System))
	{
		return false;
	}
	auto ContentTypes = [](const FModelContextProtocolToolResult& Result, FString& OutCaption)
	{
		TArray<FString> Types;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Result.JsonObject.IsValid() && Result.JsonObject->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Content)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Item.IsValid() && Item->TryGetObject(Object))
				{
					Types.Add((*Object)->GetStringField(TEXT("type")));
					(*Object)->TryGetStringField(TEXT("text"), OutCaption);
				}
			}
		}
		return FString::Join(Types, TEXT(","));
	};

	TSharedPtr<IModelContextProtocolTool> Preview = Module->FindTool(TEXT("niagara_capture_preview"));
	if (TestTrue(TEXT("niagara_capture_preview registered"), Preview.IsValid()))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("System"), System->GetPathName());
		Args->SetArrayField(TEXT("Times"), { MakeShared<FJsonValueNumber>(0.5) });
		Args->SetNumberField(TEXT("FrameSize"), 64);
		FString Caption;
		const FString Types = ContentTypes(Preview->Run(Args), Caption);
		TestEqual(TEXT("Preview proxy returns an image and the caption"), Types, FString(TEXT("image,text")));
		TestTrue(TEXT("Caption text has the tile stats"), Caption.Contains(TEXT("Tile 0")) && Caption.Contains(TEXT("particles")));
	}
	UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TArray<FColor> Source;
	Source.Init(FColor(10, 20, 30, 255), 4);
	Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Source.GetData()));
	TSharedPtr<IModelContextProtocolTool> TextureView = Module->FindTool(TEXT("texture_view"));
	if (TestTrue(TEXT("texture_view registered"), TextureView.IsValid()))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("Texture"), Texture->GetPathName());
		Args->SetNumberField(TEXT("Size"), 64);
		FString Caption;
		TestEqual(TEXT("texture_view returns an image"), ContentTypes(TextureView->Run(Args), Caption), FString(TEXT("image")));
	}
	TestTrue(TEXT("Compare and cost proxies registered"), Module->FindTool(TEXT("niagara_compare_preview")).IsValid() && Module->FindTool(TEXT("niagara_cost_view")).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

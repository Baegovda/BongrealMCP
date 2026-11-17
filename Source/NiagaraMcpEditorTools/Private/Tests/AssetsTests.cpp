#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetMcpEditorTools.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "MaterialMcpEditorTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraEffectType.h"
#include "NiagaraParameterCollection.h"

/**
 * End-to-end check of the Asset toolset: textures (import, info, generic properties, duplicate, flipbook), render
 * targets (create, resize, draw a material, bake), material / Niagara parameter collections, effect types and
 * Asset Registry references. Assets go to /Game/NiagaraMcpTests/Assets/Run_<time>.
 *
 * Run: UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests NiagaraMcp.Assets;Quit" -unattended -nopause
 */
namespace AssetMcpTests
{
	FString MakeFolder()
	{
		return FString::Printf(TEXT("/Game/NiagaraMcpTests/Assets/Run_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	const FAssetMcpPropertyInfo* FindProperty(const FAssetMcpPropertyList& List, const TCHAR* Name)
	{
		return List.Properties.FindByPredicate([Name](const FAssetMcpPropertyInfo& Info) { return Info.Name == Name; });
	}

	const FAssetMcpCollectionParameter* FindParameter(const TArray<FAssetMcpCollectionParameter>& Parameters, const TCHAR* Name)
	{
		return Parameters.FindByPredicate([Name](const FAssetMcpCollectionParameter& Info) { return Info.Name == Name; });
	}

	bool ContainsPackage(const TArray<FAssetMcpReference>& References, const FString& ObjectPath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		return References.ContainsByPredicate([&PackageName](const FAssetMcpReference& Reference) { return Reference.PackageName == PackageName; });
	}

	/** Writes a 64x64 PNG (red ramp along X, green ramp along Y) and returns its absolute path. */
	FString WriteTestPng()
	{
		const int32 Size = 64;
		TArray<FColor> Pixels;
		Pixels.SetNum(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Pixels[Y * Size + X] = FColor(static_cast<uint8>(X * 4), static_cast<uint8>(Y * 4), 128, 255);
			}
		}
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NiagaraMcpTests/Assets/Gradient.png"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		const FImageView View(Pixels.GetData(), Size, Size, EGammaSpace::sRGB);
		return FImageUtils::SaveImageByExtension(*File, View) ? File : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMcpTexturesTest, "NiagaraMcp.Assets.TexturesAndRenderTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetMcpTexturesTest::RunTest(const FString& Parameters)
{
	using namespace AssetMcpTests;
	const FString Folder = MakeFolder();

	// ------------------------------------------------------------------ ImportTexture
	const FString PngFile = WriteTestPng();
	if (!TestFalse(TEXT("Test PNG written"), PngFile.IsEmpty()))
	{
		return false;
	}
	const FAssetMcpTextureInfo Imported = UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_Gradient"), false, TEXT("Masks"), true);
	AddInfo(FString::Printf(TEXT("Imported %s %dx%d %s %s mips=%d source=%dx%d %s"), *Imported.Path, Imported.Width, Imported.Height, *Imported.PixelFormat,
		*Imported.CompressionSettings, Imported.NumMips, Imported.SourceWidth, Imported.SourceHeight, *Imported.SourceFormat));
	if (!TestEqual(TEXT("Imported path"), Imported.Path, Folder + TEXT("/T_Gradient.T_Gradient")))
	{
		return false;
	}
	TestEqual(TEXT("Imported width"), Imported.Width, 64);
	TestEqual(TEXT("Imported source width"), Imported.SourceWidth, 64);
	TestEqual(TEXT("Imported compression"), Imported.CompressionSettings, FString(TEXT("TC_Masks")));
	TestFalse(TEXT("Imported sRGB off"), Imported.bSRGB);
	TestTrue(TEXT("Imported saved"), Imported.bSaved);

	AddExpectedMessagePlain(TEXT("does not exist"), ELogVerbosity::Warning);
	TestTrue(TEXT("Missing file is rejected"), UAssetMcpEditorTools::ImportTexture(PngFile + TEXT(".missing.png"), Folder + TEXT("/T_Missing"), true, TEXT("Auto"), false).Path.IsEmpty());
	AddExpectedMessagePlain(TEXT("is not an image file"), ELogVerbosity::Warning);
	TestTrue(TEXT("Non-image is rejected"), UAssetMcpEditorTools::ImportTexture(PngFile + TEXT(".txt"), Folder + TEXT("/T_Text"), true, TEXT("Auto"), false).Path.IsEmpty());
	AddExpectedMessagePlain(TEXT("Unknown compression setting"), ELogVerbosity::Warning);
	TestTrue(TEXT("Bad compression is rejected"), UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_BadCompression"), true, TEXT("Maskz"), false).Path.IsEmpty());

	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Imported.Path);
	if (!TestNotNull(TEXT("Imported texture loads"), Texture))
	{
		return false;
	}

	// ------------------------------------------------------------------ GetTextureInfo / GetAssetProperties / SetAssetProperty
	const FAssetMcpTextureInfo Info = UAssetMcpEditorTools::GetTextureInfo(Texture);
	TestEqual(TEXT("Info height"), Info.Height, 64);
	TestEqual(TEXT("Info address"), Info.AddressX, FString(TEXT("TA_Wrap")));

	const FAssetMcpPropertyList Compression = UAssetMcpEditorTools::GetAssetProperties(Texture, TEXT("compression"));
	const FAssetMcpPropertyInfo* CompressionProperty = FindProperty(Compression, TEXT("CompressionSettings"));
	if (TestNotNull(TEXT("CompressionSettings listed"), CompressionProperty))
	{
		TestEqual(TEXT("CompressionSettings value"), CompressionProperty->Value, FString(TEXT("TC_Masks")));
		TestTrue(TEXT("CompressionSettings valid values"), CompressionProperty->ValidValues.Contains(TEXT("TC_Grayscale")));
	}
	const FAssetMcpPropertyList AllTextureProperties = UAssetMcpEditorTools::GetAssetProperties(Texture, TEXT("Any"));
	const FAssetMcpPropertyInfo* SRGBProperty = FindProperty(AllTextureProperties, TEXT("SRGB"));
	if (TestNotNull(TEXT("SRGB listed"), SRGBProperty))
	{
		TestEqual(TEXT("SRGB is a bool with two values"), SRGBProperty->ValidValues.Num(), 2);
	}
	AddInfo(FString::Printf(TEXT("Texture has %d editable properties"), AllTextureProperties.Properties.Num()));

	TestEqual(TEXT("Set AddressX"), UAssetMcpEditorTools::SetAssetProperty(Texture, TEXT("AddressX"), TEXT("Clamp"), false).Value, FString(TEXT("TA_Clamp")));
	TestEqual(TEXT("Set LODGroup"), UAssetMcpEditorTools::SetAssetProperty(Texture, TEXT("LODGroup"), TEXT("TEXTUREGROUP_Effects"), false).Value, FString(TEXT("TEXTUREGROUP_Effects")));
	const FAssetMcpSetPropertyResult Grayscale = UAssetMcpEditorTools::SetAssetProperty(Texture, TEXT("CompressionSettings"), TEXT("TC_Grayscale"), true);
	TestEqual(TEXT("Set CompressionSettings"), Grayscale.Value, FString(TEXT("TC_Grayscale")));
	TestTrue(TEXT("Set saved"), Grayscale.bSaved);
	TestEqual(TEXT("Texture info after set"), UAssetMcpEditorTools::GetTextureInfo(Texture).AddressX, FString(TEXT("TA_Clamp")));

	AddExpectedMessagePlain(TEXT("Did you mean 'CompressionSettings'"), ELogVerbosity::Warning);
	TestTrue(TEXT("Misspelled property is rejected"), UAssetMcpEditorTools::SetAssetProperty(Texture, TEXT("CompresionSetings"), TEXT("TC_Masks"), false).Property.IsEmpty());
	AddExpectedMessagePlain(TEXT("is not an entry of"), ELogVerbosity::Warning);
	TestTrue(TEXT("Bad enum value is rejected"), UAssetMcpEditorTools::SetAssetProperty(Texture, TEXT("AddressY"), TEXT("Sideways"), false).Property.IsEmpty());
	TestEqual(TEXT("Rejected value left the texture unchanged"), UAssetMcpEditorTools::GetTextureInfo(Texture).AddressY, FString(TEXT("TA_Wrap")));

	// ------------------------------------------------------------------ DuplicateAsset / CreateFlipbookTexture
	const FAssetMcpAssetResult Copy = UAssetMcpEditorTools::DuplicateAsset(Texture, Folder + TEXT("/T_Gradient_Copy"), true);
	TestEqual(TEXT("Duplicate path"), Copy.Path, Folder + TEXT("/T_Gradient_Copy.T_Gradient_Copy"));
	TestEqual(TEXT("Duplicate class"), Copy.Class, FString(TEXT("Texture2D")));
	AddExpectedMessagePlain(TEXT("already exists"), ELogVerbosity::Warning);
	TestTrue(TEXT("Duplicate onto an existing asset is rejected"), UAssetMcpEditorTools::DuplicateAsset(Texture, Folder + TEXT("/T_Gradient_Copy"), false).Path.IsEmpty());

	UTexture2D* CopyTexture = LoadObject<UTexture2D>(nullptr, *Copy.Path);
	if (TestNotNull(TEXT("Copy loads"), CopyTexture))
	{
		const FAssetMcpTextureInfo Flipbook = UAssetMcpEditorTools::CreateFlipbookTexture({ Texture, CopyTexture, Texture }, 2, Folder + TEXT("/T_Flipbook"), true);
		AddInfo(FString::Printf(TEXT("Flipbook %s %dx%d source %dx%d"), *Flipbook.Path, Flipbook.Width, Flipbook.Height, Flipbook.SourceWidth, Flipbook.SourceHeight));
		TestEqual(TEXT("Flipbook source width (2 columns)"), Flipbook.SourceWidth, 128);
		TestEqual(TEXT("Flipbook source height (2 rows)"), Flipbook.SourceHeight, 128);
		TestEqual(TEXT("Flipbook keeps frame compression"), Flipbook.CompressionSettings, FString(TEXT("TC_Grayscale")));
		AddExpectedMessagePlain(TEXT("Columns must be"), ELogVerbosity::Warning);
		TestTrue(TEXT("Bad column count is rejected"), UAssetMcpEditorTools::CreateFlipbookTexture({ Texture }, 3, Folder + TEXT("/T_FlipbookBad"), false).Path.IsEmpty());

		// A legacy gamma (Pow22) frame first: converting the other frames to Pow22 used to assert and close the editor.
		// Built from the source pixels without PostEditChange, so no async texture build is running while it is set up.
		FImage FrameImage;
		Texture->Source.GetMipImage(FrameImage, 0, 0, 0);
		UTexture2D* Legacy = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Legacy->SRGB = true;
		Legacy->bUseLegacyGamma = true;
		Legacy->Source.Init(FImageView(FrameImage, FrameImage.RawData.GetData()));
		const FAssetMcpTextureInfo LegacyFlipbook = UAssetMcpEditorTools::CreateFlipbookTexture({ Legacy, Texture }, 2, Folder + TEXT("/T_FlipbookLegacy"), false);
		TestTrue(TEXT("Flipbook from a legacy gamma frame"), !LegacyFlipbook.Path.IsEmpty() && LegacyFlipbook.SourceWidth == 128);
	}

	// ------------------------------------------------------------------ Render targets
	const FAssetMcpTextureInfo Target = UAssetMcpEditorTools::CreateRenderTarget2D(Folder + TEXT("/RT_Generate"), 64, 64, TEXT("RGBA16f"), TEXT("0, 0, 0"), true);
	TestEqual(TEXT("Render target format"), Target.RenderTargetFormat, FString(TEXT("RTF_RGBA16f")));
	TestEqual(TEXT("Render target clear color"), Target.ClearColor, FString(TEXT("0.0, 0.0, 0.0, 1.0")));
	UTextureRenderTarget2D* RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *Target.Path);
	if (!TestNotNull(TEXT("Render target loads"), RenderTarget))
	{
		return false;
	}
	const FAssetMcpTextureInfo Resized = UAssetMcpEditorTools::ResizeRenderTarget2D(RenderTarget, 128, 32, true);
	TestEqual(TEXT("Resized width"), Resized.Width, 128);
	TestEqual(TEXT("Resized height"), Resized.Height, 32);
	TestEqual(TEXT("Render target property"), UAssetMcpEditorTools::SetAssetProperty(RenderTarget, TEXT("ClearColor"), TEXT("0, 0, 1, 1"), false).Value, FString(TEXT("0.0, 0.0, 1.0, 1.0")));
	AddExpectedMessagePlain(TEXT("Unknown render target format"), ELogVerbosity::Warning);
	TestTrue(TEXT("Bad render target format is rejected"), UAssetMcpEditorTools::CreateRenderTarget2D(Folder + TEXT("/RT_Bad"), 64, 64, TEXT("RGBA99"), TEXT("0, 0, 0, 1"), false).Path.IsEmpty());

	// A constant-color unlit material, drawn into the target and baked to a texture.
	const FMaterialMcpGraphInfo MaterialInfo = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_Constant"), false);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	if (!TestNotNull(TEXT("Material created"), Material))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false);
	const FMaterialMcpExpressionInfo Constant = UMaterialMcpEditorTools::AddExpression(Material, TEXT("Constant3Vector"), -300, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, Constant.Name, TEXT("Constant"), TEXT("1, 0.5, 0.25, 1"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Constant.Name, TEXT(""), TEXT("EmissiveColor"));
	TestTrue(TEXT("Constant material compiles"), UMaterialMcpEditorTools::CompileMaterial(Material, true).bSucceeded);

	const FAssetMcpDrawResult Draw = UAssetMcpEditorTools::DrawMaterialToRenderTarget(RenderTarget, Material);
	AddInfo(FString::Printf(TEXT("Center color after draw: %s"), *Draw.CenterColor));
	TArray<FString> Channels;
	Draw.CenterColor.ParseIntoArray(Channels, TEXT(","));
	if (TestEqual(TEXT("Center color has 4 channels"), Channels.Num(), 4))
	{
		TestTrue(TEXT("Center red ~1"), FMath::IsNearlyEqual(FCString::Atof(*Channels[0]), 1.0f, 0.02f));
		TestTrue(TEXT("Center green ~0.5"), FMath::IsNearlyEqual(FCString::Atof(*Channels[1]), 0.5f, 0.02f));
		TestTrue(TEXT("Center blue ~0.25"), FMath::IsNearlyEqual(FCString::Atof(*Channels[2]), 0.25f, 0.02f));
	}

	const FAssetMcpTextureInfo Baked = UAssetMcpEditorTools::CreateTextureFromRenderTarget(RenderTarget, Folder + TEXT("/T_Baked"), TEXT("HDR"), true);
	AddInfo(FString::Printf(TEXT("Baked %s %dx%d %s %s"), *Baked.Path, Baked.Width, Baked.Height, *Baked.PixelFormat, *Baked.SourceFormat));
	TestEqual(TEXT("Baked path"), Baked.Path, Folder + TEXT("/T_Baked.T_Baked"));
	TestEqual(TEXT("Baked width"), Baked.SourceWidth, 128);
	TestEqual(TEXT("Baked height"), Baked.SourceHeight, 32);
	TestEqual(TEXT("Baked compression"), Baked.CompressionSettings, FString(TEXT("TC_HDR")));
	TestTrue(TEXT("Baked saved"), Baked.bSaved);
	AddExpectedMessagePlain(TEXT("already exists"), ELogVerbosity::Warning);
	TestTrue(TEXT("Baking onto an existing asset is rejected"), UAssetMcpEditorTools::CreateTextureFromRenderTarget(RenderTarget, Folder + TEXT("/T_Baked"), TEXT("Auto"), false).Path.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMcpCollectionsTest, "NiagaraMcp.Assets.CollectionsAndReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetMcpCollectionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetMcpTests;
	const FString Folder = MakeFolder();

	// ------------------------------------------------------------------ Material parameter collection
	const FAssetMcpMaterialParameterCollectionInfo Created = UAssetMcpEditorTools::CreateMaterialParameterCollection(Folder + TEXT("/MPC_Weather"), true);
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *Created.Path);
	if (!TestNotNull(TEXT("MPC created"), Collection))
	{
		return false;
	}
	UAssetMcpEditorTools::SetMaterialParameterCollectionParameter(Collection, TEXT("WindStrength"), TEXT("0.5"), false);

	// A material reading the collection must stay valid when the collection layout changes.
	const FMaterialMcpGraphInfo MaterialInfo = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_UsesMPC"), false);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	if (!TestNotNull(TEXT("Material created"), Material))
	{
		return false;
	}
	UMaterialMcpEditorTools::SetMaterialProperty(Material, TEXT("ShadingModel"), TEXT("Unlit"), false);
	const FMaterialMcpExpressionInfo CollectionNode = UMaterialMcpEditorTools::AddExpression(Material, TEXT("CollectionParameter"), -300, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, CollectionNode.Name, TEXT("Collection"), Collection->GetPathName());
	UMaterialMcpEditorTools::SetExpressionProperty(Material, CollectionNode.Name, TEXT("ParameterName"), TEXT("WindStrength"));
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, CollectionNode.Name, TEXT(""), TEXT("EmissiveColor"));
	TestTrue(TEXT("Material using the MPC compiles"), UMaterialMcpEditorTools::CompileMaterial(Material, true).bSucceeded);

	const FAssetMcpMaterialParameterCollectionInfo WithVector = UAssetMcpEditorTools::SetMaterialParameterCollectionParameter(Collection, TEXT("Tint"), TEXT("1, 0.5, 0, 1"), false);
	const FAssetMcpCollectionParameter* Tint = FindParameter(WithVector.Parameters, TEXT("Tint"));
	if (TestNotNull(TEXT("Tint added"), Tint))
	{
		TestEqual(TEXT("Tint is a vector"), Tint->Type, FString(TEXT("Vector")));
		TestEqual(TEXT("Tint value"), Tint->Value, FString(TEXT("1.0, 0.5, 0.0, 1.0")));
	}
	const FAssetMcpMaterialParameterCollectionInfo Updated = UAssetMcpEditorTools::SetMaterialParameterCollectionParameter(Collection, TEXT("WindStrength"), TEXT("0.75"), true);
	TestTrue(TEXT("MPC saved"), Updated.bSaved);
	AddExpectedMessagePlain(TEXT("is a vector parameter"), ELogVerbosity::Warning);
	TestTrue(TEXT("Wrong component count is rejected"), UAssetMcpEditorTools::SetMaterialParameterCollectionParameter(Collection, TEXT("Tint"), TEXT("1, 2"), false).Path.IsEmpty());

	const FAssetMcpMaterialParameterCollectionInfo ReadBack = UAssetMcpEditorTools::GetMaterialParameterCollection(Collection);
	TestEqual(TEXT("MPC has 2 parameters"), ReadBack.Parameters.Num(), 2);
	const FAssetMcpCollectionParameter* Wind = FindParameter(ReadBack.Parameters, TEXT("WindStrength"));
	if (TestNotNull(TEXT("WindStrength listed"), Wind))
	{
		TestEqual(TEXT("WindStrength is a scalar"), Wind->Type, FString(TEXT("Scalar")));
		TestEqual(TEXT("WindStrength updated"), Wind->Value, FString(TEXT("0.75")));
	}
	TestTrue(TEXT("Material using the MPC still compiles after the layout change"), UMaterialMcpEditorTools::CompileMaterial(Material, true).bSucceeded);

	// ------------------------------------------------------------------ Niagara parameter collection
	// Namespaces are unique per project, so each run uses its own (earlier runs' collections stay on disk).
	const FString Namespace = FString::Printf(TEXT("McpWeather%s"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
	const FAssetMcpNiagaraParameterCollectionInfo NpcCreated = UAssetMcpEditorTools::CreateNiagaraParameterCollection(Folder + TEXT("/NPC_Weather"), Namespace, true);
	UNiagaraParameterCollection* Npc = LoadObject<UNiagaraParameterCollection>(nullptr, *NpcCreated.Path);
	if (!TestNotNull(TEXT("NPC created"), Npc))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("NPC namespace: %s"), *NpcCreated.Namespace));
	TestTrue(TEXT("NPC namespace"), NpcCreated.Namespace.StartsWith(TEXT("McpWeather")));
	AddExpectedMessagePlain(TEXT("is invalid"), ELogVerbosity::Warning);
	TestTrue(TEXT("Bad namespace is rejected"), UAssetMcpEditorTools::CreateNiagaraParameterCollection(Folder + TEXT("/NPC_Bad"), TEXT("Bad Namespace"), false).Path.IsEmpty());

	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("WindStrength"), TEXT("float"), TEXT("0.5"), false);
	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("WindDirection"), TEXT("vec3"), TEXT("1, 0, 0"), false);
	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("Tint"), TEXT("LinearColor"), TEXT("1, 0.5, 0, 1"), false);
	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("Enabled"), TEXT("bool"), TEXT("true"), false);
	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("Count"), TEXT("int"), TEXT("3"), false);
	UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("Spin"), TEXT("quat"));
	const FAssetMcpNiagaraParameterCollectionInfo NpcUpdated = UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("WindStrength"), TEXT("float"), TEXT("0.75"), true);
	TestTrue(TEXT("NPC saved"), NpcUpdated.bSaved);
	AddExpectedMessagePlain(TEXT("already exists as"), ELogVerbosity::Warning);
	TestTrue(TEXT("Type change is rejected"), UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("WindStrength"), TEXT("vec3"), TEXT("1, 1, 1"), false).Path.IsEmpty());
	AddExpectedMessagePlain(TEXT("Unknown Niagara type"), ELogVerbosity::Warning);
	TestTrue(TEXT("Unknown type is rejected"), UAssetMcpEditorTools::AddNiagaraParameterCollectionParameter(Npc, TEXT("Foo"), TEXT("vec7"), TEXT("1"), false).Path.IsEmpty());

	const FAssetMcpNiagaraParameterCollectionInfo NpcRead = UAssetMcpEditorTools::GetNiagaraParameterCollection(Npc);
	TestEqual(TEXT("NPC has 6 parameters"), NpcRead.Parameters.Num(), 6);
	if (const FAssetMcpCollectionParameter* NpcSpin = FindParameter(NpcRead.Parameters, TEXT("Spin")); TestNotNull(TEXT("NPC Spin"), NpcSpin))
	{
		TestEqual(TEXT("NPC Spin defaults to identity"), NpcSpin->Value, FString(TEXT("0.0, 0.0, 0.0, 1.0")));
	}
	for (const FAssetMcpCollectionParameter& Parameter : NpcRead.Parameters)
	{
		AddInfo(FString::Printf(TEXT("NPC %s (%s) %s = %s"), *Parameter.Name, *Parameter.FullName, *Parameter.Type, *Parameter.Value));
	}
	if (const FAssetMcpCollectionParameter* NpcWind = FindParameter(NpcRead.Parameters, TEXT("WindStrength")); TestNotNull(TEXT("NPC WindStrength"), NpcWind))
	{
		TestEqual(TEXT("NPC WindStrength value"), NpcWind->Value, FString(TEXT("0.75")));
		TestEqual(TEXT("NPC WindStrength full name"), NpcWind->FullName, FString::Printf(TEXT("NPC.%s.WindStrength"), *NpcRead.Namespace));
	}
	if (const FAssetMcpCollectionParameter* NpcDirection = FindParameter(NpcRead.Parameters, TEXT("WindDirection")); TestNotNull(TEXT("NPC WindDirection"), NpcDirection))
	{
		TestEqual(TEXT("NPC WindDirection value"), NpcDirection->Value, FString(TEXT("1.0, 0.0, 0.0")));
	}
	if (const FAssetMcpCollectionParameter* NpcEnabled = FindParameter(NpcRead.Parameters, TEXT("Enabled")); TestNotNull(TEXT("NPC Enabled"), NpcEnabled))
	{
		TestEqual(TEXT("NPC Enabled value"), NpcEnabled->Value, FString(TEXT("true")));
	}

	// ------------------------------------------------------------------ Niagara effect type + nested property paths
	const FAssetMcpAssetResult EffectTypeResult = UAssetMcpEditorTools::CreateNiagaraEffectType(Folder + TEXT("/NET_Ambient"), true);
	TestEqual(TEXT("Effect type class"), EffectTypeResult.Class, FString(TEXT("NiagaraEffectType")));
	UNiagaraEffectType* EffectType = LoadObject<UNiagaraEffectType>(nullptr, *EffectTypeResult.Path);
	if (!TestNotNull(TEXT("Effect type loads"), EffectType))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("already exists"), ELogVerbosity::Warning);
	TestTrue(TEXT("Existing effect type path is rejected"), UAssetMcpEditorTools::CreateNiagaraEffectType(Folder + TEXT("/NET_Ambient"), false).Path.IsEmpty());

	TestEqual(TEXT("UpdateFrequency"), UAssetMcpEditorTools::SetAssetProperty(EffectType, TEXT("UpdateFrequency"), TEXT("Low"), false).Value, FString(TEXT("Low")));
	const int32 LevelCount = EffectType->GetSystemScalabilitySettings().Settings.Num();
	AddInfo(FString::Printf(TEXT("New effect type has %d system scalability entries"), LevelCount));
	const FString LevelPath = FString::Printf(TEXT("SystemScalabilitySettings.Settings[%d]"), LevelCount);
	TestEqual(TEXT("Append scalability entry and set MaxDistance"), UAssetMcpEditorTools::SetAssetProperty(EffectType, LevelPath + TEXT(".MaxDistance"), TEXT("3000"), false).Value, FString(TEXT("3000.000000")));
	TestEqual(TEXT("Entry appended"), EffectType->GetSystemScalabilitySettings().Settings.Num(), LevelCount + 1);
	TestEqual(TEXT("Bitfield bool in array element"), UAssetMcpEditorTools::SetAssetProperty(EffectType, LevelPath + TEXT(".bCullByDistance"), TEXT("true"), true).Value, FString(TEXT("true")));
	TestTrue(TEXT("Nested value stored"), EffectType->GetSystemScalabilitySettings().Settings.Last().bCullByDistance && FMath::IsNearlyEqual(EffectType->GetSystemScalabilitySettings().Settings.Last().MaxDistance, 3000.0f));
	AddExpectedMessagePlain(TEXT("out of range"), ELogVerbosity::Warning);
	TestTrue(TEXT("Out of range index is rejected"), UAssetMcpEditorTools::SetAssetProperty(EffectType, TEXT("SystemScalabilitySettings.Settings[99].MaxDistance"), TEXT("1"), false).Property.IsEmpty());
	TestEqual(TEXT("Rejected index appended nothing"), EffectType->GetSystemScalabilitySettings().Settings.Num(), LevelCount + 1);
	const FAssetMcpPropertyList Scalability = UAssetMcpEditorTools::GetAssetProperties(EffectType, TEXT("Scalability"));
	TestTrue(TEXT("Scalability properties listed"), FindProperty(Scalability, TEXT("CullReaction")) != nullptr);

	// ------------------------------------------------------------------ Asset Registry references (material -> MPC)
	// The material was saved moments ago. Without a tick the editor's file watcher has not re-gathered it, so this
	// also checks that GetAssetReferences refreshes the queried package's own dependencies.
	const FAssetMcpReferences MaterialDependencies = UAssetMcpEditorTools::GetAssetReferences(Material->GetPathName(), false, true, 100);
	TestTrue(TEXT("MPC is a dependency of the material"), ContainsPackage(MaterialDependencies.Dependencies, Collection->GetPathName()));
	TestEqual(TEXT("Referencers skipped"), MaterialDependencies.Referencers.Num(), 0);
	if (const FAssetMcpReference* Dependency = MaterialDependencies.Dependencies.FindByPredicate([](const FAssetMcpReference& R) { return R.Class == TEXT("MaterialParameterCollection"); }))
	{
		TestEqual(TEXT("Dependency asset path"), Dependency->AssetPath, Collection->GetPathName());
	}
	else
	{
		AddError(TEXT("The MPC dependency has no class"));
	}
	const FAssetMcpReferences References = UAssetMcpEditorTools::GetAssetReferences(Collection->GetPathName(), true, true, 100);
	for (const FAssetMcpReference& Reference : References.Referencers)
	{
		AddInfo(FString::Printf(TEXT("MPC referencer: %s (%s)"), *Reference.PackageName, *Reference.Class));
	}
	TestTrue(TEXT("Material is a referencer of the MPC"), ContainsPackage(References.Referencers, Material->GetPathName()));
	AddExpectedMessagePlain(TEXT("No asset found"), ELogVerbosity::Warning);
	TestTrue(TEXT("Missing asset is rejected"), UAssetMcpEditorTools::GetAssetReferences(Folder + TEXT("/DoesNotExist"), true, true, 100).PackageName.IsEmpty());
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMcpConsolidateTest, "NiagaraMcp.Assets.ConsolidateDuplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetMcpConsolidateTest::RunTest(const FString& Parameters)
{
	using namespace AssetMcpTests;
	const FString Folder = MakeFolder() + TEXT("_Consolidate");
	const FString PngFile = WriteTestPng();
	if (!TestFalse(TEXT("Test PNG written"), PngFile.IsEmpty()))
	{
		return false;
	}
	// A, B, C: the same PNG with the same settings. D: same pixels, sRGB off. E: a duplicate of A (identical source).
	const FString A = UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_A"), true, TEXT("Auto"), true).Path;
	const FString B = UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_B"), true, TEXT("Auto"), true).Path;
	const FString C = UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_C"), true, TEXT("Auto"), true).Path;
	const FString D = UAssetMcpEditorTools::ImportTexture(PngFile, Folder + TEXT("/T_D"), false, TEXT("Auto"), true).Path;
	UTexture2D* TextureA = LoadObject<UTexture2D>(nullptr, *A);
	if (!TestNotNull(TEXT("T_A imported"), TextureA) || B.IsEmpty() || C.IsEmpty() || D.IsEmpty())
	{
		return false;
	}
	const FString E = UAssetMcpEditorTools::DuplicateAsset(TextureA, Folder + TEXT("/T_E"), true).Path;

	// A material that uses B: after the merge it must use A.
	const FMaterialMcpGraphInfo MaterialInfo = UMaterialMcpEditorTools::CreateMaterial(Folder + TEXT("/M_UsesB"), false);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	if (!TestNotNull(TEXT("Material created"), Material))
	{
		return false;
	}
	const FMaterialMcpExpressionInfo Sample = UMaterialMcpEditorTools::AddExpression(Material, TEXT("TextureSample"), -300, 0);
	UMaterialMcpEditorTools::SetExpressionProperty(Material, Sample.Name, TEXT("Texture"), B);
	UMaterialMcpEditorTools::ConnectToMaterialProperty(Material, Sample.Name, TEXT(""), TEXT("EmissiveColor"));
	TestTrue(TEXT("Material using T_B compiles and saves"), UMaterialMcpEditorTools::CompileMaterial(Material, true).bSucceeded);

	const FString Group = FString::Join(TArray<FString>{ A, B, C, D, E }, TEXT("|"));

	// Bad input is rejected before anything happens.
	AddExpectedMessagePlain(TEXT("at least one duplicate"), ELogVerbosity::Warning);
	TestEqual(TEXT("A group of one is rejected"), UAssetMcpEditorTools::ConsolidateAssets({ A }, true, false, false).Groups.Num(), 0);
	AddExpectedMessagePlain(TEXT("appears more than once"), ELogVerbosity::Warning);
	TestEqual(TEXT("An asset in two groups is rejected"), UAssetMcpEditorTools::ConsolidateAssets({ A + TEXT("|") + B, C + TEXT("|") + B }, true, false, false).Groups.Num(), 0);
	TestEqual(TEXT("A material is skipped without bAllowNonTextures"),
		UAssetMcpEditorTools::ConsolidateAssets({ MaterialInfo.MaterialPath + TEXT("|") + A }, true, false, false).SkippedCount, 1);

	// Dry run: B, C, E would go; D differs in sRGB.
	const FAssetMcpConsolidateResult Dry = UAssetMcpEditorTools::ConsolidateAssets({ Group }, true, false, false);
	TestTrue(TEXT("Dry run flag"), Dry.bDryRun);
	TestEqual(TEXT("Dry run removes 3"), Dry.RemovedCount, 3);
	TestEqual(TEXT("Dry run skips 1"), Dry.SkippedCount, 1);
	if (TestEqual(TEXT("Dry run has one group"), Dry.Groups.Num(), 1) && TestEqual(TEXT("Dry run skipped list"), Dry.Groups[0].Skipped.Num(), 1))
	{
		AddInfo(TEXT("Skipped: ") + Dry.Groups[0].Skipped[0]);
		TestTrue(TEXT("D is skipped for its sRGB setting"), Dry.Groups[0].Skipped[0].Contains(TEXT("SRGB")));
	}
	TestTrue(TEXT("Dry run changed nothing"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(B)));
	TestEqual(TEXT("With bAllowSettingDifferences D would go too"), UAssetMcpEditorTools::ConsolidateAssets({ Group }, true, true, false).RemovedCount, 4);

	// The real merge.
	const FAssetMcpConsolidateResult Done = UAssetMcpEditorTools::ConsolidateAssets({ Group }, false, false, false);
	AddInfo(FString::Printf(TEXT("Removed %d, skipped %d, saved %d, redirectors fixed %d, remaining %d"), Done.RemovedCount, Done.SkippedCount,
		Done.SavedPackages.Num(), Done.RedirectorsFixed, Done.Remaining.Num()));
	TestFalse(TEXT("Real run flag"), Done.bDryRun);
	TestEqual(TEXT("Removed 3"), Done.RemovedCount, 3);
	TestEqual(TEXT("Nothing left behind"), Done.Remaining.Num(), 0);
	for (const FString& Gone : { B, C, E })
	{
		TestFalse(FString::Printf(TEXT("%s deleted"), *Gone), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Gone)));
	}
	TestTrue(TEXT("Kept texture still exists"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(A)));
	TestTrue(TEXT("Skipped texture still exists"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(D)));
	TestTrue(TEXT("Material now references T_A"), ContainsPackage(UAssetMcpEditorTools::GetAssetReferences(A, true, false, 100).Referencers, MaterialInfo.MaterialPath));
	UMaterial* Reloaded = LoadObject<UMaterial>(nullptr, *MaterialInfo.MaterialPath);
	if (TestNotNull(TEXT("Material still loads"), Reloaded))
	{
		TArray<UTexture*> Used;
		Reloaded->GetUsedTextures(Used);
		TestTrue(TEXT("Material samples T_A"), Used.ContainsByPredicate([&A](const UTexture* T) { return T && T->GetPathName() == A; }));
	}
	return !HasAnyErrors();
}

#endif

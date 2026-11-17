#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "AssetMcpEditorTools.generated.h"

class UMaterialInterface;
class UMaterialParameterCollection;
class UNiagaraParameterCollection;
class UTexture;
class UTexture2D;
class UTextureRenderTarget2D;

/** Result of creating or duplicating an asset. */
USTRUCT()
struct FAssetMcpAssetResult
{
	GENERATED_BODY()

	/** Object path of the asset, e.g. /Game/FX/NET_Ambient.NET_Ambient. */
	UPROPERTY()
	FString Path;

	/** Asset class, e.g. NiagaraEffectType. */
	UPROPERTY()
	FString Class;

	UPROPERTY()
	bool bSaved = false;
};

/** One editable property of an asset. */
USTRUCT()
struct FAssetMcpPropertyInfo
{
	GENERATED_BODY()

	/** Property name; pass it to SetAssetProperty. */
	UPROPERTY()
	FString Name;

	/** C++ type, e.g. float, bool, TEnumAsByte<TextureCompressionSettings>, FLinearColor, TArray<...>. */
	UPROPERTY()
	FString Type;

	/** Current value as text: "0.5", "true", "TC_Masks", "1, 0.5, 0, 1", an asset path, or UE text "(A=1,B=2)". */
	UPROPERTY()
	FString Value;

	/** Details panel category. */
	UPROPERTY()
	FString Category;

	/** Accepted values for enum and bool properties; empty for other types. */
	UPROPERTY()
	TArray<FString> ValidValues;
};

/** The editable properties of an asset. */
USTRUCT()
struct FAssetMcpPropertyList
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString Class;

	UPROPERTY()
	TArray<FAssetMcpPropertyInfo> Properties;
};

/** Read-back of an asset property after SetAssetProperty. */
USTRUCT()
struct FAssetMcpSetPropertyResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	/** Property path as passed in. */
	UPROPERTY()
	FString Property;

	/** Value after the change (after the asset's own clamping / validation). */
	UPROPERTY()
	FString Value;

	UPROPERTY()
	bool bSaved = false;
};

/** A package found by GetAssetReferences. */
USTRUCT()
struct FAssetMcpReference
{
	GENERATED_BODY()

	/** Package name, e.g. /Game/FX/NS_Sparks. */
	UPROPERTY()
	FString PackageName;

	/** Object path of the main asset in the package, when the Asset Registry knows it. */
	UPROPERTY()
	FString AssetPath;

	/** Class of that asset (NiagaraSystem, Material, Texture2D...), when known. */
	UPROPERTY()
	FString Class;
};

/** Referencers and dependencies of an asset. */
USTRUCT()
struct FAssetMcpReferences
{
	GENERATED_BODY()

	UPROPERTY()
	FString PackageName;

	/** Packages that use this asset. */
	UPROPERTY()
	TArray<FAssetMcpReference> Referencers;

	/** Packages this asset uses (engine script packages /Script/... are omitted). */
	UPROPERTY()
	TArray<FAssetMcpReference> Dependencies;

	/** True when a list was cut at MaxResults. */
	UPROPERTY()
	bool bTruncated = false;
};

/** Texture or render target settings. */
USTRUCT()
struct FAssetMcpTextureInfo
{
	GENERATED_BODY()

	/** Object path of the texture. */
	UPROPERTY()
	FString Path;

	/** Texture2D, TextureRenderTarget2D, TextureCube... */
	UPROPERTY()
	FString Class;

	/** Size of the built (platform) texture, or of the render target. */
	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;

	/** GPU pixel format, e.g. PF_DXT5, PF_BC4, PF_FloatRGBA. */
	UPROPERTY()
	FString PixelFormat;

	UPROPERTY()
	bool bSRGB = false;

	/** TC_Default, TC_Masks, TC_Grayscale, TC_HDR, TC_Normalmap, ... */
	UPROPERTY()
	FString CompressionSettings;

	/** TEXTUREGROUP_World, TEXTUREGROUP_Effects, ... */
	UPROPERTY()
	FString LODGroup;

	/** TMGS_FromTextureGroup, TMGS_NoMipmaps, ... */
	UPROPERTY()
	FString MipGenSettings;

	/** TA_Wrap, TA_Clamp or TA_Mirror. */
	UPROPERTY()
	FString AddressX;

	UPROPERTY()
	FString AddressY;

	/** TF_Default, TF_Nearest, TF_Bilinear, TF_Trilinear. */
	UPROPERTY()
	FString Filter;

	UPROPERTY()
	bool bNeverStream = false;

	UPROPERTY()
	int32 NumMips = 0;

	/** Size of the imported source image (0 for render targets). */
	UPROPERTY()
	int32 SourceWidth = 0;

	UPROPERTY()
	int32 SourceHeight = 0;

	/** Source pixel format, e.g. TSF_BGRA8, TSF_RGBA16F. */
	UPROPERTY()
	FString SourceFormat;

	/** Render targets only: RTF_RGBA16f, RTF_RGBA8, RTF_R32f, ... */
	UPROPERTY()
	FString RenderTargetFormat;

	/** Render targets only: clear color "R, G, B, A". */
	UPROPERTY()
	FString ClearColor;

	UPROPERTY()
	bool bSaved = false;
};

/** Result of DrawMaterialToRenderTarget. */
USTRUCT()
struct FAssetMcpDrawResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString RenderTarget;

	UPROPERTY()
	FString Material;

	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;

	/** Linear color of the center pixel after drawing, "R, G, B, A" - a quick check that something was drawn. */
	UPROPERTY()
	FString CenterColor;
};

/** A parameter of a Material or Niagara parameter collection. */
USTRUCT()
struct FAssetMcpCollectionParameter
{
	GENERATED_BODY()

	/** Parameter name as used in materials / Niagara (Niagara: without the NPC.<Namespace>. prefix). */
	UPROPERTY()
	FString Name;

	/** Material collections: Scalar or Vector. Niagara collections: the Niagara type (NiagaraFloat, Vector3f, LinearColor...). */
	UPROPERTY()
	FString Type;

	/** Default value: "0.5", "1, 0.5, 0, 1", "true". */
	UPROPERTY()
	FString Value;

	/** Niagara collections only: full parameter name, e.g. NPC.Weather.WindStrength. */
	UPROPERTY()
	FString FullName;
};

/** A Material Parameter Collection. */
USTRUCT()
struct FAssetMcpMaterialParameterCollectionInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	/** Base collection this one derives from, if any. */
	UPROPERTY()
	FString Base;

	UPROPERTY()
	TArray<FAssetMcpCollectionParameter> Parameters;

	UPROPERTY()
	bool bSaved = false;
};

/** A Niagara Parameter Collection. */
USTRUCT()
struct FAssetMcpNiagaraParameterCollectionInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	/** Collection namespace; parameters are read in Niagara as NPC.<Namespace>.<Name>. */
	UPROPERTY()
	FString Namespace;

	/** Material parameter collection that drives matching scalar / vector parameters, if any. */
	UPROPERTY()
	FString SourceMaterialCollection;

	UPROPERTY()
	TArray<FAssetMcpCollectionParameter> Parameters;

	UPROPERTY()
	bool bSaved = false;
};

/** One duplicate group handled by ConsolidateAssets. */
USTRUCT()
struct FAssetMcpConsolidateGroup
{
	GENERATED_BODY()

	/** The asset that stays (object path). */
	UPROPERTY()
	FString Kept;

	/** Duplicates merged into Kept (dry run: the ones that would be). Their references now point at Kept and their packages are deleted. */
	UPROPERTY()
	TArray<FString> Removed;

	/** Duplicates left alone, as "path: reason" (not identical, different settings, failed to load...). */
	UPROPERTY()
	TArray<FString> Skipped;
};

/** Result of ConsolidateAssets. */
USTRUCT()
struct FAssetMcpConsolidateResult
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	TArray<FAssetMcpConsolidateGroup> Groups;

	/** Number of duplicates removed (dry run: that would be removed). */
	UPROPERTY()
	int32 RemovedCount = 0;

	UPROPERTY()
	int32 SkippedCount = 0;

	/** Packages that used a duplicate, re-saved pointing at the kept asset. */
	UPROPERTY()
	TArray<FString> SavedPackages;

	/** Redirectors left at the old paths by the merge that were deleted because nothing uses them any more. */
	UPROPERTY()
	int32 RedirectorsFixed = 0;

	/** Old paths that still exist on disk afterwards: a redirector something still uses (e.g. a package that failed to load or save), or a failed delete. */
	UPROPERTY()
	TArray<FString> Remaining;
};

/** Result of RenameAssets. */
USTRUCT()
struct FAssetMcpRenameResult
{
	GENERATED_BODY()

	UPROPERTY()
	bool bDryRun = true;

	/** Renamed (dry run: would be renamed), as "old -> new" object paths. */
	UPROPERTY()
	TArray<FString> Renamed;

	/** Left alone, as "old: reason" (target exists, invalid name, failed to load, engine refused...). */
	UPROPERTY()
	TArray<FString> Skipped;

	UPROPERTY()
	int32 RenamedCount = 0;

	UPROPERTY()
	int32 SkippedCount = 0;

	/** Old paths where a redirector had to stay because something still references the old path. */
	UPROPERTY()
	TArray<FString> RedirectorsLeft;
};

/**
 * Support assets for VFX work: textures, render targets, material and Niagara parameter collections, Niagara effect
 * types, plus generic asset operations (read / set any editable property, duplicate, find references).
 *
 * Workflow: ImportTexture (PNG/TGA/EXR/...) or generate one with CreateRenderTarget2D -> DrawMaterialToRenderTarget
 * (an unlit material from the Material toolset) -> CreateTextureFromRenderTarget; stitch frames into a SubUV sheet
 * with CreateFlipbookTexture; check results with GetTextureInfo. Shared global values go in
 * CreateMaterialParameterCollection / SetMaterialParameterCollectionParameter (materials) and
 * CreateNiagaraParameterCollection / AddNiagaraParameterCollectionParameter (Niagara). Scalability budgets go in
 * CreateNiagaraEffectType. Anything else on any asset: GetAssetProperties -> SetAssetProperty.
 * GetAssetReferences answers "which systems use this texture / material / module?".
 * Create tools never overwrite: pick a new path when an asset already exists.
 */
UCLASS(BlueprintType, Hidden)
class UAssetMcpEditorTools : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Lists the editable properties of any asset (texture, render target, effect type, parameter collection, data
	 * asset...) with their type, current value, category and, for enums and bools, the valid values.
	 *
	 * @param Asset The asset, e.g. /Game/FX/Textures/T_Noise.T_Noise
	 * @param Filter Case-insensitive substring matched against property name or category, e.g. "Compression", "LOD", "Address"; "Any" = no filter
	 * @return The properties
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets")
	static FAssetMcpPropertyList GetAssetProperties(UObject* Asset, const FString& Filter = TEXT("Any"));

	/**
	 * Sets an editable property of any asset through the editor's change notifications, so textures recompress,
	 * render targets reallocate and collections update their users. Examples:
	 *   Texture: CompressionSettings = TC_Masks, SRGB = false, LODGroup = TEXTUREGROUP_Effects, AddressX = TA_Clamp, Filter = TF_Nearest
	 *   Render target: RenderTargetFormat = RTF_RGBA8, ClearColor = "1, 0, 0, 1"
	 *   Effect type: UpdateFrequency = Low, CullReaction = DeactivateImmediateResume,
	 *                SystemScalabilitySettings.Settings[0].MaxDistance = 3000, SystemScalabilitySettings.Settings[0].bCullByDistance = true
	 * Use GetAssetProperties to see names and valid values. Material and Niagara graphs have their own toolsets.
	 *
	 * @param Asset The asset
	 * @param PropertyName Property name or dotted path into structs; array elements as Name[Index] (Index = array count appends an element), e.g. SystemScalabilitySettings.Settings[0].MaxDistance
	 * @param Value New value as text: "0.5", "true", enum entry ("TC_Masks" or display name), "1, 0.5, 0, 1" for vectors/colors, an asset path, or UE text "(X=1,Y=2)"
	 * @param bSave Save the asset after the change
	 * @return The value read back after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets")
	static FAssetMcpSetPropertyResult SetAssetProperty(UObject* Asset, const FString& PropertyName, const FString& Value, bool bSave = true);

	/**
	 * Duplicates an asset (any type) to a new path. Fails if the destination exists.
	 *
	 * @param Source The asset to copy, e.g. /Game/FX/NS_Sparks
	 * @param NewAssetPath New asset path, e.g. /Game/FX/NS_Sparks_Blue
	 * @param bSave Save the copy
	 * @return The new asset
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets")
	static FAssetMcpAssetResult DuplicateAsset(UObject* Source, const FString& NewAssetPath, bool bSave = true);

	/**
	 * Lists the packages that reference an asset and/or that it depends on, from the Asset Registry (works for
	 * unloaded assets). Answers "which Niagara systems use this material / texture / module script?". The queried
	 * package is re-read from disk first; other packages are as the editor last gathered them (save them first).
	 *
	 * @param AssetPath Asset or package path, e.g. /Game/FX/Textures/T_Noise or /Game/FX/M_Ribbon.M_Ribbon
	 * @param bReferencers Include the packages that use this asset
	 * @param bDependencies Include the packages this asset uses
	 * @param MaxResults Maximum entries per list (1-1000)
	 * @return Referencers and dependencies
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets")
	static FAssetMcpReferences GetAssetReferences(const FString& AssetPath, bool bReferencers = true, bool bDependencies = true, int32 MaxResults = 100);

	/**
	 * Imports an image file from disk as a texture asset, without dialogs.
	 *
	 * @param FilePath Absolute path of a .png, .tga, .exr, .jpg, .jpeg, .hdr, .bmp, .tif or .psd file, e.g. D:/Textures/noise.png
	 * @param DestinationAssetPath New asset path, e.g. /Game/FX/Textures/T_Noise
	 * @param bSRGB sRGB color space: true for color textures, false for masks, noise, flow maps and data
	 * @param CompressionSettings TC_Default, TC_Masks, TC_Grayscale, TC_HDR, TC_Normalmap, TC_Alpha, TC_VectorDisplacementmap (the TC_ prefix is optional); "Auto" keeps the importer's choice
	 * @param bSave Save the new texture
	 * @return The new texture
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Textures")
	static FAssetMcpTextureInfo ImportTexture(const FString& FilePath, const FString& DestinationAssetPath, bool bSRGB = true, const FString& CompressionSettings = TEXT("Auto"), bool bSave = true);

	/**
	 * Reads texture settings: size, pixel format, sRGB, compression, LOD group, mips, address modes, filter, source
	 * size; for render targets also the render target format and clear color.
	 *
	 * @param Texture The texture or render target
	 * @return The texture settings
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets|Textures")
	static FAssetMcpTextureInfo GetTextureInfo(UTexture* Texture);

	/**
	 * Creates a 2D render target asset (for DrawMaterialToRenderTarget, Niagara Render Target 2D data interfaces, ...).
	 *
	 * @param AssetPath New asset path, e.g. /Game/FX/RT_Noise
	 * @param Width Width in pixels (1-8192)
	 * @param Height Height in pixels (1-8192)
	 * @param Format RTF_RGBA16f ("Auto" = RTF_RGBA16f), RTF_RGBA8, RTF_RGBA8_SRGB, RTF_RGBA32f, RTF_R16f, RTF_R32f, RTF_RG16f, RTF_R8, RTF_RG8, RTF_RGB10A2 (RTF_ prefix optional)
	 * @param ClearColor Clear color "R, G, B, A", e.g. "0, 0, 0, 1" ("Auto" = opaque black)
	 * @param bSave Save the new render target
	 * @return The new render target
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|RenderTargets")
	static FAssetMcpTextureInfo CreateRenderTarget2D(const FString& AssetPath, int32 Width = 512, int32 Height = 512, const FString& Format = TEXT("RTF_RGBA16f"), const FString& ClearColor = TEXT("0, 0, 0, 1"), bool bSave = true);

	/**
	 * Resizes a 2D render target (contents are cleared).
	 *
	 * @param RenderTarget The render target
	 * @param Width New width in pixels (1-8192)
	 * @param Height New height in pixels (1-8192)
	 * @param bSave Save the render target after the change
	 * @return The render target after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|RenderTargets")
	static FAssetMcpTextureInfo ResizeRenderTarget2D(UTextureRenderTarget2D* RenderTarget, int32 Width, int32 Height, bool bSave = true);

	/**
	 * Draws a material on a full-screen quad into a render target (UV0 0-1 covers the target). Use an Unlit material
	 * whose EmissiveColor computes the pattern (noise, gradient, mask...), then bake it with CreateTextureFromRenderTarget.
	 * Render target contents are not saved with the asset.
	 *
	 * @param RenderTarget The render target
	 * @param Material The material or material instance to draw
	 * @return Draw result with the center pixel color
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|RenderTargets")
	static FAssetMcpDrawResult DrawMaterialToRenderTarget(UTextureRenderTarget2D* RenderTarget, UMaterialInterface* Material);

	/**
	 * Bakes the current contents of a render target into a new static texture asset (e.g. after DrawMaterialToRenderTarget).
	 *
	 * @param RenderTarget The render target
	 * @param NewAssetPath New texture path, e.g. /Game/FX/Textures/T_GeneratedNoise
	 * @param CompressionSettings TC_Default, TC_Masks, TC_Grayscale, TC_HDR, ... (TC_ prefix optional); "Auto" picks TC_Default for 8-bit targets and TC_HDR for float targets
	 * @param bSave Save the new texture
	 * @return The new texture
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|RenderTargets")
	static FAssetMcpTextureInfo CreateTextureFromRenderTarget(UTextureRenderTarget2D* RenderTarget, const FString& NewAssetPath, const FString& CompressionSettings = TEXT("Auto"), bool bSave = true);

	/**
	 * Stitches equally sized textures into one flipbook / SubUV atlas, left to right then top to bottom
	 * (Rows = ceil(Frames / Columns); empty cells are black). Use the result with Sub UV Animation (SubImage Size = Columns x Rows).
	 * Frames are converted to the first frame's source format; the atlas takes the first frame's sRGB and compression.
	 *
	 * @param Frames Frame textures in order, e.g. ["/Game/FX/T_Fire_00", "/Game/FX/T_Fire_01"]
	 * @param Columns Frames per row
	 * @param NewAssetPath New texture path, e.g. /Game/FX/Textures/T_Fire_SubUV
	 * @param bSave Save the new texture
	 * @return The new atlas texture
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Textures")
	static FAssetMcpTextureInfo CreateFlipbookTexture(const TArray<UTexture2D*>& Frames, int32 Columns, const FString& NewAssetPath, bool bSave = true);

	/**
	 * Creates an empty Material Parameter Collection (global scalar / vector values that materials read with a
	 * CollectionParameter expression).
	 *
	 * @param AssetPath New asset path, e.g. /Game/FX/MPC_Weather
	 * @param bSave Save the new asset
	 * @return The new collection
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Collections")
	static FAssetMcpMaterialParameterCollectionInfo CreateMaterialParameterCollection(const FString& AssetPath, bool bSave = true);

	/**
	 * Adds a parameter to a Material Parameter Collection, or updates the default of an existing one. One number makes a
	 * scalar ("0.5"); three or four make a vector ("1, 0.5, 0" or "1, 0.5, 0, 1"). Materials that use the collection
	 * are updated (and recompiled when the layout changes).
	 *
	 * @param Collection The collection
	 * @param ParameterName Parameter name, e.g. WindStrength
	 * @param Value "0.5" (scalar) or "1, 0.5, 0, 1" (vector)
	 * @param bSave Save the collection after the change
	 * @return The collection after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Collections")
	static FAssetMcpMaterialParameterCollectionInfo SetMaterialParameterCollectionParameter(UMaterialParameterCollection* Collection, FName ParameterName, const FString& Value, bool bSave = true);

	/**
	 * Lists the scalar and vector parameters of a Material Parameter Collection with their defaults.
	 *
	 * @param Collection The collection
	 * @return The collection
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets|Collections")
	static FAssetMcpMaterialParameterCollectionInfo GetMaterialParameterCollection(UMaterialParameterCollection* Collection);

	/**
	 * Creates an empty Niagara Parameter Collection (global values Niagara systems read as NPC.<Namespace>.<Name>).
	 *
	 * @param AssetPath New asset path, e.g. /Game/FX/NPC_Weather
	 * @param Namespace Namespace (letters, digits, underscores), e.g. Weather; "Auto" uses the asset name. Made unique among collections.
	 * @param bSave Save the new asset
	 * @return The new collection
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Collections")
	static FAssetMcpNiagaraParameterCollectionInfo CreateNiagaraParameterCollection(const FString& AssetPath, const FString& Namespace = TEXT("Auto"), bool bSave = true);

	/**
	 * Adds a parameter to a Niagara Parameter Collection and sets its default value; if a parameter of that name and
	 * type exists, only its default is updated.
	 *
	 * @param Collection The collection
	 * @param ParameterName Parameter name without namespace, e.g. WindStrength
	 * @param Type float, int, bool, vec2, vec3, vec4, color (LinearColor), position, quat or an enum path
	 * @param DefaultValue Default as text: "0.5", "1, 0, 0", "1, 0.5, 0, 1", "true", an enum entry; "Default" = zero (identity for quat) for a new parameter
	 * @param bSave Save the collection after the change
	 * @return The collection after the change
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Collections")
	static FAssetMcpNiagaraParameterCollectionInfo AddNiagaraParameterCollectionParameter(UNiagaraParameterCollection* Collection, FName ParameterName, const FString& Type, const FString& DefaultValue = TEXT("Default"), bool bSave = true);

	/**
	 * Lists the parameters of a Niagara Parameter Collection with their types and defaults.
	 *
	 * @param Collection The collection
	 * @return The collection
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets|Collections")
	static FAssetMcpNiagaraParameterCollectionInfo GetNiagaraParameterCollection(UNiagaraParameterCollection* Collection);

	/**
	 * Creates a Niagara Effect Type (scalability and culling settings shared by systems). Edit it afterwards with
	 * SetAssetProperty, e.g. UpdateFrequency = Low, CullReaction = Deactivate,
	 * SystemScalabilitySettings.Settings[0].bCullByDistance = true, SystemScalabilitySettings.Settings[0].MaxDistance = 5000
	 * (index = current count appends a new entry). Assign it to a system with SetAssetProperty(System, "EffectType", <effect type path>).
	 *
	 * @param AssetPath New asset path, e.g. /Game/FX/NET_Ambient
	 * @param bSave Save the new asset
	 * @return The new effect type
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets|Niagara")
	static FAssetMcpAssetResult CreateNiagaraEffectType(const FString& AssetPath, bool bSave = true);

	// ==== v0.5 VISION: begin ====

	/**
	 * Shows a texture so you can look at it: the color image plus each channel as its own grayscale tile, with an
	 * optional flipbook grid drawn on top (lines between Columns x Rows cells). Channels All gives 6 tiles in 3 columns
	 * (row 1: RGB, RGBA over a checkerboard, R; row 2: G, B, A), RGB gives 4 tiles (RGB, R, G, B in 2 columns), R / G /
	 * B / A one tile. Pixels come from the texture's source data, so compression does not matter; 8 / 16-bit sources
	 * show their stored values (a mask value of 0.5 is mid grey), float (HDR) sources are converted to sRGB (values
	 * above 1 clip). Non-square textures keep their aspect (dark grey letterbox). The top-level MCP tool texture_view
	 * returns the same picture as a real image.
	 * Example: ViewTexture(/Game/FX/T_Smoke_SubUV, "All", 8, 8) to check an 8x8 smoke flipbook and its alpha.
	 *
	 * @param Texture The texture
	 * @param Channels All, RGB, R, G, B or A
	 * @param GridColumns Flipbook columns to outline (0 = no grid)
	 * @param GridRows Flipbook rows to outline (0 = no grid)
	 * @param Size Pixel size of each square tile (64-1024)
	 * @return The image (PNG)
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Assets|Textures")
	static FToolsetImage ViewTexture(UTexture2D* Texture, const FString& Channels = TEXT("All"), int32 GridColumns = 0, int32 GridRows = 0, int32 Size = 512);


	/**
	 * Merges duplicate assets: every reference to a duplicate (materials, Niagara systems, any asset, loaded or not) is
	 * moved to the kept asset, then the duplicate is deleted. Works on whole groups at once; run with bDryRun = true
	 * first to see what would happen. Cannot be undone (Undo and restore points do not cover deletes). Closes all open
	 * asset editors (the engine's consolidation requires it).
	 * Textures are only merged when their source pixels are byte-identical (same size, format, mips, slices and
	 * source payload) and, unless bAllowSettingDifferences, the same SRGB / CompressionSettings / LODGroup /
	 * MipGenSettings / Filter / AddressX / AddressY / bFlipGreenChannel / CompressionNoAlpha, so nothing that used a
	 * duplicate changes look. Other asset classes need bAllowNonTextures (no content check is done for them).
	 * Example group: "/Game/FX/T_Smoke|/Game/Packs/T_Smoke_copy|/Game/Old/T_Smoke2" keeps T_Smoke.
	 *
	 * @param Groups One entry per group: the asset to keep, then its duplicates, separated by "|" (package or object paths; at most 200 groups per call)
	 * @param bDryRun Only check and report; change nothing
	 * @param bAllowSettingDifferences Also merge textures whose settings (sRGB, compression, ...) differ; users of a duplicate then get the kept texture's settings
	 * @param bAllowNonTextures Allow groups of non-texture assets (same class required, content is not compared)
	 * @return Per-group result, counts, saved packages and redirector fix-up
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets")
	static FAssetMcpConsolidateResult ConsolidateAssets(const TArray<FString>& Groups, bool bDryRun = true, bool bAllowSettingDifferences = false, bool bAllowNonTextures = false);

	/**
	 * Renames and/or moves assets, updating every reference (loaded or not: users are loaded, fixed and re-saved) and
	 * deleting the redirector at the old path when nothing needs it any more. Run with bDryRun = true first. Cannot be
	 * undone by Undo; to revert, rename back. Targets that already exist are skipped, never overwritten.
	 * Example entries: "/Game/Packs/T_123|T_Smoke_Puff_01" (rename in place),
	 * "/Game/Packs/T_123|/Game/FX/Smoke/T_Smoke_Puff_01" (move and rename).
	 *
	 * @param Renames One entry per asset: old package or object path, "|", then the new name or new full package path (at most 500 per call)
	 * @param bDryRun Only validate and report; change nothing
	 * @return Renamed / skipped lists and any redirectors that had to stay
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Assets")
	static FAssetMcpRenameResult RenameAssets(const TArray<FString>& Renames, bool bDryRun = true);
	// ==== v0.5 VISION: end ====
};

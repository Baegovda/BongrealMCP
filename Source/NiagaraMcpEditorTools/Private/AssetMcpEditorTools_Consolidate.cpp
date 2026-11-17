#include "AssetMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraMcpCommon.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

using namespace NiagaraMcp;

namespace AssetMcpConsolidate
{
	static constexpr int32 MaxGroups = 200;

	/** Texture settings that change how a texture looks or samples; a merge must not silently change them. */
	static const TCHAR* const ComparedTextureSettings[] = {
		TEXT("SRGB"), TEXT("CompressionSettings"), TEXT("LODGroup"), TEXT("MipGenSettings"), TEXT("Filter"),
		TEXT("AddressX"), TEXT("AddressY"), TEXT("bFlipGreenChannel"), TEXT("CompressionNoAlpha"),
	};

	FString ToObjectPath(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.Contains(TEXT(".")))
		{
			return Trimmed;
		}
		return Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
	}

	/** Loads an asset by package or object path; returns null with a reason on failure. */
	UObject* LoadAsset(const FString& Path, FString& OutReason)
	{
		const FString ObjectPath = ToObjectPath(Path);
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			OutReason = FString::Printf(TEXT("not a valid asset path (%s)"), *Reason.ToString());
			return nullptr;
		}
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			OutReason = TEXT("no such asset");
			return nullptr;
		}
		UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Object == nullptr)
		{
			OutReason = TEXT("could not be loaded (check the asset name)");
			return nullptr;
		}
		if (Object->IsA<UObjectRedirector>())
		{
			OutReason = TEXT("is a redirector, not an asset");
			return nullptr;
		}
		return Object;
	}

	FString SettingText(UTexture* Texture, const TCHAR* Name)
	{
		const FProperty* Property = FindFProperty<FProperty>(Texture->GetClass(), Name);
		if (Property == nullptr)
		{
			return FString();
		}
		FString Text;
		Property->ExportText_InContainer(0, Text, Texture, Texture, nullptr, PPF_None);
		return Text;
	}

	/** Empty when the two textures have byte-identical source pixels; otherwise why not. */
	FString CompareTextureSource(UTexture* Keep, UTexture* Other, const FSharedBuffer& KeepPayload)
	{
		FTextureSource& A = Keep->Source;
		FTextureSource& B = Other->Source;
		if (!A.IsValid() || !B.IsValid())
		{
			return TEXT("has no source data");
		}
		if (A.GetSizeX() != B.GetSizeX() || A.GetSizeY() != B.GetSizeY())
		{
			return FString::Printf(TEXT("different size (%lldx%lld vs %lldx%lld)"), B.GetSizeX(), B.GetSizeY(), A.GetSizeX(), A.GetSizeY());
		}
		if (A.GetNumSlices() != B.GetNumSlices() || A.GetNumMips() != B.GetNumMips() || A.GetNumLayers() != B.GetNumLayers() || A.GetNumBlocks() != B.GetNumBlocks())
		{
			return TEXT("different slices / mips / layers / blocks");
		}
		for (int32 Layer = 0; Layer < A.GetNumLayers(); ++Layer)
		{
			if (A.GetFormat(Layer) != B.GetFormat(Layer))
			{
				return TEXT("different source format");
			}
		}
		if (A.GetSourceCompression() != B.GetSourceCompression())
		{
			return TEXT("different source compression (pixels not compared)");
		}
		const FSharedBuffer OtherPayload = B.GetBulkDataPayload();
		if (KeepPayload.GetSize() != OtherPayload.GetSize() || KeepPayload.GetSize() == 0
			|| FMemory::Memcmp(KeepPayload.GetData(), OtherPayload.GetData(), KeepPayload.GetSize()) != 0)
		{
			return TEXT("different pixels");
		}
		return FString();
	}

	FString CompareTextureSettings(UTexture* Keep, UTexture* Other)
	{
		TArray<FString> Differences;
		for (const TCHAR* Name : ComparedTextureSettings)
		{
			const FString KeepValue = SettingText(Keep, Name);
			const FString OtherValue = SettingText(Other, Name);
			if (KeepValue != OtherValue)
			{
				Differences.Add(FString::Printf(TEXT("%s %s vs %s"), Name, *OtherValue, *KeepValue));
			}
		}
		return Differences.Num() ? TEXT("different settings (") + FString::Join(Differences, TEXT(", ")) + TEXT(")") : FString();
	}

	/**
	 * Re-saves every package that still references one of the redirector packages (maps included: the engine's own
	 * rename does not touch unloaded maps), then deletes each redirector nothing references any more.
	 */
	void FixupRedirectorPackages(IAssetRegistry& Registry, const TArray<FString>& RedirectorPackages, TArray<FString>& OutFixed, TArray<FString>& OutLeft, TArray<FString>& OutSaved)
	{
		TSet<FName> RedirectorNames;
		// Load the redirectors first: users loaded afterwards resolve hard references through them, and soft
		// references are rewritten on save from the redirects recorded while loading.
		TArray<TStrongObjectPtr<UObjectRedirector>> Redirectors;
		for (const FString& Package : RedirectorPackages)
		{
			if (UObjectRedirector* Redirector = LoadObject<UObjectRedirector>(nullptr, *ToObjectPath(Package), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Redirectors.Emplace(Redirector);
				RedirectorNames.Add(FName(*Package));
			}
		}
		TSet<FName> UserNames;
		for (const FName& Name : RedirectorNames)
		{
			TArray<FName> Names;
			Registry.GetReferencers(Name, Names);
			for (const FName& User : Names)
			{
				if (!RedirectorNames.Contains(User) && !User.ToString().StartsWith(TEXT("/Script/")))
				{
					UserNames.Add(User);
				}
			}
		}
		TArray<TStrongObjectPtr<UPackage>> Users;
		TArray<UPackage*> ToSave;
		for (const FName& User : UserNames)
		{
			if (UPackage* Package = LoadPackage(nullptr, *User.ToString(), LOAD_NoWarn | LOAD_Quiet))
			{
				Users.Emplace(Package);
				Package->MarkPackageDirty();
				ToSave.Add(Package);
			}
		}
		if (ToSave.Num() > 0)
		{
			UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ true);
			TArray<FString> SavedFiles;
			for (UPackage* Package : ToSave)
			{
				OutSaved.AddUnique(Package->GetName());
				FString Filename;
				if (FPackageName::DoesPackageExist(Package->GetName(), &Filename))
				{
					SavedFiles.Add(FPaths::ConvertRelativePathToFull(Filename));
				}
			}
			Registry.ScanFilesSynchronous(SavedFiles, true);
		}
		Users.Empty();

		for (TStrongObjectPtr<UObjectRedirector>& Redirector : Redirectors)
		{
			const FString Package = Redirector->GetOutermost()->GetName();
			TArray<FName> StillUsedBy;
			Registry.GetReferencers(FName(*Package), StillUsedBy);
			StillUsedBy.RemoveAll([&RedirectorNames](const FName& Name) { return RedirectorNames.Contains(Name); });
			if (StillUsedBy.Num() == 0)
			{
				UObject* Object = Redirector.Get();
				Redirector.Reset();
				if (ObjectTools::DeleteObjectsUnchecked({ Object }) > 0)
				{
					OutFixed.Add(Package);
					continue;
				}
				OutLeft.Add(Package + TEXT(": could not be deleted (see the output log)"));
				continue;
			}
			TArray<FString> UserList;
			for (const FName& Name : StillUsedBy)
			{
				UserList.Add(Name.ToString());
			}
			OutLeft.Add(Package + TEXT(": still used by ") + FString::Join(UserList, TEXT(", ")));
		}
	}
}

FAssetMcpConsolidateResult UAssetMcpEditorTools::ConsolidateAssets(const TArray<FString>& Groups, bool bDryRun, bool bAllowSettingDifferences, bool bAllowNonTextures)
{
	using namespace AssetMcpConsolidate;

	if (Groups.Num() == 0 || Groups.Num() > MaxGroups)
	{
		ReportError(FString::Printf(TEXT("Pass 1-%d groups (got %d). Example: \"/Game/FX/T_Smoke|/Game/Packs/T_Smoke_copy\""), MaxGroups, Groups.Num()));
		return {};
	}
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		ReportError(TEXT("Stop Play In Editor first: assets cannot be merged while PIE is running."));
		return {};
	}

	// Parse and validate every group before touching anything.
	TArray<TArray<FString>> Parsed;
	TSet<FString> Seen;
	for (const FString& Group : Groups)
	{
		TArray<FString> Paths;
		Group.ParseIntoArray(Paths, TEXT("|"), true);
		for (FString& Path : Paths)
		{
			Path = FPackageName::ObjectPathToPackageName(ToObjectPath(Path));
		}
		if (Paths.Num() < 2)
		{
			ReportError(FString::Printf(TEXT("Group '%s' needs the asset to keep and at least one duplicate, separated by '|'."), *Group));
			return {};
		}
		for (const FString& Path : Paths)
		{
			bool bAlreadySeen = false;
			Seen.Add(Path, &bAlreadySeen);
			if (bAlreadySeen)
			{
				ReportError(FString::Printf(TEXT("'%s' appears more than once across the groups. Each asset may be in one group only."), *Path));
				return {};
			}
		}
		Parsed.Add(MoveTemp(Paths));
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (Registry.IsLoadingAssets())
	{
		Registry.WaitForCompletion();
	}

	// Engine consolidation and fix-up may ask questions on failures; never block the MCP request with a modal.
	TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);

	FAssetMcpConsolidateResult Result;
	Result.bDryRun = bDryRun;
	TArray<FString> RemovedPackages;

	for (const TArray<FString>& Paths : Parsed)
	{
		FAssetMcpConsolidateGroup& Out = Result.Groups.AddDefaulted_GetRef();
		FString Reason;
		// Loaded per group: consolidation runs garbage collection, so objects loaded for later groups would not survive.
		TStrongObjectPtr<UObject> Keep(LoadAsset(Paths[0], Reason));
		Out.Kept = Keep ? Keep->GetPathName() : Paths[0];
		if (!Keep)
		{
			for (int32 Index = 1; Index < Paths.Num(); ++Index)
			{
				Out.Skipped.Add(FString::Printf(TEXT("%s: the asset to keep %s"), *Paths[Index], *Reason));
			}
			Result.SkippedCount += Paths.Num() - 1;
			continue;
		}
		UTexture* KeepTexture = Cast<UTexture>(Keep.Get());
		FSharedBuffer KeepPayload;
		if (KeepTexture)
		{
			KeepPayload = KeepTexture->Source.GetBulkDataPayload();
		}

		TArray<TStrongObjectPtr<UObject>> Duplicates;
		for (int32 Index = 1; Index < Paths.Num(); ++Index)
		{
			UObject* Other = LoadAsset(Paths[Index], Reason);
			FString Problem;
			if (Other == nullptr)
			{
				Problem = Reason;
			}
			else if (Other->GetClass() != Keep->GetClass())
			{
				Problem = FString::Printf(TEXT("different class (%s vs %s)"), *Other->GetClass()->GetName(), *Keep->GetClass()->GetName());
			}
			else if (KeepTexture)
			{
				UTexture* OtherTexture = CastChecked<UTexture>(Other);
				Problem = CompareTextureSource(KeepTexture, OtherTexture, KeepPayload);
				if (Problem.IsEmpty() && !bAllowSettingDifferences)
				{
					Problem = CompareTextureSettings(KeepTexture, OtherTexture);
				}
			}
			else if (!bAllowNonTextures)
			{
				Problem = FString::Printf(TEXT("is a %s, not a texture (pass bAllowNonTextures; content is not compared)"), *Other->GetClass()->GetName());
			}

			if (Problem.IsEmpty())
			{
				Duplicates.Emplace(Other);
			}
			else
			{
				Out.Skipped.Add(FString::Printf(TEXT("%s: %s"), *Paths[Index], *Problem));
			}
		}
		Result.SkippedCount += Out.Skipped.Num();

		if (Duplicates.Num() == 0)
		{
			continue;
		}
		if (bDryRun)
		{
			for (const TStrongObjectPtr<UObject>& Duplicate : Duplicates)
			{
				Out.Removed.Add(Duplicate->GetPathName());
			}
			Result.RemovedCount += Duplicates.Num();
			continue;
		}

		TArray<UObject*> ToMerge;
		TArray<FString> MergePaths;
		TSet<FName> MergePackages;
		for (const TStrongObjectPtr<UObject>& Duplicate : Duplicates)
		{
			ToMerge.Add(Duplicate.Get());
			MergePaths.Add(Duplicate->GetPathName());
			MergePackages.Add(Duplicate->GetOutermost()->GetFName());
		}
		// The engine replaces references only in loaded objects (unloaded ones would go through a redirector, and its
		// fix-up shows a modal report). So load every package that uses a duplicate first; consolidation then rewrites
		// them in memory and they are saved below.
		TArray<TStrongObjectPtr<UPackage>> Referencers;
		for (const FName& PackageName : MergePackages)
		{
			TArray<FName> Names;
			Registry.GetReferencers(PackageName, Names);
			for (const FName& Name : Names)
			{
				if (MergePackages.Contains(Name) || Name == Keep->GetOutermost()->GetFName() || Name.ToString().StartsWith(TEXT("/Script/")))
				{
					continue;
				}
				if (UPackage* Loaded = LoadPackage(nullptr, *Name.ToString(), LOAD_NoWarn | LOAD_Quiet))
				{
					Referencers.Emplace(Loaded);
				}
			}
		}
		// The duplicates are about to be deleted: drop our references so the engine's delete can collect them.
		Duplicates.Empty();
		KeepPayload.Reset();

		ObjectTools::FConsolidationResults Consolidation = ObjectTools::ConsolidateObjects(Keep.Get(), ToMerge, /*bShowDeleteConfirmation*/ false);

		TSet<FString> Failed;
		for (const TObjectPtr<UObject>& Object : Consolidation.FailedConsolidationObjs)
		{
			if (Object)
			{
				Failed.Add(Object->GetPathName());
			}
		}
		for (const TObjectPtr<UObject>& Object : Consolidation.InvalidConsolidationObjs)
		{
			if (Object)
			{
				Failed.Add(Object->GetPathName());
			}
		}

		TArray<UPackage*> ToSave;
		TArray<FString> SavedFiles;
		for (const TObjectPtr<UPackage>& Package : Consolidation.DirtiedPackages)
		{
			if (Package && !Package->GetName().StartsWith(TEXT("/Temp/")) && !MergePackages.Contains(Package->GetFName()))
			{
				ToSave.Add(Package.Get());
			}
		}
		if (ToSave.Num() > 0 && Failed.Num() == 0)
		{
			UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty*/ true);
			for (UPackage* Package : ToSave)
			{
				Result.SavedPackages.AddUnique(Package->GetName());
				FString Filename;
				if (FPackageName::DoesPackageExist(Package->GetName(), &Filename))
				{
					SavedFiles.Add(FPaths::ConvertRelativePathToFull(Filename));
				}
			}
		}
		Referencers.Empty();
		if (SavedFiles.Num() > 0)
		{
			// Refresh the dependency lists of the re-saved packages so the referencer check below sees the new state.
			Registry.ScanFilesSynchronous(SavedFiles, true);
		}

		for (const FString& MergePath : MergePaths)
		{
			if (Failed.Contains(MergePath))
			{
				Out.Skipped.Add(FString::Printf(TEXT("%s: the engine could not merge it (see the output log); nothing in this group was saved"), *MergePath));
				++Result.SkippedCount;
				continue;
			}
			Out.Removed.Add(MergePath);
			++Result.RemovedCount;
			const FString PackageName = FPackageName::ObjectPathToPackageName(MergePath);
			RemovedPackages.Add(PackageName);

			// Consolidation leaves a redirector at the old path. Once nothing points at it any more, delete it.
			UObjectRedirector* Redirector = FindObject<UObjectRedirector>(nullptr, *MergePath);
			if (Redirector == nullptr)
			{
				continue;
			}
			TArray<FName> StillUsedBy;
			Registry.GetReferencers(FName(*PackageName), StillUsedBy);
			StillUsedBy.RemoveAll([&MergePackages](const FName& Name) { return MergePackages.Contains(Name); });
			if (StillUsedBy.Num() == 0)
			{
				if (ObjectTools::DeleteObjectsUnchecked({ Redirector }) > 0)
				{
					++Result.RedirectorsFixed;
				}
			}
			else
			{
				// Keep the redirector on disk so that package still loads; report it.
				SaveAssetSilently(Redirector);
			}
		}
	}

	if (!bDryRun)
	{
		for (const FString& PackageName : RemovedPackages)
		{
			if (FPackageName::DoesPackageExist(PackageName))
			{
				Result.Remaining.Add(PackageName);
			}
		}
	}
	return Result;
}

FAssetMcpRenameResult UAssetMcpEditorTools::RenameAssets(const TArray<FString>& Renames, bool bDryRun)
{
	using namespace AssetMcpConsolidate;
	static constexpr int32 MaxRenames = 500;

	if (Renames.Num() == 0 || Renames.Num() > MaxRenames)
	{
		ReportError(FString::Printf(TEXT("Pass 1-%d renames (got %d). Example: \"/Game/Packs/T_123|T_Smoke_Puff_01\""), MaxRenames, Renames.Num()));
		return {};
	}
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		ReportError(TEXT("Stop Play In Editor first: assets cannot be renamed while PIE is running."));
		return {};
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (Registry.IsLoadingAssets())
	{
		Registry.WaitForCompletion();
	}
	TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);

	FAssetMcpRenameResult Result;
	Result.bDryRun = bDryRun;

	struct FPlanned
	{
		TStrongObjectPtr<UObject> Asset;
		FString OldPackage;
		FString NewPackage;
	};
	TArray<FPlanned> Planned;
	TSet<FString> Targets;
	TSet<FString> Sources;

	auto Skip = [&Result](const FString& What, const FString& Why)
	{
		Result.Skipped.Add(FString::Printf(TEXT("%s: %s"), *What, *Why));
		++Result.SkippedCount;
	};

	for (const FString& Entry : Renames)
	{
		FString Old, Target;
		if (!Entry.Split(TEXT("|"), &Old, &Target))
		{
			Skip(Entry, TEXT("expected \"old path|new name or new path\""));
			continue;
		}
		Old = Old.TrimStartAndEnd();
		Target = Target.TrimStartAndEnd();
		const FString OldPackage = FPackageName::ObjectPathToPackageName(ToObjectPath(Old));
		FString NewPackage = Target.StartsWith(TEXT("/"))
			? FPackageName::ObjectPathToPackageName(ToObjectPath(Target))
			: FPackageName::GetLongPackagePath(OldPackage) / Target;

		FText Reason;
		const FString NewName = FPackageName::GetShortName(NewPackage);
		if (!FPackageName::IsValidLongPackageName(NewPackage, false, &Reason) || !FName::IsValidXName(NewName, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS, &Reason))
		{
			Skip(Old, FString::Printf(TEXT("invalid new name '%s' (%s)"), *NewPackage, *Reason.ToString()));
			continue;
		}
		if (NewPackage == OldPackage)
		{
			Skip(Old, TEXT("new name is the same"));
			continue;
		}
		if (NewPackage.Equals(OldPackage, ESearchCase::IgnoreCase))
		{
			Skip(Old, TEXT("case-only renames are not supported (rename to a different name)"));
			continue;
		}
		bool bTargetTaken = false;
		Targets.Add(NewPackage.ToLower(), &bTargetTaken);
		if (bTargetTaken || FPackageName::DoesPackageExist(NewPackage) || FindPackage(nullptr, *NewPackage) != nullptr)
		{
			Skip(Old, FString::Printf(TEXT("'%s' already exists or is the target of another entry"), *NewPackage));
			continue;
		}
		bool bSourceTwice = false;
		Sources.Add(OldPackage, &bSourceTwice);
		if (bSourceTwice)
		{
			Skip(Old, TEXT("listed more than once"));
			continue;
		}
		FString LoadReason;
		UObject* Asset = LoadAsset(OldPackage, LoadReason);
		if (Asset == nullptr)
		{
			Skip(Old, LoadReason);
			continue;
		}
		Planned.Add({ TStrongObjectPtr<UObject>(Asset), OldPackage, NewPackage });
	}

	if (bDryRun)
	{
		for (const FPlanned& Plan : Planned)
		{
			Result.Renamed.Add(Plan.OldPackage + TEXT(" -> ") + Plan.NewPackage);
		}
		Result.RenamedCount = Planned.Num();
		return Result;
	}
	if (Planned.Num() == 0)
	{
		return Result;
	}

	TArray<FAssetRenameData> Data;
	for (const FPlanned& Plan : Planned)
	{
		Data.Emplace(Plan.Asset.Get(), FPackageName::GetLongPackagePath(Plan.NewPackage), FPackageName::GetShortName(Plan.NewPackage));
	}
	FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get().RenameAssets(Data);

	TArray<FString> WithRedirector;
	for (const FPlanned& Plan : Planned)
	{
		const FString NewObjectPath = ToObjectPath(Plan.NewPackage);
		const bool bNewThere = FindObject<UObject>(nullptr, *NewObjectPath) != nullptr || FPackageName::DoesPackageExist(Plan.NewPackage);
		if (!bNewThere)
		{
			Skip(Plan.OldPackage, TEXT("the engine did not rename it (see the output log)"));
			continue;
		}
		Result.Renamed.Add(Plan.OldPackage + TEXT(" -> ") + Plan.NewPackage);
		++Result.RenamedCount;
		// The engine leaves a redirector when some user was not updated (unloaded maps, for one); it may not be on disk yet.
		if (FindObject<UObjectRedirector>(nullptr, *ToObjectPath(Plan.OldPackage)) != nullptr || FPackageName::DoesPackageExist(Plan.OldPackage))
		{
			WithRedirector.Add(Plan.OldPackage);
		}
	}
	Planned.Empty();
	if (WithRedirector.Num() > 0)
	{
		TArray<FString> Fixed;
		FixupRedirectorPackages(Registry, WithRedirector, Fixed, Result.RedirectorsLeft, Result.SavedPackages);
	}
	return Result;
}

FAssetMcpFixRedirectorsResult UAssetMcpEditorTools::FixRedirectors(const FString& Path, bool bDryRun)
{
	using namespace AssetMcpConsolidate;
	static constexpr int32 MaxRedirectors = 2000;

	const FString Trimmed = Path.TrimStartAndEnd();
	FText Reason;
	const FString PackagePath = FPackageName::ObjectPathToPackageName(Trimmed);
	if (!FPackageName::IsValidLongPackageName(PackagePath, false, &Reason))
	{
		ReportError(FString::Printf(TEXT("'%s' is not a valid folder or package path (%s). Example: /Game/Textures"), *Path, *Reason.ToString()));
		return {};
	}
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		ReportError(TEXT("Stop Play In Editor first: redirectors cannot be fixed while PIE is running."));
		return {};
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (Registry.IsLoadingAssets())
	{
		Registry.WaitForCompletion();
	}
	TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	if (FPackageName::DoesPackageExist(PackagePath))
	{
		Filter.PackageNames.Add(FName(*PackagePath));
	}
	else
	{
		Filter.PackagePaths.Add(FName(*PackagePath));
		Filter.bRecursivePaths = true;
	}
	TArray<FAssetData> Found;
	Registry.GetAssets(Filter, Found);

	FAssetMcpFixRedirectorsResult Result;
	Result.bDryRun = bDryRun;
	Result.FoundCount = Found.Num();
	if (Found.Num() > MaxRedirectors)
	{
		Found.SetNum(MaxRedirectors);
		Result.bTruncated = true;
	}
	TArray<FString> Packages;
	for (const FAssetData& Asset : Found)
	{
		const FString Package = Asset.PackageName.ToString();
		Packages.Add(Package);
		TArray<FName> Users;
		Registry.GetReferencers(Asset.PackageName, Users);
		FString Target;
		if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
		{
			Target = Redirector->DestinationObject ? Redirector->DestinationObject->GetPathName() : TEXT("(missing target)");
		}
		Result.Found.Add(FString::Printf(TEXT("%s -> %s (%d referencers)"), *Package, *Target, Users.Num()));
	}
	if (bDryRun || Packages.Num() == 0)
	{
		return Result;
	}
	FixupRedirectorPackages(Registry, Packages, Result.Fixed, Result.Left, Result.SavedPackages);
	Result.FixedCount = Result.Fixed.Num();
	return Result;
}

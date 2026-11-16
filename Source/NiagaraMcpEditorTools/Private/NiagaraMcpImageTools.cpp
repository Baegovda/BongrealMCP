#include "NiagaraMcpImageTools.h"

#include "Dom/JsonObject.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "ModelContextProtocolToolResults.h"
#include "NiagaraMcpCommon.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"

namespace NiagaraMcp::ImageTools
{
	namespace
	{
		const TCHAR* NiagaraToolset = TEXT("NiagaraMcpEditorTools.NiagaraMcpEditorTools");
		const TCHAR* MaterialToolset = TEXT("NiagaraMcpEditorTools.MaterialMcpEditorTools");
		const TCHAR* AssetToolset = TEXT("NiagaraMcpEditorTools.AssetMcpEditorTools");

		UE::ToolsetRegistry::FToolsetRegistry* GetRegistry()
		{
			auto Subsystem = UToolsetRegistrySubsystem::Get();
			return Subsystem.HasError() ? nullptr : &Subsystem.GetValue()->ToolsetRegistry;
		}

		/** The registry tool's entry ({name, description, inputSchema, ...}) from its toolset schema. */
		TSharedPtr<FJsonObject> FindToolSchema(const FString& ToolsetName, const FString& FullToolName)
		{
			UE::ToolsetRegistry::FToolsetRegistry* Registry = GetRegistry();
			TSharedPtr<UE::ToolsetRegistry::FToolset> Toolset = Registry ? Registry->Find(ToolsetName) : nullptr;
			if (!Toolset.IsValid())
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Schema;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Toolset->GetJsonSchema()), Schema) || !Schema.IsValid())
			{
				return nullptr;
			}
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!Schema->TryGetArrayField(TEXT("tools"), Tools))
			{
				return nullptr;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry)->GetStringField(TEXT("name")) == FullToolName)
				{
					return *Entry;
				}
			}
			return nullptr;
		}

		/** "/Game/FX/NS_A" -> "/Game/FX/NS_A.NS_A" for string asset arguments (the registry loads object paths). */
		void NormalizeAssetPaths(const TSharedPtr<FJsonObject>& Arguments)
		{
			if (!Arguments.IsValid())
			{
				return;
			}
			for (auto& Pair : Arguments->Values)
			{
				FString Text;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Text) && Text.StartsWith(TEXT("/")))
				{
					FString Leaf;
					Text.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					if (!Leaf.IsEmpty() && !Leaf.Contains(TEXT(".")) && !Leaf.Contains(TEXT(":")))
					{
						Pair.Value = MakeShared<FJsonValueString>(Text + TEXT(".") + Leaf);
					}
				}
			}
		}

		class FImageProxyTool final : public IModelContextProtocolTool
		{
		public:
			FImageProxyTool(FString InName, FString InToolset, FString InFunction, FString InPreamble)
				: Name(MoveTemp(InName)), Toolset(MoveTemp(InToolset)), FullToolName(Toolset + TEXT(".") + InFunction), Preamble(MoveTemp(InPreamble))
			{
			}

			virtual FString GetName() const override { return Name; }

			virtual FString GetDescription() const override
			{
				FString Description = Preamble;
				if (const TSharedPtr<FJsonObject> Entry = FindToolSchema(Toolset, FullToolName))
				{
					FString ToolDescription;
					if (Entry->TryGetStringField(TEXT("description"), ToolDescription) && !ToolDescription.IsEmpty())
					{
						Description += TEXT("\n\n") + ToolDescription;
					}
				}
				return Description;
			}

			virtual TSharedPtr<FJsonObject> GetInputJsonSchema() const override
			{
				if (const TSharedPtr<FJsonObject> Entry = FindToolSchema(Toolset, FullToolName))
				{
					const TSharedPtr<FJsonObject>* Input = nullptr;
					if (Entry->TryGetObjectField(TEXT("inputSchema"), Input) && Input && Input->IsValid())
					{
						return *Input;
					}
				}
				return IModelContextProtocolTool::GetInputJsonSchema();
			}

			virtual FModelContextProtocolToolResult Run(const TSharedPtr<FJsonObject>& Params) override
			{
				UE::ToolsetRegistry::FToolsetRegistry* Registry = GetRegistry();
				if (Registry == nullptr)
				{
					return UE::ModelContextProtocol::MakeErrorResult(TEXT("The Toolset Registry is not available."));
				}

				TSharedPtr<FJsonObject> Arguments = Params.IsValid() ? Params : MakeShared<FJsonObject>();
				NormalizeAssetPaths(Arguments);
				FString Json;
				FJsonSerializer::Serialize(Arguments.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));

				TFuture<TValueOrError<FString, FString>> Future = Registry->ExecuteTool(FullToolName, Json);
				if (!Future.IsReady())
				{
					return UE::ModelContextProtocol::MakeErrorResult(TEXT("The capture did not complete synchronously."));
				}
				TValueOrError<FString, FString> Result = Future.Consume();
				if (Result.HasError())
				{
					return UE::ModelContextProtocol::MakeErrorResult(Result.GetError());
				}

				// {"returnValue": {"mimeType": "image/png", "data": "<base64>"}} for FToolsetImage tools, or
				// {"returnValue": {"image": {...}, "caption": "...", ...}} for tools returning an image with numbers (keys are camelCase).
				TSharedPtr<FJsonObject> Output;
				if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.GetValue()), Output) || !Output.IsValid())
				{
					return UE::ModelContextProtocol::MakeErrorResult(TEXT("Unexpected capture result."));
				}
				const TSharedPtr<FJsonObject>* Wrapped = nullptr;
				const TSharedPtr<FJsonObject> Value = Output->TryGetObjectField(TEXT("returnValue"), Wrapped) && Wrapped ? *Wrapped : Output;
				const TSharedPtr<FJsonObject>* Nested = nullptr;
				const TSharedPtr<FJsonObject> Image = !Value->HasField(TEXT("data")) && Value->TryGetObjectField(TEXT("image"), Nested) && Nested && Nested->IsValid() ? *Nested : Value;
				FString Data;
				FString MimeType;
				if (!Image->TryGetStringField(TEXT("data"), Data) || Data.IsEmpty())
				{
					return UE::ModelContextProtocol::MakeErrorResult(TEXT("The capture returned no image."));
				}
				if (!Image->TryGetStringField(TEXT("mimeType"), MimeType) || MimeType.IsEmpty())
				{
					MimeType = TEXT("image/png");
				}
				FModelContextProtocolToolResult ImageResult = UE::ModelContextProtocol::MakeImageResult(MimeType, MoveTemp(Data));

				// The caption (per-tile numbers and hints) goes along as a text content item after the image.
				FString Caption;
				if (Value != Image && Value->TryGetStringField(TEXT("caption"), Caption) && !Caption.IsEmpty() && ImageResult.JsonObject.IsValid())
				{
					TArray<TSharedPtr<FJsonValue>> Content = ImageResult.JsonObject->HasTypedField<EJson::Array>(TEXT("content"))
						? ImageResult.JsonObject->GetArrayField(TEXT("content")) : TArray<TSharedPtr<FJsonValue>>();
					Content.Add(MakeShared<FJsonValueObject>(UE::ModelContextProtocol::MakeTextContentObject(Caption)));
					ImageResult.JsonObject->SetArrayField(TEXT("content"), Content);
				}
				return ImageResult;
			}

		private:
			FString Name;
			FString Toolset;
			FString FullToolName;
			FString Preamble;
		};

		TArray<TSharedRef<IModelContextProtocolTool>> GTools;
		FDelegateHandle GRefreshHandle;

		void AddTools()
		{
			IModelContextProtocolModule* Module = IModelContextProtocolModule::Get();
			if (Module == nullptr)
			{
				return;
			}
			if (GTools.Num() == 0)
			{
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("niagara_capture_preview"), NiagaraToolset, TEXT("CapturePreview"),
					TEXT("Renders a Niagara System off-screen at several simulation times and returns ONE contact-sheet image you can look at, followed by a text caption with numbers per tile (particles, coverage, luminance, color, screen bounds) and hints for empty or broken tiles. Use it after edits to check the effect visually (shape, color over life, timing). Same arguments as the NiagaraMcpEditorTools CapturePreview tool, but the result is a real image instead of base64 text.")));
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("niagara_compare_preview"), NiagaraToolset, TEXT("ComparePreview"),
					TEXT("Before/after visual diff of a Niagara System: the first call stores a named baseline render, later calls with the same BaselineName re-render with the same camera and return the new image with changed pixels tinted red plus a caption with changed-pixel percentages. Same arguments as the NiagaraMcpEditorTools ComparePreview tool, but the result is a real image.")));
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("niagara_cost_view"), NiagaraToolset, TEXT("CaptureCostView"),
					TEXT("Renders a Niagara System off-screen in the ShaderComplexity view mode and returns the image plus a caption with coverage, share of expensive (red or worse) pixels and mean cost per tile. Same arguments as the NiagaraMcpEditorTools CaptureCostView tool, but the result is a real image.")));
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("material_capture_preview"), MaterialToolset, TEXT("CapturePreview"),
					TEXT("Renders a material or material instance on a preview mesh and returns the image you can look at. Same arguments as the MaterialMcpEditorTools CapturePreview tool, but the result is a real image instead of base64 text.")));
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("texture_view"), AssetToolset, TEXT("ViewTexture"),
					TEXT("Shows a texture asset as an image you can look at: the color image and its R, G, B and A channels as separate grayscale tiles, optionally with a flipbook grid. Same arguments as the AssetMcpEditorTools ViewTexture tool, but the result is a real image instead of base64 text.")));
				GTools.Add(MakeShared<FImageProxyTool>(TEXT("asset_thumbnails"), NiagaraToolset, TEXT("PreviewAssets"),
					TEXT("Shows the editor thumbnails of up to 16 assets (textures, materials, meshes, Niagara systems) in one image, 4 per row in the given order, so you can choose between candidates found with FindAssets. Same arguments as the NiagaraMcpEditorTools PreviewAssets tool, but the result is a real image.")));
			}
			for (const TSharedRef<IModelContextProtocolTool>& Tool : GTools)
			{
				if (!Module->FindTool(Tool->GetName()).IsValid())
				{
					Module->AddTool(Tool);
				}
			}
		}
	}

	void Register()
	{
		IModelContextProtocolModule* Module = IModelContextProtocolModule::Get();
		if (Module == nullptr)
		{
			UE_LOG(LogNiagaraMcp, Log, TEXT("Unreal MCP is not loaded; the image tools are not registered."));
			return;
		}
		AddTools();
		// RefreshTools releases every tool; add ours back afterwards.
		if (!GRefreshHandle.IsValid())
		{
			GRefreshHandle = Module->OnRefreshTools().AddStatic(&AddTools);
		}
	}

	void Unregister()
	{
		if (IModelContextProtocolModule* Module = IModelContextProtocolModule::Get())
		{
			for (const TSharedRef<IModelContextProtocolTool>& Tool : GTools)
			{
				Module->RemoveTool(Tool);
			}
			if (GRefreshHandle.IsValid())
			{
				Module->OnRefreshTools().Remove(GRefreshHandle);
			}
		}
		GRefreshHandle.Reset();
		GTools.Reset();
	}
}

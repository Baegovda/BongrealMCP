#include "NiagaraMcpEditorTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraActions.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraGraph.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEmitter.h"
#include "NiagaraMcpCommon.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "NiagaraMcpEditorTools"

using namespace NiagaraMcp;

namespace NiagaraMcpGraph
{
	// UNiagaraNode::ReallocatePins is exported but protected, and UNiagaraNodeWithDynamicPins::OnNewTypedPinAdded is a
	// protected virtual. Member pointers formed through derived types are the standard way to reach them; these types
	// are never instantiated.
	struct FNiagaraNodeAccess : public UNiagaraNode
	{
		using FReallocatePinsFn = bool (UNiagaraNode::*)(bool);
		static FReallocatePinsFn ReallocatePinsFn() { return &FNiagaraNodeAccess::ReallocatePins; }
	};

	struct FDynamicPinsAccess : public UNiagaraNodeWithDynamicPins
	{
		using FOnNewTypedPinAddedFn = void (UNiagaraNodeWithDynamicPins::*)(UEdGraphPin*&);
		static FOnNewTypedPinAddedFn OnNewTypedPinAddedFn() { return &FDynamicPinsAccess::OnNewTypedPinAdded; }
	};

	const TCHAR* MapBaseClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapBase");
	const TCHAR* MapGetClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet");
	const TCHAR* MapSetClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet");

	UNiagaraGraph* GetGraph(UNiagaraScript* Script, bool bForWrite)
	{
		if (Script == nullptr)
		{
			ReportError(TEXT("Script is null or could not be loaded. Pass an asset path such as /Game/FX/NM_Example.NM_Example."));
			return nullptr;
		}
		UObject* Owner = Script->GetOutermostObject();
		if (bForWrite && !EnsureNoOpenEditor(Owner))
		{
			return nullptr;
		}
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
		if (Graph == nullptr)
		{
			ReportError(FString::Printf(TEXT("Script '%s' has no editable graph."), *Script->GetPathName()));
		}
		return Graph;
	}

	FString NodeIdOf(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::Digits) : FString();
	}

	UEdGraphNode* FindNode(UNiagaraGraph& Graph, const FString& NodeId)
	{
		FGuid Guid;
		const bool bIsGuid = FGuid::Parse(NodeId, Guid);
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node && ((bIsGuid && Node->NodeGuid == Guid) || Node->GetName() == NodeId))
			{
				return Node;
			}
		}
		ReportError(FString::Printf(TEXT("Node '%s' not found. Use GetScriptGraph for node ids."), *NodeId));
		return nullptr;
	}

	bool IsAddPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc && Pin->PinName == TEXT("Add");
	}

	UEdGraphPin* FindPin(UEdGraphNode& Node, FName PinName, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && (Pin->PinName == PinName || Pin->PinName.ToString().Equals(PinName.ToString(), ESearchCase::IgnoreCase)))
			{
				return Pin;
			}
		}
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				Names.Add(FString::Printf(TEXT("'%s'"), *Pin->PinName.ToString()));
			}
		}
		const TCHAR* DirectionText = Direction == EGPD_Output ? TEXT("Output") : TEXT("Input");
		ReportError(FString::Printf(TEXT("%s pin '%s' not found on node '%s'. %s pins: %s"), DirectionText, *PinName.ToString(),
			*Node.GetNodeTitle(ENodeTitleType::ListView).ToString(), DirectionText, *FString::Join(Names, TEXT(", "))));
		return nullptr;
	}

	FString* FindStringProperty(UObject* Object, FName Name)
	{
		FStrProperty* Property = FindFProperty<FStrProperty>(Object->GetClass(), Name);
		return Property ? Property->GetPropertyValuePtr_InContainer(Object) : nullptr;
	}

	void MarkNodeChanged(UEdGraphNode* Node, const TCHAR* Reason)
	{
		if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
		{
			NiagaraNode->MarkNodeRequiresSynchronization(Reason, true);
		}
		if (UEdGraph* Graph = Node ? Node->GetGraph() : nullptr)
		{
			Graph->NotifyGraphChanged();
		}
	}

	bool IsMapGet(const UEdGraphNode* Node)
	{
		UClass* MapGetClass = FindObject<UClass>(nullptr, MapGetClassPath);
		return Node && MapGetClass && Node->GetClass()->IsChildOf(MapGetClass);
	}

	/** Map Get: output pin persistent guid -> its hidden default-value input pin (private UPROPERTY, read by reflection). */
	const TMap<FGuid, FGuid>* GetMapGetDefaultPinMap(const UEdGraphNode* Node)
	{
		FMapProperty* Property = Node ? FindFProperty<FMapProperty>(Node->GetClass(), TEXT("PinOutputToPinDefaultPersistentId")) : nullptr;
		return Property ? Property->ContainerPtrToValuePtr<TMap<FGuid, FGuid>>(Node) : nullptr;
	}

	/** Module-input default of a Map Get output pin, from the graph's script variable (the "parameter panel" value). */
	FString GetScriptVariableDefault(const UEdGraphNode* Node, const UEdGraphPin* OutputPin)
	{
		const UNiagaraGraph* Graph = Node ? Cast<UNiagaraGraph>(Node->GetGraph()) : nullptr;
		const UNiagaraScriptVariable* ScriptVariable = Graph ? Graph->GetScriptVariable(OutputPin->PinName) : nullptr;
		if (ScriptVariable == nullptr || ScriptVariable->DefaultMode != ENiagaraDefaultMode::Value)
		{
			return FString();
		}
		const FNiagaraTypeDefinition& Type = ScriptVariable->Variable.GetType();
		const uint8* Data = ScriptVariable->GetDefaultValueData();
		return (Data && Type.GetScriptStruct()) ? ExportStructValue(Type.GetScriptStruct(), Data, Type.GetEnum()) : FString();
	}

	FNiagaraMcpGraphNode DescribeNode(UEdGraphNode* Node)
	{
		FNiagaraMcpGraphNode Info;
		if (Node == nullptr)
		{
			return Info;
		}
		Info.NodeId = NodeIdOf(Node);
		Info.Class = Node->GetClass()->GetName();
		Info.Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Info.PosX = Node->NodePosX;
		Info.PosY = Node->NodePosY;
		const bool bMapGet = IsMapGet(Node);
		TMap<FGuid, FString> DefaultPinOwners;
		if (const TMap<FGuid, FGuid>* DefaultPins = bMapGet ? GetMapGetDefaultPinMap(Node) : nullptr)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (const FGuid* DefaultGuid = Pin && Pin->Direction == EGPD_Output ? DefaultPins->Find(Pin->PersistentGuid) : nullptr)
				{
					DefaultPinOwners.Add(*DefaultGuid, Pin->PinName.ToString());
				}
			}
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->bOrphanedPin)
			{
				continue;
			}
			FNiagaraMcpGraphPin& PinInfo = Info.Pins.AddDefaulted_GetRef();
			PinInfo.Name = Pin->PinName.ToString();
			if (const FString* Owner = DefaultPinOwners.Find(Pin->PersistentGuid))
			{
				// Hidden default-value pin of a Map Get output; set it with SetPinDefaultValue(<output name>).
				PinInfo.Name = *Owner + TEXT(" (default)");
			}
			PinInfo.Direction = Pin->Direction == EGPD_Output ? TEXT("Output") : TEXT("Input");
			if (IsAddPin(Pin))
			{
				PinInfo.Type = TEXT("Add");
			}
			else
			{
				const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
				PinInfo.Type = Type.IsValid() ? Type.GetName() : Pin->PinType.PinCategory.ToString();
			}
			PinInfo.DefaultValue = Pin->DefaultValue;
			if (bMapGet && Pin->Direction == EGPD_Output && !IsAddPin(Pin))
			{
				PinInfo.DefaultValue = GetScriptVariableDefault(Node, Pin);
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNodeUnchecked())
				{
					PinInfo.LinkedTo.Add(NodeIdOf(Linked->GetOwningNode()) + TEXT(":") + Linked->PinName.ToString());
				}
			}
		}
		if (Node->IsA<UNiagaraNodeCustomHlsl>())
		{
			if (FString* Hlsl = FindStringProperty(Node, TEXT("CustomHlsl")))
			{
				Info.CustomHlsl = *Hlsl;
			}
		}
		return Info;
	}

	TArray<FString> AllowedStacksOf(const UNiagaraScript& Script)
	{
		TArray<FString> Stacks;
		if (const FVersionedNiagaraScriptData* ScriptData = Script.GetLatestScriptData())
		{
			for (ENiagaraScriptUsage Usage : UNiagaraScript::GetSupportedUsageContextsForBitmask(ScriptData->ModuleUsageBitmask))
			{
				Stacks.Add(UsageToString(Usage));
			}
		}
		return Stacks;
	}

	FNiagaraMcpGraphInfo DescribeGraph(UNiagaraScript& Script, UNiagaraGraph& Graph)
	{
		FNiagaraMcpGraphInfo Info;
		Info.ScriptPath = Script.GetPathName();
		Info.Usage = UsageToString(Script.GetUsage());
		if (const FVersionedNiagaraScriptData* ScriptData = Script.GetLatestScriptData())
		{
			Info.Description = ScriptData->Description.ToString();
		}
		if (Script.GetUsage() == ENiagaraScriptUsage::Module)
		{
			Info.AllowedStacks = AllowedStacksOf(Script);
		}
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node)
			{
				Info.Nodes.Add(DescribeNode(Node));
			}
		}
		return Info;
	}

	bool StacksToBitmask(const TArray<FString>& Stacks, int32& OutBitmask)
	{
		OutBitmask = 0;
		for (const FString& StackName : Stacks)
		{
			ENiagaraScriptUsage Usage;
			if (!ParseUsage(StackName, Usage))
			{
				return false;
			}
			OutBitmask |= (1 << static_cast<int32>(Usage));
		}
		return true;
	}

	UClass* ResolveNodeClass(const FString& NodeType, FString& OutOpName, UNiagaraScript*& OutFunctionScript)
	{
		const FString Type = NodeType.TrimStartAndEnd();
		FString ClassPath;
		if (Type.Equals(TEXT("MapGet"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("Map Get"), ESearchCase::IgnoreCase))
		{
			ClassPath = MapGetClassPath;
		}
		else if (Type.Equals(TEXT("MapSet"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("Map Set"), ESearchCase::IgnoreCase))
		{
			ClassPath = MapSetClassPath;
		}
		else if (LooseNameEquals(Type, TEXT("CustomHlsl")))
		{
			ClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeCustomHlsl");
		}
		else if (Type.Equals(TEXT("Reroute"), ESearchCase::IgnoreCase))
		{
			ClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeReroute");
		}
		else if (Type.StartsWith(TEXT("Op:"), ESearchCase::IgnoreCase))
		{
			OutOpName = Type.Mid(3).TrimStartAndEnd();
			ClassPath = TEXT("/Script/NiagaraEditor.NiagaraNodeOp");
		}
		else if (Type.StartsWith(TEXT("Function:"), ESearchCase::IgnoreCase))
		{
			OutFunctionScript = LoadAssetByPath<UNiagaraScript>(Type.Mid(9).TrimStartAndEnd(), TEXT("Function script"));
			if (OutFunctionScript == nullptr)
			{
				return nullptr;
			}
			return UNiagaraNodeFunctionCall::StaticClass();
		}
		else
		{
			ClassPath = Type;
		}

		UClass* Class = ClassPath.StartsWith(TEXT("/"))
			? FindObject<UClass>(nullptr, *ClassPath)
			: FindFirstObject<UClass>(*ClassPath, EFindFirstObjectOptions::NativeFirst);
		if (Class == nullptr && !ClassPath.StartsWith(TEXT("/")))
		{
			Class = FindFirstObject<UClass>(*(FString(TEXT("NiagaraNode")) + ClassPath), EFindFirstObjectOptions::NativeFirst);
		}
		if (Class == nullptr || !Class->IsChildOf(UNiagaraNode::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			ReportError(FString::Printf(TEXT("Unknown node type '%s'. Use MapGet, MapSet, CustomHlsl, Reroute, Op:<OpName>, Function:<script path> or a Niagara node class."), *NodeType));
			return nullptr;
		}
		return Class;
	}

	/** Same steps as the "Add" pin of a dynamic-pin node (UNiagaraNodeWithDynamicPins::RequestNewTypedPin). */
	UEdGraphPin* AddTypedPin(UNiagaraNodeWithDynamicPins& Node, EEdGraphPinDirection Direction, const FNiagaraTypeDefinition& Type, FName Name)
	{
		UEdGraphPin* AddPin = nullptr;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && IsAddPin(Pin))
			{
				AddPin = Pin;
				break;
			}
		}
		if (AddPin == nullptr)
		{
			ReportError(FString::Printf(TEXT("Node '%s' has no %s Add pin."), *Node.GetNodeTitle(ENodeTitleType::ListView).ToString(), Direction == EGPD_Input ? TEXT("input") : TEXT("output")));
			return nullptr;
		}

		Node.Modify();
		AddPin->Modify();
		const FEdGraphPinType AddPinType = AddPin->PinType;
		AddPin->PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
		AddPin->PinName = Name;
		AddPin->PinFriendlyName = FText::AsCultureInvariant(Name.ToString());

		// A fresh Add pin replaces the one that just became the parameter pin.
		Node.CreatePin(Direction, AddPinType, TEXT("Add"));
		(Node.*FDynamicPinsAccess::OnNewTypedPinAddedFn())(AddPin);
		Node.MarkNodeRequiresSynchronization(TEXT("NiagaraMcp AddTypedPin"), true);
		return AddPin;
	}

	FString SourceNameOf(const FText& Text)
	{
		const FString* Source = FTextInspector::GetSourceString(Text);
		return Source ? *Source : Text.ToString();
	}

	TArray<TSharedPtr<FNiagaraAction_NewNode>> GetMenuActions(UNiagaraGraph& Graph)
	{
		const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(Graph.GetSchema());
		if (Schema == nullptr)
		{
			return {};
		}
		UEdGraph* OwnerOfTemporaries = NewObject<UEdGraph>(GetTransientPackage());
		return Schema->GetGraphActions(&Graph, nullptr, OwnerOfTemporaries);
	}

	FNiagaraMcpGraphNode AddNodeFromAction(UNiagaraGraph& Graph, const FString& Spec, int32 PosX, int32 PosY)
	{
		FString Name = Spec.TrimStartAndEnd();
		FString Category;
		Name.Split(TEXT("|"), &Name, &Category);
		Name.TrimStartAndEndInline();
		Category.TrimStartAndEndInline();

		TArray<TSharedPtr<FNiagaraAction_NewNode>> Matches;
		for (const TSharedPtr<FNiagaraAction_NewNode>& Action : GetMenuActions(Graph))
		{
			if (!Action.IsValid())
			{
				continue;
			}
			const bool bName = Action->DisplayName.ToString().Equals(Name, ESearchCase::IgnoreCase) || SourceNameOf(Action->DisplayName).Equals(Name, ESearchCase::IgnoreCase);
			if (bName && (Category.IsEmpty() || FString::Join(Action->Categories, TEXT(" > ")).Contains(Category)))
			{
				Matches.Add(Action);
			}
		}
		if (Matches.Num() != 1)
		{
			TArray<FString> Candidates;
			for (const TSharedPtr<FNiagaraAction_NewNode>& Match : Matches)
			{
				Candidates.Add(FString::Printf(TEXT("'%s|%s'"), *Match->DisplayName.ToString(), *FString::Join(Match->Categories, TEXT(" > "))));
			}
			ReportError(Matches.Num() == 0
				? FString::Printf(TEXT("No menu entry named '%s'. Use ListNodeActions to find names."), *Name)
				: FString::Printf(TEXT("'%s' matches %d entries; use Action:<Name>|<Category>. Candidates: %s"), *Name, Matches.Num(), *FString::Join(Candidates, TEXT(", "))));
			return {};
		}
		// Creating a node that is not allowed in the graph opens a modal message box; check first.
		if (const UNiagaraNode* Template = Cast<UNiagaraNode>(Matches[0]->WeakNodeTemplate.Get()))
		{
			FString Reason;
			if (!Template->CanAddToGraph(&Graph, Reason))
			{
				ReportError(FString::Printf(TEXT("'%s' cannot be added to this graph: %s"), *Name, *Reason));
				return {};
			}
		}
		FScopedTransaction Transaction(LOCTEXT("AddGraphNodeAction", "Niagara MCP: Add Graph Node"));
		Graph.Modify();
		UEdGraphNode* NewNode = Matches[0]->CreateNode(&Graph, nullptr, FVector2D(PosX, PosY), false);
		if (NewNode == nullptr)
		{
			ReportError(FString::Printf(TEXT("Menu entry '%s' did not create a node."), *Name));
			Transaction.Cancel();
			return {};
		}
		MarkNodeChanged(NewNode, TEXT("NiagaraMcp AddGraphNode"));
		return DescribeNode(NewNode);
	}
}

using namespace NiagaraMcpGraph;

FNiagaraMcpGraphInfo UNiagaraMcpEditorTools::CreateModuleScript(const FString& AssetPath, const FString& Description, const TArray<FString>& AllowedStacks, bool bSave)
{
	int32 Bitmask = 0;
	if (AllowedStacks.Num() > 0 && !StacksToBitmask(AllowedStacks, Bitmask))
	{
		return {};
	}

	UNiagaraScript* DefaultModule = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
	if (DefaultModule == nullptr || Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource()) == nullptr)
	{
		ReportError(TEXT("The default module script in Niagara editor settings (DefaultModuleScript) could not be loaded."));
		return {};
	}

	FString AssetName;
	UPackage* Package = CreateNewAssetPackage(AssetPath, AssetName);
	if (Package == nullptr)
	{
		return {};
	}

	// No undo transaction: asset creation is not undoable in the editor (factories don't record it). Undoing a
	// recorded NewObject would leave an empty, broken asset in memory that crashes the next tool using it.
	// Same as UNiagaraScriptFactoryNew for module scripts: duplicate the default module template.
	UNiagaraScript* NewScript = Cast<UNiagaraScript>(StaticDuplicateObject(DefaultModule, Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional, UNiagaraScript::StaticClass()));
	if (NewScript == nullptr)
	{
		ReportError(TEXT("Duplicating the default module script failed."));
		return {};
	}
	NewScript->SetFlags(RF_Public | RF_Standalone | RF_Transactional);

	if (FVersionedNiagaraScriptData* ScriptData = NewScript->GetLatestScriptData())
	{
		if (!Description.IsEmpty())
		{
			ScriptData->Description = FText::FromString(Description);
		}
		if (Bitmask != 0)
		{
			ScriptData->ModuleUsageBitmask = Bitmask;
		}
	}

	FAssetRegistryModule::AssetCreated(NewScript);
	Package->MarkPackageDirty();
	if (bSave)
	{
		SaveAssetSilently(NewScript);
	}

	UNiagaraGraph* Graph = GetGraph(NewScript, false);
	return Graph ? DescribeGraph(*NewScript, *Graph) : FNiagaraMcpGraphInfo();
}

FNiagaraMcpGraphInfo UNiagaraMcpEditorTools::GetScriptGraph(UNiagaraScript* Script, const FString& NodeFilter, int32 Offset, int32 MaxNodes)
{
	UNiagaraGraph* Graph = GetGraph(Script, false);
	if (Graph == nullptr)
	{
		return FNiagaraMcpGraphInfo();
	}
	FNiagaraMcpGraphInfo Info = DescribeGraph(*Script, *Graph);
	if (!IsUnset(NodeFilter))
	{
		TArray<FString> Words;
		NodeFilter.ToLower().ParseIntoArrayWS(Words);
		Info.Nodes.RemoveAll([&Words](const FNiagaraMcpGraphNode& Node)
		{
			const FString Haystack = (Node.Class + TEXT(" ") + Node.Title).ToLower();
			return Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word); });
		});
	}
	Info.TotalNodes = Info.Nodes.Num();
	const int32 First = FMath::Clamp(Offset, 0, Info.Nodes.Num());
	const int32 Count = FMath::Clamp(MaxNodes, 1, 500);
	Info.Nodes = TArray<FNiagaraMcpGraphNode>(Info.Nodes.GetData() + First, FMath::Min(Count, Info.Nodes.Num() - First));
	Info.NextOffset = First + Count < Info.TotalNodes ? First + Count : -1;
	return Info;
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::AddGraphNode(UNiagaraScript* Script, const FString& NodeType, int32 PosX, int32 PosY)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	if (Graph == nullptr)
	{
		return {};
	}

	if (NodeType.TrimStart().StartsWith(TEXT("Action:"), ESearchCase::IgnoreCase))
	{
		return AddNodeFromAction(*Graph, NodeType.TrimStart().Mid(7), PosX, PosY);
	}

	FString OpName;
	UNiagaraScript* FunctionScript = nullptr;
	UClass* NodeClass = ResolveNodeClass(NodeType, OpName, FunctionScript);
	if (NodeClass == nullptr)
	{
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("AddGraphNode", "Niagara MCP: Add Graph Node"));
	Graph->Modify();

	// FGraphNodeCreator steps, with the node configured before its pins are allocated.
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
	FString PropertyError;
	if (!OpName.IsEmpty())
	{
		if (SetPropertyFromText(NodeClass, Node, Node, TEXT("OpName"), OpName, PropertyError) == nullptr)
		{
			ReportError(PropertyError);
			Transaction.Cancel();
			return {};
		}
	}
	if (FunctionScript)
	{
		UNiagaraNodeFunctionCall* FunctionCall = CastChecked<UNiagaraNodeFunctionCall>(Node);
		FunctionCall->FunctionScript = FunctionScript;
		FunctionCall->SelectedScriptVersion = FunctionScript->GetExposedVersion().VersionGuid;
	}
	if (Node->IsA<UNiagaraNodeCustomHlsl>())
	{
		if (FString* Hlsl = FindStringProperty(Node, TEXT("CustomHlsl")))
		{
			*Hlsl = TEXT("// Write the function body here; assign every output.");
		}
		SetPropertyFromText(NodeClass, Node, Node, TEXT("ScriptUsage"), UsageToString(Script->GetUsage()), PropertyError);
	}

	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	if (Node->Pins.Num() == 0)
	{
		Node->AllocateDefaultPins();
	}
	Node->NodePosX = PosX;
	Node->NodePosY = PosY;

	if (!OpName.IsEmpty() && Node->Pins.Num() == 0)
	{
		Graph->RemoveNode(Node);
		ReportError(FString::Printf(TEXT("'%s' is not a known Niagara op. Examples: Numeric::Add, Numeric::Mul, Numeric::Sine, Vector::Length."), *OpName));
		Transaction.Cancel();
		return {};
	}

	MarkNodeChanged(Node, TEXT("NiagaraMcp AddGraphNode"));
	return DescribeNode(Node);
}

FNiagaraMcpGraphInfo UNiagaraMcpEditorTools::RemoveGraphNode(UNiagaraScript* Script, const FString& NodeId)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* Node = Graph ? FindNode(*Graph, NodeId) : nullptr;
	if (Node == nullptr)
	{
		return {};
	}
	if (!Node->CanUserDeleteNode())
	{
		ReportError(FString::Printf(TEXT("Node '%s' cannot be deleted."), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("RemoveGraphNode", "Niagara MCP: Remove Graph Node"));
		Graph->Modify();
		Node->Modify();
		Node->BreakAllNodeLinks();
		Node->DestroyNode();
	}
	Graph->NotifyGraphChanged();
	return DescribeGraph(*Script, *Graph);
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::AddParameterPin(UNiagaraScript* Script, const FString& NodeId, FName ParameterName, const FString& Type)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* Node = Graph ? FindNode(*Graph, NodeId) : nullptr;
	if (Node == nullptr)
	{
		return {};
	}

	// The map node headers are private to NiagaraEditor, so classes are resolved by path.
	UClass* MapBaseClass = FindObject<UClass>(nullptr, MapBaseClassPath);
	UClass* MapSetClass = FindObject<UClass>(nullptr, MapSetClassPath);
	if (MapBaseClass == nullptr || !Node->GetClass()->IsChildOf(MapBaseClass))
	{
		ReportError(FString::Printf(TEXT("Node '%s' is a %s, not a Map Get or Map Set node."), *NodeId, *Node->GetClass()->GetName()));
		return {};
	}
	if (ParameterName.IsNone())
	{
		ReportError(TEXT("ParameterName is required, e.g. Module.Length or Particles.RibbonWidth."));
		return {};
	}
	FNiagaraTypeDefinition TypeDef;
	if (!ResolveNiagaraType(Type, TypeDef))
	{
		return {};
	}

	const bool bInputPin = MapSetClass && Node->GetClass()->IsChildOf(MapSetClass);
	const EEdGraphPinDirection Direction = bInputPin ? EGPD_Input : EGPD_Output;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinName == ParameterName)
		{
			ReportError(FString::Printf(TEXT("Node already has a pin named '%s'."), *ParameterName.ToString()));
			return {};
		}
	}

	FScopedTransaction Transaction(LOCTEXT("AddParameterPin", "Niagara MCP: Add Parameter Pin"));
	Graph->Modify();
	// Map Get / Map Set derive from UNiagaraNodeWithDynamicPins (single inheritance down to UObject).
	UEdGraphPin* NewPin = AddTypedPin(*static_cast<UNiagaraNodeWithDynamicPins*>(Cast<UNiagaraNode>(Node)), Direction, TypeDef, ParameterName);
	if (NewPin == nullptr)
	{
		Transaction.Cancel();
		return {};
	}
	MarkNodeChanged(Node, TEXT("NiagaraMcp AddParameterPin"));
	return DescribeNode(Node);
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::SetCustomHlsl(UNiagaraScript* Script, const FString& NodeId, const FString& Hlsl, const TArray<FNiagaraMcpHlslPin>& Inputs, const TArray<FNiagaraMcpHlslPin>& Outputs)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* Node = Graph ? FindNode(*Graph, NodeId) : nullptr;
	if (Node == nullptr)
	{
		return {};
	}
	UNiagaraNodeCustomHlsl* HlslNode = Cast<UNiagaraNodeCustomHlsl>(Node);
	FString* HlslText = HlslNode ? FindStringProperty(HlslNode, TEXT("CustomHlsl")) : nullptr;
	if (HlslText == nullptr)
	{
		ReportError(FString::Printf(TEXT("Node '%s' is a %s, not a Custom Hlsl node."), *NodeId, *Node->GetClass()->GetName()));
		return {};
	}

	// Resolve every declaration before touching the node, so a bad type leaves it unchanged.
	TArray<FNiagaraVariable> NewInputs;
	TArray<FNiagaraVariableBase> NewOutputs;
	TSet<FName> SeenInputs;
	for (const FNiagaraMcpHlslPin& Pin : Inputs)
	{
		FNiagaraTypeDefinition TypeDef;
		if (Pin.Name.IsNone() || SeenInputs.Contains(Pin.Name))
		{
			ReportError(FString::Printf(TEXT("Input names must be set and unique (got '%s')."), *Pin.Name.ToString()));
			return {};
		}
		if (!ResolveNiagaraType(Pin.Type, TypeDef))
		{
			return {};
		}
		SeenInputs.Add(Pin.Name);
		NewInputs.Add(FNiagaraVariable(TypeDef, Pin.Name));
	}
	TSet<FName> SeenOutputs;
	for (const FNiagaraMcpHlslPin& Pin : Outputs)
	{
		FNiagaraTypeDefinition TypeDef;
		if (Pin.Name.IsNone() || SeenOutputs.Contains(Pin.Name))
		{
			ReportError(FString::Printf(TEXT("Output names must be set and unique (got '%s')."), *Pin.Name.ToString()));
			return {};
		}
		if (!ResolveNiagaraType(Pin.Type, TypeDef))
		{
			return {};
		}
		SeenOutputs.Add(Pin.Name);
		NewOutputs.Add(FNiagaraVariableBase(TypeDef, Pin.Name));
	}

	// Keep parameter map pins unless redeclared; dropping them would cut the node out of the map flow.
	const FNiagaraTypeDefinition& MapDef = FNiagaraTypeDefinition::GetParameterMapDef();
	if (!NewInputs.ContainsByPredicate([&MapDef](const FNiagaraVariable& Var) { return Var.GetType() == MapDef; }))
	{
		int32 InsertAt = 0;
		for (const FNiagaraVariable& Existing : HlslNode->Signature.Inputs)
		{
			if (Existing.GetType() == MapDef && !SeenInputs.Contains(Existing.GetName()))
			{
				NewInputs.Insert(Existing, InsertAt++);
			}
		}
	}
	if (!NewOutputs.ContainsByPredicate([&MapDef](const FNiagaraVariableBase& Var) { return Var.GetType() == MapDef; }))
	{
		int32 InsertAt = 0;
		for (const FNiagaraVariableBase& Existing : HlslNode->Signature.Outputs)
		{
			if (Existing.GetType() == MapDef && !SeenOutputs.Contains(Existing.GetName()))
			{
				NewOutputs.Insert(Existing, InsertAt++);
			}
		}
	}

	FScopedTransaction Transaction(LOCTEXT("SetCustomHlsl", "Niagara MCP: Set Custom Hlsl"));
	Graph->Modify();
	HlslNode->Modify();
	// CustomHlsl is a private UPROPERTY and its setter is not exported; reflection writes it.
	*HlslText = Hlsl;
	HlslNode->Signature.Inputs = MoveTemp(NewInputs);
	HlslNode->Signature.Outputs = MoveTemp(NewOutputs);
	// Custom Hlsl pins are generated from Signature; ReallocatePins keeps links of pins whose name did not change.
	(HlslNode->*FNiagaraNodeAccess::ReallocatePinsFn())(true);
	HlslNode->RefreshFromExternalChanges();

	MarkNodeChanged(HlslNode, TEXT("NiagaraMcp SetCustomHlsl"));
	return DescribeNode(HlslNode);
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::ConnectPins(UNiagaraScript* Script, const FString& FromNodeId, FName FromPin, const FString& ToNodeId, FName ToPin)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* FromNode = Graph ? FindNode(*Graph, FromNodeId) : nullptr;
	UEdGraphNode* ToNode = FromNode ? FindNode(*Graph, ToNodeId) : nullptr;
	UEdGraphPin* From = ToNode ? FindPin(*FromNode, FromPin, EGPD_Output) : nullptr;
	UEdGraphPin* To = From ? FindPin(*ToNode, ToPin, EGPD_Input) : nullptr;
	if (To == nullptr)
	{
		return {};
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		ReportError(FString::Printf(TEXT("Cannot connect: %s"), *Response.Message.ToString()));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("ConnectPins", "Niagara MCP: Connect Pins"));
	Graph->Modify();
	if (!Schema->TryCreateConnection(From, To))
	{
		ReportError(TEXT("The graph schema refused the connection."));
		Transaction.Cancel();
		return {};
	}
	MarkNodeChanged(ToNode, TEXT("NiagaraMcp ConnectPins"));
	return DescribeNode(ToNode);
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::DisconnectPins(UNiagaraScript* Script, const FString& FromNodeId, FName FromPin, const FString& ToNodeId, FName ToPin)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* FromNode = Graph ? FindNode(*Graph, FromNodeId) : nullptr;
	UEdGraphNode* ToNode = FromNode ? FindNode(*Graph, ToNodeId) : nullptr;
	UEdGraphPin* From = ToNode ? FindPin(*FromNode, FromPin, EGPD_Output) : nullptr;
	UEdGraphPin* To = From ? FindPin(*ToNode, ToPin, EGPD_Input) : nullptr;
	if (To == nullptr)
	{
		return {};
	}
	if (!From->LinkedTo.Contains(To))
	{
		ReportError(TEXT("Those pins are not connected."));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("DisconnectPins", "Niagara MCP: Disconnect Pins"));
	Graph->Modify();
	Graph->GetSchema()->BreakSinglePinLink(From, To);
	MarkNodeChanged(ToNode, TEXT("NiagaraMcp DisconnectPins"));
	return DescribeNode(ToNode);
}

FNiagaraMcpGraphNode UNiagaraMcpEditorTools::SetPinDefaultValue(UNiagaraScript* Script, const FString& NodeId, FName PinName, const FString& Value)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	UEdGraphNode* Node = Graph ? FindNode(*Graph, NodeId) : nullptr;
	if (Node == nullptr)
	{
		return {};
	}

	// Map Get outputs (Module.X): the default lives on the graph's script variable, which drives the hidden,
	// read-only default pin and the module input's default in every stack.
	if (IsMapGet(Node))
	{
		UEdGraphPin* const* FoundOutput = Node->Pins.FindByPredicate([PinName](const UEdGraphPin* Candidate)
		{
			return Candidate && Candidate->Direction == EGPD_Output && Candidate->PinName.ToString().Equals(PinName.ToString(), ESearchCase::IgnoreCase);
		});
		const UEdGraphPin* OutputPin = FoundOutput ? *FoundOutput : nullptr;
		if (OutputPin)
		{
			UNiagaraScriptVariable* ScriptVariable = Graph->GetScriptVariable(OutputPin->PinName);
			if (ScriptVariable == nullptr)
			{
				ReportError(FString::Printf(TEXT("'%s' has no parameter entry in the script graph."), *OutputPin->PinName.ToString()));
				return {};
			}
			const FNiagaraTypeDefinition Type = ScriptVariable->Variable.GetType();
			const UScriptStruct* Struct = Type.GetScriptStruct();
			if (Struct == nullptr || Type.IsDataInterface() || Type.IsUObject())
			{
				ReportError(FString::Printf(TEXT("'%s' (%s) has no value default."), *OutputPin->PinName.ToString(), *Type.GetName()));
				return {};
			}
			FStructOnScope NewValue(Struct);
			if (const uint8* Current = ScriptVariable->GetDefaultValueData())
			{
				FMemory::Memcpy(NewValue.GetStructMemory(), Current, FMath::Min<int32>(Struct->GetStructureSize(), Type.GetSize()));
			}
			FString ValueError;
			if (!ImportStructValue(Struct, NewValue.GetStructMemory(), Value, Type.GetEnum(), ValueError))
			{
				ReportError(FString::Printf(TEXT("'%s': %s"), *OutputPin->PinName.ToString(), *ValueError));
				return {};
			}

			FScopedTransaction Transaction(LOCTEXT("SetModuleInputDefault", "Niagara MCP: Set Module Input Default"));
			Graph->Modify();
			ScriptVariable->Modify();
			ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
			ScriptVariable->SetDefaultValueData(NewValue.GetStructMemory());
			// Syncs the default pins and flags the graph for recompile.
			FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
			ScriptVariable->PostEditChangeProperty(ChangedEvent);
			MarkNodeChanged(Node, TEXT("NiagaraMcp SetModuleInputDefault"));
			return DescribeNode(Node);
		}
	}

	UEdGraphPin* Pin = FindPin(*Node, PinName, EGPD_Input);
	if (Pin == nullptr)
	{
		return {};
	}
	if (Pin->LinkedTo.Num() > 0)
	{
		ReportError(FString::Printf(TEXT("Pin '%s' is connected; its default value is ignored. Disconnect it first."), *PinName.ToString()));
		return {};
	}

	FScopedTransaction Transaction(LOCTEXT("SetPinDefaultValue", "Niagara MCP: Set Pin Default"));
	Graph->Modify();
	Node->Modify();
	Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
	MarkNodeChanged(Node, TEXT("NiagaraMcp SetPinDefaultValue"));
	return DescribeNode(Node);
}

FNiagaraMcpGraphInfo UNiagaraMcpEditorTools::SetModuleScriptInfo(UNiagaraScript* Script, const FString& Description, const TArray<FString>& AllowedStacks, bool bSave)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	if (Graph == nullptr)
	{
		return {};
	}
	if (Script->GetUsage() != ENiagaraScriptUsage::Module)
	{
		ReportError(TEXT("Only module scripts have allowed stacks."));
		return {};
	}
	int32 Bitmask = 0;
	if (AllowedStacks.Num() > 0 && !StacksToBitmask(AllowedStacks, Bitmask))
	{
		return {};
	}
	FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData();
	if (ScriptData == nullptr)
	{
		ReportError(TEXT("Script has no version data."));
		return {};
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SetModuleScriptInfo", "Niagara MCP: Set Module Script Info"));
		Script->Modify();
		if (!Description.IsEmpty())
		{
			ScriptData->Description = FText::FromString(Description);
		}
		if (Bitmask != 0)
		{
			ScriptData->ModuleUsageBitmask = Bitmask;
		}
		Script->MarkPackageDirty();
	}
	if (bSave)
	{
		SaveAssetSilently(Script);
	}
	return DescribeGraph(*Script, *Graph);
}

FNiagaraMcpApplyInfo UNiagaraMcpEditorTools::ApplyScriptChanges(UNiagaraScript* Script, bool bSave)
{
	FNiagaraMcpApplyInfo Info;
	UNiagaraGraph* Graph = GetGraph(Script, true);
	if (Graph == nullptr)
	{
		return Info;
	}
	Info.ScriptPath = Script->GetPathName();
	Graph->NotifyGraphChanged();

	// Same as the script editor's Apply (FNiagaraScriptToolkit::OnApply -> RefreshAllScriptsFromExternalChanges):
	// every function call node that references the script refreshes its pins from the edited graph and is flagged
	// for recompilation. Like the engine, nodes in transient packages (open script editors, scratch-pad edit
	// copies) are included, otherwise their next Apply would write the old pins back. One transaction, so an
	// Undo of the graph edits can also undo the caller refresh.
	TSet<UNiagaraSystem*> AffectedSystems;
	TSet<UObject*> OtherCallers; // emitter assets and module/function scripts that call this script
	{
		FScopedTransaction Transaction(LOCTEXT("ApplyScriptChanges", "Niagara MCP: Apply Script Changes"));
		for (TObjectIterator<UNiagaraNodeFunctionCall> It; It; ++It)
		{
			UNiagaraNodeFunctionCall* FunctionCall = *It;
			if (!IsValid(FunctionCall) || FunctionCall->FunctionScript != Script || FunctionCall->HasAnyFlags(RF_ClassDefaultObject))
			{
				continue;
			}
			FunctionCall->Modify();
			FunctionCall->RefreshFromExternalChanges();
			FunctionCall->MarkNodeRequiresSynchronization(TEXT("NiagaraMcp ApplyScriptChanges"), true);
			++Info.RefreshedModuleNodes;
			if (FunctionCall->GetOutermost() == GetTransientPackage())
			{
				continue; // editor copies: refreshed, but nothing to compile or save
			}
			if (UNiagaraSystem* System = FunctionCall->GetTypedOuter<UNiagaraSystem>())
			{
				AffectedSystems.Add(System);
			}
			else if (UNiagaraEmitter* Emitter = FunctionCall->GetTypedOuter<UNiagaraEmitter>())
			{
				OtherCallers.Add(Emitter);
			}
			else if (UNiagaraScript* CallerScript = FunctionCall->GetTypedOuter<UNiagaraScript>())
			{
				OtherCallers.Add(CallerScript);
			}
		}
	}
	// Bumps the traversal cache and resets open system previews, as the script editor's Apply does.
	FNiagaraEditorModule::Get().ScriptApplied(Script);

	bool bAllSaved = true;
	if (bSave)
	{
		bAllSaved &= SaveAssetSilently(Script);
	}
	for (UNiagaraSystem* System : AffectedSystems)
	{
		System->RequestCompile(false);
		Info.RecompiledSystems.Add(System->GetPathName());
		if (bSave)
		{
			bAllSaved &= SaveAssetSilently(System);
		}
	}
	for (UObject* Caller : OtherCallers)
	{
		Info.RecompiledSystems.Add(Caller->GetPathName());
		if (bSave)
		{
			bAllSaved &= SaveAssetSilently(Caller);
		}
	}
	Info.bSaved = bSave && bAllSaved;
	return Info;
}

TArray<FNiagaraMcpNodeAction> UNiagaraMcpEditorTools::ListNodeActions(UNiagaraScript* Script, const FString& Filter, int32 MaxResults)
{
	TArray<FNiagaraMcpNodeAction> Result;
	UNiagaraGraph* Graph = GetGraph(Script, false);
	if (Graph == nullptr)
	{
		return Result;
	}
	TArray<FString> Words;
	if (!IsUnset(Filter))
	{
		Filter.ToLower().ParseIntoArrayWS(Words);
	}
	for (const TSharedPtr<FNiagaraAction_NewNode>& Action : GetMenuActions(*Graph))
	{
		if (!Action.IsValid())
		{
			continue;
		}
		const FString Category = FString::Join(Action->Categories, TEXT(" > "));
		const FString Haystack = (Action->DisplayName.ToString() + TEXT(" ") + Category).ToLower();
		if (Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word); }))
		{
			continue;
		}
		FNiagaraMcpNodeAction& Entry = Result.AddDefaulted_GetRef();
		Entry.Name = Action->DisplayName.ToString();
		Entry.Category = Category;
		Entry.Tooltip = Action->ToolTip.ToString().Left(200);
		if (Result.Num() >= FMath::Clamp(MaxResults, 1, 500))
		{
			break;
		}
	}
	return Result;
}

FNiagaraMcpGraphInfo UNiagaraMcpEditorTools::LayoutScriptGraph(UNiagaraScript* Script, bool bSave)
{
	UNiagaraGraph* Graph = GetGraph(Script, true);
	if (Graph == nullptr)
	{
		return {};
	}
	// Column = longest chain of links feeding the node.
	TMap<UEdGraphNode*, int32> Column;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Column.Add(Node, 0);
		}
	}
	for (int32 Pass = 0; Pass < Column.Num(); ++Pass)
	{
		bool bChanged = false;
		for (TPair<UEdGraphNode*, int32>& Entry : Column)
		{
			for (const UEdGraphPin* Pin : Entry.Key->Pins)
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Input)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const int32* Source = Linked ? Column.Find(Linked->GetOwningNode()) : nullptr;
					if (Source && *Source + 1 > Entry.Value)
					{
						Entry.Value = *Source + 1;
						bChanged = true;
					}
				}
			}
		}
		if (!bChanged)
		{
			break;
		}
	}

	TMap<int32, TArray<UEdGraphNode*>> ByColumn;
	for (const TPair<UEdGraphNode*, int32>& Entry : Column)
	{
		ByColumn.FindOrAdd(Entry.Value).Add(Entry.Key);
	}
	{
		FScopedTransaction Transaction(LOCTEXT("LayoutScriptGraph", "Niagara MCP: Layout Script Graph"));
		Graph->Modify();
		for (TPair<int32, TArray<UEdGraphNode*>>& Entry : ByColumn)
		{
			Entry.Value.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosY < B.NodePosY; });
			int32 Y = 0;
			for (UEdGraphNode* Node : Entry.Value)
			{
				Node->Modify();
				Node->NodePosX = Entry.Key * 420;
				Node->NodePosY = Y;
				Y += 120 + 26 * Node->Pins.Num();
			}
		}
	}
	Graph->NotifyGraphChanged();
	if (bSave)
	{
		SaveAssetSilently(Script->GetOutermostObject());
	}
	return DescribeGraph(*Script, *Graph);
}

#undef LOCTEXT_NAMESPACE

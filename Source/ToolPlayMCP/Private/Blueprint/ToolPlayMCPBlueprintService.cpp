#include "Blueprint/ToolPlayMCPBlueprintService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/NoExportTypes.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	struct FToolPlayMCPBlueprintSession
	{
		TWeakObjectPtr<UBlueprint> Blueprint;
		TMap<FString, TWeakObjectPtr<UEdGraph>> GraphAliases;
		TMap<FString, TWeakObjectPtr<UEdGraphNode>> NodeAliases;
	};

	TMap<FString, FToolPlayMCPBlueprintSession> GBlueprintSessions;

	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	FString ToCondensedJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	FString CleanNodeKind(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("null");
		}

		FString Name = Node->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("K2Node_"));
		Name.RemoveFromStart(TEXT("UK2Node_"));
		return Name;
	}

	FString PinDirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("in") : TEXT("out");
	}

	FString PinTypeToString(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("unknown");
		}

		FString Type = Pin->PinType.PinCategory.ToString();
		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			Type += TEXT(":") + Pin->PinType.PinSubCategory.ToString();
		}
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Type += TEXT(":") + Pin->PinType.PinSubCategoryObject->GetName();
		}
		if (Pin->PinType.IsArray())
		{
			Type += TEXT("[]");
		}
		return Type.IsEmpty() ? TEXT("unknown") : Type;
	}

	FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		FString Type = PinType.PinCategory.ToString();
		if (!PinType.PinSubCategory.IsNone())
		{
			Type += TEXT(":") + PinType.PinSubCategory.ToString();
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Type += TEXT(":") + PinType.PinSubCategoryObject->GetName();
		}
		if (PinType.IsArray())
		{
			Type += TEXT("[]");
		}
		return Type.IsEmpty() ? TEXT("unknown") : Type;
	}

	bool ResolveBlueprintPinType(const FString& RawTypeName, FEdGraphPinType& OutPinType, FString& OutError)
	{
		FString TypeName = RawTypeName;
		TypeName.TrimStartAndEndInline();
		const bool bArray = TypeName.RemoveFromEnd(TEXT("[]"));
		const FString Normalized = TypeName.ToLower();

		OutPinType = FEdGraphPinType();
		OutPinType.ContainerType = bArray ? EPinContainerType::Array : EPinContainerType::None;

		if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (Normalized == TEXT("byte"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (Normalized == TEXT("int") || Normalized == TEXT("integer"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (Normalized == TEXT("int64"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (Normalized == TEXT("float"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		}
		else if (Normalized == TEXT("double"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Normalized == TEXT("name"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (Normalized == TEXT("string"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (Normalized == TEXT("text"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (Normalized == TEXT("vector"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (Normalized == TEXT("rotator"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (Normalized == TEXT("transform"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else if (Normalized == TEXT("linear_color") || Normalized == TEXT("linearcolor") || Normalized == TEXT("color"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		}
		else if (Normalized.StartsWith(TEXT("object:")))
		{
			const FString ClassPath = TypeName.RightChop(7);
			UClass* ObjectClass = FSoftClassPath(ClassPath).TryLoadClass<UObject>();
			if (!ObjectClass)
			{
				ObjectClass = FindObject<UClass>(nullptr, *ClassPath);
			}
			if (!ObjectClass)
			{
				OutError = FString::Printf(TEXT("Unable to resolve object class '%s'."), *ClassPath);
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = ObjectClass;
		}
		else if (Normalized.StartsWith(TEXT("class:")))
		{
			const FString ClassPath = TypeName.RightChop(6);
			UClass* MetaClass = FSoftClassPath(ClassPath).TryLoadClass<UObject>();
			if (!MetaClass)
			{
				MetaClass = FindObject<UClass>(nullptr, *ClassPath);
			}
			if (!MetaClass)
			{
				OutError = FString::Printf(TEXT("Unable to resolve class '%s'."), *ClassPath);
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			OutPinType.PinSubCategoryObject = MetaClass;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported Blueprint variable type '%s'."), *RawTypeName);
			return false;
		}

		return true;
	}

	void AddBlueprintVariablesJson(const UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Root)
	{
		TArray<TSharedPtr<FJsonValue>> Variables;
		if (Blueprint)
		{
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				TSharedRef<FJsonObject> VariableObject = MakeShared<FJsonObject>();
				VariableObject->SetStringField(TEXT("name"), Variable.VarName.ToString());
				VariableObject->SetStringField(TEXT("type"), PinTypeToString(Variable.VarType));
				VariableObject->SetStringField(TEXT("category"), Variable.Category.ToString());
				if (!Variable.DefaultValue.IsEmpty())
				{
					VariableObject->SetStringField(TEXT("default"), Variable.DefaultValue);
				}
				Variables.Add(MakeShared<FJsonValueObject>(VariableObject));
			}
		}
		Root->SetArrayField(TEXT("variables"), Variables);
	}

	void AddNodeMetadata(const UEdGraphNode* Node, const TSharedRef<FJsonObject>& NodeObject)
	{
		if (const UK2Node_CallFunction* FunctionNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (UFunction* Function = FunctionNode->GetTargetFunction())
			{
				NodeObject->SetStringField(TEXT("function"), Function->GetPathName());
			}
		}
		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			NodeObject->SetStringField(TEXT("event"), EventNode->CustomFunctionName.ToString());
			if (EventNode->EventReference.GetMemberName() != NAME_None)
			{
				NodeObject->SetStringField(TEXT("event_ref"), EventNode->EventReference.GetMemberName().ToString());
			}
		}
		if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
			{
				NodeObject->SetStringField(TEXT("macro"), MacroGraph->GetPathName());
			}
		}
		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			NodeObject->SetStringField(TEXT("variable"), VariableNode->GetVarNameString());
		}
	}

	TSharedRef<FJsonObject> BuildEditResult(const FString& Operation, UBlueprint* Blueprint)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), Blueprint != nullptr);
		Root->SetStringField(TEXT("operation"), Operation);
		Root->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
		Root->SetBoolField(TEXT("reexport_recommended"), true);
		return Root;
	}

	FString CreateSession(UBlueprint* Blueprint)
	{
		const FString SessionId = FString::Printf(
			TEXT("blueprint_%s_%s"),
			Blueprint ? *Blueprint->GetName() : TEXT("invalid"),
			*FGuid::NewGuid().ToString(EGuidFormats::Short));
		GBlueprintSessions.Add(SessionId).Blueprint = Blueprint;
		return SessionId;
	}

	bool ResolveGraph(const FString& SessionId, const FString& GraphAlias, UEdGraph*& OutGraph, UBlueprint*& OutBlueprint, FString& OutError)
	{
		FToolPlayMCPBlueprintSession* Session = GBlueprintSessions.Find(SessionId);
		if (!Session || !Session->Blueprint.IsValid())
		{
			OutError = FString::Printf(TEXT("Unknown or expired Blueprint session: %s"), *SessionId);
			return false;
		}

		TWeakObjectPtr<UEdGraph>* GraphPtr = Session->GraphAliases.Find(GraphAlias);
		OutGraph = GraphPtr ? GraphPtr->Get() : nullptr;
		OutBlueprint = Session->Blueprint.Get();
		if (!OutGraph)
		{
			OutError = FString::Printf(TEXT("Unknown or expired graph alias: %s"), *GraphAlias);
			return false;
		}
		return true;
	}

	bool ResolveNode(const FString& SessionId, const FString& NodeAlias, UEdGraphNode*& OutNode, UBlueprint*& OutBlueprint, FString& OutError)
	{
		FToolPlayMCPBlueprintSession* Session = GBlueprintSessions.Find(SessionId);
		if (!Session || !Session->Blueprint.IsValid())
		{
			OutError = FString::Printf(TEXT("Unknown or expired Blueprint session: %s"), *SessionId);
			return false;
		}

		TWeakObjectPtr<UEdGraphNode>* NodePtr = Session->NodeAliases.Find(NodeAlias);
		OutNode = NodePtr ? NodePtr->Get() : nullptr;
		OutBlueprint = Session->Blueprint.Get();
		if (!OutNode)
		{
			OutError = FString::Printf(TEXT("Unknown or expired node alias: %s"), *NodeAlias);
			return false;
		}
		return true;
	}

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UFunction* ResolveFunction(const FString& FunctionPathOrName)
	{
		if (UFunction* Function = FindObject<UFunction>(nullptr, *FunctionPathOrName))
		{
			return Function;
		}

		FString ClassPath;
		FString FunctionName;
		if (!FunctionPathOrName.Split(TEXT(":"), &ClassPath, &FunctionName))
		{
			FunctionPathOrName.Split(TEXT("."), &ClassPath, &FunctionName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}

		UClass* Class = Cast<UClass>(FSoftObjectPath(ClassPath).TryLoad());
		if (!Class)
		{
			Class = FindObject<UClass>(nullptr, *ClassPath);
		}
		return Class && !FunctionName.IsEmpty() ? Class->FindFunctionByName(FName(*FunctionName)) : nullptr;
	}

	void FinishPlacedNode(UEdGraph* Graph, UEdGraphNode* Node, int32 PositionX, int32 PositionY)
	{
		Node->SetFlags(RF_Transactional);
		Node->CreateNewGuid();
		Node->NodePosX = PositionX;
		Node->NodePosY = PositionY;
		Graph->AddNode(Node, true, false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Graph->NotifyGraphChanged();
	}

	void MarkBlueprintEdited(UBlueprint* Blueprint)
	{
		if (Blueprint)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Blueprint->MarkPackageDirty();
		}
	}

	UStruct* GetBlueprintVariableSource(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}
		if (Blueprint->SkeletonGeneratedClass)
		{
			return Blueprint->SkeletonGeneratedClass;
		}
		return Blueprint->GeneratedClass;
	}

	TSharedRef<FJsonObject> ExportGraph(UEdGraph* Graph, const FString& GraphAlias, const FString& GraphType, FToolPlayMCPBlueprintSession& Session)
	{
		TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("id"), GraphAlias);
		GraphObject->SetStringField(TEXT("name"), Graph ? Graph->GetName() : FString());
		GraphObject->SetStringField(TEXT("type"), GraphType);
		GraphObject->SetStringField(TEXT("class"), Graph ? Graph->GetClass()->GetName() : FString());

		TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
		TMap<const UEdGraphNode*, FString> NodeAliases;
		int32 NodeIndex = 0;

		if (Graph)
		{
			Session.GraphAliases.Add(GraphAlias, Graph);
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const FString NodeAlias = FString::Printf(TEXT("%s.n%d"), *GraphAlias, NodeIndex++);
				NodeAliases.Add(Node, NodeAlias);
				Session.NodeAliases.Add(NodeAlias, Node);

				TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
				NodeObject->SetStringField(TEXT("k"), CleanNodeKind(Node));
				NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NodeObject->SetStringField(TEXT("class"), Node->GetClass()->GetName());
				NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
				NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
				AddNodeMetadata(Node, NodeObject);

				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}

					TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
					PinObject->SetStringField(TEXT("n"), Pin->PinName.ToString());
					PinObject->SetStringField(TEXT("d"), PinDirectionToString(Pin->Direction));
					PinObject->SetStringField(TEXT("t"), PinTypeToString(Pin));
					if (!Pin->DefaultValue.IsEmpty())
					{
						PinObject->SetStringField(TEXT("v"), Pin->DefaultValue);
					}
					if (Pin->LinkedTo.Num() > 0)
					{
						PinObject->SetNumberField(TEXT("links"), Pin->LinkedTo.Num());
					}
					Pins.Add(MakeShared<FJsonValueObject>(PinObject));
				}
				NodeObject->SetArrayField(TEXT("pins"), Pins);
				NodesObject->SetObjectField(NodeAlias, NodeObject);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Edges;
		for (const TPair<const UEdGraphNode*, FString>& Pair : NodeAliases)
		{
			for (const UEdGraphPin* Pin : Pair.Key->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode())
					{
						continue;
					}

					const FString* ToAlias = NodeAliases.Find(LinkedPin->GetOwningNode());
					if (!ToAlias)
					{
						continue;
					}

					TArray<TSharedPtr<FJsonValue>> Edge;
					Edge.Add(MakeShared<FJsonValueString>(Pair.Value));
					Edge.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
					Edge.Add(MakeShared<FJsonValueString>(*ToAlias));
					Edge.Add(MakeShared<FJsonValueString>(LinkedPin->PinName.ToString()));
					Edges.Add(MakeShared<FJsonValueArray>(Edge));
				}
			}
		}

		GraphObject->SetObjectField(TEXT("nodes"), NodesObject);
		GraphObject->SetArrayField(TEXT("edges"), Edges);
		GraphObject->SetNumberField(TEXT("node_count"), NodeAliases.Num());
		GraphObject->SetNumberField(TEXT("edge_count"), Edges.Num());
		return GraphObject;
	}

	FString BuildOutputPath(const UBlueprint* Blueprint)
	{
		const FString FileName = FString::Printf(TEXT("%s_blueprint_compact.json"), Blueprint ? *Blueprint->GetName() : TEXT("Blueprint"));
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ToolPlayMCP"), TEXT("BlueprintExports"), FileName);
	}
}

bool FToolPlayMCPBlueprintService::ExportBlueprintByPath(const FString& AssetPath, FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
		return false;
	}

	OutSessionId = CreateSession(Blueprint);
	FToolPlayMCPBlueprintSession& Session = GBlueprintSessions.FindChecked(OutSessionId);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("session_id"), OutSessionId);
	Root->SetStringField(TEXT("asset"), Blueprint->GetPathName());
	Root->SetStringField(TEXT("name"), Blueprint->GetName());
	Root->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
	AddBlueprintVariablesJson(Blueprint, Root);

	TArray<TSharedPtr<FJsonValue>> Graphs;
	int32 GraphIndex = 0;
	auto AddGraphs = [&](const TArray<UEdGraph*>& SourceGraphs, const FString& Type)
	{
		for (UEdGraph* Graph : SourceGraphs)
		{
			const FString GraphAlias = FString::Printf(TEXT("g%d"), GraphIndex++);
			Graphs.Add(MakeShared<FJsonValueObject>(ExportGraph(Graph, GraphAlias, Type, Session)));
		}
	};

	AddGraphs(Blueprint->UbergraphPages, TEXT("event"));
	AddGraphs(Blueprint->FunctionGraphs, TEXT("function"));
	AddGraphs(Blueprint->MacroGraphs, TEXT("macro"));

	Root->SetArrayField(TEXT("graphs"), Graphs);
	Root->SetStringField(TEXT("note"), TEXT("Graph/node aliases are editor-session handles. Re-export after structural edits."));
	OutJson = ToJsonString(Root);

	OutSavedPath = BuildOutputPath(Blueprint);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutSavedPath), true);
	FFileHelper::SaveStringToFile(OutJson, *OutSavedPath);
	return true;
}

bool FToolPlayMCPBlueprintService::AddFunctionCallNode(const FString& SessionId, const FString& GraphAlias, const FString& FunctionPath, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError)
{
	UEdGraph* Graph = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveGraph(SessionId, GraphAlias, Graph, Blueprint, OutError))
	{
		return false;
	}

	UFunction* Function = ResolveFunction(FunctionPath);
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Unable to resolve UFunction: %s"), *FunctionPath);
		return false;
	}

	Graph->Modify();
	Blueprint->Modify();
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(Function);
	FinishPlacedNode(Graph, Node, PositionX, PositionY);
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_blueprint_function_call_node"), Blueprint);
	Root->SetStringField(TEXT("graph"), GraphAlias);
	Root->SetStringField(TEXT("function"), Function->GetPathName());
	Root->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::AddCustomEventNode(const FString& SessionId, const FString& GraphAlias, const FString& EventName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError)
{
	UEdGraph* Graph = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveGraph(SessionId, GraphAlias, Graph, Blueprint, OutError))
	{
		return false;
	}

	if (EventName.IsEmpty())
	{
		OutError = TEXT("event_name is required.");
		return false;
	}

	Graph->Modify();
	Blueprint->Modify();
	UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
	Node->CustomFunctionName = FName(*EventName);
	FinishPlacedNode(Graph, Node, PositionX, PositionY);
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_blueprint_custom_event_node"), Blueprint);
	Root->SetStringField(TEXT("graph"), GraphAlias);
	Root->SetStringField(TEXT("event_name"), EventName);
	Root->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::SetPinDefault(const FString& SessionId, const FString& NodeAlias, const FString& PinName, const FString& DefaultValue, FString& OutJson, FString& OutError)
{
	UEdGraphNode* Node = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveNode(SessionId, NodeAlias, Node, Blueprint, OutError))
	{
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(Node, PinName);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Unknown pin '%s' on node '%s'."), *PinName, *NodeAlias);
		return false;
	}
	if (Pin->Direction != EGPD_Input)
	{
		OutError = TEXT("Only input pin defaults can be edited.");
		return false;
	}

	Node->Modify();
	Pin->Modify();
	Pin->DefaultValue = DefaultValue;
	Node->PinDefaultValueChanged(Pin);
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("set_blueprint_pin_default"), Blueprint);
	Root->SetStringField(TEXT("node"), NodeAlias);
	Root->SetStringField(TEXT("pin"), PinName);
	Root->SetStringField(TEXT("default"), DefaultValue);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::ConnectPins(const FString& SessionId, const FString& FromNodeAlias, const FString& FromPinName, const FString& ToNodeAlias, const FString& ToPinName, FString& OutJson, FString& OutError)
{
	UEdGraphNode* FromNode = nullptr;
	UEdGraphNode* ToNode = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveNode(SessionId, FromNodeAlias, FromNode, Blueprint, OutError))
	{
		return false;
	}
	UBlueprint* ToBlueprint = nullptr;
	if (!ResolveNode(SessionId, ToNodeAlias, ToNode, ToBlueprint, OutError))
	{
		return false;
	}

	UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
	UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
	if (!FromPin || !ToPin)
	{
		OutError = TEXT("Unable to resolve one or both pins.");
		return false;
	}
	if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
	{
		OutError = TEXT("connect_blueprint_pins expects output -> input.");
		return false;
	}

	FromNode->GetGraph()->Modify();
	FromPin->Modify();
	ToPin->Modify();
	const UEdGraphSchema* Schema = FromNode->GetGraph()->GetSchema();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		OutError = Response.Message.ToString();
		return false;
	}
	Schema->TryCreateConnection(FromPin, ToPin);
	FromNode->GetGraph()->NotifyGraphChanged();
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("connect_blueprint_pins"), Blueprint);
	Root->SetStringField(TEXT("from"), FromNodeAlias + TEXT(".") + FromPinName);
	Root->SetStringField(TEXT("to"), ToNodeAlias + TEXT(".") + ToPinName);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::DisconnectPin(const FString& SessionId, const FString& NodeAlias, const FString& PinName, FString& OutJson, FString& OutError)
{
	UEdGraphNode* Node = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveNode(SessionId, NodeAlias, Node, Blueprint, OutError))
	{
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(Node, PinName);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Unknown pin '%s' on node '%s'."), *PinName, *NodeAlias);
		return false;
	}

	Node->GetGraph()->Modify();
	Pin->Modify();
	Pin->BreakAllPinLinks();
	Node->GetGraph()->NotifyGraphChanged();
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("disconnect_blueprint_pin"), Blueprint);
	Root->SetStringField(TEXT("node"), NodeAlias);
	Root->SetStringField(TEXT("pin"), PinName);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::RemoveNode(const FString& SessionId, const FString& NodeAlias, FString& OutJson, FString& OutError)
{
	UEdGraphNode* Node = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveNode(SessionId, NodeAlias, Node, Blueprint, OutError))
	{
		return false;
	}

	Node->GetGraph()->Modify();
	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("remove_blueprint_node"), Blueprint);
	Root->SetStringField(TEXT("node"), NodeAlias);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::CompileBlueprint(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("compile_blueprint"), Blueprint);
	Root->SetBoolField(TEXT("compiled"), true);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::ListVariables(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
		return false;
	}

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("list_blueprint_variables"), Blueprint);
	Root->SetBoolField(TEXT("reexport_recommended"), false);
	AddBlueprintVariablesJson(Blueprint, Root);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::AddMemberVariable(const FString& AssetPath, const FString& VariableName, const FString& TypeName, const FString& DefaultValue, const FString& Category, FString& OutJson, FString& OutError)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
		return false;
	}
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("variable_name is required.");
		return false;
	}

	FEdGraphPinType PinType;
	if (!ResolveBlueprintPinType(TypeName, PinType, OutError))
	{
		return false;
	}

	Blueprint->Modify();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType, DefaultValue))
	{
		OutError = FString::Printf(TEXT("Unable to add Blueprint variable '%s'."), *VariableName);
		return false;
	}

	if (!Category.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VariableName), nullptr, FText::FromString(Category), true);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_blueprint_member_variable"), Blueprint);
	Root->SetStringField(TEXT("variable"), VariableName);
	Root->SetStringField(TEXT("type"), PinTypeToString(PinType));
	if (!DefaultValue.IsEmpty())
	{
		Root->SetStringField(TEXT("default"), DefaultValue);
	}
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::SetMemberVariableDefault(const FString& AssetPath, const FString& VariableName, const FString& DefaultValue, FString& OutJson, FString& OutError)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(AssetPath).TryLoad());
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Asset is not a Blueprint: %s"), *AssetPath);
		return false;
	}

	const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName));
	if (!Blueprint->NewVariables.IsValidIndex(VariableIndex))
	{
		OutError = FString::Printf(TEXT("Unknown Blueprint member variable '%s'."), *VariableName);
		return false;
	}

	Blueprint->Modify();
	Blueprint->NewVariables[VariableIndex].DefaultValue = DefaultValue;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("set_blueprint_variable_default"), Blueprint);
	Root->SetStringField(TEXT("variable"), VariableName);
	Root->SetStringField(TEXT("default"), DefaultValue);
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::AddVariableGetNode(const FString& SessionId, const FString& GraphAlias, const FString& VariableName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError)
{
	UEdGraph* Graph = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveGraph(SessionId, GraphAlias, Graph, Blueprint, OutError))
	{
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName)) == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Unknown Blueprint member variable '%s'."), *VariableName);
		return false;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		OutError = TEXT("Graph does not use the K2 schema.");
		return false;
	}

	Graph->Modify();
	Blueprint->Modify();
	UK2Node_VariableGet* Node = Schema->SpawnVariableGetNode(FVector2D(PositionX, PositionY), Graph, FName(*VariableName), GetBlueprintVariableSource(Blueprint));
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Unable to spawn get node for '%s'."), *VariableName);
		return false;
	}
	Graph->NotifyGraphChanged();
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_blueprint_variable_get_node"), Blueprint);
	Root->SetStringField(TEXT("graph"), GraphAlias);
	Root->SetStringField(TEXT("variable"), VariableName);
	Root->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutJson = ToCondensedJsonString(Root);
	return true;
}

bool FToolPlayMCPBlueprintService::AddVariableSetNode(const FString& SessionId, const FString& GraphAlias, const FString& VariableName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError)
{
	UEdGraph* Graph = nullptr;
	UBlueprint* Blueprint = nullptr;
	if (!ResolveGraph(SessionId, GraphAlias, Graph, Blueprint, OutError))
	{
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName)) == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Unknown Blueprint member variable '%s'."), *VariableName);
		return false;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		OutError = TEXT("Graph does not use the K2 schema.");
		return false;
	}

	Graph->Modify();
	Blueprint->Modify();
	UK2Node_VariableSet* Node = Schema->SpawnVariableSetNode(FVector2D(PositionX, PositionY), Graph, FName(*VariableName), GetBlueprintVariableSource(Blueprint));
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Unable to spawn set node for '%s'."), *VariableName);
		return false;
	}
	Graph->NotifyGraphChanged();
	MarkBlueprintEdited(Blueprint);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_blueprint_variable_set_node"), Blueprint);
	Root->SetStringField(TEXT("graph"), GraphAlias);
	Root->SetStringField(TEXT("variable"), VariableName);
	Root->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutJson = ToCondensedJsonString(Root);
	return true;
}

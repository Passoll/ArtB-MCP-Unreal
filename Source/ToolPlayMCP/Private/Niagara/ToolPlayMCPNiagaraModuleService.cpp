#include "Niagara/ToolPlayMCPNiagaraModuleService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_Niagara.h"
#include "Engine/SkeletalMesh.h"
#include "NiagaraGraph.h"
#include "NiagaraDataInterfaceSkeletalMesh.h"
#include "NiagaraDataInterfaceVolumeTexture.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
	struct FToolPlayMCPNiagaraSession
	{
		TWeakObjectPtr<UNiagaraSystem> System;
		TMap<FString, TWeakObjectPtr<UNiagaraNodeFunctionCall>> ModuleAliases;
		TMap<FString, TWeakObjectPtr<UEdGraphNode>> GraphNodeAliases;
		TMap<FString, TWeakObjectPtr<UNiagaraScript>> StackScripts;
		TMap<FString, TWeakObjectPtr<UNiagaraNodeOutput>> StackOutputs;
		TMap<FString, FGuid> StackEmitterIds;
	};

	TMap<FString, FToolPlayMCPNiagaraSession> GNiagaraSessions;

	FString EnumValueToString(const TCHAR* EnumPath, int64 Value)
	{
		if (const UEnum* Enum = FindObject<UEnum>(nullptr, EnumPath))
		{
			return Enum->GetNameStringByValue(Value);
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	FString ScriptUsageToString(ENiagaraScriptUsage Usage)
	{
		return EnumValueToString(TEXT("/Script/Niagara.ENiagaraScriptUsage"), static_cast<int64>(Usage));
	}

	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
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
		Name.RemoveFromStart(TEXT("NiagaraNode"));
		Name.RemoveFromStart(TEXT("UNiagaraNode"));
		return Name;
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
	}

	FString FunctionName(const UNiagaraNodeFunctionCall* FunctionCall)
	{
		if (!FunctionCall)
		{
			return FString();
		}

		const FString Name = FunctionCall->GetFunctionName();
		if (!Name.IsEmpty())
		{
			return Name;
		}

		if (FunctionCall->FunctionScript)
		{
			return FunctionCall->FunctionScript->GetName();
		}

		return NodeTitle(FunctionCall);
	}

	UNiagaraGraph* GetGraphFromScript(const UNiagaraScript* Script)
	{
		if (!Script)
		{
			return nullptr;
		}

		UNiagaraScriptSourceBase* SourceBase = const_cast<UNiagaraScript*>(Script)->GetLatestSource();
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
		return Source ? Source->NodeGraph : nullptr;
	}

	UNiagaraNodeOutput* FindOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return nullptr;
		}

		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode && OutputNode->GetUsage() == Usage && OutputNode->GetUsageId() == UsageId)
			{
				return OutputNode;
			}
		}
		return nullptr;
	}

	bool ResolveStack(const FString& SessionId, const FString& StackAlias, UNiagaraScript*& OutScript, UNiagaraNodeOutput*& OutOutputNode, FString& OutError)
	{
		FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
		if (!Session)
		{
			OutError = FString::Printf(TEXT("Unknown Niagara session: %s"), *SessionId);
			return false;
		}

		TWeakObjectPtr<UNiagaraScript>* ScriptPtr = Session->StackScripts.Find(StackAlias);
		TWeakObjectPtr<UNiagaraNodeOutput>* OutputPtr = Session->StackOutputs.Find(StackAlias);
		OutScript = ScriptPtr ? ScriptPtr->Get() : nullptr;
		OutOutputNode = OutputPtr ? OutputPtr->Get() : nullptr;
		if (!OutScript || !OutOutputNode)
		{
			OutError = FString::Printf(TEXT("Unknown or expired Niagara stack alias: %s. Re-export the system before editing."), *StackAlias);
			return false;
		}
		return true;
	}

	bool ResolveSourceScriptForModule(const FString& SessionId, UNiagaraNodeFunctionCall* Module, UNiagaraScript*& OutScript, FString& OutError)
	{
		FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
		if (!Session)
		{
			OutError = FString::Printf(TEXT("Unknown Niagara session: %s"), *SessionId);
			return false;
		}

		UNiagaraGraph* ModuleGraph = Module ? Module->GetNiagaraGraph() : nullptr;
		for (const TPair<FString, TWeakObjectPtr<UNiagaraScript>>& Pair : Session->StackScripts)
		{
			UNiagaraScript* CandidateScript = Pair.Value.Get();
			if (CandidateScript && GetGraphFromScript(CandidateScript) == ModuleGraph)
			{
				OutScript = CandidateScript;
				return true;
			}
		}

		OutError = TEXT("Could not resolve owning script for module. Re-export the system and try again.");
		return false;
	}

	FGuid ResolveEmitterIdForStackAlias(const FToolPlayMCPNiagaraSession& Session, const FString& StackAlias)
	{
		if (!StackAlias.StartsWith(TEXT("e")))
		{
			return FGuid();
		}

		FString EmitterIndexText;
		for (int32 Index = 1; Index < StackAlias.Len(); ++Index)
		{
			const TCHAR Char = StackAlias[Index];
			if (!FChar::IsDigit(Char))
			{
				break;
			}
			EmitterIndexText.AppendChar(Char);
		}

		if (EmitterIndexText.IsEmpty())
		{
			return FGuid();
		}

		UNiagaraSystem* System = Session.System.Get();
		const int32 EmitterIndex = FCString::Atoi(*EmitterIndexText);
		return System && System->GetEmitterHandles().IsValidIndex(EmitterIndex)
			? System->GetEmitterHandle(EmitterIndex).GetId()
			: FGuid();
	}

	TSharedRef<FJsonObject> BuildEditResult(const FString& Operation, UNiagaraSystem* System)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), true);
		Root->SetStringField(TEXT("operation"), Operation);
		Root->SetStringField(TEXT("asset_path"), System ? System->GetPathName() : FString());
		Root->SetBoolField(TEXT("reexport_required"), true);
		Root->SetBoolField(TEXT("compile_requested"), true);
		Root->SetBoolField(TEXT("compile_result_included"), false);
		Root->SetStringField(TEXT("validation_next_step"), TEXT("Call niagara diagnostics with force=true and wait=true before saving or claiming success."));
		return Root;
	}

	UEdGraphPin* FindParameterMapPin(UNiagaraNodeFunctionCall* Module, EEdGraphPinDirection Direction)
	{
		if (!Module)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Module->Pins)
		{
			if (!Pin || Pin->Direction != Direction)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Contains(TEXT("Map")) || PinName.Contains(TEXT("Parameter")))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool RemoveModuleNodeFromParameterMapChain(UNiagaraNodeFunctionCall* Module, FString& OutError)
	{
		if (!Module)
		{
			OutError = TEXT("Invalid module.");
			return false;
		}

		UEdGraph* Graph = Module->GetGraph();
		UEdGraphPin* InputPin = FindParameterMapPin(Module, EGPD_Input);
		UEdGraphPin* OutputPin = FindParameterMapPin(Module, EGPD_Output);
		if (!Graph || !InputPin || !OutputPin)
		{
			OutError = FString::Printf(TEXT("Module does not expose parameter map pins: %s"), *Module->GetName());
			return false;
		}

		TArray<UEdGraphPin*> PreviousPins = InputPin->LinkedTo;
		TArray<UEdGraphPin*> NextPins = OutputPin->LinkedTo;
		Graph->Modify();
		Module->Modify();

		InputPin->BreakAllPinLinks();
		OutputPin->BreakAllPinLinks();

		for (UEdGraphPin* PreviousPin : PreviousPins)
		{
			for (UEdGraphPin* NextPin : NextPins)
			{
				if (PreviousPin && NextPin)
				{
					PreviousPin->MakeLinkTo(NextPin);
				}
			}
		}

		Module->DestroyNode();
		Graph->NotifyGraphChanged();
		return true;
	}

	void AddPinDefaults(const UEdGraphNode* Node, const TSharedRef<FJsonObject>& NodeObject)
	{
		TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
		bool bHasDefaults = false;

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0 && !Pin->DefaultValue.IsEmpty())
			{
				Defaults->SetStringField(Pin->PinName.ToString(), Pin->DefaultValue);
				bHasDefaults = true;
			}
		}

		if (bHasDefaults)
		{
			NodeObject->SetObjectField(TEXT("defaults"), Defaults);
		}
	}

	TSharedRef<FJsonObject> ExportCompactGraph(
		UNiagaraGraph* Graph,
		const FString& AliasPrefix,
		const FString& SessionId,
		const int32 MaxNodes = 80,
		const int32 MaxEdges = 160)
	{
		TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		if (!Graph)
		{
			GraphObject->SetBoolField(TEXT("available"), false);
			return GraphObject;
		}

		GraphObject->SetBoolField(TEXT("available"), true);
		TSharedRef<FJsonObject> NodesObject = MakeShared<FJsonObject>();
		TMap<const UEdGraphNode*, FString> NodeAliases;
		int32 NodeIndex = 0;
		FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || NodeIndex >= MaxNodes)
			{
				continue;
			}

			const FString Kind = CleanNodeKind(Node);
			if (Kind.Contains(TEXT("Reroute")))
			{
				continue;
			}

			const FString Alias = FString::Printf(TEXT("%s.g%d"), *AliasPrefix, NodeIndex++);
			NodeAliases.Add(Node, Alias);
			if (Session)
			{
				Session->GraphNodeAliases.Add(Alias, Node);
			}

			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("k"), Kind);
			NodeObject->SetStringField(TEXT("title"), NodeTitle(Node));

			if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				NodeObject->SetStringField(TEXT("fn"), FunctionName(FunctionCall));
				if (FunctionCall->FunctionScript)
				{
					NodeObject->SetStringField(TEXT("script"), FunctionCall->FunctionScript->GetPathName());
					NodeObject->SetStringField(TEXT("usage"), ScriptUsageToString(FunctionCall->FunctionScript->GetUsage()));
				}
			}

			AddPinDefaults(Node, NodeObject);
			NodesObject->SetObjectField(Alias, NodeObject);
		}

		TArray<TSharedPtr<FJsonValue>> Edges;
		for (const TPair<const UEdGraphNode*, FString>& Pair : NodeAliases)
		{
			const UEdGraphNode* Node = Pair.Key;
			for (const UEdGraphPin* Pin : Node->Pins)
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
					if (!ToAlias || Edges.Num() >= MaxEdges)
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
		GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphObject->SetBoolField(TEXT("truncated"), Graph->Nodes.Num() > MaxNodes || Edges.Num() >= MaxEdges);
		return GraphObject;
	}

	void CollectTraversalSummary(
		UNiagaraGraph* Graph,
		ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		const TSharedRef<FJsonObject>& ScriptObject)
	{
		if (!Graph)
		{
			return;
		}

		TArray<UNiagaraNode*> TraversedNodes;
		Graph->BuildTraversal(TraversedNodes, Usage, UsageId, false);

		TArray<TSharedPtr<FJsonValue>> Visits;
		int32 VisitIndex = 0;
		for (const UNiagaraNode* Node : TraversedNodes)
		{
			if (!Node || VisitIndex >= 80)
			{
				continue;
			}

			TSharedRef<FJsonObject> Visit = MakeShared<FJsonObject>();
			Visit->SetStringField(TEXT("id"), FString::Printf(TEXT("v%d"), VisitIndex++));
			Visit->SetStringField(TEXT("k"), CleanNodeKind(Node));
			Visit->SetStringField(TEXT("title"), NodeTitle(Node));
			if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				Visit->SetStringField(TEXT("fn"), FunctionName(FunctionCall));
			}
			Visits.Add(MakeShared<FJsonValueObject>(Visit));
		}

		ScriptObject->SetArrayField(TEXT("traversal"), Visits);
		ScriptObject->SetStringField(TEXT("stack_adapter"), TEXT("graph_traversal_fallback"));
		ScriptObject->SetStringField(TEXT("parameter_map_status"), TEXT("not_exported_v0_builder_not_linkable_from_plugin"));
	}

	bool GetModuleInputs(
		UNiagaraNodeFunctionCall* Module,
		TArray<FNiagaraVariable>& OutInputs,
		TSet<FNiagaraVariable>& OutHiddenInputs,
		FString& OutError)
	{
		if (!Module)
		{
			OutError = TEXT("Invalid Niagara module alias.");
			return false;
		}

		FCompileConstantResolver Resolver;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Module,
			OutInputs,
			OutHiddenInputs,
			Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
			false);
		return true;
	}

	bool MatchesInputName(const FNiagaraVariable& Variable, const FString& InputName)
	{
		const FString FullName = Variable.GetName().ToString();
		if (FullName.Equals(InputName, ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FNiagaraParameterHandle Handle(Variable.GetName());
		return Handle.GetName().ToString().Equals(InputName, ESearchCase::IgnoreCase);
	}

	TArray<TSharedPtr<FJsonValue>> ExportEnumValues(const UEnum* Enum)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (!Enum)
		{
			return Values;
		}

		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
			Value->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
			Value->SetNumberField(TEXT("value"), Enum->GetValueByIndex(Index));
			Values.Add(MakeShared<FJsonValueObject>(Value));
		}
		return Values;
	}

	FString NiagaraValueKind(const FNiagaraVariable& Input)
	{
		const FNiagaraTypeDefinition Type = Input.GetType();
		if (Type.IsEnum())
		{
			return TEXT("enum");
		}
		if (Type.IsDataInterface())
		{
			return TEXT("data_interface");
		}
		if (Type.IsUObject())
		{
			return TEXT("object");
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return TEXT("bool");
		}
		if (Type == FNiagaraTypeDefinition::GetFloatDef() || Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return TEXT("scalar");
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def() || Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			return TEXT("vector");
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			return TEXT("color");
		}
		return TEXT("struct");
	}

	bool IsStaticSwitchInput(UNiagaraNodeFunctionCall* Module, const FNiagaraVariable& Input)
	{
		UNiagaraGraph* CalledGraph = Module ? Module->GetCalledGraph() : nullptr;
		return CalledGraph ? CalledGraph->FindStaticSwitchInputs().Contains(Input) : false;
	}

	UEdGraphPin* FindExistingOverridePin(UNiagaraNodeFunctionCall* Module, const FNiagaraVariable& Input)
	{
		if (!Module)
		{
			return nullptr;
		}

		const FNiagaraParameterHandle ModuleHandle(Input.GetName());
		const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
		const FString AliasedName = AliasedHandle.GetParameterHandleString().ToString();

		UEdGraphNode* OverrideNode = nullptr;
		for (UEdGraphPin* Pin : Module->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() != 1)
			{
				continue;
			}
			if (UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				OverrideNode = Pin->LinkedTo[0] ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
				break;
			}
		}

		if (!OverrideNode || !OverrideNode->GetClass()->GetName().Contains(TEXT("ParameterMapSet")))
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : OverrideNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Equals(AliasedName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	void AddPinDetails(const UEdGraphPin* Pin, const TSharedRef<FJsonObject>& Object)
	{
		if (!Pin)
		{
			Object->SetBoolField(TEXT("has_override_pin"), false);
			return;
		}

		Object->SetBoolField(TEXT("has_override_pin"), true);
		Object->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		Object->SetStringField(TEXT("autogenerated_default_value"), Pin->AutogeneratedDefaultValue);
		Object->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());

		TSharedRef<FJsonObject> PinType = MakeShared<FJsonObject>();
		PinType->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinType->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
		PinType->SetStringField(TEXT("subcategory_object"), Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
		Object->SetObjectField(TEXT("pin_type"), PinType);
	}

	TSharedRef<FJsonObject> BuildStaticSwitchPinItem(const UEdGraphPin* Pin, const TSet<UEdGraphPin*>& HiddenPins)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		const FString PinName = Pin ? Pin->PinName.ToString() : FString();
		Item->SetStringField(TEXT("name"), PinName);
		Item->SetStringField(TEXT("full_name"), PinName);
		Item->SetStringField(TEXT("type"), Pin && Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetName() : Pin ? Pin->PinType.PinCategory.ToString() : FString());
		Item->SetStringField(TEXT("value_kind"), TEXT("static_switch"));
		Item->SetStringField(TEXT("source"), TEXT("static_switch_pin"));
		Item->SetBoolField(TEXT("hidden"), Pin ? HiddenPins.Contains(const_cast<UEdGraphPin*>(Pin)) : false);
		Item->SetBoolField(TEXT("is_data_interface"), false);
		Item->SetBoolField(TEXT("is_object"), false);
		Item->SetBoolField(TEXT("is_enum"), Pin && Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) != nullptr);
		Item->SetBoolField(TEXT("is_static_switch"), true);
		if (const UEnum* Enum = Pin ? Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) : nullptr)
		{
			Item->SetStringField(TEXT("enum"), Enum->GetPathName());
			Item->SetArrayField(TEXT("enum_values"), ExportEnumValues(Enum));
		}
		AddPinDetails(Pin, Item);
		return Item;
	}

	void GetStaticSwitchPins(UNiagaraNodeFunctionCall* Module, TArray<UEdGraphPin*>& OutPins, TSet<UEdGraphPin*>& OutHiddenPins)
	{
		if (!Module)
		{
			return;
		}

		FCompileConstantResolver Resolver;
		FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(*Module, OutPins, OutHiddenPins, Resolver);
	}

	UEdGraphPin* FindStaticSwitchPin(UNiagaraNodeFunctionCall* Module, const FString& InputName, TSet<UEdGraphPin*>* OutHiddenPins = nullptr)
	{
		TArray<UEdGraphPin*> StaticSwitchPins;
		TSet<UEdGraphPin*> HiddenStaticSwitchPins;
		GetStaticSwitchPins(Module, StaticSwitchPins, HiddenStaticSwitchPins);
		if (OutHiddenPins)
		{
			*OutHiddenPins = HiddenStaticSwitchPins;
		}

		for (UEdGraphPin* Pin : StaticSwitchPins)
		{
			if (!Pin)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (PinName.Equals(InputName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool NormalizeBoolStaticSwitchValue(const FString& RawValue, FString& OutValue)
	{
		const FString CleanValue = RawValue.TrimStartAndEnd();
		if (CleanValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || CleanValue.Equals(TEXT("1")) || CleanValue.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
		{
			OutValue = TEXT("true");
			return true;
		}
		if (CleanValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) || CleanValue.Equals(TEXT("0")) || CleanValue.Equals(TEXT("no"), ESearchCase::IgnoreCase))
		{
			OutValue = TEXT("false");
			return true;
		}
		return false;
	}

	bool NormalizeEnumStaticSwitchValue(const UEnum* Enum, const FString& RawValue, FString& OutValue)
	{
		if (!Enum)
		{
			return false;
		}

		const FString CleanValue = RawValue.TrimStartAndEnd();
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			const FString Name = Enum->GetNameStringByIndex(Index);
			const FString FullName = Enum->GetNameByIndex(Index).ToString();
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			const FString ValueText = FString::Printf(TEXT("%lld"), Enum->GetValueByIndex(Index));
			if (CleanValue.Equals(Name, ESearchCase::IgnoreCase) ||
				CleanValue.Equals(FullName, ESearchCase::IgnoreCase) ||
				CleanValue.Equals(DisplayName, ESearchCase::IgnoreCase) ||
				CleanValue.Equals(ValueText, ESearchCase::IgnoreCase))
			{
				OutValue = Name;
				return true;
			}
		}
		return false;
	}

	bool NormalizeStaticSwitchValue(const UEdGraphPin* Pin, const FString& RawValue, FString& OutValue, FString& OutError)
	{
		if (!Pin)
		{
			OutError = TEXT("Invalid static switch pin.");
			return false;
		}

		if (const UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			if (NormalizeEnumStaticSwitchValue(Enum, RawValue, OutValue))
			{
				return true;
			}

			TArray<FString> AllowedValues;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				AllowedValues.Add(FString::Printf(
					TEXT("%s (%s)"),
					*Enum->GetDisplayNameTextByIndex(Index).ToString(),
					*Enum->GetNameStringByIndex(Index)));
			}
			OutError = FString::Printf(TEXT("Invalid enum value '%s' for static switch '%s'. Allowed: %s"), *RawValue, *Pin->PinName.ToString(), *FString::Join(AllowedValues, TEXT(", ")));
			return false;
		}

		if (NormalizeBoolStaticSwitchValue(RawValue, OutValue))
		{
			return true;
		}

		OutValue = RawValue.TrimStartAndEnd();
		return true;
	}

	TSharedRef<FJsonObject> BuildModuleInputSummary(UNiagaraNodeFunctionCall* Module)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		TArray<FNiagaraVariable> Inputs;
		TSet<FNiagaraVariable> HiddenInputs;
		FString Error;
		if (!GetModuleInputs(Module, Inputs, HiddenInputs, Error))
		{
			Summary->SetBoolField(TEXT("available"), false);
			Summary->SetStringField(TEXT("error"), Error);
			return Summary;
		}

		TArray<TSharedPtr<FJsonValue>> StaticSwitches;
		TArray<TSharedPtr<FJsonValue>> HiddenStaticSwitches;
		TArray<UEdGraphPin*> StaticSwitchPins;
		TSet<UEdGraphPin*> HiddenStaticSwitchPins;
		GetStaticSwitchPins(Module, StaticSwitchPins, HiddenStaticSwitchPins);
		int32 HiddenCount = 0;
		for (const FNiagaraVariable& Input : Inputs)
		{
			const bool bHidden = HiddenInputs.Contains(Input);
			const bool bStaticSwitch = IsStaticSwitchInput(Module, Input);
			if (bHidden)
			{
				HiddenCount++;
			}
			if (!bStaticSwitch)
			{
				continue;
			}

			const FNiagaraParameterHandle Handle(Input.GetName());
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
			Item->SetStringField(TEXT("full_name"), Input.GetName().ToString());
			Item->SetStringField(TEXT("type"), Input.GetType().GetName());
			Item->SetStringField(TEXT("value_kind"), NiagaraValueKind(Input));
			Item->SetBoolField(TEXT("hidden"), bHidden);
			if (const UEnum* Enum = Input.GetType().GetEnum())
			{
				Item->SetStringField(TEXT("enum"), Enum->GetPathName());
				Item->SetArrayField(TEXT("enum_values"), ExportEnumValues(Enum));
			}
			AddPinDetails(FindExistingOverridePin(Module, Input), Item);

			TSharedPtr<FJsonValueObject> Value = MakeShared<FJsonValueObject>(Item);
			StaticSwitches.Add(Value);
			if (bHidden)
			{
				HiddenStaticSwitches.Add(Value);
			}
		}
		for (UEdGraphPin* Pin : StaticSwitchPins)
		{
			if (!Pin)
			{
				continue;
			}
			TSharedPtr<FJsonValueObject> Value = MakeShared<FJsonValueObject>(BuildStaticSwitchPinItem(Pin, HiddenStaticSwitchPins));
			StaticSwitches.Add(Value);
			if (HiddenStaticSwitchPins.Contains(Pin))
			{
				HiddenStaticSwitches.Add(Value);
			}
		}

		Summary->SetBoolField(TEXT("available"), true);
		Summary->SetNumberField(TEXT("input_count"), Inputs.Num());
		Summary->SetNumberField(TEXT("hidden_count"), HiddenCount);
		Summary->SetNumberField(TEXT("static_switch_count"), StaticSwitches.Num());
		Summary->SetNumberField(TEXT("hidden_static_switch_count"), HiddenStaticSwitches.Num());
		Summary->SetBoolField(TEXT("requires_input_review"), HiddenStaticSwitches.Num() > 0);
		Summary->SetStringField(TEXT("review_reason"), HiddenStaticSwitches.Num() > 0 ? TEXT("Module has hidden static switches that can change execution branches.") : FString());
		Summary->SetArrayField(TEXT("static_switches"), StaticSwitches);
		Summary->SetArrayField(TEXT("hidden_static_switches"), HiddenStaticSwitches);
		return Summary;
	}

	UNiagaraSystem* ResolveSystem(const FString& SessionId)
	{
		FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
		return Session ? Session->System.Get() : nullptr;
	}

	FString MakeGraphNodeAlias(FToolPlayMCPNiagaraSession& Session, const FString& ModuleAlias, UEdGraphNode* Node)
	{
		for (const TPair<FString, TWeakObjectPtr<UEdGraphNode>>& Pair : Session.GraphNodeAliases)
		{
			if (Pair.Value.Get() == Node)
			{
				return Pair.Key;
			}
		}

		int32 Index = 0;
		FString Alias;
		do
		{
			Alias = FString::Printf(TEXT("%s.p%d"), *ModuleAlias, Index++);
		}
		while (Session.GraphNodeAliases.Contains(Alias));
		Session.GraphNodeAliases.Add(Alias, Node);
		return Alias;
	}

	UEdGraphNode* ResolveGraphNode(FToolPlayMCPNiagaraSession& Session, const FString& Alias)
	{
		if (TWeakObjectPtr<UEdGraphNode>* NodePtr = Session.GraphNodeAliases.Find(Alias))
		{
			return NodePtr->Get();
		}
		return nullptr;
	}

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, const EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	template<typename NodeType>
	NodeType* CreateNiagaraGraphNode(UNiagaraGraph* Graph, int32 X, int32 Y)
	{
		FGraphNodeCreator<NodeType> NodeCreator(*Graph);
		NodeType* Node = NodeCreator.CreateNode();
		Node->SetFlags(RF_Transactional);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		NodeCreator.Finalize();
		return Node;
	}

	UEdGraphNode* CreateNiagaraGraphNodeByClass(UNiagaraGraph* Graph, UClass* NodeClass, int32 X, int32 Y)
	{
		if (!Graph || !NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
		{
			return nullptr;
		}

		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
		if (!Node)
		{
			return nullptr;
		}

		Node->SetFlags(RF_Transactional);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	bool ResolveNiagaraPinType(const FString& TypeName, FNiagaraTypeDefinition& OutType, FString& OutError)
	{
		FString Normalized = TypeName.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("float") || Normalized == TEXT("scalar") || Normalized == TEXT("double"))
		{
			OutType = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
		{
			OutType = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (Normalized == TEXT("int") || Normalized == TEXT("integer"))
		{
			OutType = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (Normalized == TEXT("vec2") || Normalized == TEXT("vector2") || Normalized == TEXT("vector2d") || Normalized == TEXT("float2"))
		{
			OutType = FNiagaraTypeDefinition::GetVec2Def();
			return true;
		}
		if (Normalized == TEXT("vec3") || Normalized == TEXT("vector") || Normalized == TEXT("vector3") || Normalized == TEXT("float3"))
		{
			OutType = FNiagaraTypeDefinition::GetVec3Def();
			return true;
		}
		if (Normalized == TEXT("position"))
		{
			OutType = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}
		if (Normalized == TEXT("vec4") || Normalized == TEXT("vector4") || Normalized == TEXT("float4"))
		{
			OutType = FNiagaraTypeDefinition::GetVec4Def();
			return true;
		}
		if (Normalized == TEXT("color") || Normalized == TEXT("linearcolor"))
		{
			OutType = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}
		if (Normalized == TEXT("quat") || Normalized == TEXT("quaternion"))
		{
			OutType = FNiagaraTypeDefinition::GetQuatDef();
			return true;
		}
		if (Normalized == TEXT("parametermap") || Normalized == TEXT("map"))
		{
			OutType = FNiagaraTypeDefinition::GetParameterMapDef();
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported Niagara dynamic pin value_type: %s"), *TypeName);
		return false;
	}

	bool IsNiagaraDynamicAddPin(const UEdGraphPin* Pin)
	{
		static const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc && Pin->PinType.PinSubCategory == DynamicAddPinSubCategory;
	}

	UEdGraphPin* FindNiagaraDynamicAddPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && IsNiagaraDynamicAddPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraphPin* AddNiagaraDynamicPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName PinName, const FNiagaraTypeDefinition& PinType)
	{
		static const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
		static const FName ParameterPinSubCategory(TEXT("ParameterPin"));
		if (!Node)
		{
			return nullptr;
		}

		UEdGraphPin* AddPin = FindNiagaraDynamicAddPin(Node, Direction);
		if (!AddPin)
		{
			return nullptr;
		}

		AddPin->Modify();
		AddPin->PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(PinType);
		AddPin->PinName = PinName;
		AddPin->PinFriendlyName = FText::AsCultureInvariant(PinName.ToString());
		AddPin->PersistentGuid = FGuid::NewGuid();

		if (Node->GetClass()->GetName().Contains(TEXT("ParameterMapSet")))
		{
			AddPin->PinType.PinSubCategory = ParameterPinSubCategory;
		}

		Node->CreatePin(Direction, FEdGraphPinType(UEdGraphSchema_Niagara::PinCategoryMisc, DynamicAddPinSubCategory, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()), TEXT("Add"));
		if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
		{
			NiagaraNode->MarkNodeRequiresSynchronization(TEXT("ToolPlayMCP::AddNiagaraDynamicPin"), true);
		}
		return AddPin;
	}

	void RefreshCustomHlslSignatureFromPins(UNiagaraNodeCustomHlsl* Node)
	{
		if (!Node)
		{
			return;
		}

		Node->Modify();
		FNiagaraFunctionSignature Signature = Node->Signature;
		Signature.Inputs.Empty();
		Signature.Outputs.Empty();

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || IsNiagaraDynamicAddPin(Pin))
			{
				continue;
			}

			const bool bNeedsValue = Pin->Direction == EGPD_Input;
			FNiagaraVariable Variable = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin, bNeedsValue);
			if (!Variable.IsValid())
			{
				continue;
			}

			if (Pin->Direction == EGPD_Input)
			{
				Signature.Inputs.Add(Variable);
			}
			else if (Pin->Direction == EGPD_Output)
			{
				Signature.Outputs.Add(Variable);
			}
		}

		Node->Signature = Signature;
		Node->MarkNodeRequiresSynchronization(TEXT("ToolPlayMCP::RefreshCustomHlslSignatureFromPins"), true);
	}

	void SetCustomHlslText(UNiagaraNodeCustomHlsl* Node, const FString& Hlsl)
	{
		if (!Node)
		{
			return;
		}

		if (FStrProperty* HlslProperty = FindFProperty<FStrProperty>(Node->GetClass(), TEXT("CustomHlsl")))
		{
			HlslProperty->SetPropertyValue_InContainer(Node, Hlsl);
			Node->ReconstructNode();
		}
	}

	UNiagaraScript* CreateScratchModuleScript(UNiagaraSystem* System, ENiagaraScriptUsage TargetUsage, const FString& ModuleName, FString& OutError)
	{
		if (!System)
		{
			OutError = TEXT("Invalid Niagara system.");
			return nullptr;
		}

		System->Modify();
		if (!System->HasAnyFlags(RF_Transactional))
		{
			System->SetFlags(RF_Transactional);
		}

		const FString BaseName = ModuleName.IsEmpty() ? TEXT("MCP_LocalModule") : ModuleName;
		const FName UniqueName = MakeUniqueObjectName(System, UNiagaraScript::StaticClass(), FName(*BaseName));
		UNiagaraScript* NewScript = nullptr;

		UNiagaraScript* DefaultModule = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
		if (DefaultModule && Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource()) && Cast<UNiagaraScriptSource>(DefaultModule->GetLatestSource())->NodeGraph)
		{
			NewScript = Cast<UNiagaraScript>(StaticDuplicateObject(DefaultModule, System, UniqueName));
		}

		if (!NewScript)
		{
			OutError = TEXT("Failed to create Niagara scratch module script. The editor default module script was not available.");
			return nullptr;
		}

		NewScript->ClearFlags(RF_Public | RF_Standalone);
		NewScript->SetFlags(RF_Transactional);
		NewScript->GetLatestScriptData()->ModuleUsageBitmask |= (1 << static_cast<int32>(TargetUsage));
		System->ScratchPadScripts.Add(NewScript);
		return NewScript;
	}

	bool ApplyGraphOperation(
		const FString& SessionId,
		const FString& ModuleAlias,
		FToolPlayMCPNiagaraSession& Session,
		UNiagaraNodeFunctionCall* Module,
		const TSharedRef<FJsonObject>& Operation,
		TArray<TSharedPtr<FJsonValue>>& OutChanges,
		FString& OutError)
	{
		UNiagaraGraph* Graph = Module ? Module->GetCalledGraph() : nullptr;
		if (!Graph)
		{
			OutError = FString::Printf(TEXT("Module has no editable called graph: %s"), *ModuleAlias);
			return false;
		}

		FString Type;
		Operation->TryGetStringField(TEXT("type"), Type);
		Graph->Modify();

		if (Type.Equals(TEXT("add_node"), ESearchCase::IgnoreCase))
		{
			FString NodeKind;
			FString AliasHint;
			int32 X = 0;
			int32 Y = 0;
			Operation->TryGetStringField(TEXT("node_kind"), NodeKind);
			Operation->TryGetStringField(TEXT("alias"), AliasHint);
			Operation->TryGetNumberField(TEXT("x"), X);
			Operation->TryGetNumberField(TEXT("y"), Y);

			UEdGraphNode* NewNode = nullptr;
			if (NodeKind.Equals(TEXT("custom_hlsl"), ESearchCase::IgnoreCase))
			{
				UNiagaraNodeCustomHlsl* CustomNode = CreateNiagaraGraphNode<UNiagaraNodeCustomHlsl>(Graph, X, Y);
				FString Hlsl;
				FString ScriptUsage;
				Operation->TryGetStringField(TEXT("hlsl"), Hlsl);
				Operation->TryGetStringField(TEXT("script_usage"), ScriptUsage);
				SetCustomHlslText(CustomNode, Hlsl);
				if (ScriptUsage.Equals(TEXT("dynamic_input"), ESearchCase::IgnoreCase) || ScriptUsage.Equals(TEXT("dynamicinput"), ESearchCase::IgnoreCase))
				{
					FString OutputTypeName;
					FNiagaraTypeDefinition OutputType = FNiagaraTypeDefinition::GetFloatDef();
					Operation->TryGetStringField(TEXT("output_type"), OutputTypeName);
					if (!ResolveNiagaraPinType(OutputTypeName, OutputType, OutError))
					{
						return false;
					}
					CustomNode->ScriptUsage = ENiagaraScriptUsage::DynamicInput;
					AddNiagaraDynamicPin(CustomNode, EGPD_Input, FName(TEXT("Map")), FNiagaraTypeDefinition::GetParameterMapDef());
					AddNiagaraDynamicPin(CustomNode, EGPD_Output, FName(TEXT("CustomHLSLOutput")), OutputType);
				}
				else
				{
					CustomNode->ScriptUsage = ENiagaraScriptUsage::Function;
				}
				RefreshCustomHlslSignatureFromPins(CustomNode);
				NewNode = CustomNode;
			}
			else if (NodeKind.Equals(TEXT("op"), ESearchCase::IgnoreCase))
			{
				UNiagaraNodeOp* OpNode = CreateNiagaraGraphNode<UNiagaraNodeOp>(Graph, X, Y);
				FString OpName;
				Operation->TryGetStringField(TEXT("op_name"), OpName);
				OpNode->OpName = FName(*OpName);
				OpNode->AllocateDefaultPins();
				NewNode = OpNode;
			}
			else if (NodeKind.Equals(TEXT("function_call"), ESearchCase::IgnoreCase))
			{
				FString ScriptAssetPath;
				Operation->TryGetStringField(TEXT("script_asset_path"), ScriptAssetPath);
				UNiagaraScript* FunctionScript = Cast<UNiagaraScript>(FSoftObjectPath(ScriptAssetPath).TryLoad());
				if (!FunctionScript)
				{
					OutError = FString::Printf(TEXT("Function call asset is not a Niagara Script: %s"), *ScriptAssetPath);
					return false;
				}
				UNiagaraNodeFunctionCall* FunctionNode = CreateNiagaraGraphNode<UNiagaraNodeFunctionCall>(Graph, X, Y);
				FunctionNode->FunctionScript = FunctionScript;
				FunctionNode->SelectedScriptVersion = FunctionScript->GetExposedVersion().VersionGuid;
				FunctionNode->AllocateDefaultPins();
				NewNode = FunctionNode;
			}
			else if (NodeKind.Equals(TEXT("parameter_map_set"), ESearchCase::IgnoreCase) || NodeKind.Equals(TEXT("parameter_map_set_node"), ESearchCase::IgnoreCase))
			{
				UClass* ParameterMapSetClass = LoadClass<UEdGraphNode>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
				if (!ParameterMapSetClass)
				{
					OutError = TEXT("Could not load NiagaraNodeParameterMapSet class from NiagaraEditor.");
					return false;
				}
				NewNode = CreateNiagaraGraphNodeByClass(Graph, ParameterMapSetClass, X, Y);
			}
			else
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara graph node_kind: %s"), *NodeKind);
				return false;
			}

			if (!NewNode)
			{
				OutError = TEXT("Failed to create Niagara graph node.");
				return false;
			}

			FString Alias = AliasHint.IsEmpty() ? MakeGraphNodeAlias(Session, ModuleAlias, NewNode) : AliasHint;
			Session.GraphNodeAliases.Add(Alias, NewNode);
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Type);
			Change->SetStringField(TEXT("alias"), Alias);
			Change->SetStringField(TEXT("title"), NodeTitle(NewNode));
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			return true;
		}

		if (Type.Equals(TEXT("add_dynamic_pin"), ESearchCase::IgnoreCase))
		{
			FString NodeAlias;
			FString DirectionText;
			FString PinName;
			FString ValueType;
			Operation->TryGetStringField(TEXT("node"), NodeAlias);
			Operation->TryGetStringField(TEXT("direction"), DirectionText);
			Operation->TryGetStringField(TEXT("name"), PinName);
			Operation->TryGetStringField(TEXT("value_type"), ValueType);

			UEdGraphNode* DynamicNode = ResolveGraphNode(Session, NodeAlias);
			if (!FindNiagaraDynamicAddPin(DynamicNode, EGPD_Input) && !FindNiagaraDynamicAddPin(DynamicNode, EGPD_Output))
			{
				OutError = FString::Printf(TEXT("Node does not support Niagara dynamic pins: %s"), *NodeAlias);
				return false;
			}
			if (PinName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("add_dynamic_pin requires a non-empty name.");
				return false;
			}

			EEdGraphPinDirection Direction = EGPD_Input;
			if (DirectionText.Equals(TEXT("output"), ESearchCase::IgnoreCase) || DirectionText.Equals(TEXT("out"), ESearchCase::IgnoreCase))
			{
				Direction = EGPD_Output;
			}
			else if (!DirectionText.IsEmpty() && !DirectionText.Equals(TEXT("input"), ESearchCase::IgnoreCase) && !DirectionText.Equals(TEXT("in"), ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("Unsupported dynamic pin direction: %s"), *DirectionText);
				return false;
			}

			FNiagaraTypeDefinition PinType;
			if (!ResolveNiagaraPinType(ValueType, PinType, OutError))
			{
				return false;
			}

			DynamicNode->Modify();
			UEdGraphPin* NewPin = AddNiagaraDynamicPin(DynamicNode, Direction, FName(*PinName), PinType);
			if (!NewPin)
			{
				OutError = FString::Printf(TEXT("Failed to add dynamic pin %s to node %s."), *PinName, *NodeAlias);
				return false;
			}
			NewPin->Modify();
			if (UNiagaraNodeCustomHlsl* CustomNode = Cast<UNiagaraNodeCustomHlsl>(DynamicNode))
			{
				RefreshCustomHlslSignatureFromPins(CustomNode);
			}

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Type);
			Change->SetStringField(TEXT("node"), NodeAlias);
			Change->SetStringField(TEXT("pin"), NewPin->PinName.ToString());
			Change->SetStringField(TEXT("direction"), Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
			Change->SetStringField(TEXT("value_type"), PinType.GetName());
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			return true;
		}

		if (Type.Equals(TEXT("connect"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("disconnect"), ESearchCase::IgnoreCase))
		{
			FString FromNodeAlias;
			FString FromPinName;
			FString ToNodeAlias;
			FString ToPinName;
			Operation->TryGetStringField(TEXT("from_node"), FromNodeAlias);
			Operation->TryGetStringField(TEXT("from_pin"), FromPinName);
			Operation->TryGetStringField(TEXT("to_node"), ToNodeAlias);
			Operation->TryGetStringField(TEXT("to_pin"), ToPinName);

			UEdGraphPin* FromPin = FindPinByName(ResolveGraphNode(Session, FromNodeAlias), FromPinName, EGPD_Output);
			UEdGraphPin* ToPin = FindPinByName(ResolveGraphNode(Session, ToNodeAlias), ToPinName, EGPD_Input);
			if (!FromPin || !ToPin)
			{
				OutError = FString::Printf(TEXT("Could not resolve pins for %s: %s.%s -> %s.%s"), *Type, *FromNodeAlias, *FromPinName, *ToNodeAlias, *ToPinName);
				return false;
			}

			if (Type.Equals(TEXT("connect"), ESearchCase::IgnoreCase))
			{
				if (UEdGraphNode* FromNode = FromPin->GetOwningNode())
				{
					FromNode->Modify();
				}
				if (UEdGraphNode* ToNode = ToPin->GetOwningNode())
				{
					ToNode->Modify();
				}
				FromPin->Modify();
				ToPin->Modify();
				const UEdGraphSchema* Schema = Graph->GetSchema();
				if (!Schema || !Schema->TryCreateConnection(FromPin, ToPin))
				{
					OutError = FString::Printf(TEXT("Niagara schema rejected connection: %s.%s -> %s.%s"), *FromNodeAlias, *FromPinName, *ToNodeAlias, *ToPinName);
					return false;
				}
			}
			else
			{
				if (UEdGraphNode* FromNode = FromPin->GetOwningNode())
				{
					FromNode->Modify();
				}
				if (UEdGraphNode* ToNode = ToPin->GetOwningNode())
				{
					ToNode->Modify();
				}
				FromPin->Modify();
				ToPin->Modify();
				FromPin->BreakLinkTo(ToPin);
			}

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Type);
			Change->SetStringField(TEXT("from_node"), FromNodeAlias);
			Change->SetStringField(TEXT("to_node"), ToNodeAlias);
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			return true;
		}

		if (Type.Equals(TEXT("set_custom_hlsl"), ESearchCase::IgnoreCase))
		{
			FString NodeAlias;
			FString Hlsl;
			Operation->TryGetStringField(TEXT("node"), NodeAlias);
			Operation->TryGetStringField(TEXT("hlsl"), Hlsl);
			UNiagaraNodeCustomHlsl* CustomNode = Cast<UNiagaraNodeCustomHlsl>(ResolveGraphNode(Session, NodeAlias));
			if (!CustomNode)
			{
				OutError = FString::Printf(TEXT("Node is not a custom_hlsl Niagara node: %s"), *NodeAlias);
				return false;
			}
			CustomNode->Modify();
			SetCustomHlslText(CustomNode, Hlsl);

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Type);
			Change->SetStringField(TEXT("node"), NodeAlias);
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			return true;
		}

		if (Type.Equals(TEXT("remove_node"), ESearchCase::IgnoreCase))
		{
			FString NodeAlias;
			Operation->TryGetStringField(TEXT("node"), NodeAlias);
			UEdGraphNode* Node = ResolveGraphNode(Session, NodeAlias);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Unknown Niagara graph node alias: %s"), *NodeAlias);
				return false;
			}
			Node->Modify();
			Node->BreakAllNodeLinks();
			Node->DestroyNode();
			Session.GraphNodeAliases.Remove(NodeAlias);

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Type);
			Change->SetStringField(TEXT("node"), NodeAlias);
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported Niagara module graph patch operation type: %s"), *Type);
		return false;
	}

	FString NormalizeUserParameterName(const FString& UserParameter)
	{
		FString Name = UserParameter.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			Name = TEXT("DeathNoiseTexture");
		}
		Name.ReplaceInline(TEXT(" "), TEXT("_"));
		if (!Name.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			Name = FString::Printf(TEXT("User.%s"), *Name);
		}
		return Name;
	}

	UNiagaraDataInterfaceVolumeTexture* FindLinkedVolumeTextureDI(UEdGraphPin& OverridePin)
	{
		for (UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
		{
			UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
			if (!InputNode)
			{
				continue;
			}

			if (const FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface")))
			{
				if (UNiagaraDataInterfaceVolumeTexture* VolumeTextureDI = Cast<UNiagaraDataInterfaceVolumeTexture>(DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode)))
				{
					return VolumeTextureDI;
				}
			}
		}
		return nullptr;
	}

	UNiagaraDataInterfaceSkeletalMesh* FindLinkedSkeletalMeshDI(UEdGraphPin& OverridePin)
	{
		for (UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
		{
			UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
			if (!InputNode)
			{
				continue;
			}

			UObject* DataInterface = nullptr;
			if (const FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface")))
			{
				DataInterface = DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode);
			}

			if (UNiagaraDataInterfaceSkeletalMesh* SkeletalMeshDI = Cast<UNiagaraDataInterfaceSkeletalMesh>(DataInterface))
			{
				return SkeletalMeshDI;
			}
		}
		return nullptr;
	}
}

FString FToolPlayMCPNiagaraModuleService::CreateSession(UNiagaraSystem* System)
{
	const FString SessionId = FString::Printf(
		TEXT("niagara_%s_%s"),
		System ? *System->GetName() : TEXT("invalid"),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));

	FToolPlayMCPNiagaraSession& Session = GNiagaraSessions.Add(SessionId);
	Session.System = System;
	return SessionId;
}

TSharedRef<FJsonObject> FToolPlayMCPNiagaraModuleService::ExportScriptStack(
	UNiagaraScript* Script,
	ENiagaraScriptUsage Usage,
	const FString& StageName,
	const FString& AliasPrefix,
	const FString& SessionId)
{
	TSharedRef<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
	ScriptObject->SetStringField(TEXT("stage"), StageName);
	ScriptObject->SetStringField(TEXT("alias"), AliasPrefix);
	ScriptObject->SetStringField(TEXT("usage"), ScriptUsageToString(Usage));
	ScriptObject->SetBoolField(TEXT("available"), Script != nullptr);
	if (!Script)
	{
		return ScriptObject;
	}

	ScriptObject->SetStringField(TEXT("script"), Script->GetPathName());
	ScriptObject->SetStringField(TEXT("usage_id"), Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));

	UNiagaraGraph* Graph = GetGraphFromScript(Script);
	if (FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId))
	{
		Session->StackScripts.Add(AliasPrefix, Script);
		if (UNiagaraNodeOutput* OutputNode = FindOutputNode(Graph, Usage, Script->GetUsageId()))
		{
			Session->StackOutputs.Add(AliasPrefix, OutputNode);
		}
	}

	TArray<UNiagaraNode*> TraversedNodes;
	if (Graph)
	{
		Graph->BuildTraversal(TraversedNodes, Usage, Script->GetUsageId(), false);
	}

	TArray<TSharedPtr<FJsonValue>> Modules;
	int32 ModuleIndex = 0;
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);

	for (UNiagaraNode* Node : TraversedNodes)
	{
		UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node);
		if (!FunctionCall)
		{
			continue;
		}

		const FString ModuleAlias = FString::Printf(TEXT("%s.m%d"), *AliasPrefix, ModuleIndex++);
		if (Session)
		{
			Session->ModuleAliases.Add(ModuleAlias, FunctionCall);
		}

		TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
		Module->SetStringField(TEXT("id"), ModuleAlias);
		Module->SetStringField(TEXT("name"), FunctionName(FunctionCall));
		Module->SetStringField(TEXT("node_title"), NodeTitle(FunctionCall));
		if (FunctionCall->FunctionScript)
		{
			Module->SetStringField(TEXT("script"), FunctionCall->FunctionScript->GetPathName());
			Module->SetStringField(TEXT("usage"), ScriptUsageToString(FunctionCall->FunctionScript->GetUsage()));
		}

		UNiagaraGraph* CalledGraph = FunctionCall->GetCalledGraph();
		Module->SetObjectField(TEXT("input_summary"), BuildModuleInputSummary(FunctionCall));
		Module->SetObjectField(TEXT("compact_graph"), ExportCompactGraph(CalledGraph, ModuleAlias, SessionId, 60, 120));
		Modules.Add(MakeShared<FJsonValueObject>(Module));
	}

	ScriptObject->SetArrayField(TEXT("modules"), Modules);
	ScriptObject->SetObjectField(TEXT("compact_graph"), ExportCompactGraph(Graph, AliasPrefix, SessionId));
	CollectTraversalSummary(Graph, Usage, Script->GetUsageId(), ScriptObject);
	return ScriptObject;
}

UNiagaraNodeFunctionCall* FToolPlayMCPNiagaraModuleService::ResolveModule(const FString& SessionId, const FString& ModuleAlias)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session)
	{
		return nullptr;
	}

	TWeakObjectPtr<UNiagaraNodeFunctionCall>* Module = Session->ModuleAliases.Find(ModuleAlias);
	return Module ? Module->Get() : nullptr;
}

bool FToolPlayMCPNiagaraModuleService::AddModuleToStack(const FString& SessionId, const FString& TargetStackAlias, const FString& ScriptAssetPath, int32 TargetIndex, const FString& SuggestedName, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraScript* StackScript = nullptr;
	UNiagaraNodeOutput* OutputNode = nullptr;
	if (!ResolveStack(SessionId, TargetStackAlias, StackScript, OutputNode, OutError))
	{
		return false;
	}

	UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(FSoftObjectPath(ScriptAssetPath).TryLoad());
	if (!ModuleScript)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara Script: %s"), *ScriptAssetPath);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Add Niagara Module")));
	Session->System->Modify();
	StackScript->Modify();
	if (UNiagaraGraph* Graph = OutputNode->GetNiagaraGraph())
	{
		Graph->Modify();
	}

	UNiagaraNodeFunctionCall* NewModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *OutputNode, TargetIndex, SuggestedName);
	if (!NewModule)
	{
		Transaction.Cancel();
		OutError = FString::Printf(TEXT("Failed to add module %s to stack %s."), *ScriptAssetPath, *TargetStackAlias);
		return false;
	}

	if (UNiagaraGraph* Graph = OutputNode->GetNiagaraGraph())
	{
		Graph->NotifyGraphChanged();
	}
	Session->System->MarkPackageDirty();
	Session->System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("add_niagara_module"), Session->System.Get());
	Root->SetStringField(TEXT("target_stack"), TargetStackAlias);
	Root->SetStringField(TEXT("script_asset_path"), ScriptAssetPath);
	Root->SetStringField(TEXT("new_module_name"), FunctionName(NewModule));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::CreateLocalModule(const FString& SessionId, const FString& TargetStackAlias, int32 TargetIndex, const FString& ModuleName, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraScript* StackScript = nullptr;
	UNiagaraNodeOutput* OutputNode = nullptr;
	if (!ResolveStack(SessionId, TargetStackAlias, StackScript, OutputNode, OutError))
	{
		return false;
	}

	UNiagaraSystem* System = Session->System.Get();
	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Create Niagara Local Module")));
	UNiagaraScript* LocalScript = CreateScratchModuleScript(System, OutputNode->GetUsage(), ModuleName, OutError);
	if (!LocalScript)
	{
		Transaction.Cancel();
		return false;
	}

	StackScript->Modify();
	if (UNiagaraGraph* StackGraph = OutputNode->GetNiagaraGraph())
	{
		StackGraph->Modify();
	}

	UNiagaraNodeFunctionCall* NewModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(LocalScript, *OutputNode, TargetIndex, ModuleName);
	if (!NewModule)
	{
		Transaction.Cancel();
		OutError = FString::Printf(TEXT("Failed to add local Niagara module to stack %s."), *TargetStackAlias);
		return false;
	}

	FString NewAlias;
	int32 Index = 0;
	do
	{
		NewAlias = FString::Printf(TEXT("%s.local%d"), *TargetStackAlias, Index++);
	}
	while (Session->ModuleAliases.Contains(NewAlias));
	Session->ModuleAliases.Add(NewAlias, NewModule);

	if (UNiagaraGraph* StackGraph = OutputNode->GetNiagaraGraph())
	{
		StackGraph->NotifyGraphChanged();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("create_niagara_local_module"), System);
	Root->SetStringField(TEXT("target_stack"), TargetStackAlias);
	Root->SetStringField(TEXT("module"), NewAlias);
	Root->SetStringField(TEXT("module_name"), FunctionName(NewModule));
	Root->SetStringField(TEXT("scratch_script"), LocalScript->GetPathName());
	Root->SetStringField(TEXT("note"), TEXT("Re-export the Niagara system to get stable module and graph aliases before detailed graph edits."));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::ApplyModuleGraphPatch(const FString& SessionId, const FString& ModuleAlias, const TArray<TSharedPtr<FJsonValue>>& Operations, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	if (!Module)
	{
		OutError = FString::Printf(TEXT("Unknown or expired module alias: %s"), *ModuleAlias);
		return false;
	}

	UNiagaraScript* SourceScript = nullptr;
	if (!ResolveSourceScriptForModule(SessionId, Module, SourceScript, OutError))
	{
		return false;
	}

	UNiagaraGraph* Graph = Module->GetCalledGraph();
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Module has no editable graph: %s"), *ModuleAlias);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Patch Niagara Module Graph")));
	Session->System->Modify();
	SourceScript->Modify();
	Module->Modify();
	Graph->Modify();

	TArray<TSharedPtr<FJsonValue>> Changes;
	for (int32 OperationIndex = 0; OperationIndex < Operations.Num(); ++OperationIndex)
	{
		const TSharedPtr<FJsonObject>* OperationObject = nullptr;
		if (!Operations[OperationIndex].IsValid() || !Operations[OperationIndex]->TryGetObject(OperationObject) || !OperationObject || !OperationObject->IsValid())
		{
			Transaction.Cancel();
			OutError = FString::Printf(TEXT("Niagara graph patch operation %d is not an object."), OperationIndex);
			return false;
		}

		if (!ApplyGraphOperation(SessionId, ModuleAlias, *Session, Module, OperationObject->ToSharedRef(), Changes, OutError))
		{
			Transaction.Cancel();
			OutError = FString::Printf(TEXT("Operation %d failed: %s"), OperationIndex, *OutError);
			return false;
		}
	}

	Graph->NotifyGraphChanged();
	Session->System->MarkPackageDirty();
	Session->System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("apply_niagara_module_graph_patch"), Session->System.Get());
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetNumberField(TEXT("operation_count"), Operations.Num());
	Root->SetArrayField(TEXT("changes"), Changes);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::RemoveModule(const FString& SessionId, const FString& ModuleAlias, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	if (!Module)
	{
		OutError = FString::Printf(TEXT("Unknown or expired module alias: %s"), *ModuleAlias);
		return false;
	}

	UNiagaraScript* SourceScript = nullptr;
	if (!ResolveSourceScriptForModule(SessionId, Module, SourceScript, OutError))
	{
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Remove Niagara Module")));
	Session->System->Modify();
	SourceScript->Modify();
	if (!RemoveModuleNodeFromParameterMapChain(Module, OutError))
	{
		Transaction.Cancel();
		return false;
	}

	if (UNiagaraGraph* Graph = GetGraphFromScript(SourceScript))
	{
		Graph->NotifyGraphChanged();
	}
	Session->ModuleAliases.Remove(ModuleAlias);
	Session->System->MarkPackageDirty();
	Session->System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("remove_niagara_module"), Session->System.Get());
	Root->SetStringField(TEXT("module"), ModuleAlias);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::MoveModule(const FString& SessionId, const FString& ModuleAlias, const FString& TargetStackAlias, int32 TargetIndex, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	if (!Module)
	{
		OutError = FString::Printf(TEXT("Unknown or expired module alias: %s"), *ModuleAlias);
		return false;
	}

	UNiagaraScript* SourceScript = nullptr;
	if (!ResolveSourceScriptForModule(SessionId, Module, SourceScript, OutError))
	{
		return false;
	}

	UNiagaraScript* TargetScript = nullptr;
	UNiagaraNodeOutput* TargetOutputNode = nullptr;
	if (!ResolveStack(SessionId, TargetStackAlias, TargetScript, TargetOutputNode, OutError))
	{
		return false;
	}

	UNiagaraScript* ModuleScript = Module->FunctionScript;
	if (!ModuleScript)
	{
		OutError = FString::Printf(TEXT("Module has no function script and cannot be moved safely: %s"), *ModuleAlias);
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Move Niagara Module")));
	Session->System->Modify();
	SourceScript->Modify();
	TargetScript->Modify();

	UNiagaraNodeFunctionCall* MovedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *TargetOutputNode, TargetIndex, Module->GetFunctionName());

	if (!MovedModule)
	{
		Transaction.Cancel();
		OutError = FString::Printf(TEXT("Failed to insert moved module at target stack: %s"), *TargetStackAlias);
		return false;
	}

	if (!RemoveModuleNodeFromParameterMapChain(Module, OutError))
	{
		Transaction.Cancel();
		return false;
	}

	if (UNiagaraGraph* Graph = TargetOutputNode->GetNiagaraGraph())
	{
		Graph->NotifyGraphChanged();
	}
	Session->ModuleAliases.FindOrAdd(ModuleAlias) = MovedModule;
	Session->System->MarkPackageDirty();
	Session->System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("move_niagara_module"), Session->System.Get());
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("target_stack"), TargetStackAlias);
	Root->SetNumberField(TEXT("target_index"), TargetIndex);
	Root->SetStringField(TEXT("warning"), TEXT("Move is implemented as insert-then-remove and may not preserve module input overrides. Re-export and inspect the new module alias."));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::SetModuleEnabled(const FString& SessionId, const FString& ModuleAlias, bool bEnabled, FString& OutJson, FString& OutError)
{
	FToolPlayMCPNiagaraSession* Session = GNiagaraSessions.Find(SessionId);
	if (!Session || !Session->System.IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown or expired Niagara session: %s"), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	if (!Module)
	{
		OutError = FString::Printf(TEXT("Unknown or expired module alias: %s"), *ModuleAlias);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Module Enabled")));
	Session->System->Modify();
	Module->Modify();
	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Module, bEnabled);
	Session->System->MarkPackageDirty();
	Session->System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = BuildEditResult(TEXT("set_niagara_module_enabled"), Session->System.Get());
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetBoolField(TEXT("enabled"), bEnabled);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::ListModuleInputs(const FString& SessionId, const FString& ModuleAlias, FString& OutJson, FString& OutError)
{
	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("name"), FunctionName(Module));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FNiagaraVariable& Input : Inputs)
	{
		const FNiagaraParameterHandle Handle(Input.GetName());
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
		Item->SetStringField(TEXT("full_name"), Input.GetName().ToString());
		Item->SetStringField(TEXT("type"), Input.GetType().GetName());
		Item->SetStringField(TEXT("value_kind"), NiagaraValueKind(Input));
		Item->SetStringField(TEXT("source"), TEXT("stack_function_input"));
		Item->SetBoolField(TEXT("hidden"), HiddenInputs.Contains(Input));
		Item->SetBoolField(TEXT("is_data_interface"), Input.GetType().IsDataInterface());
		Item->SetBoolField(TEXT("is_object"), Input.GetType().IsUObject());
		Item->SetBoolField(TEXT("is_enum"), Input.GetType().IsEnum());

		if (const UEnum* Enum = Input.GetType().GetEnum())
		{
			Item->SetStringField(TEXT("enum"), Enum->GetPathName());
			Item->SetArrayField(TEXT("enum_values"), ExportEnumValues(Enum));
		}

		Item->SetBoolField(TEXT("is_static_switch"), IsStaticSwitchInput(Module, Input));

		AddPinDetails(FindExistingOverridePin(Module, Input), Item);
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TArray<UEdGraphPin*> StaticSwitchPins;
	TSet<UEdGraphPin*> HiddenStaticSwitchPins;
	GetStaticSwitchPins(Module, StaticSwitchPins, HiddenStaticSwitchPins);
	for (UEdGraphPin* Pin : StaticSwitchPins)
	{
		if (Pin)
		{
			Items.Add(MakeShared<FJsonValueObject>(BuildStaticSwitchPinItem(Pin, HiddenStaticSwitchPins)));
		}
	}
	Root->SetArrayField(TEXT("inputs"), Items);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::GetModuleInputOverride(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, FString& OutJson, FString& OutError)
{
	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	UEdGraphPin* OverridePin = FindExistingOverridePin(Module, *MatchedInput);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetBoolField(TEXT("has_override_pin"), OverridePin != nullptr);

	TArray<TSharedPtr<FJsonValue>> Links;
	if (OverridePin)
	{
		Root->SetStringField(TEXT("default_value"), OverridePin->DefaultValue);
		for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
		{
			UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("node_class"), LinkedNode ? LinkedNode->GetClass()->GetName() : TEXT(""));
			Link->SetStringField(TEXT("node_title"), LinkedNode ? LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT(""));
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
			{
				Link->SetStringField(TEXT("input_name"), InputNode->Input.GetName().ToString());
				Link->SetStringField(TEXT("input_type"), InputNode->Input.GetType().GetName());
				UObject* ObjectAsset = nullptr;
				if (const FObjectProperty* ObjectAssetProperty = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("ObjectAsset")))
				{
					ObjectAsset = ObjectAssetProperty->GetObjectPropertyValue_InContainer(InputNode);
				}
				Link->SetStringField(TEXT("object_asset"), ObjectAsset ? ObjectAsset->GetPathName() : TEXT(""));
				UObject* DataInterface = nullptr;
				if (const FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface")))
				{
					DataInterface = DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode);
				}
				Link->SetStringField(TEXT("data_interface"), DataInterface ? DataInterface->GetPathName() : TEXT(""));
				if (UNiagaraDataInterfaceVolumeTexture* VolumeTextureDI = Cast<UNiagaraDataInterfaceVolumeTexture>(DataInterface))
				{
					Link->SetStringField(TEXT("volume_texture"), VolumeTextureDI->Texture ? VolumeTextureDI->Texture->GetPathName() : TEXT(""));
					Link->SetStringField(TEXT("texture_user_parameter"), VolumeTextureDI->TextureUserParameter.Parameter.GetName().ToString());
				}
			}
			Links.Add(MakeShared<FJsonValueObject>(Link));
		}
	}
	Root->SetArrayField(TEXT("links"), Links);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::SetModuleInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& Value, FString& OutJson, FString& OutError)
{
	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'."), *SessionId);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Module Input")));
	System->Modify();
	Module->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}

	const FNiagaraParameterHandle ModuleHandle(MatchedInput->GetName());
	const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Module,
		AliasedHandle,
		MatchedInput->GetType(),
		FGuid(),
		FGuid());

	OverridePin.Modify();
	const FString PreviousDefaultValue = OverridePin.DefaultValue;
	if (const UEdGraphSchema* Schema = OverridePin.GetSchema())
	{
		Schema->TrySetDefaultValue(OverridePin, Value);
	}
	else
	{
		OverridePin.DefaultValue = Value;
	}

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	const UEdGraphPin* ConfirmedPin = FindExistingOverridePin(Module, *MatchedInput);
	if (!ConfirmedPin)
	{
		OutError = FString::Printf(TEXT("Failed to confirm Niagara override pin after setting '%s' on module '%s'."), *InputName, *ModuleAlias);
		return false;
	}

	const bool bHasStoredValue = !ConfirmedPin->DefaultValue.IsEmpty() || ConfirmedPin->LinkedTo.Num() > 0 || Value.IsEmpty();
	if (!bHasStoredValue)
	{
		OutError = FString::Printf(TEXT("Niagara override pin for '%s' was created but did not store the requested value '%s'."), *InputName, *Value);
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetStringField(TEXT("value"), Value);
	Root->SetStringField(TEXT("previous_default_value"), PreviousDefaultValue);
	Root->SetStringField(TEXT("stored_default_value"), ConfirmedPin->DefaultValue);
	Root->SetBoolField(TEXT("confirmed_override_pin"), true);
	Root->SetBoolField(TEXT("default_value_exact_match"), ConfirmedPin->DefaultValue.Equals(Value, ESearchCase::CaseSensitive));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::SetModuleStaticSwitch(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& Value, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'. Re-export the system before editing."), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	if (!Module)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara module alias '%s'. Re-export the system before editing."), *ModuleAlias);
		return false;
	}

	TSet<UEdGraphPin*> HiddenStaticSwitchPins;
	UEdGraphPin* StaticSwitchPin = FindStaticSwitchPin(Module, InputName, &HiddenStaticSwitchPins);
	if (!StaticSwitchPin)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no static switch named '%s'. Use list_niagara_module_inputs and look for source='static_switch_pin'."), *ModuleAlias, *InputName);
		return false;
	}

	FString NormalizedValue;
	if (!NormalizeStaticSwitchValue(StaticSwitchPin, Value, NormalizedValue, OutError))
	{
		return false;
	}

	const FString PreviousValue = StaticSwitchPin->DefaultValue;
	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Static Switch")));
	System->Modify();
	Module->Modify();
	StaticSwitchPin->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}

	bool bUsedSchemaSetter = false;
	if (const UEdGraphSchema* Schema = StaticSwitchPin->GetSchema())
	{
		Schema->TrySetDefaultValue(*StaticSwitchPin, NormalizedValue);
		bUsedSchemaSetter = true;
	}
	if (!bUsedSchemaSetter || !StaticSwitchPin->DefaultValue.Equals(NormalizedValue, ESearchCase::CaseSensitive))
	{
		StaticSwitchPin->DefaultValue = NormalizedValue;
		StaticSwitchPin->AutogeneratedDefaultValue = NormalizedValue;
	}

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->NotifyGraphChanged();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("operation"), TEXT("set_niagara_static_switch"));
	Root->SetStringField(TEXT("asset_path"), System->GetPathName());
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), StaticSwitchPin->PinName.ToString());
	Root->SetStringField(TEXT("value"), Value);
	Root->SetStringField(TEXT("normalized_value"), NormalizedValue);
	Root->SetStringField(TEXT("previous_value"), PreviousValue);
	Root->SetStringField(TEXT("actual_value"), StaticSwitchPin->DefaultValue);
	Root->SetBoolField(TEXT("used_schema_setter"), bUsedSchemaSetter);
	Root->SetBoolField(TEXT("hidden"), HiddenStaticSwitchPins.Contains(StaticSwitchPin));
	Root->SetBoolField(TEXT("reexport_required"), true);
	Root->SetStringField(TEXT("next_step"), TEXT("Re-export the Niagara system, then call compile_status with force=true and wait=true."));
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::SetModuleObjectInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& AssetPath, FString& OutJson, FString& OutError)
{
	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	UClass* DataObjectType = MatchedInput->GetType().GetClass();
	if (!DataObjectType)
	{
		OutError = FString::Printf(TEXT("Input '%s' is not an object/data-interface input."), *InputName);
		return false;
	}

	UObject* ObjectAsset = FSoftObjectPath(AssetPath).TryLoad();
	if (!ObjectAsset)
	{
		OutError = FString::Printf(TEXT("Failed to load object asset '%s'."), *AssetPath);
		return false;
	}

	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'."), *SessionId);
		return false;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Set Niagara Module Object Input")));
	System->Modify();
	Module->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}

	const FNiagaraParameterHandle ModuleHandle(MatchedInput->GetName());
	const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Module,
		AliasedHandle,
		MatchedInput->GetType(),
		FGuid(),
		FGuid());

	OverridePin.Modify();
	if (OverridePin.LinkedTo.Num() > 0)
	{
		TArray<UEdGraphNode*> OldLinkedNodes;
		for (UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				OldLinkedNodes.AddUnique(LinkedPin->GetOwningNode());
			}
		}

		OverridePin.BreakAllPinLinks();
		for (UEdGraphNode* OldLinkedNode : OldLinkedNodes)
		{
			if (OldLinkedNode && OldLinkedNode->GetGraph() == Module->GetGraph())
			{
				OldLinkedNode->Modify();
				OldLinkedNode->DestroyNode();
			}
		}
	}

	FNiagaraStackGraphUtilities::SetObjectAssetValueForFunctionInput(
		OverridePin,
		DataObjectType,
		AliasedHandle.GetParameterHandleString().ToString(),
		ObjectAsset,
		FGuid());

	for (UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
	{
		UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
		if (!InputNode)
		{
			continue;
		}

		UObject* DataInterface = nullptr;
		if (const FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface")))
		{
			DataInterface = DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode);
		}

		if (UNiagaraDataInterfaceVolumeTexture* VolumeTextureDI = Cast<UNiagaraDataInterfaceVolumeTexture>(DataInterface))
		{
			VolumeTextureDI->Modify();
			VolumeTextureDI->SetTexture(Cast<UTexture>(ObjectAsset));
		}
	}

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetStringField(TEXT("asset"), AssetPath);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::BindModuleInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'."), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	if (MatchedInput->GetType().IsDataInterface() || MatchedInput->GetType().IsUObject())
	{
		OutError = FString::Printf(TEXT("Input '%s' is an object/data-interface input. Use the object-specific binding tool instead."), *InputName);
		return false;
	}

	const FString UserParameterName = NormalizeUserParameterName(UserParameter.IsEmpty() ? InputName : UserParameter);
	const FNiagaraVariable UserVariable(MatchedInput->GetType(), FName(*UserParameterName));

	const FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Bind Niagara Module Input To User Parameter")));
	System->Modify();
	Module->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}
	FNiagaraUserRedirectionParameterStore& UserParameters = System->GetExposedParameters();
	if (UserParameters.IndexOf(UserVariable) == INDEX_NONE)
	{
		const bool bInitialize = true;
		const bool bTriggerRebind = true;
		UserParameters.AddParameter(UserVariable, bInitialize, bTriggerRebind);
	}

	const FNiagaraParameterHandle ModuleHandle(MatchedInput->GetName());
	const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Module,
		AliasedHandle,
		MatchedInput->GetType(),
		FGuid(),
		FGuid());

	OverridePin.Modify();
	if (OverridePin.LinkedTo.Num() > 0)
	{
		TArray<UEdGraphNode*> OldLinkedNodes;
		for (UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				OldLinkedNodes.AddUnique(LinkedPin->GetOwningNode());
			}
		}

		OverridePin.BreakAllPinLinks();
		for (UEdGraphNode* OldLinkedNode : OldLinkedNodes)
		{
			if (OldLinkedNode && OldLinkedNode->GetGraph() == Module->GetGraph())
			{
				OldLinkedNode->Modify();
				OldLinkedNode->DestroyNode();
			}
		}
	}

	TSet<FNiagaraVariableBase> KnownParameters;
	KnownParameters.Add(UserVariable);
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, UserVariable, KnownParameters);

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetStringField(TEXT("type"), MatchedInput->GetType().GetName());
	Root->SetStringField(TEXT("user_parameter"), UserParameterName);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::BindSkeletalMeshInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, const FString& DefaultAssetPath, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'."), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	UClass* DataObjectType = MatchedInput->GetType().GetClass();
	if (!DataObjectType || !DataObjectType->IsChildOf(UNiagaraDataInterfaceSkeletalMesh::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Input '%s' is not a Niagara Skeletal Mesh data-interface input."), *InputName);
		return false;
	}

	USkeletalMesh* DefaultMesh = nullptr;
	if (!DefaultAssetPath.TrimStartAndEnd().IsEmpty())
	{
		DefaultMesh = Cast<USkeletalMesh>(FSoftObjectPath(DefaultAssetPath).TryLoad());
		if (!DefaultMesh)
		{
			OutError = FString::Printf(TEXT("Default asset '%s' is not a skeletal mesh."), *DefaultAssetPath);
			return false;
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Bind Niagara Skeletal Mesh Input To User Parameter")));
	System->Modify();
	Module->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}

	const FNiagaraParameterHandle ModuleHandle(MatchedInput->GetName());
	const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Module,
		AliasedHandle,
		MatchedInput->GetType(),
		FGuid(),
		FGuid());
	OverridePin.Modify();

	UNiagaraDataInterfaceSkeletalMesh* SkeletalMeshDI = FindLinkedSkeletalMeshDI(OverridePin);
	if (!SkeletalMeshDI)
	{
		UNiagaraDataInterface* CreatedDI = nullptr;
		FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
			OverridePin,
			DataObjectType,
			AliasedHandle.GetParameterHandleString().ToString(),
			CreatedDI,
			FGuid());
		SkeletalMeshDI = Cast<UNiagaraDataInterfaceSkeletalMesh>(CreatedDI);
	}

	if (!SkeletalMeshDI)
	{
		Transaction.Cancel();
		OutError = FString::Printf(TEXT("Failed to create or find Skeletal Mesh data interface for input '%s'."), *InputName);
		return false;
	}

	const FString UserParameterName = NormalizeUserParameterName(UserParameter);
	const FNiagaraVariable UserVariable(MatchedInput->GetType(), FName(*UserParameterName));

	FNiagaraUserRedirectionParameterStore& UserParameters = System->GetExposedParameters();
	int32 DataInterfaceOffset = UserParameters.IndexOf(UserVariable);
	if (DataInterfaceOffset == INDEX_NONE)
	{
		const bool bInitialize = true;
		const bool bTriggerRebind = true;
		UserParameters.AddParameter(UserVariable, bInitialize, bTriggerRebind, &DataInterfaceOffset);
		if (DataInterfaceOffset != INDEX_NONE)
		{
			SkeletalMeshDI->CopyTo(UserParameters.GetDataInterface(DataInterfaceOffset));
		}
	}

	UNiagaraDataInterfaceSkeletalMesh* UserSkeletalMeshDI =
		DataInterfaceOffset != INDEX_NONE
			? Cast<UNiagaraDataInterfaceSkeletalMesh>(UserParameters.GetDataInterface(DataInterfaceOffset))
			: nullptr;

	SkeletalMeshDI->Modify();
	SkeletalMeshDI->MeshUserParameter.Parameter = UserVariable;
	if (DefaultMesh)
	{
		SkeletalMeshDI->DefaultMesh = DefaultMesh;
	}

	if (UserSkeletalMeshDI)
	{
		UserSkeletalMeshDI->Modify();
		UserSkeletalMeshDI->MeshUserParameter.Parameter = UserVariable;
		if (DefaultMesh)
		{
			UserSkeletalMeshDI->DefaultMesh = DefaultMesh;
		}
	}

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);
	UserParameters.OnInterfaceChange();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetStringField(TEXT("user_parameter"), UserParameterName);
	Root->SetStringField(TEXT("default_asset"), DefaultMesh ? DefaultMesh->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("mesh_user_parameter"), SkeletalMeshDI->MeshUserParameter.Parameter.GetName().ToString());
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraModuleService::BindVolumeTextureInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, const FString& DefaultAssetPath, FString& OutJson, FString& OutError)
{
	UNiagaraSystem* System = ResolveSystem(SessionId);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Invalid Niagara session '%s'."), *SessionId);
		return false;
	}

	UNiagaraNodeFunctionCall* Module = ResolveModule(SessionId, ModuleAlias);
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	if (!GetModuleInputs(Module, Inputs, HiddenInputs, OutError))
	{
		return false;
	}

	const FNiagaraVariable* MatchedInput = nullptr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		if (MatchesInputName(Input, InputName))
		{
			MatchedInput = &Input;
			break;
		}
	}

	if (!MatchedInput)
	{
		OutError = FString::Printf(TEXT("Module '%s' has no input named '%s'."), *ModuleAlias, *InputName);
		return false;
	}

	UClass* DataObjectType = MatchedInput->GetType().GetClass();
	if (!DataObjectType || !DataObjectType->IsChildOf(UNiagaraDataInterfaceVolumeTexture::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Input '%s' is not a Niagara Volume Texture data-interface input."), *InputName);
		return false;
	}

	UObject* ObjectAsset = nullptr;
	if (!DefaultAssetPath.TrimStartAndEnd().IsEmpty())
	{
		ObjectAsset = FSoftObjectPath(DefaultAssetPath).TryLoad();
		if (!ObjectAsset)
		{
			OutError = FString::Printf(TEXT("Failed to load default texture asset '%s'."), *DefaultAssetPath);
			return false;
		}
		if (!ObjectAsset->IsA<UTexture>())
		{
			OutError = FString::Printf(TEXT("Default asset '%s' is not a texture."), *DefaultAssetPath);
			return false;
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("ToolPlayMCP: Bind Niagara Volume Texture Input To User Parameter")));
	System->Modify();
	Module->Modify();
	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
	}

	const FNiagaraParameterHandle ModuleHandle(MatchedInput->GetName());
	const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, Module);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Module,
		AliasedHandle,
		MatchedInput->GetType(),
		FGuid(),
		FGuid());
	OverridePin.Modify();

	UNiagaraDataInterfaceVolumeTexture* VolumeTextureDI = FindLinkedVolumeTextureDI(OverridePin);
	if (!VolumeTextureDI)
	{
		FNiagaraStackGraphUtilities::SetObjectAssetValueForFunctionInput(
			OverridePin,
			DataObjectType,
			AliasedHandle.GetParameterHandleString().ToString(),
			ObjectAsset,
			FGuid());
		VolumeTextureDI = FindLinkedVolumeTextureDI(OverridePin);
	}

	if (!VolumeTextureDI)
	{
		Transaction.Cancel();
		OutError = FString::Printf(TEXT("Failed to create or find Volume Texture data interface for input '%s'."), *InputName);
		return false;
	}

	const FString UserParameterName = NormalizeUserParameterName(UserParameter);
	const FNiagaraVariable UserTextureVariable(FNiagaraTypeDefinition::GetUTextureDef(), FName(*UserParameterName));

	FNiagaraUserRedirectionParameterStore& UserParameters = System->GetExposedParameters();
	int32 ParameterOffset = UserParameters.IndexOf(UserTextureVariable);
	if (ParameterOffset == INDEX_NONE)
	{
		const bool bInitialize = true;
		const bool bTriggerRebind = true;
		UserParameters.AddParameter(UserTextureVariable, bInitialize, bTriggerRebind, &ParameterOffset);
	}
	if (ObjectAsset && ParameterOffset != INDEX_NONE)
	{
		UserParameters.SetUObject(ObjectAsset, ParameterOffset);
	}

	VolumeTextureDI->Modify();
	VolumeTextureDI->TextureUserParameter.Parameter = UserTextureVariable;
	if (ObjectAsset)
	{
		VolumeTextureDI->SetTexture(Cast<UTexture>(ObjectAsset));
	}

	if (UEdGraph* Graph = Module->GetGraph())
	{
		Graph->Modify();
		Graph->MarkPackageDirty();
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("module"), ModuleAlias);
	Root->SetStringField(TEXT("input"), InputName);
	Root->SetStringField(TEXT("full_input"), MatchedInput->GetName().ToString());
	Root->SetStringField(TEXT("user_parameter"), UserParameterName);
	Root->SetStringField(TEXT("default_asset"), ObjectAsset ? ObjectAsset->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("volume_texture"), VolumeTextureDI->Texture ? VolumeTextureDI->Texture->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("texture_user_parameter"), VolumeTextureDI->TextureUserParameter.Parameter.GetName().ToString());
	OutJson = ToJsonString(Root);
	return true;
}


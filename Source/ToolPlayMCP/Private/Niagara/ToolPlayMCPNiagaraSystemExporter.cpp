#include "Niagara/ToolPlayMCPNiagaraSystemExporter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/EngineTypes.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraShared.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Niagara/ToolPlayMCPNiagaraModuleService.h"
#include "UObject/SoftObjectPath.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

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

	FString CompileStatusToString(ENiagaraScriptCompileStatus Status)
	{
		return EnumValueToString(TEXT("/Script/Niagara.ENiagaraScriptCompileStatus"), static_cast<int64>(Status));
	}

	FString CompileEventSeverityToString(FNiagaraCompileEventSeverity Severity)
	{
		return EnumValueToString(TEXT("/Script/NiagaraShader.FNiagaraCompileEventSeverity"), static_cast<int64>(Severity));
	}

	int32 CompileStatusSeverity(ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Error:
			return 4;
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
			return 3;
		case ENiagaraScriptCompileStatus::NCS_Dirty:
		case ENiagaraScriptCompileStatus::NCS_Unknown:
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			return 2;
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			return 1;
		default:
			return 0;
		}
	}

	bool IsWarningStatus(ENiagaraScriptCompileStatus Status)
	{
		return Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings ||
			Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
	}

	bool IsBlockingStatus(ENiagaraScriptCompileStatus Status)
	{
		return Status == ENiagaraScriptCompileStatus::NCS_Unknown ||
			Status == ENiagaraScriptCompileStatus::NCS_Dirty ||
			Status == ENiagaraScriptCompileStatus::NCS_BeingCreated;
	}

	bool IsEmitterLifecycleUsage(ENiagaraScriptUsage Usage)
	{
		return Usage == ENiagaraScriptUsage::EmitterSpawnScript ||
			Usage == ENiagaraScriptUsage::EmitterUpdateScript;
	}

	FString SimTargetToString(ENiagaraSimTarget SimTarget)
	{
		return EnumValueToString(TEXT("/Script/Niagara.ENiagaraSimTarget"), static_cast<int64>(SimTarget));
	}

	FString VariableToString(const FNiagaraVariable& Variable)
	{
		return FString::Printf(TEXT("%s:%s"), *Variable.GetName().ToString(), *Variable.GetType().GetName());
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	void AddParameterStoreVariables(const FNiagaraParameterStore& Store, const FString& FieldName, const TSharedRef<FJsonObject>& Object)
	{
		TArray<FNiagaraVariable> Variables;
		Store.GetParameters(Variables);

		TArray<FString> Names;
		for (const FNiagaraVariable& Variable : Variables)
		{
			Names.Add(VariableToString(Variable));
		}
		Names.Sort();
		Object->SetArrayField(FieldName, StringArrayToJson(Names));
	}

	void SetScriptField(const TSharedRef<FJsonObject>& Parent, const FString& Name, UNiagaraScript* Script, ENiagaraScriptUsage Usage, const FString& AliasPrefix, const FString& SessionId)
	{
		Parent->SetObjectField(Name, FToolPlayMCPNiagaraModuleService::ExportScriptStack(Script, Usage, Name, AliasPrefix, SessionId));
	}

	TSharedRef<FJsonObject> BuildCompileStatusObject(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile);

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

	UNiagaraNodeOutput* FindOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid())
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

	FString FunctionName(const UNiagaraNodeFunctionCall* FunctionCall)
	{
		if (!FunctionCall)
		{
			return FString();
		}

		if (!FunctionCall->GetFunctionName().IsEmpty())
		{
			return FunctionCall->GetFunctionName();
		}
		return FunctionCall->FunctionScript ? FunctionCall->FunctionScript->GetName() : FunctionCall->GetName();
	}

	ENiagaraModuleDependencyUsage ConvertScriptUsageToDependencyUsage(ENiagaraScriptUsage ScriptUsage)
	{
		if (ScriptUsage == ENiagaraScriptUsage::ParticleEventScript)
		{
			return ENiagaraModuleDependencyUsage::Event;
		}
		if (ScriptUsage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			return ENiagaraModuleDependencyUsage::SimulationStage;
		}
		if (ScriptUsage == ENiagaraScriptUsage::EmitterSpawnScript || ScriptUsage == ENiagaraScriptUsage::SystemSpawnScript || ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated || ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScript)
		{
			return ENiagaraModuleDependencyUsage::Spawn;
		}
		if (ScriptUsage == ENiagaraScriptUsage::EmitterUpdateScript || ScriptUsage == ENiagaraScriptUsage::SystemUpdateScript || ScriptUsage == ENiagaraScriptUsage::ParticleUpdateScript)
		{
			return ENiagaraModuleDependencyUsage::Update;
		}
		return ENiagaraModuleDependencyUsage::None;
	}

	bool IsUsageAllowed(ENiagaraScriptUsage ModuleUsage, int32 AllowedUsageBitmask)
	{
		const ENiagaraModuleDependencyUsage Usage = ConvertScriptUsageToDependencyUsage(ModuleUsage);
		return (AllowedUsageBitmask & (1 << static_cast<int32>(Usage))) != 0;
	}

	FString DependencyTypeToString(ENiagaraModuleDependencyType Type)
	{
		return Type == ENiagaraModuleDependencyType::PreDependency ? TEXT("pre") : TEXT("post");
	}

	FString DependencyConstraintToString(ENiagaraModuleDependencyScriptConstraint Constraint)
	{
		return Constraint == ENiagaraModuleDependencyScriptConstraint::SameScript ? TEXT("same_script") : TEXT("all_scripts");
	}

	TSharedRef<FJsonObject> ExportModuleDependencyData(const FNiagaraStackModuleData& ModuleData)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		UNiagaraNodeFunctionCall* Module = ModuleData.WeakModuleNode.Get();
		Object->SetStringField(TEXT("name"), FunctionName(Module));
		Object->SetStringField(TEXT("usage"), ScriptUsageToString(ModuleData.Usage));
		Object->SetNumberField(TEXT("index"), ModuleData.Index);
		Object->SetBoolField(TEXT("enabled"), Module ? Module->GetDesiredEnabledState() == ENodeEnabledState::Enabled : false);
		Object->SetStringField(TEXT("script"), Module && Module->FunctionScript ? Module->FunctionScript->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> Provides;
		if (Module && Module->GetScriptData())
		{
			for (const FName& Provided : Module->GetScriptData()->ProvidedDependencies)
			{
				Provides.Add(MakeShared<FJsonValueString>(Provided.ToString()));
			}
		}
		Object->SetArrayField(TEXT("provides"), Provides);
		return Object;
	}

	void AppendScriptStackModuleData(
		UNiagaraScript* Script,
		ENiagaraScriptUsage Usage,
		const FGuid& EmitterHandleId,
		TArray<FNiagaraStackModuleData>& InOutStackData,
		TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*>& InOutModuleOutputs)
	{
		UNiagaraGraph* Graph = GetGraphFromScript(Script);
		UNiagaraNodeOutput* OutputNode = FindOutputNode(Graph, Usage);
		if (!Graph || !OutputNode)
		{
			return;
		}

		TArray<UNiagaraNode*> TraversedNodes;
		Graph->BuildTraversal(TraversedNodes, Usage, OutputNode->GetUsageId(), false);
		for (UNiagaraNode* Node : TraversedNodes)
		{
			UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(Node);
			if (!Module || !Module->FunctionScript || Module->FunctionScript->GetUsage() != ENiagaraScriptUsage::Module)
			{
				continue;
			}

			FNiagaraStackModuleData ModuleData;
			ModuleData.WeakModuleNode = Module;
			ModuleData.Usage = Usage;
			ModuleData.UsageId = OutputNode->GetUsageId();
			ModuleData.Index = InOutStackData.Num();
			ModuleData.EmitterHandleId = EmitterHandleId;
			InOutStackData.Add(ModuleData);
			InOutModuleOutputs.Add(Module, OutputNode);
		}
	}

	bool DoesStackModuleProvideDependencyLocal(
		const FNiagaraStackModuleData& StackModuleData,
		const FNiagaraModuleDependency& SourceModuleRequiredDependency,
		const UNiagaraNodeOutput& SourceOutputNode,
		const TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*>& ModuleOutputs)
	{
		UNiagaraNodeFunctionCall* ModuleNode = StackModuleData.WeakModuleNode.Get();
		if (!ModuleNode || !ModuleNode->FunctionScript)
		{
			return false;
		}

		FVersionedNiagaraScriptData* ScriptData = ModuleNode->FunctionScript->GetScriptData(ModuleNode->SelectedScriptVersion);
		if (!ScriptData || !ScriptData->ProvidedDependencies.Contains(SourceModuleRequiredDependency.Id))
		{
			return false;
		}

		if (SourceModuleRequiredDependency.ScriptConstraint == ENiagaraModuleDependencyScriptConstraint::AllScripts)
		{
			return true;
		}

		if (SourceModuleRequiredDependency.ScriptConstraint == ENiagaraModuleDependencyScriptConstraint::SameScript)
		{
			UNiagaraNodeOutput* OutputNode = ModuleOutputs.FindRef(ModuleNode);
			return OutputNode != nullptr &&
				UNiagaraScript::IsEquivalentUsage(OutputNode->GetUsage(), SourceOutputNode.GetUsage()) &&
				OutputNode->GetUsageId() == SourceOutputNode.GetUsageId();
		}
		return false;
	}

	void CollectImplicitSystemDependencies(const TArray<FNiagaraStackModuleData>& SystemStackData, TSet<FName>& OutImplicitDependencies)
	{
		for (const FNiagaraStackModuleData& ModuleData : SystemStackData)
		{
			const UNiagaraNodeFunctionCall* Module = ModuleData.WeakModuleNode.Get();
			if (!Module || !Module->FunctionScript)
			{
				continue;
			}

			if (Module->FunctionScript->GetPathName() == TEXT("/Niagara/Modules/System/SystemState.SystemState") || FunctionName(Module).Equals(TEXT("SystemState"), ESearchCase::IgnoreCase))
			{
				OutImplicitDependencies.Add(TEXT("SystemState"));
			}
		}
	}

	void AddDependencyIssueForModule(
		const FNiagaraStackModuleData& SourceModuleData,
		const TArray<FNiagaraStackModuleData>& StackData,
		const TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*>& ModuleOutputs,
		const TSet<FName>& ImplicitDependencies,
		TArray<TSharedPtr<FJsonValue>>& InOutIssues)
	{
		UNiagaraNodeFunctionCall* SourceModule = SourceModuleData.WeakModuleNode.Get();
		UNiagaraNodeOutput* SourceOutput = SourceModule ? ModuleOutputs.FindRef(SourceModule) : nullptr;
		FVersionedNiagaraScriptData* SourceScriptData = SourceModule ? SourceModule->GetScriptData() : nullptr;
		if (!SourceModule || !SourceOutput || !SourceScriptData || SourceScriptData->RequiredDependencies.Num() == 0)
		{
			return;
		}

		const int32 ModuleIndex = StackData.IndexOfByPredicate([SourceModule](const FNiagaraStackModuleData& ModuleData)
		{
			return ModuleData.WeakModuleNode.Get() == SourceModule;
		});
		if (ModuleIndex == INDEX_NONE)
		{
			return;
		}

		const TArray<ENiagaraScriptUsage> SupportedUsages = UNiagaraScript::GetSupportedUsageContextsForBitmask(SourceScriptData->ModuleUsageBitmask);
		for (const FNiagaraModuleDependency& RequiredDependency : SourceScriptData->RequiredDependencies)
		{
			if (!IsUsageAllowed(SourceOutput->GetUsage(), RequiredDependency.OnlyEvaluateInScriptUsage))
			{
				continue;
			}

			const bool bImplicitDependencyProviderFound =
				RequiredDependency.ScriptConstraint == ENiagaraModuleDependencyScriptConstraint::AllScripts &&
				ImplicitDependencies.Contains(RequiredDependency.Id);

			bool bDependencyProviderFound = false;
			TArray<TSharedPtr<FJsonValue>> ProviderCandidates;
			for (int32 CandidateIndex = 0; CandidateIndex < StackData.Num(); ++CandidateIndex)
			{
				const FNiagaraStackModuleData& CandidateData = StackData[CandidateIndex];
				if (!DoesStackModuleProvideDependencyLocal(CandidateData, RequiredDependency, *SourceOutput, ModuleOutputs))
				{
					continue;
				}

				UNiagaraNodeFunctionCall* CandidateModule = CandidateData.WeakModuleNode.Get();
				const bool bCorrectOrder =
					(RequiredDependency.Type == ENiagaraModuleDependencyType::PreDependency && CandidateIndex < ModuleIndex) ||
					(RequiredDependency.Type == ENiagaraModuleDependencyType::PostDependency && CandidateIndex > ModuleIndex);
				const bool bEnabled = CandidateModule && CandidateModule->GetDesiredEnabledState() == ENodeEnabledState::Enabled;
				const bool bUsageIsSupported = UNiagaraScript::ContainsEquivilentUsage(SupportedUsages, CandidateData.Usage);
				const bool bCorrectVersion = CandidateModule && CandidateModule->GetScriptData()
					? RequiredDependency.IsVersionAllowed(CandidateModule->GetScriptData()->Version)
					: false;

				TSharedRef<FJsonObject> Candidate = ExportModuleDependencyData(CandidateData);
				Candidate->SetBoolField(TEXT("correct_order"), bCorrectOrder);
				Candidate->SetBoolField(TEXT("enabled"), bEnabled);
				Candidate->SetBoolField(TEXT("correct_version"), bCorrectVersion);
				Candidate->SetBoolField(TEXT("usage_supported_for_move_fix"), bUsageIsSupported);
				ProviderCandidates.Add(MakeShared<FJsonValueObject>(Candidate));

				if (bCorrectOrder && bEnabled && bCorrectVersion)
				{
					bDependencyProviderFound = true;
				}
			}

			if (bImplicitDependencyProviderFound)
			{
				bDependencyProviderFound = true;
			}

			if (!bDependencyProviderFound)
			{
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), TEXT("error"));
				Issue->SetStringField(TEXT("summary"), TEXT("The module has unmet dependencies."));
				Issue->SetStringField(TEXT("module"), FunctionName(SourceModule));
				Issue->SetStringField(TEXT("module_script"), SourceModule->FunctionScript ? SourceModule->FunctionScript->GetPathName() : FString());
				Issue->SetNumberField(TEXT("module_index"), ModuleIndex);
				Issue->SetStringField(TEXT("usage"), ScriptUsageToString(SourceModuleData.Usage));
				Issue->SetStringField(TEXT("dependency_id"), RequiredDependency.Id.ToString());
				Issue->SetStringField(TEXT("dependency_type"), DependencyTypeToString(RequiredDependency.Type));
				Issue->SetStringField(TEXT("script_constraint"), DependencyConstraintToString(RequiredDependency.ScriptConstraint));
				Issue->SetStringField(TEXT("required_version"), RequiredDependency.RequiredVersion);
				Issue->SetStringField(TEXT("description"), RequiredDependency.Description.ToString());
				Issue->SetArrayField(TEXT("provider_candidates"), ProviderCandidates);
				Issue->SetStringField(TEXT("likely_fix"), ProviderCandidates.Num() == 0
					? TEXT("Add a module that provides this dependency in the required stack/usage.")
					: TEXT("Move, enable, or switch version of a provider candidate so it satisfies order/enabled/version constraints."));
				InOutIssues.Add(MakeShared<FJsonValueObject>(Issue));
			}
		}
	}

	TSharedRef<FJsonObject> BuildStackIssuesObject(UNiagaraSystem* System)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset"), System ? System->GetPathName() : FString());
		Root->SetStringField(TEXT("name"), System ? System->GetName() : FString());
		TArray<TSharedPtr<FJsonValue>> Issues;

		if (!System)
		{
			Root->SetBoolField(TEXT("ok"), false);
			Root->SetArrayField(TEXT("issues"), Issues);
			return Root;
		}

		TArray<FNiagaraStackModuleData> SystemStackData;
		TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*> SystemModuleOutputs;
		TSet<FName> NoImplicitDependencies;
		TSet<FName> SystemImplicitDependencies;
		AppendScriptStackModuleData(System->GetSystemSpawnScript(), ENiagaraScriptUsage::SystemSpawnScript, FGuid(), SystemStackData, SystemModuleOutputs);
		AppendScriptStackModuleData(System->GetSystemUpdateScript(), ENiagaraScriptUsage::SystemUpdateScript, FGuid(), SystemStackData, SystemModuleOutputs);
		CollectImplicitSystemDependencies(SystemStackData, SystemImplicitDependencies);
		for (const FNiagaraStackModuleData& ModuleData : SystemStackData)
		{
			AddDependencyIssueForModule(ModuleData, SystemStackData, SystemModuleOutputs, NoImplicitDependencies, Issues);
		}

		int32 EmitterIndex = 0;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (!EmitterData)
			{
				EmitterIndex++;
				continue;
			}

			TArray<FNiagaraStackModuleData> EmitterStackData;
			TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*> EmitterModuleOutputs;
			AppendScriptStackModuleData(EmitterData->EmitterSpawnScriptProps.Script, ENiagaraScriptUsage::EmitterSpawnScript, Handle.GetId(), EmitterStackData, EmitterModuleOutputs);
			AppendScriptStackModuleData(EmitterData->EmitterUpdateScriptProps.Script, ENiagaraScriptUsage::EmitterUpdateScript, Handle.GetId(), EmitterStackData, EmitterModuleOutputs);
			AppendScriptStackModuleData(EmitterData->SpawnScriptProps.Script, ENiagaraScriptUsage::ParticleSpawnScript, Handle.GetId(), EmitterStackData, EmitterModuleOutputs);
			AppendScriptStackModuleData(EmitterData->UpdateScriptProps.Script, ENiagaraScriptUsage::ParticleUpdateScript, Handle.GetId(), EmitterStackData, EmitterModuleOutputs);

			TArray<FNiagaraStackModuleData> CandidateStackData = SystemStackData;
			TMap<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*> CandidateModuleOutputs = SystemModuleOutputs;
			for (FNiagaraStackModuleData ModuleData : EmitterStackData)
			{
				ModuleData.Index = CandidateStackData.Num();
				CandidateStackData.Add(ModuleData);
			}
			for (const TPair<UNiagaraNodeFunctionCall*, UNiagaraNodeOutput*>& Pair : EmitterModuleOutputs)
			{
				CandidateModuleOutputs.Add(Pair.Key, Pair.Value);
			}

			TArray<TSharedPtr<FJsonValue>> EmitterIssues;
			for (const FNiagaraStackModuleData& ModuleData : EmitterStackData)
			{
				AddDependencyIssueForModule(ModuleData, CandidateStackData, CandidateModuleOutputs, SystemImplicitDependencies, EmitterIssues);
			}

			for (const TSharedPtr<FJsonValue>& IssueValue : EmitterIssues)
			{
				if (IssueValue.IsValid() && IssueValue->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> Issue = IssueValue->AsObject();
					Issue->SetStringField(TEXT("emitter"), FString::Printf(TEXT("e%d"), EmitterIndex));
					Issue->SetStringField(TEXT("emitter_name"), Handle.GetName().ToString());
				}
				Issues.Add(IssueValue);
			}
			EmitterIndex++;
		}

		Root->SetBoolField(TEXT("ok"), true);
		Root->SetBoolField(TEXT("has_issues"), Issues.Num() > 0);
		Root->SetNumberField(TEXT("issue_count"), Issues.Num());
		Root->SetArrayField(TEXT("issues"), Issues);
		Root->SetStringField(TEXT("note"), TEXT("These are Niagara stack/module dependency issues. They can exist even when VM scripts report successfully compiled."));
		return Root;
	}

	void AddBlockingReason(TArray<TSharedPtr<FJsonValue>>& Reasons, const FString& Reason)
	{
		if (!Reason.IsEmpty())
		{
			Reasons.Add(MakeShared<FJsonValueString>(Reason));
		}
	}

	TSharedRef<FJsonObject> BuildDiagnosticsSummary(const TSharedRef<FJsonObject>& Compile, const TSharedRef<FJsonObject>& Stack)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		const bool bCompileReady = Compile->GetBoolField(TEXT("compile_ready"));
		const bool bCompileHasErrors = Compile->GetBoolField(TEXT("has_errors"));
		const bool bCompileHasWarnings = Compile->GetBoolField(TEXT("has_warnings"));
		const bool bHasBlockingStatus = Compile->GetBoolField(TEXT("has_blocking_status"));
		const bool bStackHasIssues = Stack->GetBoolField(TEXT("has_issues"));
		const int32 CompileErrorCount = static_cast<int32>(Compile->GetNumberField(TEXT("error_count")));
		const int32 CompileWarningCount = static_cast<int32>(Compile->GetNumberField(TEXT("warning_count")));
		const int32 StackIssueCount = static_cast<int32>(Stack->GetNumberField(TEXT("issue_count")));

		TArray<TSharedPtr<FJsonValue>> BlockingReasons;
		if (!bCompileReady)
		{
			AddBlockingReason(BlockingReasons, TEXT("Compile diagnostics are not ready/clean."));
		}
		if (bCompileHasErrors)
		{
			AddBlockingReason(BlockingReasons, FString::Printf(TEXT("Compile diagnostics report %d error(s)."), CompileErrorCount));
		}
		if (bHasBlockingStatus)
		{
			AddBlockingReason(BlockingReasons, TEXT("At least one script has a blocking compile status such as NCS_Unknown, NCS_Dirty, or NCS_BeingCreated."));
		}
		if (bStackHasIssues)
		{
			AddBlockingReason(BlockingReasons, FString::Printf(TEXT("Stack diagnostics report %d issue(s), such as unmet module dependencies."), StackIssueCount));
		}

		Summary->SetBoolField(TEXT("ok_to_save_or_claim_success"), BlockingReasons.Num() == 0);
		Summary->SetBoolField(TEXT("has_errors"), bCompileHasErrors || bStackHasIssues || !bCompileReady);
		Summary->SetBoolField(TEXT("has_warnings"), bCompileHasWarnings);
		Summary->SetBoolField(TEXT("compile_ready"), bCompileReady);
		Summary->SetBoolField(TEXT("compile_has_errors"), bCompileHasErrors);
		Summary->SetBoolField(TEXT("compile_has_blocking_status"), bHasBlockingStatus);
		Summary->SetBoolField(TEXT("stack_has_issues"), bStackHasIssues);
		Summary->SetNumberField(TEXT("compile_error_count"), CompileErrorCount);
		Summary->SetNumberField(TEXT("compile_warning_count"), CompileWarningCount);
		Summary->SetNumberField(TEXT("stack_issue_count"), StackIssueCount);
		Summary->SetArrayField(TEXT("blocking_reasons"), BlockingReasons);
		Summary->SetStringField(TEXT("guidance"), BlockingReasons.Num() == 0
			? TEXT("Compile and stack diagnostics look clean.")
			: TEXT("Fix compile and/or stack diagnostics before saving or claiming the Niagara system is valid."));
		return Summary;
	}

	TSharedRef<FJsonObject> BuildDiagnosticsObject(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Compile = BuildCompileStatusObject(System, bForceCompile, bWaitForCompile);
		TSharedRef<FJsonObject> Stack = BuildStackIssuesObject(System);
		Root->SetStringField(TEXT("asset"), System ? System->GetPathName() : FString());
		Root->SetStringField(TEXT("name"), System ? System->GetName() : FString());
		Root->SetObjectField(TEXT("summary"), BuildDiagnosticsSummary(Compile, Stack));
		Root->SetObjectField(TEXT("compile"), Compile);
		Root->SetObjectField(TEXT("stack"), Stack);
		Root->SetStringField(TEXT("note"), TEXT("Use this combined diagnostics result after Niagara edits. Compile and stack issues are separate layers and can disagree."));
		return Root;
	}

	void AddScriptCompileRecord(
		TArray<TSharedPtr<FJsonValue>>& Records,
		const FString& Scope,
		const FString& EmitterAlias,
		const FString& EmitterName,
		const FString& Stage,
		UNiagaraScript* Script,
		ENiagaraScriptUsage Usage,
		ENiagaraScriptCompileStatus& InOutWorstStatus,
		int32& InOutErrorCount,
		int32& InOutWarningCount)
	{
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("scope"), Scope);
		Record->SetStringField(TEXT("emitter"), EmitterAlias);
		Record->SetStringField(TEXT("emitter_name"), EmitterName);
		Record->SetStringField(TEXT("stage"), Stage);
		Record->SetStringField(TEXT("usage"), ScriptUsageToString(Usage));

		if (!Script)
		{
			Record->SetBoolField(TEXT("valid"), false);
			Record->SetStringField(TEXT("status"), TEXT("missing_script"));
			Record->SetArrayField(TEXT("compile_errors"), TArray<TSharedPtr<FJsonValue>>());
			Records.Add(MakeShared<FJsonValueObject>(Record));
			return;
		}

		Record->SetBoolField(TEXT("valid"), true);
		Record->SetStringField(TEXT("script"), Script->GetPathName());
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		Record->SetStringField(TEXT("status"), CompileStatusToString(Status));

		const FNiagaraVMExecutableData& ExecutableData = Script->GetVMExecutableData();
		TArray<FString> CompileErrors;
		TArray<FString> ShaderCompileErrors;
		TArray<TSharedPtr<FJsonValue>> CompileMessages;
		for (const FNiagaraCompileEvent& Event : ExecutableData.LastCompileEvents)
		{
			TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
			MessageObject->SetStringField(TEXT("severity"), CompileEventSeverityToString(Event.Severity));
			MessageObject->SetStringField(TEXT("message"), Event.Message);
			MessageObject->SetStringField(TEXT("short_description"), Event.ShortDescription);
			if (Event.NodeGuid.IsValid())
			{
				MessageObject->SetStringField(TEXT("node_guid"), Event.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
			if (Event.PinGuid.IsValid())
			{
				MessageObject->SetStringField(TEXT("pin_guid"), Event.PinGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
			CompileMessages.Add(MakeShared<FJsonValueObject>(MessageObject));

			if (Event.Severity == FNiagaraCompileEventSeverity::Error)
			{
				CompileErrors.Add(Event.Message);
			}
			else if (Event.Severity == FNiagaraCompileEventSeverity::Warning)
			{
				InOutWarningCount++;
			}
		}
		if (!ExecutableData.ErrorMsg.IsEmpty() && !CompileErrors.Contains(ExecutableData.ErrorMsg))
		{
			CompileErrors.Add(ExecutableData.ErrorMsg);
		}
		if (const FNiagaraShaderScript* ShaderScript = Script->GetRenderThreadScript())
		{
			for (const FString& ShaderError : ShaderScript->GetCompileErrors())
			{
				if (!ShaderError.IsEmpty())
				{
					ShaderCompileErrors.AddUnique(ShaderError);
					CompileErrors.AddUnique(ShaderError);
				}
			}
		}
		const bool bBenignUnknownEmitterLifecycle =
			IsEmitterLifecycleUsage(Usage) &&
			Status == ENiagaraScriptCompileStatus::NCS_Unknown &&
			CompileMessages.Num() == 0 &&
			ExecutableData.ErrorMsg.IsEmpty() &&
			ShaderCompileErrors.Num() == 0;
		const bool bSummaryBlockingStatus = IsBlockingStatus(Status) && !bBenignUnknownEmitterLifecycle;
		Record->SetBoolField(TEXT("summary_blocking"), bSummaryBlockingStatus);
		if (bBenignUnknownEmitterLifecycle)
		{
			Record->SetStringField(TEXT("summary_note"), TEXT("Emitter lifecycle script is NCS_Unknown but has no compile messages or executable error; not counted as blocking."));
		}
		if (bSummaryBlockingStatus)
		{
			CompileErrors.Add(FString::Printf(
				TEXT("Compile status is %s. The script is not confirmed compiled; recompile or inspect the Niagara editor message log."),
				*CompileStatusToString(Status)));
		}
		CompileErrors.Sort();
		ShaderCompileErrors.Sort();
		Record->SetArrayField(TEXT("compile_errors"), StringArrayToJson(CompileErrors));
		Record->SetArrayField(TEXT("shader_compile_errors"), StringArrayToJson(ShaderCompileErrors));
		Record->SetArrayField(TEXT("compile_messages"), CompileMessages);
		Record->SetNumberField(TEXT("compile_error_count"), CompileErrors.Num());
		Record->SetNumberField(TEXT("shader_compile_error_count"), ShaderCompileErrors.Num());

		if (Status == ENiagaraScriptCompileStatus::NCS_Error || ShaderCompileErrors.Num() > 0)
		{
			InOutErrorCount += FMath::Max(1, CompileErrors.Num());
		}
		else if (bSummaryBlockingStatus)
		{
			InOutErrorCount++;
		}
		else if (CompileErrors.Num() > 0)
		{
			InOutWarningCount += CompileErrors.Num();
		}
		if (IsWarningStatus(Status))
		{
			InOutWarningCount++;
		}

		if (!bBenignUnknownEmitterLifecycle && CompileStatusSeverity(Status) > CompileStatusSeverity(InOutWorstStatus))
		{
			InOutWorstStatus = Status;
		}

		Records.Add(MakeShared<FJsonValueObject>(Record));
	}

	TSharedRef<FJsonObject> BuildCompileStatusObject(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile)
	{
		if (bForceCompile)
		{
			System->RequestCompile(true);
		}
		if (bWaitForCompile)
		{
			System->WaitForCompilationComplete(true, false);
		}

		TArray<TSharedPtr<FJsonValue>> ScriptRecords;
		ENiagaraScriptCompileStatus WorstStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;

		AddScriptCompileRecord(ScriptRecords, TEXT("system"), TEXT("system"), TEXT("system"), TEXT("system_spawn"), System->GetSystemSpawnScript(), ENiagaraScriptUsage::SystemSpawnScript, WorstStatus, ErrorCount, WarningCount);
		AddScriptCompileRecord(ScriptRecords, TEXT("system"), TEXT("system"), TEXT("system"), TEXT("system_update"), System->GetSystemUpdateScript(), ENiagaraScriptUsage::SystemUpdateScript, WorstStatus, ErrorCount, WarningCount);

		int32 EmitterIndex = 0;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FString EmitterAlias = FString::Printf(TEXT("e%d"), EmitterIndex++);
			const FString EmitterName = Handle.GetName().ToString();
			FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
			if (!EmitterData)
			{
				continue;
			}

			AddScriptCompileRecord(ScriptRecords, TEXT("emitter"), EmitterAlias, EmitterName, TEXT("emitter_spawn"), EmitterData->EmitterSpawnScriptProps.Script, ENiagaraScriptUsage::EmitterSpawnScript, WorstStatus, ErrorCount, WarningCount);
			AddScriptCompileRecord(ScriptRecords, TEXT("emitter"), EmitterAlias, EmitterName, TEXT("emitter_update"), EmitterData->EmitterUpdateScriptProps.Script, ENiagaraScriptUsage::EmitterUpdateScript, WorstStatus, ErrorCount, WarningCount);
			AddScriptCompileRecord(ScriptRecords, TEXT("emitter"), EmitterAlias, EmitterName, TEXT("particle_spawn"), EmitterData->SpawnScriptProps.Script, ENiagaraScriptUsage::ParticleSpawnScript, WorstStatus, ErrorCount, WarningCount);
			AddScriptCompileRecord(ScriptRecords, TEXT("emitter"), EmitterAlias, EmitterName, TEXT("particle_update"), EmitterData->UpdateScriptProps.Script, ENiagaraScriptUsage::ParticleUpdateScript, WorstStatus, ErrorCount, WarningCount);

			int32 EventIndex = 0;
			for (const FNiagaraEventScriptProperties& Event : EmitterData->GetEventHandlers())
			{
				const FString StageName = FString::Printf(TEXT("event_%d:%s"), EventIndex++, *Event.SourceEventName.ToString());
				AddScriptCompileRecord(ScriptRecords, TEXT("event"), EmitterAlias, EmitterName, StageName, Event.Script, ENiagaraScriptUsage::ParticleEventScript, WorstStatus, ErrorCount, WarningCount);
			}

			for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
			{
				if (!Stage)
				{
					continue;
				}
				AddScriptCompileRecord(ScriptRecords, TEXT("simulation_stage"), EmitterAlias, EmitterName, Stage->SimulationStageName.ToString(), Stage->Script, ENiagaraScriptUsage::ParticleSimulationStageScript, WorstStatus, ErrorCount, WarningCount);
			}
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset"), System->GetPathName());
		Root->SetStringField(TEXT("name"), System->GetName());
		Root->SetBoolField(TEXT("forced_compile"), bForceCompile);
		Root->SetBoolField(TEXT("waited"), bWaitForCompile);
		Root->SetBoolField(TEXT("needs_request_compile"), System->NeedsRequestCompile());
		Root->SetStringField(TEXT("status"), CompileStatusToString(WorstStatus));
		Root->SetBoolField(TEXT("has_errors"), ErrorCount > 0);
		Root->SetBoolField(TEXT("has_warnings"), WarningCount > 0);
		Root->SetBoolField(TEXT("compile_ready"), ErrorCount == 0 && !IsBlockingStatus(WorstStatus));
		Root->SetBoolField(TEXT("has_blocking_status"), IsBlockingStatus(WorstStatus));
		Root->SetNumberField(TEXT("error_count"), ErrorCount);
		Root->SetNumberField(TEXT("warning_count"), WarningCount);
		Root->SetArrayField(TEXT("scripts"), ScriptRecords);
		return Root;
	}

	TSharedRef<FJsonObject> ExportRenderer(const UNiagaraRendererProperties* Renderer)
	{
		TSharedRef<FJsonObject> RendererObject = MakeShared<FJsonObject>();
		RendererObject->SetStringField(TEXT("class"), Renderer ? Renderer->GetClass()->GetName() : TEXT("null"));
		RendererObject->SetStringField(TEXT("name"), Renderer ? Renderer->GetName() : TEXT(""));
		return RendererObject;
	}

	TSharedRef<FJsonObject> ExportEmitter(const FNiagaraEmitterHandle& Handle, const FString& EmitterAlias, const FString& SessionId)
	{
		TSharedRef<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
		EmitterObject->SetStringField(TEXT("id"), EmitterAlias);
		EmitterObject->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterObject->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterObject->SetStringField(TEXT("mode"), EnumValueToString(TEXT("/Script/Niagara.ENiagaraEmitterMode"), static_cast<int64>(Handle.GetEmitterMode())));

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			EmitterObject->SetBoolField(TEXT("valid"), false);
			return EmitterObject;
		}

		EmitterObject->SetBoolField(TEXT("valid"), true);
		EmitterObject->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
		EmitterObject->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
		EmitterObject->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));

		TSharedRef<FJsonObject> Stages = MakeShared<FJsonObject>();
		SetScriptField(Stages, TEXT("emitter_spawn"), EmitterData->EmitterSpawnScriptProps.Script, ENiagaraScriptUsage::EmitterSpawnScript, EmitterAlias + TEXT(".emitter_spawn"), SessionId);
		SetScriptField(Stages, TEXT("emitter_update"), EmitterData->EmitterUpdateScriptProps.Script, ENiagaraScriptUsage::EmitterUpdateScript, EmitterAlias + TEXT(".emitter_update"), SessionId);
		SetScriptField(Stages, TEXT("particle_spawn"), EmitterData->SpawnScriptProps.Script, ENiagaraScriptUsage::ParticleSpawnScript, EmitterAlias + TEXT(".particle_spawn"), SessionId);
		SetScriptField(Stages, TEXT("particle_update"), EmitterData->UpdateScriptProps.Script, ENiagaraScriptUsage::ParticleUpdateScript, EmitterAlias + TEXT(".particle_update"), SessionId);
		EmitterObject->SetObjectField(TEXT("stages"), Stages);

		TArray<TSharedPtr<FJsonValue>> Renderers;
		for (const UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
		{
			Renderers.Add(MakeShared<FJsonValueObject>(ExportRenderer(Renderer)));
		}
		EmitterObject->SetArrayField(TEXT("renderers"), Renderers);

		TArray<TSharedPtr<FJsonValue>> EventHandlers;
		for (const FNiagaraEventScriptProperties& Event : EmitterData->GetEventHandlers())
		{
			TSharedRef<FJsonObject> EventObject = MakeShared<FJsonObject>();
			EventObject->SetStringField(TEXT("source_event"), Event.SourceEventName.ToString());
			EventObject->SetNumberField(TEXT("spawn_number"), Event.SpawnNumber);
			EventObject->SetObjectField(TEXT("script"), FToolPlayMCPNiagaraModuleService::ExportScriptStack(Event.Script, ENiagaraScriptUsage::ParticleEventScript, TEXT("event"), EmitterAlias + TEXT(".event"), SessionId));
			EventHandlers.Add(MakeShared<FJsonValueObject>(EventObject));
		}
		EmitterObject->SetArrayField(TEXT("event_handlers"), EventHandlers);

		TArray<TSharedPtr<FJsonValue>> SimulationStages;
		for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
		{
			TSharedRef<FJsonObject> StageObject = MakeShared<FJsonObject>();
			StageObject->SetStringField(TEXT("class"), Stage ? Stage->GetClass()->GetName() : TEXT("null"));
			StageObject->SetStringField(TEXT("name"), Stage ? Stage->GetName() : TEXT(""));
			StageObject->SetStringField(TEXT("stage_name"), Stage ? Stage->SimulationStageName.ToString() : TEXT(""));
			StageObject->SetBoolField(TEXT("enabled"), Stage ? Stage->bEnabled : false);
			if (Stage && Stage->Script)
			{
				StageObject->SetObjectField(TEXT("script"), FToolPlayMCPNiagaraModuleService::ExportScriptStack(Stage->Script, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->SimulationStageName.ToString(), EmitterAlias + TEXT(".sim_stage.") + Stage->SimulationStageName.ToString(), SessionId));
			}
			SimulationStages.Add(MakeShared<FJsonValueObject>(StageObject));
		}
		EmitterObject->SetArrayField(TEXT("simulation_stages"), SimulationStages);
		return EmitterObject;
	}
}

bool FToolPlayMCPNiagaraSystemExporter::ExportSystemByPath(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	const FSoftObjectPath SoftPath(AssetPath);
	UObject* Object = SoftPath.TryLoad();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *AssetPath);
		return false;
	}

	return ExportSystem(System, OutJson, OutError);
}

bool FToolPlayMCPNiagaraSystemExporter::ExportSystem(UNiagaraSystem* System, FString& OutJson, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara System.");
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString SessionId = FToolPlayMCPNiagaraModuleService::CreateSession(System);
	Root->SetStringField(TEXT("session_id"), SessionId);
	Root->SetStringField(TEXT("asset"), System->GetPathName());
	Root->SetStringField(TEXT("name"), System->GetName());
	Root->SetObjectField(TEXT("compile"), BuildCompileStatusObject(System, false, false));
	AddParameterStoreVariables(System->GetExposedParameters(), TEXT("user_parameters"), Root);

	TSharedRef<FJsonObject> SystemStages = MakeShared<FJsonObject>();
	SetScriptField(SystemStages, TEXT("system_spawn"), System->GetSystemSpawnScript(), ENiagaraScriptUsage::SystemSpawnScript, TEXT("system.system_spawn"), SessionId);
	SetScriptField(SystemStages, TEXT("system_update"), System->GetSystemUpdateScript(), ENiagaraScriptUsage::SystemUpdateScript, TEXT("system.system_update"), SessionId);
	Root->SetObjectField(TEXT("system_stages"), SystemStages);

	TArray<TSharedPtr<FJsonValue>> Emitters;
	int32 EmitterIndex = 0;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FString EmitterAlias = FString::Printf(TEXT("e%d"), EmitterIndex++);
		TSharedRef<FJsonObject> EmitterObject = ExportEmitter(Handle, EmitterAlias, SessionId);
		Emitters.Add(MakeShared<FJsonValueObject>(EmitterObject));
	}
	Root->SetArrayField(TEXT("emitters"), Emitters);

	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPNiagaraSystemExporter::ExportCompileStatusByPath(const FString& AssetPath, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError)
{
	const FSoftObjectPath SoftPath(AssetPath);
	UObject* Object = SoftPath.TryLoad();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *AssetPath);
		return false;
	}

	return ExportCompileStatus(System, bForceCompile, bWaitForCompile, OutJson, OutError);
}

bool FToolPlayMCPNiagaraSystemExporter::ExportCompileStatus(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara System.");
		return false;
	}

	OutJson = ToJsonString(BuildCompileStatusObject(System, bForceCompile, bWaitForCompile));
	return true;
}

bool FToolPlayMCPNiagaraSystemExporter::ExportStackIssuesByPath(const FString& AssetPath, FString& OutJson, FString& OutError)
{
	const FSoftObjectPath SoftPath(AssetPath);
	UObject* Object = SoftPath.TryLoad();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *AssetPath);
		return false;
	}

	return ExportStackIssues(System, OutJson, OutError);
}

bool FToolPlayMCPNiagaraSystemExporter::ExportStackIssues(UNiagaraSystem* System, FString& OutJson, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara System.");
		return false;
	}

	OutJson = ToJsonString(BuildStackIssuesObject(System));
	return true;
}

bool FToolPlayMCPNiagaraSystemExporter::ExportDiagnosticsByPath(const FString& AssetPath, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError)
{
	const FSoftObjectPath SoftPath(AssetPath);
	UObject* Object = SoftPath.TryLoad();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset is not a Niagara System: %s"), *AssetPath);
		return false;
	}

	return ExportDiagnostics(System, bForceCompile, bWaitForCompile, OutJson, OutError);
}

bool FToolPlayMCPNiagaraSystemExporter::ExportDiagnostics(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara System.");
		return false;
	}

	OutJson = ToJsonString(BuildDiagnosticsObject(System, bForceCompile, bWaitForCompile));
	return true;
}

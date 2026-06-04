#include "Niagara/ToolPlayMCPNiagaraSystemExporter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/EngineTypes.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Niagara/ToolPlayMCPNiagaraModuleService.h"
#include "UObject/SoftObjectPath.h"

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
		if (IsBlockingStatus(Status))
		{
			CompileErrors.Add(FString::Printf(
				TEXT("Compile status is %s. The script is not confirmed compiled; recompile or inspect the Niagara editor message log."),
				*CompileStatusToString(Status)));
		}
		CompileErrors.Sort();
		Record->SetArrayField(TEXT("compile_errors"), StringArrayToJson(CompileErrors));
		Record->SetArrayField(TEXT("compile_messages"), CompileMessages);
		Record->SetNumberField(TEXT("compile_error_count"), CompileErrors.Num());

		if (Status == ENiagaraScriptCompileStatus::NCS_Error)
		{
			InOutErrorCount += FMath::Max(1, CompileErrors.Num());
		}
		else if (IsBlockingStatus(Status))
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

		if (CompileStatusSeverity(Status) > CompileStatusSeverity(InOutWorstStatus))
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


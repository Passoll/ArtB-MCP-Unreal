#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"

class FJsonObject;
class FJsonValue;
class UNiagaraNodeFunctionCall;
class UNiagaraScript;
class UNiagaraSystem;

class FToolPlayMCPNiagaraModuleService
{
public:
	static FString CreateSession(UNiagaraSystem* System);
	static TSharedRef<FJsonObject> ExportScriptStack(
		UNiagaraScript* Script,
		ENiagaraScriptUsage Usage,
		const FString& StageName,
		const FString& AliasPrefix,
		const FString& SessionId);

	static UNiagaraNodeFunctionCall* ResolveModule(const FString& SessionId, const FString& ModuleAlias);
	static bool AddModuleToStack(const FString& SessionId, const FString& TargetStackAlias, const FString& ScriptAssetPath, int32 TargetIndex, const FString& SuggestedName, FString& OutJson, FString& OutError);
	static bool CreateLocalModule(const FString& SessionId, const FString& TargetStackAlias, int32 TargetIndex, const FString& ModuleName, FString& OutJson, FString& OutError);
	static bool ApplyModuleGraphPatch(const FString& SessionId, const FString& ModuleAlias, const TArray<TSharedPtr<FJsonValue>>& Operations, FString& OutJson, FString& OutError);
	static bool RemoveModule(const FString& SessionId, const FString& ModuleAlias, FString& OutJson, FString& OutError);
	static bool MoveModule(const FString& SessionId, const FString& ModuleAlias, const FString& TargetStackAlias, int32 TargetIndex, FString& OutJson, FString& OutError);
	static bool SetModuleEnabled(const FString& SessionId, const FString& ModuleAlias, bool bEnabled, FString& OutJson, FString& OutError);
	static bool ListModuleInputs(const FString& SessionId, const FString& ModuleAlias, FString& OutJson, FString& OutError);
	static bool GetModuleInputOverride(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, FString& OutJson, FString& OutError);
	static bool SetModuleInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& Value, FString& OutJson, FString& OutError);
	static bool SetModuleObjectInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool BindModuleInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, FString& OutJson, FString& OutError);
	static bool BindSkeletalMeshInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, const FString& DefaultAssetPath, FString& OutJson, FString& OutError);
	static bool BindVolumeTextureInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, const FString& DefaultAssetPath, FString& OutJson, FString& OutError);

	static bool ListInputs(const FString& SessionId, const FString& ModuleAlias, FString& OutJson, FString& OutError) { return ListModuleInputs(SessionId, ModuleAlias, OutJson, OutError); }
	static bool GetInputOverride(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, FString& OutJson, FString& OutError) { return GetModuleInputOverride(SessionId, ModuleAlias, InputName, OutJson, OutError); }
	static bool SetInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& Value, FString& OutJson, FString& OutError) { return SetModuleInput(SessionId, ModuleAlias, InputName, Value, OutJson, OutError); }
	static bool SetObjectInput(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& AssetPath, FString& OutJson, FString& OutError) { return SetModuleObjectInput(SessionId, ModuleAlias, InputName, AssetPath, OutJson, OutError); }
	static bool BindInputToUserParameter(const FString& SessionId, const FString& ModuleAlias, const FString& InputName, const FString& UserParameter, FString& OutJson, FString& OutError) { return BindModuleInputToUserParameter(SessionId, ModuleAlias, InputName, UserParameter, OutJson, OutError); }
};

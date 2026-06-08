#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPNiagaraSystemService
{
public:
	static bool CreateSystem(const FString& PackagePath, const FString& AssetName, const FString& TemplateAssetPath, FString& OutJson, FString& OutError);
	static bool AddEmitter(const FString& SystemAssetPath, const FString& EmitterAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError);
	static bool AddDefaultEmitter(const FString& SystemAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError);
	static bool RemoveUserParameter(const FString& SystemAssetPath, const FString& UserParameter, FString& OutJson, FString& OutError);
	static bool SetEmitterSimTarget(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& SimTarget, FString& OutJson, FString& OutError);
	static bool ListRenderers(const FString& SystemAssetPath, const FString& EmitterAlias, FString& OutJson, FString& OutError);
	static bool GetRendererSchema(const FString& RendererTypeOrClass, FString& OutJson, FString& OutError);
	static bool AddRenderer(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& RendererType, int32 TargetIndex, const FString& MeshAssetPath, FString& OutJson, FString& OutError);
	static bool RemoveRenderer(const FString& SystemAssetPath, const FString& EmitterAlias, int32 RendererIndex, FString& OutJson, FString& OutError);
	static bool SetRendererProperty(const FString& SystemAssetPath, const FString& EmitterAlias, int32 RendererIndex, const FString& Property, const FString& Value, FString& OutJson, FString& OutError);
	static bool ListSimulationStages(const FString& SystemAssetPath, const FString& EmitterAlias, FString& OutJson, FString& OutError);
	static bool AddSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, const FString& StageName, int32 TargetIndex, FString& OutJson, FString& OutError);
	static bool RemoveSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, FString& OutJson, FString& OutError);
	static bool MoveSimulationStage(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, int32 TargetIndex, FString& OutJson, FString& OutError);
	static bool SetSimulationStageProperty(const FString& SystemAssetPath, const FString& EmitterAlias, int32 StageIndex, const FString& Property, const FString& Value, FString& OutJson, FString& OutError);
	static bool ConfigureSpriteRenderer(
		const FString& SystemAssetPath,
		const FString& EmitterAlias,
		int32 RendererIndex,
		const FString& FacingMode,
		const FString& Alignment,
		float PivotU,
		float PivotV,
		FString& OutJson,
		FString& OutError);
};

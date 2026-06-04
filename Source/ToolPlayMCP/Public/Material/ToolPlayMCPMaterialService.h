#pragma once

#include "CoreMinimal.h"
#include "Graph/ToolPlayMCPGraphExportTypes.h"
#include "Material/ToolPlayMCPCompactMaterialTypes.h"

class UBlueprint;
class UEdGraphNode;
class UMaterial;
class UMaterialInstance;
struct FAssetData;

class FToolPlayMCPMaterialService
{
public:
	static bool ExportSelectedCompact(FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError) { return ExportSelectedMaterialCompact(OutJson, OutSessionId, OutSavedPath, OutError); }
	static bool ExportCompactByPath(const FString& AssetPath, FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError) { return ExportMaterialCompactByPath(AssetPath, OutJson, OutSessionId, OutSavedPath, OutError); }
	static bool CreateAsset(const FString& PackagePath, const FString& AssetName, FString& OutJson, FString& OutError) { return CreateMaterialAsset(PackagePath, AssetName, OutJson, OutError); }
	static bool ListFunctionsByPath(const FString& AssetPath, FString& OutJson, FString& OutError) { return ListMaterialFunctionsByPath(AssetPath, OutJson, OutError); }
	static bool DescribeFunctionInterfaceByPath(const FString& FunctionPath, FString& OutJson, FString& OutError) { return DescribeMaterialFunctionInterfaceByPath(FunctionPath, OutJson, OutError); }
	static bool GetNodeConfigByAlias(const FString& AssetPath, const FString& NodeAlias, FString& OutJson, FString& OutError) { return GetMaterialNodeConfigByAlias(AssetPath, NodeAlias, OutJson, OutError); }
	static bool GetNodeConfigSchema(const FString& NodeKindOrClassPath, FString& OutJson, FString& OutError) { return GetMaterialNodeConfigSchema(NodeKindOrClassPath, OutJson, OutError); }
	static bool TraceParameterByPath(const FString& AssetPath, const FString& ParameterName, FString& OutJson, FString& OutError) { return TraceMaterialParameterByPath(AssetPath, ParameterName, OutJson, OutError); }
	static bool TraceOutputByPath(const FString& AssetPath, const FString& OutputName, FString& OutJson, FString& OutError) { return TraceMaterialOutputByPath(AssetPath, OutputName, OutJson, OutError); }
	static bool ValidatePatch(const FString& PatchJson, FString& OutJson, FString& OutError) { return ValidateMaterialPatch(PatchJson, OutJson, OutError); }
	static bool ApplyPatch(const FString& PatchJson, FString& OutJson, FString& OutError) { return ApplyMaterialPatch(PatchJson, OutJson, OutError); }
	static bool BuildCompactGraphByPath(const FString& AssetPath, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError) { return BuildCompactMaterialGraphByPath(AssetPath, OutGraph, OutSessionId, OutError); }
	static bool BuildCompactMaterialGraphByPath(const FString& AssetPath, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError);

private:
	static bool ExportSelectedAsset(FString& OutJson, FString& OutSavedPath, FString& OutError);
	static bool ExportSelectedMaterialCompact(FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError);
	static bool ExportMaterialCompactByPath(const FString& AssetPath, FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError);
	static bool CreateMaterialAsset(const FString& PackagePath, const FString& AssetName, FString& OutJson, FString& OutError);
	static bool SaveAssetByPath(const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool ListMaterialFunctionsByPath(const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool DescribeMaterialFunctionInterfaceByPath(const FString& FunctionPath, FString& OutJson, FString& OutError);
	static bool GetMaterialNodeConfigByAlias(const FString& AssetPath, const FString& NodeAlias, FString& OutJson, FString& OutError);
	static bool GetMaterialNodeConfigSchema(const FString& NodeKindOrClassPath, FString& OutJson, FString& OutError);
	static bool TraceMaterialParameterByPath(const FString& AssetPath, const FString& ParameterName, FString& OutJson, FString& OutError);
	static bool TraceMaterialOutputByPath(const FString& AssetPath, const FString& OutputName, FString& OutJson, FString& OutError);
	static bool ValidateMaterialPatch(const FString& PatchJson, FString& OutJson, FString& OutError);
	static bool ApplyMaterialPatch(const FString& PatchJson, FString& OutJson, FString& OutError);
	static bool GetSingleSelectedAsset(FAssetData& OutAssetData, FString& OutError);
	static bool ExportAsset(const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static bool ExportBlueprint(UBlueprint* Blueprint, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static bool ExportMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static bool ExportCompactMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError);
	static bool ExportCompactMaterialInstance(UMaterialInstance* MaterialInstance, const FAssetData& AssetData, FToolPlayMCPCompactMaterialGraph& OutGraph, FString& OutSessionId, FString& OutError);
	static FString SerializeDocument(const FToolPlayMCPGraphExportDocument& Document);
	static FString SerializeCompactMaterialGraph(const FToolPlayMCPCompactMaterialGraph& Document);
	static FString BuildOutputPath(const FAssetData& AssetData);
	static FString BuildCompactOutputPath(const FAssetData& AssetData);
	static FString BuildNodeId(const UEdGraphNode* Node);
};

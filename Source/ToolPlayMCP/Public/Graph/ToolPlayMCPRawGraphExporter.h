#pragma once

#include "CoreMinimal.h"
#include "Graph/ToolPlayMCPGraphExportTypes.h"

class UBlueprint;
class UMaterial;
class UEdGraphNode;
struct FAssetData;

class FToolPlayMCPRawGraphExporter
{
public:
	static bool ExportSelectedAsset(FString& OutJson, FString& OutSavedPath, FString& OutError);
	static FString BuildNodeId(const UEdGraphNode* Node);

private:
	static bool GetSingleSelectedAsset(FAssetData& OutAssetData, FString& OutError);
	static bool ExportAsset(const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static bool ExportBlueprint(UBlueprint* Blueprint, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static bool ExportMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError);
	static FString SerializeDocument(const FToolPlayMCPGraphExportDocument& Document);
	static FString BuildOutputPath(const FAssetData& AssetData);
};

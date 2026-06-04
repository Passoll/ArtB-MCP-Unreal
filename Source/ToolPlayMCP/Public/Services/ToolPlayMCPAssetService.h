#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPAssetService
{
public:
	static bool SaveAssetByPath(const FString& AssetPath, FString& OutJson, FString& OutError);
};

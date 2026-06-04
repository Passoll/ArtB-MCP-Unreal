#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPNiagaraSystemService
{
public:
	static bool CreateSystem(const FString& PackagePath, const FString& AssetName, const FString& TemplateAssetPath, FString& OutJson, FString& OutError);
	static bool AddEmitter(const FString& SystemAssetPath, const FString& EmitterAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError);
	static bool AddDefaultEmitter(const FString& SystemAssetPath, const FString& EmitterName, FString& OutJson, FString& OutError);
};

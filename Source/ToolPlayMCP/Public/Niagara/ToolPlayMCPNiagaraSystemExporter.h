#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;

class FToolPlayMCPNiagaraSystemExporter
{
public:
	static bool ExportSystemByPath(const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool ExportSystem(UNiagaraSystem* System, FString& OutJson, FString& OutError);
	static bool ExportCompileStatusByPath(const FString& AssetPath, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError);
	static bool ExportCompileStatus(UNiagaraSystem* System, bool bForceCompile, bool bWaitForCompile, FString& OutJson, FString& OutError);
};

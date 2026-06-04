#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPNiagaraCatalog
{
public:
	static bool SearchScripts(
		const FString& Query,
		const FString& Usage,
		const FString& Source,
		int32 Limit,
		FString& OutJson,
		FString& OutError);
};

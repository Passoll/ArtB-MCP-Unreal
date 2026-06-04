#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPSelectionService
{
public:
	static bool ExportSelectedGraphNodes(FString& OutJson, FString& OutError);
	static bool ExportSelection(FString& OutJson, FString& OutError);
};

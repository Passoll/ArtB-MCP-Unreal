#pragma once

#include "CoreMinimal.h"

class FToolPlayMCPBlueprintService
{
public:
	static bool ExportBlueprintByPath(const FString& AssetPath, FString& OutJson, FString& OutSessionId, FString& OutSavedPath, FString& OutError);
	static bool AddFunctionCallNode(const FString& SessionId, const FString& GraphAlias, const FString& FunctionPath, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError);
	static bool AddCustomEventNode(const FString& SessionId, const FString& GraphAlias, const FString& EventName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError);
	static bool SetPinDefault(const FString& SessionId, const FString& NodeAlias, const FString& PinName, const FString& DefaultValue, FString& OutJson, FString& OutError);
	static bool ConnectPins(const FString& SessionId, const FString& FromNodeAlias, const FString& FromPinName, const FString& ToNodeAlias, const FString& ToPinName, FString& OutJson, FString& OutError);
	static bool DisconnectPin(const FString& SessionId, const FString& NodeAlias, const FString& PinName, FString& OutJson, FString& OutError);
	static bool RemoveNode(const FString& SessionId, const FString& NodeAlias, FString& OutJson, FString& OutError);
	static bool CompileBlueprint(const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool ListVariables(const FString& AssetPath, FString& OutJson, FString& OutError);
	static bool AddMemberVariable(const FString& AssetPath, const FString& VariableName, const FString& TypeName, const FString& DefaultValue, const FString& Category, FString& OutJson, FString& OutError);
	static bool SetMemberVariableDefault(const FString& AssetPath, const FString& VariableName, const FString& DefaultValue, FString& OutJson, FString& OutError);
	static bool AddVariableGetNode(const FString& SessionId, const FString& GraphAlias, const FString& VariableName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError);
	static bool AddVariableSetNode(const FString& SessionId, const FString& GraphAlias, const FString& VariableName, int32 PositionX, int32 PositionY, FString& OutJson, FString& OutError);
};

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FToolPlayMCPBridgeCommandSpec
{
	FString Name;
	FString Domain;
	FString Description;
	FString ParamsExampleJson;
	FString InputSchemaJson;
	FString UsageTopic;
};

struct FToolPlayMCPToolsetSpec
{
	FString Domain;
	FString Description;
};

class FToolPlayMCPToolRegistry
{
public:
	static const TArray<FToolPlayMCPBridgeCommandSpec>& GetCommandSpecs();
	static TArray<FToolPlayMCPToolsetSpec> GetToolsets();
	static TSharedRef<FJsonObject> CommandSpecToJson(const FToolPlayMCPBridgeCommandSpec& Spec);
	static TSharedRef<FJsonObject> BuildToolsetsJson();
	static bool BuildToolsetDescriptionJson(const FString& Domain, TSharedRef<FJsonObject>& OutJson, FString& OutError);

private:
	static TSharedPtr<FJsonObject> ParseObject(const FString& Json);
};

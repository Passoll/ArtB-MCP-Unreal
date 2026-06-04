#pragma once

#include "CoreMinimal.h"

struct FToolPlayMCPCompactMaterialNode
{
	FString K;

	FString Label;

	FString V;
};

struct FToolPlayMCPCompactMaterialGraph
{
	FString Asset;

	FString Scope = TEXT("asset");

	FString Parent;

	TMap<FString, FString> Overrides;

	TMap<FString, FToolPlayMCPCompactMaterialNode> Nodes;

	TArray<TArray<FString>> Edges;
};

struct FToolPlayMCPMaterialNodeBinding
{
	FString Alias;
	FString ExpressionGuid;
	TWeakObjectPtr<class UMaterialExpression> Expression;
};

struct FToolPlayMCPMaterialGraphSession
{
	FString SessionId;
	FString AssetPath;
	TMap<FString, FToolPlayMCPMaterialNodeBinding> NodeBindings;
};

#pragma once

#include "CoreMinimal.h"
#include "ToolPlayMCPGraphExportTypes.generated.h"

USTRUCT()
struct FToolPlayMCPGraphExportPin
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Direction;

	UPROPERTY()
	FString Type;

	UPROPERTY()
	FString DefaultValue;

	UPROPERTY()
	TArray<FString> LinkedTo;
};

USTRUCT()
struct FToolPlayMCPGraphExportNode
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FString NodeClass;

	UPROPERTY()
	int32 PositionX = 0;

	UPROPERTY()
	int32 PositionY = 0;

	UPROPERTY()
	TMap<FString, FString> Metadata;

	UPROPERTY()
	TArray<FToolPlayMCPGraphExportPin> Pins;
};

USTRUCT()
struct FToolPlayMCPGraphExportLink
{
	GENERATED_BODY()

	UPROPERTY()
	FString FromNode;

	UPROPERTY()
	FString FromPin;

	UPROPERTY()
	FString ToNode;

	UPROPERTY()
	FString ToPin;

	UPROPERTY()
	TMap<FString, FString> Metadata;
};

USTRUCT()
struct FToolPlayMCPGraphExportGraph
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString GraphClass;

	UPROPERTY()
	TArray<FToolPlayMCPGraphExportNode> Nodes;

	UPROPERTY()
	TArray<FToolPlayMCPGraphExportLink> Links;
};

USTRUCT()
struct FToolPlayMCPGraphExportDocument
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetName;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString AssetClass;

	UPROPERTY()
	FString ExporterVersion = TEXT("0.1");

	UPROPERTY()
	TArray<FString> Messages;

	UPROPERTY()
	TArray<FToolPlayMCPGraphExportGraph> Graphs;
};

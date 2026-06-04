#include "Graph/ToolPlayMCPRawGraphExporter.h"

#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "ContentBrowserModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "JsonObjectConverter.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	FString ExtractPinType(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("unknown");
		}
		return Pin->PinType.PinCategory.IsNone() ? TEXT("unknown") : Pin->PinType.PinCategory.ToString();
	}

	FString BuildMaterialExpressionId(const UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return TEXT("invalid-material-node");
		}
		return Expression->MaterialExpressionGuid.IsValid()
			? Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
			: Expression->GetName();
	}

	FString BuildMaterialInputName(const UMaterialExpression* Expression, const int32 InputIndex)
	{
		const FName InputName = Expression ? Expression->GetInputName(InputIndex) : NAME_None;
		if (!InputName.IsNone())
		{
			return InputName.ToString();
		}
		return FString::Printf(TEXT("Input%d"), InputIndex);
	}

	FString BuildMaterialOutputName(const UMaterialExpression* Expression, const int32 OutputIndex)
	{
		if (Expression && Expression->Outputs.IsValidIndex(OutputIndex) && !Expression->Outputs[OutputIndex].OutputName.IsNone())
		{
			return Expression->Outputs[OutputIndex].OutputName.ToString();
		}
		return OutputIndex == 0 ? TEXT("Output") : FString::Printf(TEXT("Output%d"), OutputIndex);
	}

	void AddBlueprintGraph(UEdGraph* Graph, FToolPlayMCPGraphExportDocument& OutDocument)
	{
		if (!Graph)
		{
			return;
		}

		FToolPlayMCPGraphExportGraph ExportGraph;
		ExportGraph.Name = Graph->GetName();
		ExportGraph.GraphClass = Graph->GetClass()->GetName();

		TSet<FString> AddedLinks;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FToolPlayMCPGraphExportNode ExportNode;
			ExportNode.Id = FToolPlayMCPRawGraphExporter::BuildNodeId(Node);
			ExportNode.Name = Node->GetName();
			ExportNode.Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			ExportNode.NodeClass = Node->GetClass()->GetName();
			ExportNode.PositionX = Node->NodePosX;
			ExportNode.PositionY = Node->NodePosY;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				FToolPlayMCPGraphExportPin ExportPin;
				ExportPin.Name = Pin->PinName.ToString();
				ExportPin.Direction = PinDirectionToString(Pin->Direction);
				ExportPin.Type = ExtractPinType(Pin);
				ExportPin.DefaultValue = Pin->DefaultValue;

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode())
					{
						continue;
					}

					ExportPin.LinkedTo.Add(FString::Printf(
						TEXT("%s.%s"),
						*FToolPlayMCPRawGraphExporter::BuildNodeId(LinkedPin->GetOwningNode()),
						*LinkedPin->PinName.ToString()));

					if (Pin->Direction == EGPD_Output)
					{
						const FString LinkKey = FString::Printf(
							TEXT("%s.%s->%s.%s"),
							*ExportNode.Id,
							*Pin->PinName.ToString(),
							*FToolPlayMCPRawGraphExporter::BuildNodeId(LinkedPin->GetOwningNode()),
							*LinkedPin->PinName.ToString());
						if (!AddedLinks.Contains(LinkKey))
						{
							AddedLinks.Add(LinkKey);
							FToolPlayMCPGraphExportLink Link;
							Link.FromNode = ExportNode.Id;
							Link.FromPin = Pin->PinName.ToString();
							Link.ToNode = FToolPlayMCPRawGraphExporter::BuildNodeId(LinkedPin->GetOwningNode());
							Link.ToPin = LinkedPin->PinName.ToString();
							ExportGraph.Links.Add(MoveTemp(Link));
						}
					}
				}

				ExportNode.Pins.Add(MoveTemp(ExportPin));
			}

			ExportGraph.Nodes.Add(MoveTemp(ExportNode));
		}

		OutDocument.Graphs.Add(MoveTemp(ExportGraph));
	}
}

bool FToolPlayMCPRawGraphExporter::ExportSelectedAsset(FString& OutJson, FString& OutSavedPath, FString& OutError)
{
	FAssetData AssetData;
	if (!GetSingleSelectedAsset(AssetData, OutError))
	{
		return false;
	}

	FToolPlayMCPGraphExportDocument Document;
	if (!ExportAsset(AssetData, Document, OutError))
	{
		return false;
	}

	OutJson = SerializeDocument(Document);
	OutSavedPath = BuildOutputPath(AssetData);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutSavedPath), true);
	if (!FFileHelper::SaveStringToFile(OutJson, *OutSavedPath))
	{
		OutError = FString::Printf(TEXT("Failed to save graph export to '%s'."), *OutSavedPath);
		return false;
	}

	return true;
}

bool FToolPlayMCPRawGraphExporter::GetSingleSelectedAsset(FAssetData& OutAssetData, FString& OutError)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	if (SelectedAssets.Num() != 1)
	{
		OutError = SelectedAssets.Num() == 0
			? TEXT("Select one Blueprint or Material asset in the Content Browser.")
			: TEXT("Select exactly one asset in the Content Browser.");
		return false;
	}

	OutAssetData = SelectedAssets[0];
	return true;
}

bool FToolPlayMCPRawGraphExporter::ExportAsset(const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	UObject* AssetObject = AssetData.GetAsset();
	if (!AssetObject)
	{
		OutError = TEXT("Failed to load selected asset.");
		return false;
	}

	OutDocument.AssetName = AssetData.AssetName.ToString();
	OutDocument.AssetPath = AssetData.GetSoftObjectPath().ToString();
	OutDocument.AssetClass = AssetObject->GetClass()->GetName();

	if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetObject))
	{
		return ExportBlueprint(Blueprint, AssetData, OutDocument, OutError);
	}

	if (UMaterial* Material = Cast<UMaterial>(AssetObject))
	{
		return ExportMaterial(Material, AssetData, OutDocument, OutError);
	}

	OutError = FString::Printf(TEXT("Unsupported asset class '%s'. Select a Blueprint or Material."), *AssetObject->GetClass()->GetName());
	return false;
}

bool FToolPlayMCPRawGraphExporter::ExportBlueprint(UBlueprint* Blueprint, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Invalid Blueprint asset.");
		return false;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		AddBlueprintGraph(Graph, OutDocument);
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		AddBlueprintGraph(Graph, OutDocument);
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		AddBlueprintGraph(Graph, OutDocument);
	}

	if (OutDocument.Graphs.IsEmpty())
	{
		OutDocument.Messages.Add(TEXT("Blueprint contains no exported graphs."));
	}
	return true;
}

bool FToolPlayMCPRawGraphExporter::ExportMaterial(UMaterial* Material, const FAssetData& AssetData, FToolPlayMCPGraphExportDocument& OutDocument, FString& OutError)
{
	if (!Material)
	{
		OutError = TEXT("Invalid Material asset.");
		return false;
	}

	FToolPlayMCPGraphExportGraph Graph;
	Graph.Name = Material->GetName();
	Graph.GraphClass = TEXT("MaterialExpressionGraph");

	TMap<const UMaterialExpression*, FString> NodeIds;
	for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
	{
		if (Expression)
		{
			NodeIds.Add(Expression, BuildMaterialExpressionId(Expression));
		}
	}

	for (UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
	{
		if (!Expression)
		{
			continue;
		}

		FToolPlayMCPGraphExportNode Node;
		Node.Id = NodeIds.FindRef(Expression);
		Node.Name = Expression->GetName();
		Node.Title = Expression->GetDescription();
		Node.NodeClass = Expression->GetClass()->GetName();
		Node.PositionX = Expression->MaterialExpressionEditorX;
		Node.PositionY = Expression->MaterialExpressionEditorY;

		if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
		{
			Node.Metadata.Add(TEXT("parameter"), Parameter->ParameterName.ToString());
		}
		if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
		{
			Node.Metadata.Add(TEXT("texture"), TextureSample->Texture ? TextureSample->Texture->GetPathName() : FString());
		}
		if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			Node.Metadata.Add(TEXT("function"), FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetPathName() : FString());
		}

		for (FExpressionInputIterator It(Expression); It; ++It)
		{
			const int32 InputIndex = It.Index;
			const FExpressionInput* Input = It.Input;
			FToolPlayMCPGraphExportPin Pin;
			Pin.Name = BuildMaterialInputName(Expression, InputIndex);
			Pin.Direction = TEXT("input");
			Pin.Type = TEXT("material-input");
			if (Input && Input->Expression)
			{
				const FString FromNode = NodeIds.FindRef(Input->Expression);
				const FString FromPin = BuildMaterialOutputName(Input->Expression, Input->OutputIndex);
				Pin.LinkedTo.Add(FString::Printf(TEXT("%s.%s"), *FromNode, *FromPin));

				FToolPlayMCPGraphExportLink Link;
				Link.FromNode = FromNode;
				Link.FromPin = FromPin;
				Link.ToNode = Node.Id;
				Link.ToPin = Pin.Name;
				Graph.Links.Add(MoveTemp(Link));
			}
			Node.Pins.Add(MoveTemp(Pin));
		}

		for (int32 OutputIndex = 0; OutputIndex < Expression->Outputs.Num(); ++OutputIndex)
		{
			FToolPlayMCPGraphExportPin Pin;
			Pin.Name = BuildMaterialOutputName(Expression, OutputIndex);
			Pin.Direction = TEXT("output");
			Pin.Type = TEXT("material-output");
			Node.Pins.Add(MoveTemp(Pin));
		}

		Graph.Nodes.Add(MoveTemp(Node));
	}

	OutDocument.Graphs.Add(MoveTemp(Graph));
	return true;
}

FString FToolPlayMCPRawGraphExporter::SerializeDocument(const FToolPlayMCPGraphExportDocument& Document)
{
	FString Json;
	FJsonObjectConverter::UStructToJsonObjectString(Document, Json);
	return Json;
}

FString FToolPlayMCPRawGraphExporter::BuildOutputPath(const FAssetData& AssetData)
{
	const FString FileName = FString::Printf(TEXT("%s_graph.json"), *AssetData.AssetName.ToString());
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ToolPlayMCP"), TEXT("GraphExports"), FileName);
}

FString FToolPlayMCPRawGraphExporter::BuildNodeId(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("invalid-node");
	}
	return Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : Node->GetName();
}

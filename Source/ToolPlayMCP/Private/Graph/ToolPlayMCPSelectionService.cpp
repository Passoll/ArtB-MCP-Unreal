#include "Graph/ToolPlayMCPSelectionService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "IContentBrowserSingleton.h"
#include "Layout/WidgetPath.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString ToJsonString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	FString PinTypeToString(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("unknown");
		}

		if (!Pin->PinType.PinCategory.IsNone())
		{
			return Pin->PinType.PinCategory.ToString();
		}

		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			return Pin->PinType.PinSubCategoryObject->GetName();
		}

		return TEXT("unknown");
	}

	TSharedRef<FJsonObject> ExportLinkedPin(const UEdGraphPin* LinkedPin)
	{
		TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
		if (!LinkedPin)
		{
			return Link;
		}

		const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
		Link->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
		Link->SetStringField(TEXT("direction"), PinDirectionToString(LinkedPin->Direction));
		if (LinkedNode)
		{
			Link->SetStringField(TEXT("node_title"), LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Link->SetStringField(TEXT("node_name"), LinkedNode->GetName());
			Link->SetStringField(TEXT("node_class"), LinkedNode->GetClass()->GetName());
			Link->SetStringField(TEXT("node_path"), LinkedNode->GetPathName());
		}
		return Link;
	}

	TSharedRef<FJsonObject> ExportPin(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return PinObject;
		}

		PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObject->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
		PinObject->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinObject->SetStringField(TEXT("type"), PinTypeToString(Pin));
		PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		PinObject->SetBoolField(TEXT("hidden"), Pin->bHidden);

		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			Links.Add(MakeShared<FJsonValueObject>(ExportLinkedPin(LinkedPin)));
		}
		PinObject->SetArrayField(TEXT("links"), MoveTemp(Links));
		return PinObject;
	}

	TSharedRef<FJsonObject> ExportNode(const UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeObject;
		}

		NodeObject->SetStringField(TEXT("name"), Node->GetName());
		NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObject->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeObject->SetStringField(TEXT("path"), Node->GetPathName());
		NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeObject->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());

		if (const UEdGraph* Graph = Node->GetGraph())
		{
			NodeObject->SetStringField(TEXT("graph_name"), Graph->GetName());
			NodeObject->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
			NodeObject->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			if (const UObject* GraphOuter = Graph->GetOuter())
			{
				NodeObject->SetStringField(TEXT("graph_outer"), GraphOuter->GetPathName());
				NodeObject->SetStringField(TEXT("graph_outer_class"), GraphOuter->GetClass()->GetName());
			}
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(ExportPin(Pin)));
		}
		NodeObject->SetArrayField(TEXT("pins"), MoveTemp(Pins));
		return NodeObject;
	}

	TSharedPtr<SGraphEditor> FindFocusedGraphEditor(FString& OutFocusPath)
	{
		TSharedPtr<SWidget> FocusedWidget = FSlateApplication::Get().GetKeyboardFocusedWidget();
		if (!FocusedWidget.IsValid())
		{
			FocusedWidget = FSlateApplication::Get().GetUserFocusedWidget(0);
		}
		if (!FocusedWidget.IsValid())
		{
			return nullptr;
		}

		FWidgetPath WidgetPath;
		if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(FocusedWidget.ToSharedRef(), WidgetPath, EVisibility::All) || !WidgetPath.IsValid())
		{
			return nullptr;
		}

		OutFocusPath = WidgetPath.ToString();
		for (int32 Index = WidgetPath.Widgets.Num() - 1; Index >= 0; --Index)
		{
			const TSharedRef<SWidget>& Widget = WidgetPath.Widgets[Index].Widget;
			const FString TypeName = Widget->GetTypeAsString();
			if (TypeName.Contains(TEXT("SGraphEditor")))
			{
				return StaticCastSharedRef<SGraphEditor>(Widget);
			}
		}

		return nullptr;
	}

	TSharedRef<FJsonObject> ExportAssetData(const FAssetData& AssetData)
	{
		TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		AssetObject->SetStringField(TEXT("asset_path"), AssetData.GetSoftObjectPath().ToString());
		AssetObject->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		AssetObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		AssetObject->SetStringField(TEXT("class_path"), AssetData.AssetClassPath.ToString());
		return AssetObject;
	}

	TSharedRef<FJsonObject> ExportContentBrowserSelection()
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FAssetData> SelectedAssets;
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), true);
		Root->SetStringField(TEXT("source"), TEXT("content_browser"));
		Root->SetNumberField(TEXT("count"), SelectedAssets.Num());
		TArray<TSharedPtr<FJsonValue>> Assets;
		for (const FAssetData& AssetData : SelectedAssets)
		{
			Assets.Add(MakeShared<FJsonValueObject>(ExportAssetData(AssetData)));
		}
		Root->SetArrayField(TEXT("assets"), MoveTemp(Assets));
		return Root;
	}

	TSharedRef<FJsonObject> ExportGraphSelection(TSharedPtr<SGraphEditor> GraphEditor, const FString& FocusPath)
	{
		const FGraphPanelSelectionSet& SelectedNodes = GraphEditor->GetSelectedNodes();

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), true);
		Root->SetStringField(TEXT("source"), TEXT("focused_graph_editor"));
		Root->SetStringField(TEXT("focus_path"), FocusPath);
		Root->SetNumberField(TEXT("count"), SelectedNodes.Num());

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UObject* Object : SelectedNodes)
		{
			if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			{
				Nodes.Add(MakeShared<FJsonValueObject>(ExportNode(Node)));
			}
		}

		Root->SetNumberField(TEXT("graph_node_count"), Nodes.Num());
		Root->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
		return Root;
	}
}

bool FToolPlayMCPSelectionService::ExportSelectedGraphNodes(FString& OutJson, FString& OutError)
{
	FString FocusPath;
	TSharedPtr<SGraphEditor> GraphEditor = FindFocusedGraphEditor(FocusPath);
	if (!GraphEditor.IsValid())
	{
		OutError = TEXT("No focused GraphEditor found. Click a Material, Blueprint, or Niagara graph panel and select graph nodes first.");
		return false;
	}

	TSharedRef<FJsonObject> Root = ExportGraphSelection(GraphEditor, FocusPath);
	OutJson = ToJsonString(Root);
	return true;
}

bool FToolPlayMCPSelectionService::ExportSelection(FString& OutJson, FString& OutError)
{
	FString FocusPath;
	TSharedPtr<SGraphEditor> GraphEditor = FindFocusedGraphEditor(FocusPath);
	if (GraphEditor.IsValid() && GraphEditor->GetSelectedNodes().Num() > 0)
	{
		OutJson = ToJsonString(ExportGraphSelection(GraphEditor, FocusPath));
		return true;
	}

	TSharedRef<FJsonObject> AssetSelection = ExportContentBrowserSelection();
	if (AssetSelection->GetIntegerField(TEXT("count")) > 0)
	{
		OutJson = ToJsonString(AssetSelection);
		return true;
	}

	OutError = TEXT("No graph nodes or Content Browser assets are selected. Select graph nodes first, or select assets in the Content Browser.");
	return false;
}

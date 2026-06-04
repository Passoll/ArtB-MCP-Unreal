#include "SToolPlayMCPDebugPanel.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Internationalization/Text.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolPlayMCP.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SToolPlayMCPDebugPanel"

namespace
{
	FString BuildBridgeRequestJson(const FString& Command, const FString& ParamsJson)
	{
		TSharedPtr<FJsonObject> Params;
		const TSharedRef<TJsonReader<>> ParamsReader = TJsonReaderFactory<>::Create(ParamsJson.IsEmpty() ? TEXT("{}") : ParamsJson);
		if (!FJsonSerializer::Deserialize(ParamsReader, Params) || !Params.IsValid())
		{
			return FString();
		}

		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("command"), Command);
		Request->SetObjectField(TEXT("params"), Params.ToSharedRef());

		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Request, Writer);
		return Output;
	}
}

void SToolPlayMCPDebugPanel::Construct(const FArguments& InArgs)
{
	for (const FToolPlayMCPBridgeCommandSpec& Spec : FToolPlayMCPBridgeServer::GetCommandSpecs())
	{
		ToolItems.Add(MakeShared<FToolPlayMCPDebugToolItem>(Spec));
	}

	ChildSlot
	[
		SNew(SBorder)
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("IntroText", "ToolPlay MCP Debug Panel: select a bridge tool, edit params JSON, then call it locally."))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("StartMCPServerButton", "Start MCP Server"))
					.OnClicked(this, &SToolPlayMCPDebugPanel::HandleStartMCPServerClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("StopMCPServerButton", "Stop MCP Server"))
					.OnClicked(this, &SToolPlayMCPDebugPanel::HandleStopMCPServerClicked)
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot()
				.Value(0.32f)
				[
					SAssignNew(ToolListView, SListView<TSharedPtr<FToolPlayMCPDebugToolItem>>)
					.ListItemsSource(&ToolItems)
					.OnGenerateRow(this, &SToolPlayMCPDebugPanel::GenerateToolRow)
					.OnSelectionChanged(this, &SToolPlayMCPDebugPanel::HandleToolSelectionChanged)
					.SelectionMode(ESelectionMode::Single)
				]
				+ SSplitter::Slot()
				.Value(0.68f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(8.0f, 0.0f, 0.0f, 6.0f)
					[
						SAssignNew(ToolDescriptionText, STextBlock)
						.Text(LOCTEXT("NoToolSelected", "Select a tool from the list."))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.FillHeight(0.35f)
					.Padding(8.0f, 0.0f, 0.0f, 8.0f)
					[
						SAssignNew(ParamsTextBox, SMultiLineEditableTextBox)
						.HintText(LOCTEXT("ParamsHint", "Params JSON, for example: {\"asset_path\":\"/Game/...\"}"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(8.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("CallToolButton", "Call Selected Tool"))
							.OnClicked(this, &SToolPlayMCPDebugPanel::HandleCallToolClicked)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SAssignNew(StatusText, STextBlock)
							.Text(LOCTEXT("InitialStatus", "Ready."))
							.AutoWrapText(true)
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(0.65f)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SAssignNew(OutputTextBox, SMultiLineEditableTextBox)
						.IsReadOnly(true)
					]
				]
			]
		]
	];

	if (ToolItems.Num() > 0 && ToolListView.IsValid())
	{
		ToolListView->SetSelection(ToolItems[0]);
	}
}

TSharedRef<ITableRow> SToolPlayMCPDebugPanel::GenerateToolRow(TSharedPtr<FToolPlayMCPDebugToolItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Label = Item.IsValid()
		? FString::Printf(TEXT("[%s] %s"), *Item->Spec.Domain, *Item->Spec.Name)
		: TEXT("Invalid tool");

	return SNew(STableRow<TSharedPtr<FToolPlayMCPDebugToolItem>>, OwnerTable)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Label))
	];
}

void SToolPlayMCPDebugPanel::HandleToolSelectionChanged(TSharedPtr<FToolPlayMCPDebugToolItem> Item, ESelectInfo::Type SelectInfo)
{
	SelectedTool = Item;
	if (!SelectedTool.IsValid())
	{
		return;
	}

	if (ToolDescriptionText.IsValid())
	{
		FString Description = FString::Printf(
			TEXT("%s\nDomain: %s\n%s"),
			*SelectedTool->Spec.Name,
			*SelectedTool->Spec.Domain,
			*SelectedTool->Spec.Description);
		if (!SelectedTool->Spec.UsageTopic.IsEmpty())
		{
			Description += FString::Printf(TEXT("\nUsage topic: %s"), *SelectedTool->Spec.UsageTopic);
		}
		if (!SelectedTool->Spec.InputSchemaJson.IsEmpty())
		{
			Description += FString::Printf(TEXT("\nInput schema:\n%s"), *SelectedTool->Spec.InputSchemaJson);
		}
		ToolDescriptionText->SetText(FText::FromString(Description));
	}
	if (ParamsTextBox.IsValid())
	{
		ParamsTextBox->SetText(FText::FromString(SelectedTool->Spec.ParamsExampleJson));
	}
}

FReply SToolPlayMCPDebugPanel::HandleCallToolClicked()
{
	if (!SelectedTool.IsValid())
	{
		SetStatusMessage(LOCTEXT("NoToolStatus", "Select a tool first."));
		return FReply::Handled();
	}

	const FString ParamsJson = ParamsTextBox.IsValid() ? ParamsTextBox->GetText().ToString() : TEXT("{}");
	const FString RequestJson = BuildBridgeRequestJson(SelectedTool->Spec.Name, ParamsJson);
	if (RequestJson.IsEmpty())
	{
		SetStatusMessage(LOCTEXT("InvalidParamsStatus", "Params must be a JSON object."));
		return FReply::Handled();
	}

	const FString ResponseJson = FToolPlayMCPBridgeServer::ExecuteRequestJson(RequestJson);
	if (OutputTextBox.IsValid())
	{
		OutputTextBox->SetText(FText::FromString(ResponseJson));
	}
	SetStatusMessage(FText::Format(LOCTEXT("ToolCalledStatus", "Called {0}."), FText::FromString(SelectedTool->Spec.Name)));
	return FReply::Handled();
}

FReply SToolPlayMCPDebugPanel::HandleStartMCPServerClicked()
{
	FString Message;
	FToolPlayMCPModule& Module = FModuleManager::LoadModuleChecked<FToolPlayMCPModule>(TEXT("ToolPlayMCP"));
	Module.StartPythonMCPServer(Message);
	SetStatusMessage(FText::FromString(Message));
	return FReply::Handled();
}

FReply SToolPlayMCPDebugPanel::HandleStopMCPServerClicked()
{
	FString Message;
	FToolPlayMCPModule& Module = FModuleManager::LoadModuleChecked<FToolPlayMCPModule>(TEXT("ToolPlayMCP"));
	Module.StopPythonMCPServer(Message);
	SetStatusMessage(FText::FromString(Message));
	return FReply::Handled();
}

void SToolPlayMCPDebugPanel::SetStatusMessage(const FText& InMessage)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(InMessage);
	}
}

#undef LOCTEXT_NAMESPACE

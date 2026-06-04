#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "ToolPlayMCPBridgeServer.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;
class STableViewBase;
class STextBlock;
class ITableRow;
template <typename ItemType> class SListView;

struct FToolPlayMCPDebugToolItem
{
	explicit FToolPlayMCPDebugToolItem(const FToolPlayMCPBridgeCommandSpec& InSpec)
		: Spec(InSpec)
	{
	}

	FToolPlayMCPBridgeCommandSpec Spec;
};

class SToolPlayMCPDebugPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SToolPlayMCPDebugPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<ITableRow> GenerateToolRow(TSharedPtr<FToolPlayMCPDebugToolItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void HandleToolSelectionChanged(TSharedPtr<FToolPlayMCPDebugToolItem> Item, ESelectInfo::Type SelectInfo);
	FReply HandleCallToolClicked();
	FReply HandleStartMCPServerClicked();
	FReply HandleStopMCPServerClicked();
	void SetStatusMessage(const FText& InMessage);

	TArray<TSharedPtr<FToolPlayMCPDebugToolItem>> ToolItems;
	TSharedPtr<SListView<TSharedPtr<FToolPlayMCPDebugToolItem>>> ToolListView;
	TSharedPtr<FToolPlayMCPDebugToolItem> SelectedTool;
	TSharedPtr<STextBlock> ToolDescriptionText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SMultiLineEditableTextBox> ParamsTextBox;
	TSharedPtr<SMultiLineEditableTextBox> OutputTextBox;
};

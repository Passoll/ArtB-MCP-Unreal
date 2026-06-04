// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolPlayMCP.h"

#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "LevelEditor.h"
#include "Misc/Paths.h"
#include "SToolPlayMCPDebugPanel.h"
#include "Styling/AppStyle.h"
#include "ToolPlayMCPBridgeServer.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FToolPlayMCPModule"

namespace
{
	const FName DebugTabName(TEXT("ToolPlayMCP.DebugPanel"));
}

DEFINE_LOG_CATEGORY_STATIC(LogToolPlayMCP, Log, All);

void FToolPlayMCPModule::StartupModule()
{
	RegisterTabSpawner();
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FToolPlayMCPModule::RegisterMenus));

	BridgeServer = MakeUnique<FToolPlayMCPBridgeServer>();
	BridgeServer->Start(55557);
}

void FToolPlayMCPModule::ShutdownModule()
{
	FString StopMessage;
	StopPythonMCPServer(StopMessage);
	BridgeServer.Reset();

	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		ToolMenus->UnregisterOwner(this);
	}

	UnregisterTabSpawner();
}

FName FToolPlayMCPModule::GetDebugTabName()
{
	return DebugTabName;
}

bool FToolPlayMCPModule::StartPythonMCPServer(FString& OutMessage)
{
	if (IsPythonMCPServerRunning())
	{
		OutMessage = TEXT("ToolPlayMCP Python MCP server is already running.");
		UE_LOG(LogToolPlayMCP, Display, TEXT("%s"), *OutMessage);
		return true;
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ToolPlayMCP"));
	if (!Plugin.IsValid())
	{
		OutMessage = TEXT("Unable to locate ToolPlayMCP plugin.");
		UE_LOG(LogToolPlayMCP, Error, TEXT("%s"), *OutMessage);
		return false;
	}

	const FString ScriptPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("MCPServer"), TEXT("toolplay_mcp_server.py"));
	if (!FPaths::FileExists(ScriptPath))
	{
		OutMessage = FString::Printf(TEXT("MCP server script not found: %s"), *ScriptPath);
		UE_LOG(LogToolPlayMCP, Error, TEXT("%s"), *OutMessage);
		return false;
	}

	const FString PythonExecutable = TEXT("python");
	const FString Args = FString::Printf(TEXT("-u \"%s\""), *ScriptPath);
	const FString WorkingDirectory = FPaths::ProjectDir();

	uint32 ProcessId = 0;
	PythonMCPServerProcess = FPlatformProcess::CreateProc(
		*PythonExecutable,
		*Args,
		true,
		true,
		true,
		&ProcessId,
		0,
		*WorkingDirectory,
		nullptr,
		nullptr);

	if (!PythonMCPServerProcess.IsValid())
	{
		OutMessage = TEXT("Failed to start ToolPlayMCP Python MCP server. Make sure 'python' is available and the 'mcp' package is installed.");
		UE_LOG(LogToolPlayMCP, Error, TEXT("%s"), *OutMessage);
		return false;
	}

	OutMessage = FString::Printf(TEXT("Started ToolPlayMCP Python MCP server. PID: %u"), ProcessId);
	UE_LOG(LogToolPlayMCP, Display, TEXT("%s"), *OutMessage);
	UE_LOG(LogToolPlayMCP, Display, TEXT("MCP server script: %s"), *ScriptPath);
	UE_LOG(LogToolPlayMCP, Warning, TEXT("This starts the Python stdio MCP process from Unreal. Most MCP clients should launch stdio servers themselves via client config."));
	return true;
}

bool FToolPlayMCPModule::StopPythonMCPServer(FString& OutMessage)
{
	if (!PythonMCPServerProcess.IsValid())
	{
		OutMessage = TEXT("ToolPlayMCP Python MCP server is not running.");
		return true;
	}

	if (FPlatformProcess::IsProcRunning(PythonMCPServerProcess))
	{
		FPlatformProcess::TerminateProc(PythonMCPServerProcess, true);
	}

	FPlatformProcess::CloseProc(PythonMCPServerProcess);
	PythonMCPServerProcess.Reset();

	OutMessage = TEXT("Stopped ToolPlayMCP Python MCP server.");
	UE_LOG(LogToolPlayMCP, Display, TEXT("%s"), *OutMessage);
	return true;
}

bool FToolPlayMCPModule::IsPythonMCPServerRunning()
{
	return PythonMCPServerProcess.IsValid() && FPlatformProcess::IsProcRunning(PythonMCPServerProcess);
}

void FToolPlayMCPModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& WindowSection = WindowMenu->FindOrAddSection("WindowLayout");
	WindowSection.AddMenuEntry(
		"OpenToolPlayMCPDebugPanel",
		LOCTEXT("OpenDebugPanelLabel", "ToolPlay MCP Debug Panel"),
		LOCTEXT("OpenDebugPanelTooltip", "Open the ToolPlay MCP debug panel."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(DebugTabName);
		}))
	);
}

void FToolPlayMCPModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		DebugTabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SToolPlayMCPDebugPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("DebugPanelTabTitle", "ToolPlay MCP Debug Panel"))
		.SetTooltipText(LOCTEXT("DebugPanelTabTooltip", "Inspect and call ToolPlay MCP bridge tools."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FToolPlayMCPModule::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DebugTabName);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FToolPlayMCPModule, ToolPlayMCP)

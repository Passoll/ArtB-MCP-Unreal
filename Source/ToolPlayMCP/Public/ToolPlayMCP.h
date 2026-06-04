// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

class FToolPlayMCPBridgeServer;

class FToolPlayMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FName GetDebugTabName();

	bool StartPythonMCPServer(FString& OutMessage);
	bool StopPythonMCPServer(FString& OutMessage);
	bool IsPythonMCPServerRunning();

private:
	void RegisterMenus();
	void RegisterTabSpawner();
	void UnregisterTabSpawner();

	TUniquePtr<FToolPlayMCPBridgeServer> BridgeServer;
	FProcHandle PythonMCPServerProcess;
};

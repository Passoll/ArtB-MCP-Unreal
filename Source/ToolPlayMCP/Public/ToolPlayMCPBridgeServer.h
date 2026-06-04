#pragma once

#include "CoreMinimal.h"
#include "TickableEditorObject.h"
#include "ToolRegistry/ToolPlayMCPToolRegistry.h"

class FSocket;
class FTcpListener;
class FJsonObject;
struct FIPv4Endpoint;

class FToolPlayMCPBridgeServer final : public FTickableEditorObject
{
public:
	FToolPlayMCPBridgeServer();
	virtual ~FToolPlayMCPBridgeServer() override;

	bool Start(uint16 InPort = 55557);
	void Stop();
	bool IsRunning() const;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	static const TArray<FToolPlayMCPBridgeCommandSpec>& GetCommandSpecs();
	static FString ExecuteRequestJson(const FString& RequestJson);

private:
	bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);
	void ProcessClientSocket(FSocket* ClientSocket);
	static FString HandleRequestJson(const FString& RequestJson);
	static TSharedPtr<FJsonObject> HandleCommand(const TSharedPtr<FJsonObject>& Request);
	static FString BuildResponseJson(bool bOk, const TSharedPtr<FJsonObject>& Result, const FString& Error);

	TUniquePtr<FTcpListener> Listener;
	TArray<FSocket*> ClientSockets;
	TMap<FSocket*, FString> PendingBuffers;
	uint16 Port = 55557;
};

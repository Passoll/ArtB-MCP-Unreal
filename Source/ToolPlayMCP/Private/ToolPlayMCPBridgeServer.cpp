#include "ToolPlayMCPBridgeServer.h"

#include "Common/TcpListener.h"
#include "Blueprint/ToolPlayMCPBlueprintService.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Graph/ToolPlayMCPSelectionService.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Material/ToolPlayMCPMaterialService.h"
#include "Niagara/ToolPlayMCPNiagaraModuleService.h"
#include "Niagara/ToolPlayMCPNiagaraSystemExporter.h"
#include "Niagara/ToolPlayMCPNiagaraSystemService.h"
#include "Services/ToolPlayMCPAssetService.h"
#include "Niagara/ToolPlayMCPNiagaraCatalog.h"
#include "ToolRegistry/ToolPlayMCPToolRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogToolPlayMCPBridge, Log, All);

namespace
{
	TSharedPtr<FJsonObject> ParseJsonObjectPayload(const FString& Json)
	{
		TSharedPtr<FJsonObject> Payload;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (FJsonSerializer::Deserialize(Reader, Payload) && Payload.IsValid())
		{
			return Payload;
		}

		return nullptr;
	}

	FString JsonObjectPayloadToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

}

FToolPlayMCPBridgeServer::FToolPlayMCPBridgeServer() = default;

FToolPlayMCPBridgeServer::~FToolPlayMCPBridgeServer()
{
	Stop();
}

bool FToolPlayMCPBridgeServer::Start(uint16 InPort)
{
	if (IsRunning())
	{
		return true;
	}

	Port = InPort;
	const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), Port);
	Listener = MakeUnique<FTcpListener>(Endpoint, FTimespan::FromMilliseconds(50), true);
	Listener->OnConnectionAccepted().BindRaw(this, &FToolPlayMCPBridgeServer::HandleConnectionAccepted);

	const bool bStarted = Listener->Init();
	if (!bStarted)
	{
		Listener.Reset();
		UE_LOG(LogToolPlayMCPBridge, Error, TEXT("Bridge failed to listen on 127.0.0.1:%d"), Port);
		return false;
	}

	UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Bridge listening on 127.0.0.1:%d"), Port);
	return true;
}

void FToolPlayMCPBridgeServer::Stop()
{
	if (Listener.IsValid())
	{
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Bridge stopping. Closing %d client socket(s)."), ClientSockets.Num());
	}

	for (FSocket* Socket : ClientSockets)
	{
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		}
	}

	ClientSockets.Reset();
	PendingBuffers.Reset();
	Listener.Reset();
}

bool FToolPlayMCPBridgeServer::IsRunning() const
{
	return Listener.IsValid();
}

void FToolPlayMCPBridgeServer::Tick(float DeltaTime)
{
	for (int32 Index = ClientSockets.Num() - 1; Index >= 0; --Index)
	{
		FSocket* ClientSocket = ClientSockets[Index];
		if (!ClientSocket)
		{
			ClientSockets.RemoveAtSwap(Index);
			continue;
		}

		ESocketConnectionState ConnectionState = ClientSocket->GetConnectionState();
		if (ConnectionState == SCS_NotConnected || ConnectionState == SCS_ConnectionError)
		{
			UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Client disconnected."));
			PendingBuffers.Remove(ClientSocket);
			ClientSocket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			ClientSockets.RemoveAtSwap(Index);
			continue;
		}

		ProcessClientSocket(ClientSocket);
	}
}

TStatId FToolPlayMCPBridgeServer::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(FToolPlayMCPBridgeServer, STATGROUP_Tickables);
}

bool FToolPlayMCPBridgeServer::IsTickable() const
{
	return IsRunning();
}

const TArray<FToolPlayMCPBridgeCommandSpec>& FToolPlayMCPBridgeServer::GetCommandSpecs()
{
	return FToolPlayMCPToolRegistry::GetCommandSpecs();
}

FString FToolPlayMCPBridgeServer::ExecuteRequestJson(const FString& RequestJson)
{
	return HandleRequestJson(RequestJson);
}

bool FToolPlayMCPBridgeServer::HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	if (!ClientSocket)
	{
		return false;
	}

	ClientSocket->SetNonBlocking(true);
	ClientSockets.Add(ClientSocket);
	PendingBuffers.Add(ClientSocket, FString());
	UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Accepted client connection from %s"), *ClientEndpoint.ToString());
	return true;
}

void FToolPlayMCPBridgeServer::ProcessClientSocket(FSocket* ClientSocket)
{
	uint32 PendingDataSize = 0;
	while (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
	{
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(FMath::Min(PendingDataSize, 65536u));

		int32 BytesRead = 0;
		if (!ClientSocket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead) || BytesRead <= 0)
		{
			return;
		}

		const FUTF8ToTCHAR ConvertedChunk(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), BytesRead);
		const FString Chunk(ConvertedChunk.Length(), ConvertedChunk.Get());
		FString& PendingBuffer = PendingBuffers.FindOrAdd(ClientSocket);
		PendingBuffer += Chunk;

		FString Line;
		FString Remainder;
		while (PendingBuffer.Split(TEXT("\n"), &Line, &Remainder))
		{
			PendingBuffer = Remainder;
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty())
			{
				continue;
			}

			UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Received request: %s"), *Line);
			const FString Response = HandleRequestJson(Line) + TEXT("\n");
			UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Sending response: %s"), *Response.Left(2048));
			FTCHARToUTF8 Utf8Response(*Response);
			int32 BytesSent = 0;
			ClientSocket->Send(reinterpret_cast<const uint8*>(Utf8Response.Get()), Utf8Response.Length(), BytesSent);
		}
	}
}

FString FToolPlayMCPBridgeServer::HandleRequestJson(const FString& RequestJson)
{
	TSharedPtr<FJsonObject> Request;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
	if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
	{
		UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("Invalid JSON request: %s"), *RequestJson);
		return BuildResponseJson(false, nullptr, TEXT("Invalid JSON request."));
	}

	TSharedPtr<FJsonObject> Result = HandleCommand(Request);
	if (!Result.IsValid())
	{
		return BuildResponseJson(false, nullptr, TEXT("Command failed."));
	}

	return BuildResponseJson(true, Result, FString());
}

TSharedPtr<FJsonObject> FToolPlayMCPBridgeServer::HandleCommand(const TSharedPtr<FJsonObject>& Request)
{
	FString Command;
	if (!Request->TryGetStringField(TEXT("command"), Command))
	{
		UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("Request missing command field."));
		return nullptr;
	}

	UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Handling command: %s"), *Command);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Command == TEXT("ping"))
	{
		Result->SetStringField(TEXT("message"), TEXT("pong"));
		return Result;
	}

	if (Command == TEXT("list_tools"))
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FToolPlayMCPBridgeCommandSpec& Spec : GetCommandSpecs())
		{
			Tools.Add(MakeShared<FJsonValueObject>(FToolPlayMCPToolRegistry::CommandSpecToJson(Spec)));
		}
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetArrayField(TEXT("tools"), Tools);
		Result->SetObjectField(TEXT("toolsets"), FToolPlayMCPToolRegistry::BuildToolsetsJson());
		return Result;
	}

	if (Command == TEXT("list_toolsets"))
	{
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetObjectField(TEXT("toolsets"), FToolPlayMCPToolRegistry::BuildToolsetsJson());
		return Result;
	}

	if (Command == TEXT("describe_toolset"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString Domain;
		(*Params)->TryGetStringField(TEXT("domain"), Domain);
		TSharedRef<FJsonObject> Description = MakeShared<FJsonObject>();
		FString Error;
		if (!FToolPlayMCPToolRegistry::BuildToolsetDescriptionJson(Domain, Description, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		Result->SetObjectField(TEXT("toolset"), Description);
		return Result;
	}

	if (Command == TEXT("get_selected_graph_nodes"))
	{
		FString Json;
		FString Error;
		if (!FToolPlayMCPSelectionService::ExportSelectedGraphNodes(Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("get_selected_graph_nodes failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("selection"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("selection_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_selection"))
	{
		FString Json;
		FString Error;
		if (!FToolPlayMCPSelectionService::ExportSelection(Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("get_selection failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("selection"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("selection_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("export_material_compact"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("export_material_compact missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Exporting compact material graph for asset_path=%s"), *AssetPath);

		FString Json;
		FString SessionId;
		FString SavedPath;
		FString Error;
		if (!FToolPlayMCPMaterialService::ExportCompactByPath(AssetPath, Json, SessionId, SavedPath, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("export_material_compact failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("session_id"), SessionId);
		Result->SetStringField(TEXT("saved_path"), SavedPath);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("export_material_compact succeeded. Session=%s SavedPath=%s"), *SessionId, *SavedPath);

		TSharedPtr<FJsonObject> CompactGraph;
		const TSharedRef<TJsonReader<>> GraphReader = TJsonReaderFactory<>::Create(Json);
		if (FJsonSerializer::Deserialize(GraphReader, CompactGraph) && CompactGraph.IsValid())
		{
			Result->SetObjectField(TEXT("graph"), CompactGraph.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("graph_json"), Json);
		}

		return Result;
	}

	if (Command == TEXT("export_blueprint_compact"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);

		FString Json;
		FString SessionId;
		FString SavedPath;
		FString Error;
		if (!FToolPlayMCPBlueprintService::ExportBlueprintByPath(AssetPath, Json, SessionId, SavedPath, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("session_id"), SessionId);
		Result->SetStringField(TEXT("saved_path"), SavedPath);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("blueprint"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("blueprint_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("list_blueprint_variables") || Command == TEXT("add_blueprint_member_variable") || Command == TEXT("set_blueprint_variable_default") || Command == TEXT("add_blueprint_function_call_node") || Command == TEXT("add_blueprint_custom_event_node") || Command == TEXT("add_blueprint_variable_get_node") || Command == TEXT("add_blueprint_variable_set_node") || Command == TEXT("set_blueprint_pin_default") || Command == TEXT("connect_blueprint_pins") || Command == TEXT("disconnect_blueprint_pin") || Command == TEXT("remove_blueprint_node") || Command == TEXT("compile_blueprint"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString Json;
		FString Error;
		bool bSucceeded = false;
		if (Command == TEXT("list_blueprint_variables"))
		{
			FString AssetPath;
			(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
			bSucceeded = FToolPlayMCPBlueprintService::ListVariables(AssetPath, Json, Error);
		}
		else if (Command == TEXT("add_blueprint_member_variable"))
		{
			FString AssetPath;
			FString VariableName;
			FString TypeName;
			FString DefaultValue;
			FString Category;
			(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
			(*Params)->TryGetStringField(TEXT("variable_name"), VariableName);
			(*Params)->TryGetStringField(TEXT("type"), TypeName);
			(*Params)->TryGetStringField(TEXT("default"), DefaultValue);
			(*Params)->TryGetStringField(TEXT("category"), Category);
			bSucceeded = FToolPlayMCPBlueprintService::AddMemberVariable(AssetPath, VariableName, TypeName, DefaultValue, Category, Json, Error);
		}
		else if (Command == TEXT("set_blueprint_variable_default"))
		{
			FString AssetPath;
			FString VariableName;
			FString DefaultValue;
			(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
			(*Params)->TryGetStringField(TEXT("variable_name"), VariableName);
			(*Params)->TryGetStringField(TEXT("default"), DefaultValue);
			bSucceeded = FToolPlayMCPBlueprintService::SetMemberVariableDefault(AssetPath, VariableName, DefaultValue, Json, Error);
		}
		else if (Command == TEXT("add_blueprint_function_call_node"))
		{
			FString SessionId;
			FString GraphAlias;
			FString FunctionPath;
			int32 X = 0;
			int32 Y = 0;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("graph"), GraphAlias);
			(*Params)->TryGetStringField(TEXT("function_path"), FunctionPath);
			(*Params)->TryGetNumberField(TEXT("x"), X);
			(*Params)->TryGetNumberField(TEXT("y"), Y);
			bSucceeded = FToolPlayMCPBlueprintService::AddFunctionCallNode(SessionId, GraphAlias, FunctionPath, X, Y, Json, Error);
		}
		else if (Command == TEXT("add_blueprint_custom_event_node"))
		{
			FString SessionId;
			FString GraphAlias;
			FString EventName;
			int32 X = 0;
			int32 Y = 0;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("graph"), GraphAlias);
			(*Params)->TryGetStringField(TEXT("event_name"), EventName);
			(*Params)->TryGetNumberField(TEXT("x"), X);
			(*Params)->TryGetNumberField(TEXT("y"), Y);
			bSucceeded = FToolPlayMCPBlueprintService::AddCustomEventNode(SessionId, GraphAlias, EventName, X, Y, Json, Error);
		}
		else if (Command == TEXT("add_blueprint_variable_get_node"))
		{
			FString SessionId;
			FString GraphAlias;
			FString VariableName;
			int32 X = 0;
			int32 Y = 0;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("graph"), GraphAlias);
			(*Params)->TryGetStringField(TEXT("variable_name"), VariableName);
			(*Params)->TryGetNumberField(TEXT("x"), X);
			(*Params)->TryGetNumberField(TEXT("y"), Y);
			bSucceeded = FToolPlayMCPBlueprintService::AddVariableGetNode(SessionId, GraphAlias, VariableName, X, Y, Json, Error);
		}
		else if (Command == TEXT("add_blueprint_variable_set_node"))
		{
			FString SessionId;
			FString GraphAlias;
			FString VariableName;
			int32 X = 0;
			int32 Y = 0;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("graph"), GraphAlias);
			(*Params)->TryGetStringField(TEXT("variable_name"), VariableName);
			(*Params)->TryGetNumberField(TEXT("x"), X);
			(*Params)->TryGetNumberField(TEXT("y"), Y);
			bSucceeded = FToolPlayMCPBlueprintService::AddVariableSetNode(SessionId, GraphAlias, VariableName, X, Y, Json, Error);
		}
		else if (Command == TEXT("set_blueprint_pin_default"))
		{
			FString SessionId;
			FString NodeAlias;
			FString PinName;
			FString DefaultValue;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("node"), NodeAlias);
			(*Params)->TryGetStringField(TEXT("pin"), PinName);
			(*Params)->TryGetStringField(TEXT("default"), DefaultValue);
			bSucceeded = FToolPlayMCPBlueprintService::SetPinDefault(SessionId, NodeAlias, PinName, DefaultValue, Json, Error);
		}
		else if (Command == TEXT("connect_blueprint_pins"))
		{
			FString SessionId;
			FString FromNode;
			FString FromPin;
			FString ToNode;
			FString ToPin;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("from_node"), FromNode);
			(*Params)->TryGetStringField(TEXT("from_pin"), FromPin);
			(*Params)->TryGetStringField(TEXT("to_node"), ToNode);
			(*Params)->TryGetStringField(TEXT("to_pin"), ToPin);
			bSucceeded = FToolPlayMCPBlueprintService::ConnectPins(SessionId, FromNode, FromPin, ToNode, ToPin, Json, Error);
		}
		else if (Command == TEXT("disconnect_blueprint_pin"))
		{
			FString SessionId;
			FString NodeAlias;
			FString PinName;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("node"), NodeAlias);
			(*Params)->TryGetStringField(TEXT("pin"), PinName);
			bSucceeded = FToolPlayMCPBlueprintService::DisconnectPin(SessionId, NodeAlias, PinName, Json, Error);
		}
		else if (Command == TEXT("remove_blueprint_node"))
		{
			FString SessionId;
			FString NodeAlias;
			(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
			(*Params)->TryGetStringField(TEXT("node"), NodeAlias);
			bSucceeded = FToolPlayMCPBlueprintService::RemoveNode(SessionId, NodeAlias, Json, Error);
		}
		else
		{
			FString AssetPath;
			(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
			bSucceeded = FToolPlayMCPBlueprintService::CompileBlueprint(AssetPath, Json, Error);
		}

		if (!bSucceeded)
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("data"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("export_niagara_system"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("export_niagara_system missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Exporting Niagara system for asset_path=%s"), *AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemExporter::ExportSystemByPath(AssetPath, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("export_niagara_system failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_niagara_compile_status"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		bool bForce = false;
		bool bWait = true;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetBoolField(TEXT("force"), bForce);
		(*Params)->TryGetBoolField(TEXT("wait"), bWait);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemExporter::ExportCompileStatusByPath(AssetPath, bForce, bWait, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("diagnostics"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("diagnostics_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_niagara_stack_issues"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemExporter::ExportStackIssuesByPath(AssetPath, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("diagnostics"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("diagnostics_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_niagara_diagnostics"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		bool bForce = false;
		bool bWait = true;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetBoolField(TEXT("force"), bForce);
		(*Params)->TryGetBoolField(TEXT("wait"), bWait);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemExporter::ExportDiagnosticsByPath(AssetPath, bForce, bWait, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("diagnostics"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("diagnostics_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("create_niagara_system"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString PackagePath;
		FString AssetName;
		FString TemplateAssetPath;
		(*Params)->TryGetStringField(TEXT("package_path"), PackagePath);
		(*Params)->TryGetStringField(TEXT("asset_name"), AssetName);
		(*Params)->TryGetStringField(TEXT("template_asset_path"), TemplateAssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::CreateSystem(PackagePath, AssetName, TemplateAssetPath, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("add_niagara_emitter"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString EmitterAssetPath;
		FString EmitterName;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter_name"), EmitterName);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::AddEmitter(SystemAssetPath, EmitterAssetPath, EmitterName, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("add_niagara_default_emitter"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString EmitterName;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter_name"), EmitterName);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::AddDefaultEmitter(SystemAssetPath, EmitterName, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			return Payload;
		}
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("result_json"), Json);
		return Result;
	}

	if (Command == TEXT("set_niagara_emitter_sim_target"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString EmitterAlias;
		FString SimTarget;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter"), EmitterAlias);
		(*Params)->TryGetStringField(TEXT("sim_target"), SimTarget);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::SetEmitterSimTarget(SystemAssetPath, EmitterAlias, SimTarget, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("remove_niagara_user_parameter"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString UserParameter;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("user_parameter"), UserParameter);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::RemoveUserParameter(SystemAssetPath, UserParameter, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("configure_niagara_sprite_renderer"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString EmitterAlias;
		FString FacingMode;
		FString Alignment;
		int32 RendererIndex = 0;
		double PivotU = 0.5;
		double PivotV = 0.5;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter"), EmitterAlias);
		(*Params)->TryGetStringField(TEXT("facing_mode"), FacingMode);
		(*Params)->TryGetStringField(TEXT("alignment"), Alignment);
		(*Params)->TryGetNumberField(TEXT("renderer_index"), RendererIndex);
		(*Params)->TryGetNumberField(TEXT("pivot_u"), PivotU);
		(*Params)->TryGetNumberField(TEXT("pivot_v"), PivotV);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraSystemService::ConfigureSpriteRenderer(
			SystemAssetPath,
			EmitterAlias,
			RendererIndex,
			FacingMode,
			Alignment,
			static_cast<float>(PivotU),
			static_cast<float>(PivotV),
			Json,
			Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("system"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("system_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("list_niagara_renderers") || Command == TEXT("get_niagara_renderer_schema") || Command == TEXT("add_niagara_renderer") || Command == TEXT("remove_niagara_renderer") || Command == TEXT("set_niagara_renderer_property") ||
		Command == TEXT("list_niagara_simulation_stages") || Command == TEXT("add_niagara_simulation_stage") || Command == TEXT("remove_niagara_simulation_stage") || Command == TEXT("move_niagara_simulation_stage") || Command == TEXT("set_niagara_simulation_stage_property"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SystemAssetPath;
		FString EmitterAlias;
		FString RendererType;
		FString MeshAssetPath;
		FString Property;
		FString Value;
		FString StageName;
		int32 RendererIndex = 0;
		int32 StageIndex = 0;
		int32 TargetIndex = INDEX_NONE;
		(*Params)->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath);
		(*Params)->TryGetStringField(TEXT("emitter"), EmitterAlias);
		(*Params)->TryGetStringField(TEXT("renderer_type"), RendererType);
		(*Params)->TryGetStringField(TEXT("mesh_asset_path"), MeshAssetPath);
		(*Params)->TryGetStringField(TEXT("property"), Property);
		(*Params)->TryGetStringField(TEXT("value"), Value);
		(*Params)->TryGetStringField(TEXT("stage_name"), StageName);
		(*Params)->TryGetNumberField(TEXT("renderer_index"), RendererIndex);
		(*Params)->TryGetNumberField(TEXT("stage_index"), StageIndex);
		(*Params)->TryGetNumberField(TEXT("target_index"), TargetIndex);

		if (Value.IsEmpty())
		{
			const TSharedPtr<FJsonValue> ValueField = (*Params)->TryGetField(TEXT("value"));
			if (ValueField.IsValid() && !ValueField->IsNull())
			{
				if (ValueField->Type == EJson::String)
				{
					Value = ValueField->AsString();
				}
				else
				{
					const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Value);
					FJsonSerializer::Serialize(ValueField.ToSharedRef(), TEXT(""), Writer);
				}
			}
		}

		FString Json;
		FString Error;
		bool bSucceeded = false;
		if (Command == TEXT("list_niagara_renderers"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::ListRenderers(SystemAssetPath, EmitterAlias, Json, Error);
		}
		else if (Command == TEXT("get_niagara_renderer_schema"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::GetRendererSchema(RendererType, Json, Error);
		}
		else if (Command == TEXT("add_niagara_renderer"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::AddRenderer(SystemAssetPath, EmitterAlias, RendererType, TargetIndex, MeshAssetPath, Json, Error);
		}
		else if (Command == TEXT("remove_niagara_renderer"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::RemoveRenderer(SystemAssetPath, EmitterAlias, RendererIndex, Json, Error);
		}
		else if (Command == TEXT("set_niagara_renderer_property"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::SetRendererProperty(SystemAssetPath, EmitterAlias, RendererIndex, Property, Value, Json, Error);
		}
		else if (Command == TEXT("list_niagara_simulation_stages"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::ListSimulationStages(SystemAssetPath, EmitterAlias, Json, Error);
		}
		else if (Command == TEXT("add_niagara_simulation_stage"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::AddSimulationStage(SystemAssetPath, EmitterAlias, StageName, TargetIndex, Json, Error);
		}
		else if (Command == TEXT("remove_niagara_simulation_stage"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::RemoveSimulationStage(SystemAssetPath, EmitterAlias, StageIndex, Json, Error);
		}
		else if (Command == TEXT("move_niagara_simulation_stage"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::MoveSimulationStage(SystemAssetPath, EmitterAlias, StageIndex, TargetIndex, Json, Error);
		}
		else if (Command == TEXT("set_niagara_simulation_stage_property"))
		{
			bSucceeded = FToolPlayMCPNiagaraSystemService::SetSimulationStageProperty(SystemAssetPath, EmitterAlias, StageIndex, Property, Value, Json, Error);
		}

		if (!bSucceeded)
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("result"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("result_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("search_niagara_modules"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString Query;
		FString Usage;
		FString Source;
		int32 Limit = 20;
		(*Params)->TryGetStringField(TEXT("query"), Query);
		(*Params)->TryGetStringField(TEXT("usage"), Usage);
		(*Params)->TryGetStringField(TEXT("source"), Source);
		(*Params)->TryGetNumberField(TEXT("limit"), Limit);

		FString Json;
		FString Error;
		if (!FToolPlayMCPNiagaraCatalog::SearchScripts(Query, Usage, Source, Limit, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("catalog"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("catalog_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("add_niagara_module") || Command == TEXT("create_niagara_local_module") || Command == TEXT("apply_niagara_module_graph_patch") || Command == TEXT("remove_niagara_module") || Command == TEXT("move_niagara_module") || Command == TEXT("set_niagara_module_enabled") || Command == TEXT("list_niagara_module_inputs") || Command == TEXT("get_niagara_module_input_override") || Command == TEXT("set_niagara_module_input") || Command == TEXT("set_niagara_static_switch") || Command == TEXT("set_niagara_module_object_input") || Command == TEXT("bind_niagara_module_input_to_user_param"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString SessionId;
		FString ModuleAlias;
		(*Params)->TryGetStringField(TEXT("session_id"), SessionId);
		(*Params)->TryGetStringField(TEXT("module"), ModuleAlias);

		FString Json;
		FString Error;
		bool bSucceeded = false;
		if (Command == TEXT("add_niagara_module"))
		{
			FString TargetStack;
			FString ScriptAssetPath;
			FString SuggestedName;
			int32 TargetIndex = INDEX_NONE;
			(*Params)->TryGetStringField(TEXT("target_stack"), TargetStack);
			(*Params)->TryGetStringField(TEXT("script_asset_path"), ScriptAssetPath);
			(*Params)->TryGetStringField(TEXT("suggested_name"), SuggestedName);
			(*Params)->TryGetNumberField(TEXT("target_index"), TargetIndex);
			bSucceeded = FToolPlayMCPNiagaraModuleService::AddModuleToStack(SessionId, TargetStack, ScriptAssetPath, TargetIndex, SuggestedName, Json, Error);
		}
		else if (Command == TEXT("create_niagara_local_module"))
		{
			FString TargetStack;
			FString ModuleName;
			int32 TargetIndex = INDEX_NONE;
			(*Params)->TryGetStringField(TEXT("target_stack"), TargetStack);
			(*Params)->TryGetStringField(TEXT("module_name"), ModuleName);
			(*Params)->TryGetNumberField(TEXT("target_index"), TargetIndex);
			bSucceeded = FToolPlayMCPNiagaraModuleService::CreateLocalModule(SessionId, TargetStack, TargetIndex, ModuleName, Json, Error);
		}
		else if (Command == TEXT("apply_niagara_module_graph_patch"))
		{
			const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
			if (!(*Params)->TryGetArrayField(TEXT("ops"), Ops) || !Ops)
			{
				Result->SetBoolField(TEXT("ok"), false);
				Result->SetStringField(TEXT("error"), TEXT("Missing ops array."));
				return Result;
			}
			bSucceeded = FToolPlayMCPNiagaraModuleService::ApplyModuleGraphPatch(SessionId, ModuleAlias, *Ops, Json, Error);
		}
		else if (Command == TEXT("remove_niagara_module"))
		{
			bSucceeded = FToolPlayMCPNiagaraModuleService::RemoveModule(SessionId, ModuleAlias, Json, Error);
		}
		else if (Command == TEXT("move_niagara_module"))
		{
			FString TargetStack;
			int32 TargetIndex = INDEX_NONE;
			(*Params)->TryGetStringField(TEXT("target_stack"), TargetStack);
			(*Params)->TryGetNumberField(TEXT("target_index"), TargetIndex);
			bSucceeded = FToolPlayMCPNiagaraModuleService::MoveModule(SessionId, ModuleAlias, TargetStack, TargetIndex, Json, Error);
		}
		else if (Command == TEXT("set_niagara_module_enabled"))
		{
			bool bEnabled = true;
			(*Params)->TryGetBoolField(TEXT("enabled"), bEnabled);
			bSucceeded = FToolPlayMCPNiagaraModuleService::SetModuleEnabled(SessionId, ModuleAlias, bEnabled, Json, Error);
		}
		else if (Command == TEXT("list_niagara_module_inputs"))
		{
			bSucceeded = FToolPlayMCPNiagaraModuleService::ListInputs(SessionId, ModuleAlias, Json, Error);
		}
		else if (Command == TEXT("get_niagara_module_input_override"))
		{
			FString InputName;
			(*Params)->TryGetStringField(TEXT("input"), InputName);
			bSucceeded = FToolPlayMCPNiagaraModuleService::GetInputOverride(SessionId, ModuleAlias, InputName, Json, Error);
		}
		else if (Command == TEXT("set_niagara_module_input"))
		{
			FString InputName;
			FString Value;
			(*Params)->TryGetStringField(TEXT("input"), InputName);
			(*Params)->TryGetStringField(TEXT("value"), Value);
			bSucceeded = FToolPlayMCPNiagaraModuleService::SetInput(SessionId, ModuleAlias, InputName, Value, Json, Error);
		}
		else if (Command == TEXT("set_niagara_static_switch"))
		{
			FString InputName;
			FString Value;
			(*Params)->TryGetStringField(TEXT("input"), InputName);
			(*Params)->TryGetStringField(TEXT("value"), Value);
			bSucceeded = FToolPlayMCPNiagaraModuleService::SetStaticSwitch(SessionId, ModuleAlias, InputName, Value, Json, Error);
		}
		else if (Command == TEXT("set_niagara_module_object_input"))
		{
			FString InputName;
			FString AssetPath;
			(*Params)->TryGetStringField(TEXT("input"), InputName);
			(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
			bSucceeded = FToolPlayMCPNiagaraModuleService::SetObjectInput(SessionId, ModuleAlias, InputName, AssetPath, Json, Error);
		}
		else if (Command == TEXT("bind_niagara_module_input_to_user_param"))
		{
			FString InputName;
			FString UserParameter;
			FString BindingKind = TEXT("auto");
			FString DefaultAssetPath;
			(*Params)->TryGetStringField(TEXT("input"), InputName);
			(*Params)->TryGetStringField(TEXT("user_parameter"), UserParameter);
			(*Params)->TryGetStringField(TEXT("binding_kind"), BindingKind);
			(*Params)->TryGetStringField(TEXT("default_asset_path"), DefaultAssetPath);
			const bool bUseSkeletalMeshBinding = BindingKind.Equals(TEXT("skeletal_mesh"), ESearchCase::IgnoreCase);
			const bool bUseVolumeTextureBinding =
				BindingKind.Equals(TEXT("volume_texture"), ESearchCase::IgnoreCase) ||
				(!BindingKind.Equals(TEXT("value"), ESearchCase::IgnoreCase) && !DefaultAssetPath.IsEmpty());
			bSucceeded = bUseSkeletalMeshBinding
				? FToolPlayMCPNiagaraModuleService::BindSkeletalMeshInputToUserParameter(SessionId, ModuleAlias, InputName, UserParameter, DefaultAssetPath, Json, Error)
				: bUseVolumeTextureBinding
				? FToolPlayMCPNiagaraModuleService::BindVolumeTextureInputToUserParameter(SessionId, ModuleAlias, InputName, UserParameter, DefaultAssetPath, Json, Error)
				: FToolPlayMCPNiagaraModuleService::BindInputToUserParameter(SessionId, ModuleAlias, InputName, UserParameter, Json, Error);
		}

		if (!bSucceeded)
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("data"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("create_material_asset"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("create_material_asset missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString PackagePath;
		FString AssetName;
		(*Params)->TryGetStringField(TEXT("package_path"), PackagePath);
		(*Params)->TryGetStringField(TEXT("asset_name"), AssetName);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::CreateAsset(PackagePath, AssetName, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("create_material_asset failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("asset"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("asset_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("save_asset"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("save_asset missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPAssetService::SaveAssetByPath(AssetPath, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("save_asset failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("asset"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("asset_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("list_material_functions"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("list_material_functions missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Listing material functions for asset_path=%s"), *AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::ListFunctionsByPath(AssetPath, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("list_material_functions failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("data"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("describe_material_function_interface"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("describe_material_function_interface missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString FunctionPath;
		(*Params)->TryGetStringField(TEXT("function_path"), FunctionPath);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Describing material function interface for function_path=%s"), *FunctionPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::DescribeFunctionInterfaceByPath(FunctionPath, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("describe_material_function_interface failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("interface"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("interface_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_material_node_config"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		FString NodeAlias;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetStringField(TEXT("node"), NodeAlias);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::GetNodeConfigByAlias(AssetPath, NodeAlias, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("config"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("config_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("get_material_node_config_schema"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString Kind;
		(*Params)->TryGetStringField(TEXT("kind"), Kind);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::GetNodeConfigSchema(Kind, Json, Error))
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("schema"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("schema_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("trace_material_parameter"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("trace_material_parameter missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		FString ParameterName;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetStringField(TEXT("parameter"), ParameterName);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Tracing material parameter '%s' for asset_path=%s"), *ParameterName, *AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::TraceParameterByPath(AssetPath, ParameterName, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("trace_material_parameter failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("trace"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("trace_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("trace_material_output"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("trace_material_output missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		FString OutputName;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetStringField(TEXT("output"), OutputName);
		UE_LOG(LogToolPlayMCPBridge, Display, TEXT("Tracing material output '%s' for asset_path=%s"), *OutputName, *AssetPath);

		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::TraceOutputByPath(AssetPath, OutputName, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("trace_material_output failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		Result->SetBoolField(TEXT("ok"), true);
		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetObjectField(TEXT("trace"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetStringField(TEXT("trace_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("validate_material_patch") || Command == TEXT("apply_material_patch"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("%s missing params object."), *Command);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		const FString PatchJson = JsonObjectPayloadToString(Params->ToSharedRef());
		FString Json;
		FString Error;
		const bool bSucceeded = Command == TEXT("apply_material_patch")
			? FToolPlayMCPMaterialService::ApplyPatch(PatchJson, Json, Error)
			: FToolPlayMCPMaterialService::ValidatePatch(PatchJson, Json, Error);

		if (!bSucceeded)
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("%s failed: %s"), *Command, *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetBoolField(TEXT("ok"), Payload->GetBoolField(TEXT("ok")));
			Result->SetObjectField(TEXT("patch"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetBoolField(TEXT("ok"), true);
			Result->SetStringField(TEXT("patch_json"), Json);
		}
		return Result;
	}

	if (Command == TEXT("set_material_parameter"))
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("set_material_parameter missing params object."));
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("Missing params object."));
			return Result;
		}

		FString AssetPath;
		FString ParameterName;
		FString ValueType;
		(*Params)->TryGetStringField(TEXT("asset_path"), AssetPath);
		(*Params)->TryGetStringField(TEXT("parameter"), ParameterName);
		(*Params)->TryGetStringField(TEXT("type"), ValueType);
		const TSharedPtr<FJsonValue> Value = (*Params)->TryGetField(TEXT("value"));
		if (AssetPath.IsEmpty() || ParameterName.IsEmpty() || ValueType.IsEmpty() || !Value.IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("set_material_parameter requires asset_path, parameter, type, and value."));
			return Result;
		}

		TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("op"), TEXT("set_parameter"));
		Op->SetStringField(TEXT("parameter"), ParameterName);
		Op->SetStringField(TEXT("value_type"), ValueType);
		Op->SetField(TEXT("value"), Value);

		TArray<TSharedPtr<FJsonValue>> Ops;
		Ops.Add(MakeShared<FJsonValueObject>(Op));
		TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
		Patch->SetStringField(TEXT("asset_path"), AssetPath);
		Patch->SetArrayField(TEXT("ops"), Ops);

		const FString PatchJson = JsonObjectPayloadToString(Patch);
		FString Json;
		FString Error;
		if (!FToolPlayMCPMaterialService::ApplyPatch(PatchJson, Json, Error))
		{
			UE_LOG(LogToolPlayMCPBridge, Error, TEXT("set_material_parameter failed: %s"), *Error);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		}

		if (TSharedPtr<FJsonObject> Payload = ParseJsonObjectPayload(Json))
		{
			Result->SetBoolField(TEXT("ok"), Payload->GetBoolField(TEXT("ok")));
			Result->SetObjectField(TEXT("patch"), Payload.ToSharedRef());
		}
		else
		{
			Result->SetBoolField(TEXT("ok"), true);
			Result->SetStringField(TEXT("patch_json"), Json);
		}
		return Result;
	}

	Result->SetBoolField(TEXT("ok"), false);
	Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command '%s'."), *Command));
	UE_LOG(LogToolPlayMCPBridge, Warning, TEXT("Unknown command: %s"), *Command);
	return Result;
}

FString FToolPlayMCPBridgeServer::BuildResponseJson(bool bOk, const TSharedPtr<FJsonObject>& Result, const FString& Error)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), bOk);
	if (!Error.IsEmpty())
	{
		Response->SetStringField(TEXT("error"), Error);
	}
	if (Result.IsValid())
	{
		Response->SetObjectField(TEXT("result"), Result.ToSharedRef());
	}

	FString Output;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Response, Writer);
	return Output;
}

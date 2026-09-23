// Copyright Jad Deeb. All Rights Reserved.

#include "PanoViewerServer.h"
#include "PanoCaptureModule.h"
#include "HttpPath.h"
#include "HttpRouteHandle.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	constexpr uint32 FirstPort = 8765;
	// The router refuses "/" as a route, so everything lives under this prefix.
	const TCHAR* RoutePrefix = TEXT("/pano");
	constexpr uint32 PortAttempts = 10;

	// Only tours opened through GetViewerUrl are served: /pano/<name>/... maps to the folder registered for <name>.
	TMap<FString, FString> GTourFolders;
	uint32 GPort = 0;
	TSharedPtr<IHttpRouter> GRouter;
	FHttpRouteHandle GRouteHandle;

	FString UrlDecode(const FString& In)
	{
		TArray<uint8> Bytes;
		int32 LiteralStart = 0;
		// Plain characters are converted a whole run at a time, so surrogate pairs (emoji) stay together.
		auto FlushLiteral = [&](int32 End)
		{
			if (End > LiteralStart)
			{
				const FTCHARToUTF8 Utf8(&In[LiteralStart], End - LiteralStart);
				Bytes.Append((const uint8*)Utf8.Get(), Utf8.Length());
			}
		};

		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			if (In[Index] == TEXT('%') && Index + 2 < In.Len() && FChar::IsHexDigit(In[Index + 1]) && FChar::IsHexDigit(In[Index + 2]))
			{
				FlushLiteral(Index);
				Bytes.Add((uint8)FParse::HexDigit(In[Index + 1]) << 4 | (uint8)FParse::HexDigit(In[Index + 2]));
				Index += 2;
				LiteralStart = Index + 1;
			}
		}
		FlushLiteral(In.Len());

		const auto Decoded = StringCast<TCHAR>((const UTF8CHAR*)Bytes.GetData(), Bytes.Num());
		return FString::ConstructFromPtrSize(Decoded.Get(), Decoded.Length());
	}

	FString UrlEncode(const FString& In)
	{
		FString Out;
		const FTCHARToUTF8 Utf8(*In);
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Byte = (uint8)Utf8.Get()[Index];
			if ((Byte < 128 && FChar::IsAlnum((TCHAR)Byte)) || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~')
			{
				Out.AppendChar((TCHAR)Byte);
			}
			else
			{
				Out += FString::Printf(TEXT("%%%02X"), Byte);
			}
		}
		return Out;
	}

	FString GetContentType(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		if (Extension == TEXT("html")) return TEXT("text/html; charset=utf-8");
		if (Extension == TEXT("js")) return TEXT("text/javascript; charset=utf-8");
		if (Extension == TEXT("css")) return TEXT("text/css; charset=utf-8");
		if (Extension == TEXT("json")) return TEXT("application/json; charset=utf-8");
		if (Extension == TEXT("jpg") || Extension == TEXT("jpeg")) return TEXT("image/jpeg");
		if (Extension == TEXT("png")) return TEXT("image/png");
		if (Extension == TEXT("glb")) return TEXT("model/gltf-binary");
		if (Extension == TEXT("gltf")) return TEXT("model/gltf+json");
		if (Extension == TEXT("svg")) return TEXT("image/svg+xml");
		return TEXT("application/octet-stream");
	}

	/**
	 * Only answer requests addressed to this machine by a loopback name. A page on another site that rebinds its
	 * DNS name to 127.0.0.1 still sends its own name as the Host, so it can't read the tours.
	 */
	bool IsLocalHost(const FHttpServerRequest& Request)
	{
		const TArray<FString>* Hosts = Request.Headers.Find(TEXT("Host"));
		if (!Hosts || Hosts->Num() != 1)
		{
			return false;
		}
		const FString Host = (*Hosts)[0].TrimStartAndEnd();
		for (const TCHAR* Name : { TEXT("localhost"), TEXT("127.0.0.1"), TEXT("[::1]") })
		{
			if (Host.Equals(FString::Printf(TEXT("%s:%u"), Name, GPort), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		auto NotFound = [&OnComplete]()
		{
			OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound));
			return true;
		};

		if (!IsLocalHost(Request))
		{
			OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Forbidden));
			return true;
		}

		FString RelativePath = UrlDecode(Request.RelativePath.GetPath());
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (RelativePath.RemoveFromStart(TEXT("/"))) {}

		// <tour name>/<file inside the tour folder>
		FString TourName;
		FString FilePath;
		if (!RelativePath.Split(TEXT("/"), &TourName, &FilePath))
		{
			return NotFound();
		}
		const FString* Folder = GTourFolders.Find(TourName);
		if (!Folder)
		{
			return NotFound();
		}
		if (FilePath.IsEmpty() || FilePath.EndsWith(TEXT("/")))
		{
			FilePath += TEXT("index.html");
		}

		// Only files inside the tour folder: no parent directories, and no ':' (drive letters, alternate data streams).
		const FString FullPath = FPaths::ConvertRelativePathToFull(*Folder / FilePath);
		TArray<uint8> Bytes;
		if (FilePath.Contains(TEXT("..")) || FilePath.Contains(TEXT(":")) || !FullPath.StartsWith(*Folder + TEXT("/"))
			|| !FFileHelper::LoadFileToArray(Bytes, *FullPath, FILEREAD_Silent))
		{
			return NotFound();
		}

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(MoveTemp(Bytes), GetContentType(FullPath));
		// Captures are rewritten in place, so never let the browser show a stale image.
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
		OnComplete(MoveTemp(Response));
		return true;
	}

	/** The address the HTTP server will bind this port to, read the way FHttpServerConfig::GetListenerConfig reads it. */
	FString GetConfiguredBindAddress(uint32 Port)
	{
		const TCHAR* Section = TEXT("HTTPServer.Listeners");
		FString Address = TEXT("localhost"); // the engine's code default (HttpServerConfig.h)
		GConfig->GetString(Section, TEXT("DefaultBindAddress"), Address, GEngineIni);

		TArray<FString> Overrides;
		if (GConfig->GetArray(Section, TEXT("ListenerOverrides"), Overrides, GEngineIni))
		{
			for (FString Entry : Overrides)
			{
				Entry.TrimStartAndEndInline();
				Entry.ReplaceInline(TEXT("("), TEXT(""));
				Entry.ReplaceInline(TEXT(")"), TEXT(""));
				uint32 EntryPort = 0;
				if (FParse::Value(*Entry, TEXT("Port="), EntryPort) && EntryPort == Port)
				{
					FParse::Value(*Entry, TEXT("BindAddress="), Address);
					break;
				}
			}
		}
		return Address;
	}

	bool IsLoopbackAddress(const FString& Address)
	{
		return Address.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || Address.StartsWith(TEXT("127.")) || Address == TEXT("::1");
	}

	bool EnsureServer()
	{
		if (GRouter.IsValid())
		{
			return true;
		}

		FHttpServerModule& HttpServer = FHttpServerModule::Get();
		for (uint32 Port = FirstPort; Port < FirstPort + PortAttempts; ++Port)
		{
			// The tours are private files: never listen beyond this machine, whatever the project's HTTP settings say.
			// The plugin's Config/DefaultEngine.ini pins these ports to localhost; this catches a config that overrides it.
			const FString BindAddress = GetConfiguredBindAddress(Port);
			if (!IsLoopbackAddress(BindAddress))
			{
				UE_LOG(LogPanoCapture, Warning, TEXT("Not serving tours on port %u: the HTTP server config binds it to '%s', not localhost."), Port, *BindAddress);
				continue;
			}

			TSharedPtr<IHttpRouter> Router = HttpServer.GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
			if (!Router.IsValid())
			{
				continue;
			}

			// Catches every path below the prefix: the router falls back to parent paths when nothing more specific is bound.
			const FHttpPath Route(RoutePrefix);
			if (!Route.IsValidPath())
			{
				continue;
			}
			FHttpRouteHandle Handle = Router->BindRoute(Route, EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateStatic(&HandleRequest));
			if (!Handle.IsValid())
			{
				continue;
			}
			HttpServer.StartAllListeners();
			GRouter = Router;
			GRouteHandle = Handle;
			GPort = Port;
			UE_LOG(LogPanoCapture, Log, TEXT("Pano viewer server listening on http://localhost:%u"), Port);
			return true;
		}

		UE_LOG(LogPanoCapture, Error, TEXT("Could not start the pano viewer server on ports %u-%u."), FirstPort, FirstPort + PortAttempts - 1);
		return false;
	}
}

FString PanoViewerServer::GetViewerUrl(const FString& TourFolder)
{
	if (!EnsureServer())
	{
		return FString();
	}

	FString Folder = FPaths::ConvertRelativePathToFull(TourFolder);
	FPaths::NormalizeDirectoryName(Folder);
	const FString Name = FPaths::GetCleanFilename(Folder);
	GTourFolders.Add(Name, Folder);

	return FString::Printf(TEXT("http://localhost:%u%s/%s/index.html"), GPort, RoutePrefix, *UrlEncode(Name));
}

void PanoViewerServer::Shutdown()
{
	// The router belongs to the HTTP server module; let go of it before modules unload rather than at static destruction.
	if (GRouter.IsValid() && GRouteHandle.IsValid())
	{
		GRouter->UnbindRoute(GRouteHandle);
	}
	GRouteHandle.Reset();
	GRouter.Reset();
	GTourFolders.Empty();
	GPort = 0;
}

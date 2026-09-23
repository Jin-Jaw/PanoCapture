// Copyright Jad Deeb. All Rights Reserved.

#include "PanoCaptureModule.h"
#include "PanoViewerServer.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogPanoCapture);

class FPanoCaptureModule : public IModuleInterface
{
public:
	virtual void ShutdownModule() override
	{
		// The viewer server's router is owned by the HTTP server module; release it here, not at static destruction.
		PanoViewerServer::Shutdown();
	}
};

IMPLEMENT_MODULE(FPanoCaptureModule, PanoCapture)

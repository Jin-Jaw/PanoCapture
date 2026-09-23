// Copyright Jad Deeb. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Serves captured tours to the browser from the editor, on localhost only. Browsers block WebGL textures
 * loaded from file:// pages, so the viewer needs to come from a web server. Only tour folders opened through
 * GetViewerUrl are served, and only to requests addressed to localhost.
 */
namespace PanoViewerServer
{
	/** Starts the server if needed and returns the viewer URL for a tour folder, or an empty string on failure. */
	PANOCAPTURE_API FString GetViewerUrl(const FString& TourFolder);

	/** Unbinds the route and releases the router. Called when the module shuts down. */
	void Shutdown();
}

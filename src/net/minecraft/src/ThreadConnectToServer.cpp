#include "ThreadConnectToServer.h"

#include "platform/Log.h"
#include <exception>
#include <iostream>
#include <stdexcept>

#include "GuiConnecting.h"
#include "Minecraft.h"
#include "NetClientHandler.h"
#include "Packet2Handshake.h"
#include "Session.h"
#include "java/String.h"

ThreadConnectToServer::ThreadConnectToServer(GuiConnecting *guiconnecting, Minecraft *minecraft, const std::string &s, int_t i)
	: mc(minecraft)
	, hostName(s)
	, port(i)
{
	(void)guiconnecting;
}

ThreadConnectToServer::~ThreadConnectToServer()
{
	cancel();
	if (worker.joinable() && !worker.isCurrent())
		worker.join();
	std::lock_guard<std::mutex> guard(resultLock);
	if (resultHandler != nullptr)
	{
		resultHandler->disconnect();
		delete resultHandler;
		resultHandler = nullptr;
	}
}

void ThreadConnectToServer::start()
{
#ifdef DSI_PLATFORM
	// No thread backend exists on this platform (see NetworkManager.h's own
	// DSI_PLATFORM comment) -- worker.start() below would silently never run
	// run() at all (platform/compat's shadow std::thread never invokes its
	// callable; confirmed real-hardware symptom: GuiConnecting's
	// "Connecting..." screen sat there forever with nothing to poll, since
	// resultHandler/errorPending never left their initial state). Run
	// synchronously instead: the whole connection attempt (WiFi association
	// wait, DNS, TCP connect) is already a single blocking call chain by
	// design -- see DsiNetworkSocket::connect()'s own comment -- matching
	// vanilla Minecraft's own blocking-connect-with-a-spinner UX, just
	// without a background thread keeping the frame animating while it
	// runs. run() already catches every exception it can throw internally
	// (sets resultError/errorPending instead), so nothing here needs its
	// own try/catch.
	run();
#else
	if (!worker.start(&ThreadConnectToServer::wiiThreadEntry, this, 32 * 1024, 64))
		throw std::runtime_error("Could not create connection thread");
#endif
}

void ThreadConnectToServer::cancel()
{
	cancelled.store(true);
}

NetClientHandler *ThreadConnectToServer::takeHandler()
{
	std::lock_guard<std::mutex> guard(resultLock);
	NetClientHandler *handler = resultHandler;
	resultHandler = nullptr;
	return handler;
}

bool ThreadConnectToServer::takeError(std::string &message)
{
	std::lock_guard<std::mutex> guard(resultLock);
	if (!errorPending)
		return false;
	message = resultError;
	errorPending = false;
	return true;
}

void *ThreadConnectToServer::wiiThreadEntry(void *argument)
{
	static_cast<ThreadConnectToServer *>(argument)->run();
	return nullptr;
}

void ThreadConnectToServer::run()
{
	try
	{
		NetClientHandler *handler = new NetClientHandler(mc, hostName, port);
		if (cancelled.load())
		{
			handler->disconnect();
			delete handler;
			return;
		}
		handler->addToSendQueue(new Packet2Handshake(mc->session->username));
		std::lock_guard<std::mutex> guard(resultLock);
		resultHandler = handler;
	}
	catch (std::exception &exception)
	{
		if (cancelled.load())
			return;
		MC_LOG_ERROR("game", "%s\n", exception.what());
		std::lock_guard<std::mutex> guard(resultLock);
		resultError = exception.what();
		errorPending = true;
	}
}

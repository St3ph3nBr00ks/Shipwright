#include "Network.h"
#include <spdlog/spdlog.h>
#include <libultraship/libultraship.h>

// MARK: - Public

void Network::Enable(const char* host, uint16_t port) {
#ifdef ENABLE_REMOTE_CONTROL
    if (isEnabled) {
        return;
    }

    if (SDLNet_ResolveHost(&networkAddress, host, port) == -1) {
        SPDLOG_ERROR("[Network] SDLNet_ResolveHost: {}", SDLNet_GetError());
    }

    isEnabled = true;

    // First check if there is a thread running, if so, join it
    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    receiveThread = std::thread(&Network::ReceiveFromServer, this);
#endif
}

void Network::Disable() {
    if (!isEnabled) {
        return;
    }

    isEnabled = false;
    receiveThread.join();
}

void Network::OnIncomingData(char payload[512]) {
}

void Network::OnIncomingJson(nlohmann::json payload) {
}

void Network::OnConnected() {
}

void Network::OnDisconnected() {
}

void Network::ProcessOutgoingPackets() {
}

void Network::SendDataToRemote(const char* payload) {
#ifdef ENABLE_REMOTE_CONTROL
    SPDLOG_DEBUG("[Network] Sending data: {}", payload);
    SDLNet_TCP_Send(networkSocket, payload, strlen(payload) + 1);
#endif
}

void Network::SendJsonToRemote(nlohmann::json payload) {
    SendDataToRemote(payload.dump().c_str());
}

// MARK: - Private

void Network::ReceiveFromServer() {
#ifdef ENABLE_REMOTE_CONTROL
    while (isEnabled) {
        while (!isConnected && isEnabled) {
            SPDLOG_TRACE("[Network] Attempting to make connection to server...");
            networkSocket = SDLNet_TCP_Open(&networkAddress);

            if (networkSocket) {
                isConnected = true;
                receivedData.clear();
                SPDLOG_INFO("[Network] Connection to server established!");

                OnConnected();
                break;
            }
        }

        SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(1);
        if (networkSocket) {
            SDLNet_TCP_AddSocket(socketSet, networkSocket);
        }

        // Log 750 (2026-07-28) surfaced a silent-exit case where the
        // receive loop's while-condition failed WITHOUT any preceding
        // error log — leaving `Ending receiving thread` as the only
        // trace. Track the reason so post-hoc triage can distinguish
        // among the exit paths. Defaults to loopConditionFailed;
        // overwritten on each `break` site.
        const char* exitReason = "loopConditionFailed";

        // Listen to socket messages
        while (isConnected && networkSocket && isEnabled) {
            // we check first if socket has data, to not block in the TCP_Recv
            int socketsReady = SDLNet_CheckSockets(socketSet, 0);

            if (socketsReady == -1) {
                SPDLOG_ERROR("[Network] SDLNet_CheckSockets: {}", SDLNet_GetError());
                exitReason = "checkSocketsError";
                break;
            }

            // Always process outgoing packets
            ProcessOutgoingPackets();

            if (socketsReady == 0) {
                // No incoming data
                continue;
            }

            char remoteDataReceived[512];
            memset(remoteDataReceived, 0, sizeof(remoteDataReceived));
            int len = SDLNet_TCP_Recv(networkSocket, &remoteDataReceived, sizeof(remoteDataReceived));
            if (!len || !networkSocket || len == -1) {
                // Distinguish peer graceful close (len==0) from actual
                // error (len<0) vs external socket-null (race). All three
                // hit the same `break` today; separating them lets us
                // identify server-side kicks vs local socket issues.
                if (!networkSocket) {
                    SPDLOG_ERROR("[Network] SDLNet_TCP_Recv: networkSocket became null externally");
                    exitReason = "externalSocketNull";
                } else if (len == 0) {
                    SPDLOG_ERROR("[Network] SDLNet_TCP_Recv: peer graceful close (len=0). SDL error='{}'",
                                 SDLNet_GetError());
                    exitReason = "peerGracefulClose";
                } else if (len == -1) {
                    SPDLOG_ERROR("[Network] SDLNet_TCP_Recv: recv error (len=-1). SDL error='{}'",
                                 SDLNet_GetError());
                    exitReason = "recvError";
                } else {
                    SPDLOG_ERROR("[Network] SDLNet_TCP_Recv: unexpected exit (len={}). SDL error='{}'",
                                 len, SDLNet_GetError());
                    exitReason = "recvUnexpected";
                }
                break;
            }

            HandleRemoteData(remoteDataReceived);

            receivedData.append(remoteDataReceived, len);

            // Proess all complete packets
            size_t delimiterPos = receivedData.find('\0');
            while (delimiterPos != std::string::npos) {
                // Extract the complete packet until the delimiter
                std::string packet = receivedData.substr(0, delimiterPos);
                // Remove the packet (including the delimiter) from the received data
                receivedData.erase(0, delimiterPos + 1);
                HandleRemoteJson(packet);
                // Find the next delimiter
                delimiterPos = receivedData.find('\0');
            }
        }

        if (socketSet) {
            SDLNet_FreeSocketSet(socketSet);
        }

        // Diagnostic snapshot of the three loop-condition flags at
        // exit time. If exitReason=="loopConditionFailed", one of these
        // flags flipped externally without going through the
        // error-log break paths above — which is the log-750 mystery.
        // A WARN log fires in that case so future triage catches it.
        const bool wasConnected = isConnected;
        const bool wasEnabled   = isEnabled;
        const bool socketWasNull = (networkSocket == nullptr);
        if (std::string(exitReason) == "loopConditionFailed") {
            SPDLOG_WARN("[Network] Receive loop exited silently — exitReason={} "
                        "isConnected={} isEnabled={} networkSocketNull={}. "
                        "One of the loop-condition flags was mutated externally "
                        "(no SDLNet error path fired). If this recurs during "
                        "gameplay, capture what other Anchor / scene-load "
                        "activity was happening in the ~5s window before this line.",
                        exitReason, wasConnected, wasEnabled, socketWasNull);
        }

        if (isConnected) {
            SDLNet_TCP_Close(networkSocket);
            networkSocket = nullptr;
            isConnected = false;
            receivedData.clear();
            OnDisconnected();
            SPDLOG_INFO("[Network] Ending receiving thread... exitReason={} isEnabled={}",
                        exitReason, wasEnabled);
        } else {
            // If isConnected was already false at cleanup, the normal
            // "Ending receiving thread" branch is skipped — historically
            // silent. Log so triage sees SOMETHING when the loop exits
            // via external isConnected mutation.
            SPDLOG_WARN("[Network] Receive loop cleanup skipped socket-close "
                        "(isConnected was already false). exitReason={} isEnabled={} "
                        "socketNull={}",
                        exitReason, wasEnabled, socketWasNull);
        }
    }
#endif
}

void Network::HandleRemoteData(char payload[512]) {
    OnIncomingData(payload);
}

void Network::HandleRemoteJson(std::string payload) {
    SPDLOG_DEBUG("[Network] Received json: {}", payload);
    nlohmann::json jsonPayload;
    try {
        jsonPayload = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Failed to parse json: \n{}\n{}\n", payload, e.what());
        return;
    }

    try {
        OnIncomingJson(jsonPayload);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Network] Exception handling incoming JSON: {}", e.what());
    } catch (...) { SPDLOG_ERROR("[Network] Unknown exception handling incoming JSON"); }
}

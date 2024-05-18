#include "network_manager.hpp"

#include <chrono>

#include <Geode/ui/GeodeUI.hpp>

#include <data/packets/all.hpp>
#include <managers/admin.hpp>
#include <managers/error_queues.hpp>
#include <managers/account.hpp>
#include <managers/profile_cache.hpp>
#include <managers/friend_list.hpp>
#include <managers/settings.hpp>
#include <managers/central_server.hpp>
#include <managers/room.hpp>
#include <ui/menu/admin/admin_popup.hpp>
#include <util/net.hpp>
#include <util/cocos.hpp>
#include <util/rng.hpp>
#include <util/debug.hpp>
#include <util/format.hpp>
#include <ui/notification/panel.hpp>

using namespace geode::prelude;
using namespace util::data;

NetworkManager::NetworkManager() {
    util::net::initialize();

    this->setupBuiltinListeners();

    // boot up the threads

    threadMain.setLoopFunction(&NetworkManager::threadMainFunc);
    threadMain.setStartFunction([] { geode::utils::thread::setName("Network (out) Thread"); });
    threadMain.start(this);

    threadRecv.setLoopFunction(&NetworkManager::threadRecvFunc);
    threadRecv.setStartFunction([] { geode::utils::thread::setName("Network (in) Thread"); });
    threadRecv.start(this);
}

NetworkManager::~NetworkManager() {
    log::debug("cleaning up..");

    // clear listeners
    this->removeAllListeners();
    builtinListeners.lock()->clear();

    log::debug("waiting for output thread to terminate..");
    threadMain.stopAndWait();
    log::debug("waiting for input thread to terminate..");
    threadRecv.stopAndWait();

    if (this->connected()) {
        log::debug("disconnecting from the server..");
        try {
            this->disconnect(false, true);
        } catch (const std::exception& e) {
            log::warn("error trying to disconnect: {}", e.what());
        }
    }

    util::net::cleanup();

    log::info("Goodbye!");
}

Result<> NetworkManager::connect(const std::string_view addr, unsigned short port, const std::string_view serverId, bool standalone) {
    if (_deferredConnect || (this->connected() && !this->handshaken())) {
        return Err("already trying to connect, please wait");
    }

    if (this->connected()) {
        this->disconnect(false);
    }

    _connectingStandalone = standalone;

    lastReceivedPacket = util::time::now();

    if (!standalone) {
        GLOBED_REQUIRE_SAFE(!GlobedAccountManager::get().authToken.lock()->empty(), "attempting to connect with no authtoken set in account manager")
    }

    _deferredConnect = true;
    _deferredAddr = addr;
    _deferredPort = port;
    _deferredServerId = serverId;
    secretKey = util::rng::Random::get().generate<uint32_t>();

    return Ok();
}

Result<> NetworkManager::connectWithView(const GameServer& gsview) {
    return this->connect(gsview.address.ip, gsview.address.port, gsview.id);
}

Result<> NetworkManager::connectStandalone() {
    auto _server = GameServerManager::get().getServer(GameServerManager::STANDALONE_ID);
    if (!_server.has_value()) {
        return Err(fmt::format("failed to find server by standalone ID"));
    }

    auto server = _server.value();

    return this->connect(server.address.ip, server.address.port, GameServerManager::STANDALONE_ID, true);
}

void NetworkManager::disconnect(bool quiet, bool noclear) {
    _handshaken = false;
    _loggedin = false;
    _connectingStandalone = false;
    _deferredConnect = false;

    if (!this->connected()) {
        return;
    }

    if (!quiet) {
        // send it directly instead of pushing to the queue
        (void) gameSocket.sendPacket(DisconnectPacket::create());
    }

    gameSocket.disconnect();

    // singletons could have been destructed before NetworkManager, so this could be UB. Additionally will break autoconnect.
    if (!noclear) {
        RoomManager::get().setGlobal();
        GameServerManager::get().clearActive();
        AdminManager::get().deauthorize();
    }
}

void NetworkManager::disconnectWithMessage(const std::string_view message, bool quiet) {
    ErrorQueues::get().error(fmt::format("You have been disconnected from the active server.\n\nReason: <cy>{}</c>", message));
    this->disconnect(quiet);
}

void NetworkManager::send(std::shared_ptr<Packet> packet) {
    GLOBED_REQUIRE(this->connected(), "tried to send a packet while disconnected")
    packetQueue.push(std::move(packet));
}

void NetworkManager::addListener(CCNode* target, packetid_t id, PacketCallback&& callback, bool overrideBuiltin) {
    auto* listener = PacketListener::create(id, std::move(callback), target, overrideBuiltin);
    target->setUserObject(util::cocos::spr(fmt::format("packet-listener-{}", id)), listener);

    this->registerPacketListener(id, listener);
}

void NetworkManager::removeListener(CCNode* target, packetid_t id) {
    auto key = util::cocos::spr(fmt::format("packet-listener-{}", id));

    target->setUserObject(key, nullptr);

    // the listener will unregister itself in the destructor.
}

void NetworkManager::removeAllListeners() {
    listeners.lock()->clear();
}

// tasks

void NetworkManager::taskPingServers() {
    taskQueue.push(NetworkThreadTask::PingServers);
}

// threads

void NetworkManager::threadMainFunc() {
    if (_suspended) {
        std::this_thread::sleep_for(util::time::millis(250));
        return;
    }

    if (_deferredConnect) {
        auto result = gameSocket.connect(_deferredAddr, _deferredPort);
        _deferredConnect = false;

        if (result) {
            log::debug("tcp connection successful, sending the handshake");
            // successful connection
            GameServerManager::get().setActive(_deferredServerId);
            gameSocket.createBox();

            // if we have ignore on, use 0xffff as a magic value that bypasses protocol checks
            // TODO: actually make it 0xffff
            uint16_t proto = ignoreProtocolMismatch ? 5 : PROTOCOL_VERSION;

            auto packet = CryptoHandshakeStartPacket::create(proto, CryptoPublicKey(gameSocket.cryptoBox->extractPublicKey()));
            this->send(packet);
        } else {
            this->disconnect(true);
            ErrorQueues::get().error(fmt::format("Failed to connect: <cy>{}</c>", result.unwrapErr()));
            return;
        }
    }

    this->maybeSendKeepalive();

    while (auto packet = packetQueue.popTimeout(util::time::millis(200))) {
        try {
            if (packetLogging) {
                this->logPacketToFile(packet.value());
            }

            auto result = gameSocket.sendPacket(packet.value());
            if (!result) {
                log::debug("failed to send packet {}: {}", packet.value()->getPacketId(), result.unwrapErr());
                this->disconnectWithMessage(result.unwrapErr());
                return;
            }

        } catch (const std::exception& e) {
            ErrorQueues::get().error(e.what());
            this->disconnect(true, false);
        }
    }

    while (auto task_ = taskQueue.tryPop()) {
        auto task = task_.value();
        if (task == NetworkThreadTask::PingServers) {
            auto& sm = GameServerManager::get();
            auto activeServer = sm.getActiveId();

            for (auto& [serverId, server] : sm.getAllServers()) {
                if (serverId == activeServer) continue;

                auto pingId = sm.startPing(serverId);
                auto result = gameSocket.sendPacketTo(PingPacket::create(pingId), server.address.ip, server.address.port);

                if (result.isErr()) {
                    ErrorQueues::get().warn(result.unwrapErr());
                }
            }
        }
    }

    std::this_thread::yield();
}

void NetworkManager::threadRecvFunc() {
    if (_suspended || _deferredConnect) {
        std::this_thread::sleep_for(util::time::millis(100));
        return;
    }

    bool fromConnected;
    bool timedOut;

    auto packet_ = gameSocket.recvPacket(100, fromConnected, timedOut);

    if (timedOut) {
        this->maybeDisconnectIfDead();
        return;
    }

    if (packet_.isErr()) {
        ErrorQueues::get().debugWarn(fmt::format("failed to receive a packet: {}", packet_.unwrapErr()));

        if (this->connected()) {
            this->disconnectWithMessage(packet_.unwrapErr(), true);
        }

        return;
    }

    auto packet = packet_.unwrap();

    if (packetLogging) {
        this->logPacketToFile(packet);
    }

    packetid_t packetId = packet->getPacketId();

    if (packetId == PingResponsePacket::PACKET_ID) {
        this->handlePingResponse(packet);
        return;
    }

    // if it's not a ping packet, and it's NOT from the currently connected server, we reject it
    if (!fromConnected) {
        return;
    }

    lastReceivedPacket = util::time::now();

    this->callListener(packet);
}

void NetworkManager::callListener(std::shared_ptr<Packet> packet) {
    packetid_t packetId = packet->getPacketId();

    bool hasListeners = !(*listeners.lock())[packetId].empty();
    // if we have listeners, check if any of them disable builtin listeners

    bool overrideBuiltin = false;
    if (hasListeners) {
        for (auto* listener : (*listeners.lock())[packetId]) {
            if (listener->overrideBuiltin) {
                overrideBuiltin = true;
                break;
            }
        }
    }

    bool invokedBuiltin = false;

    // call any builtin listeners
    if (!overrideBuiltin) {
        auto builtin = builtinListeners.lock();
        if (builtin->contains(packetId)) {
            (*builtin)[packetId](packet);
            invokedBuiltin = true;
        }
    }

    if (!hasListeners) {
        if (!invokedBuiltin) {
            this->handleUnhandledPacket(packetId);
        }

        return;
    }

    // if there are registered listeners, schedule them to be called on the next frame
    Loader::get()->queueInMainThread([this, packetId, packet = std::move(packet)] {
        for (auto* listener : (*listeners.lock())[packetId]) {
            listener->invokeCallback(packet);
        }
    });
}

void NetworkManager::handleUnhandledPacket(packetid_t packetId) {
    // if suppressed, do nothing
    auto suppressed_ = suppressed.lock();

    if (suppressed_->contains(packetId) && util::time::systemNow() > suppressed_->at(packetId)) {
        suppressed_->erase(packetId);
    }

    // else show a warning
    if (!suppressed_->contains(packetId)) {
        ErrorQueues::get().debugWarn(fmt::format("Unhandled packet: {}", packetId));
    }
}

void NetworkManager::setupBuiltinListeners() {
    addBuiltinListenerSafe<CryptoHandshakeResponsePacket>([this](std::shared_ptr<CryptoHandshakeResponsePacket> packet) {
        log::debug("handshake successful, logging in");
        auto key = packet->data.key;

        gameSocket.cryptoBox->setPeerKey(key.data());
        _handshaken = true;
        // and lets try to login!
        auto& am = GlobedAccountManager::get();
        std::string authtoken;

        if (!_connectingStandalone) {
            authtoken = *am.authToken.lock();
        }

        auto& pcm = ProfileCacheManager::get();
        pcm.setOwnDataAuto();
        pcm.pendingChanges = false;

        auto& settings = GlobedSettings::get();

        if (settings.globed.fragmentationLimit == 0) {
            settings.globed.fragmentationLimit = 65000;
        }

        auto gddata = am.gdData.lock();
        auto pkt = LoginPacket::create(this->secretKey, gddata->accountId, gddata->userId, gddata->accountName, authtoken, pcm.getOwnData(), settings.globed.fragmentationLimit);
        this->send(pkt);
    });

    addBuiltinListener<KeepaliveResponsePacket>([](auto packet) {
        GameServerManager::get().finishKeepalive(packet->playerCount);
    });

    addBuiltinListener<KeepaliveTCPResponsePacket>([](auto) {});

    addBuiltinListener<ServerDisconnectPacket>([this](auto packet) {
        this->disconnectWithMessage(packet->message);
    });

    addBuiltinListener<ServerBannedPacket>([this](auto packet) {
        using namespace std::chrono;

        std::string reason = packet->message;
        if (reason.empty()) {
            reason = "No reason given";
        }

        auto msg = fmt::format(
            "<cy>You have been</c> <cr>Banned:</c>\n{}\n<cy>Expires at:</c>\n{}\n<cy>Question/Appeals? Join the </c><cb>Discord.</c>",
            reason,
            packet->timestamp == 0 ? "Permanent" : util::format::formatDateTime(sys_seconds(seconds(packet->timestamp)))
        );

        this->disconnectWithMessage(msg);
    });

    addBuiltinListener<ServerMutedPacket>([this](auto packet) {
        using namespace std::chrono;

        std::string reason = packet->reason;
        if (reason.empty()) {
            reason = "No reason given";
        }

        auto msg = fmt::format(
            "<cy>You have been</c> <cr>Muted:</c>\n{}\n<cy>Expires at:</c>\n{}\n<cy>Question/Appeals? Join the </c><cb>Discord.</c>",
            reason,
            packet->timestamp == 0 ? "Permanent" : util::format::formatDateTime(sys_seconds(seconds(packet->timestamp)))
        );

        ErrorQueues::get().notice(msg);
    });

    addBuiltinListener<LoggedInPacket>([this](auto packet) {
        log::info("Successfully logged into the server!");
        connectedTps = packet->tps;
        _loggedin = true;

        // these are not thread-safe, so delay it
        Loader::get()->queueInMainThread([specialUserData = packet->specialUserData] {
            auto& pcm = ProfileCacheManager::get();
            pcm.setOwnSpecialData(specialUserData);

            RoomManager::get().setGlobal();
        });

        // claim the tcp thread to allow udp packets through
        this->send(ClaimThreadPacket::create(this->secretKey));

        // try to login as an admin if we can
        auto& am = GlobedAccountManager::get();
        if (am.hasAdminPassword()) {
            auto password = am.getAdminPassword();
            if (password.has_value()) {
                this->send(AdminAuthPacket::create(password.value()));
            }
        }
    });

    addBuiltinListener<LoginFailedPacket>([this](auto packet) {
        ErrorQueues::get().error(fmt::format("<cr>Authentication failed!</c> The server rejected the login attempt.\n\nReason: <cy>{}</c>", packet->message));
        GlobedAccountManager::get().authToken.lock()->clear();
        this->disconnect(true);
    });

    addBuiltinListener<ServerNoticePacket>([](auto packet) {
        ErrorQueues::get().notice(packet->message);
    });

    addBuiltinListener<ProtocolMismatchPacket>([this](auto packet) {
        log::warn("Failed to connect because of protocol mismatch. Server: {}, client: {}", packet->serverProtocol, PROTOCOL_VERSION);

#ifdef GLOBED_DEBUG
        // if we are in debug mode, allow the user to override it
        Loader::get()->queueInMainThread([this, serverProtocol = packet->serverProtocol] {
            geode::createQuickPopup("Globed Error",
                fmt::format("Protocol mismatch (client: v{}, server: v{}). Override the protocol for this session and allow to connect to the server anyway? <cy>(Not recommended!)</c>", PROTOCOL_VERSION, serverProtocol),
                "Cancel", "Yes", [this](FLAlertLayer*, bool override) {
                    if (override) {
                        this->toggleIgnoreProtocolMismatch(true);
                    }
                }
            );
        });
#else
        // if we are not in debug, show an error tleling the user to update the mod

        if (packet->serverProtocol < PROTOCOL_VERSION) {
            std::string message = "Your Globed version is <cy>too new</c> for this server. Downgrade the mod to an older version or ask the server owner to update their server.";
            ErrorQueues::get().error(message);
        } else {
            Loader::get()->queueInMainThread([] {
                std::string message = "Your Globed version is <cr>outdated</c>, please <cg>update</c> Globed in order to connect. If the update doesn't appear, <cy>restart your game</c>.";
                geode::createQuickPopup("Globed Error", message, "Cancel", "Update", [](FLAlertLayer*, bool update) {
                    if (!update) return;

                    geode::openModsList();
                });
            });
        }
#endif

        this->disconnect(true);
    });

    addBuiltinListener<AdminAuthSuccessPacket>([this](auto packet) {
        AdminManager::get().setAuthorized(std::move(packet->role), std::move(packet->allRoles));
        ErrorQueues::get().success("Successfully authorized");
    });

    addBuiltinListenerSafe<AdminAuthFailedPacket>([this](auto packet) {
        ErrorQueues::get().warn("Login failed");

        auto& am = GlobedAccountManager::get();
        am.clearAdminPassword();
    });

    addBuiltinListener<AdminSuccessMessagePacket>([](auto packet) {
        ErrorQueues::get().success(packet->message);
    });

    addBuiltinListener<AdminErrorPacket>([](auto packet) {
        ErrorQueues::get().warn(packet->message);
    });

    addBuiltinListenerSafe<RoomInvitePacket>([](auto packet) {
        GlobedNotificationPanel::get()->addInviteNotification(packet->roomID, packet->roomToken, packet->playerData);
    });

    addBuiltinListenerSafe<RoomInfoPacket>([](auto packet) {
        ErrorQueues::get().success("Room configuration updated");

        RoomManager::get().setInfo(packet->info);
    });

    addBuiltinListener<RoomJoinedPacket>([](auto packet) {});

    addBuiltinListener<RoomJoinFailedPacket>([](auto packet) {
        ErrorQueues::get().error(fmt::format("Failed to join room: {}", packet->message));
    });
}

void NetworkManager::handlePingResponse(std::shared_ptr<Packet> packet) {
    if (auto* pingr = packet->tryDowncast<PingResponsePacket>()) {
        GameServerManager::get().finishPing(pingr->id, pingr->playerCount);
    }
}

void NetworkManager::maybeSendKeepalive() {
    if (_loggedin) {
        auto now = util::time::now();
        if ((now - lastKeepalive) > KEEPALIVE_INTERVAL) {
            lastKeepalive = now;
            this->send(KeepalivePacket::create());
            GameServerManager::get().startKeepalive();
        }

        // this is only done to keep the nat hole open
        if ((now - lastTcpKeepalive) > TCP_KEEPALIVE_INTERVAL) {
            lastTcpKeepalive = now;
            this->send(KeepaliveTCPPacket::create());
        }
    }
}

// Disconnects from the server if there has been no response for a while
void NetworkManager::maybeDisconnectIfDead() {
    if (!this->connected()) return;

    auto elapsed = util::time::now() - lastReceivedPacket;

    // if we haven't had a handshake response in 5 seconds, assume the server is dead
    if (!this->handshaken() && elapsed > util::time::seconds(5)) {
        ErrorQueues::get().error("Failed to connect to the server. No response was received after 5 seconds.");
        this->disconnect(true);
    } else if (elapsed > DISCONNECT_AFTER) {
        ErrorQueues::get().error("The server you were connected to is not responding to any requests. <cy>You have been disconnected.</c>");
        try {
            this->disconnect();
        } catch (const std::exception& e) {
            log::warn("failed to disconnect from a dead server: {}", e.what());
            this->disconnect(true);
        }
    }
}

void NetworkManager::addBuiltinListener(packetid_t id, PacketCallback&& callback) {
    (*builtinListeners.lock())[id] = std::move(callback);
}

void NetworkManager::registerPacketListener(packetid_t packet, PacketListener* listener) {
#ifdef GLOBED_DEBUG
    log::debug("Registering listener (id {}) for {}", packet, listener->owner);
#endif

    (*listeners.lock())[packet].insert(listener);
}

void NetworkManager::unregisterPacketListener(packetid_t packet, PacketListener* listener) {
#ifdef GLOBED_DEBUG
    // note: is it safe to access listener->owner here?
    // at the time of user object destruction, i believe the node is still valid,
    // but we are inside of ~CCNode(), which means main vtable is set to CCNode vtable,
    // and therefore this would always print 'class cocos2d::CCNode' instead of the real class.
    log::debug("Unregistering listener (id {}) for {}", packet, listener->owner);
#endif

    listeners.lock()->at(packet).erase(listener);
}

void NetworkManager::toggleIgnoreProtocolMismatch(bool state) {
    ignoreProtocolMismatch = state;
}

void NetworkManager::togglePacketLogging(bool enabled) {
    packetLogging = enabled;
}

void NetworkManager::logPacketToFile(std::shared_ptr<Packet> packet) {
    log::debug("{} packet: {}", packet->getPacketId() < 20000 ? "Sending" : "Receiving", packet->getPacketId());

    auto folder = Mod::get()->getSaveDir() / "packets";
    (void) geode::utils::file::createDirectoryAll(folder);
    util::misc::callOnce("networkmanager-log-to-file", [&] {
        log::debug("Packet log folder: {}", folder);
    });

    auto datetime = util::format::formatDateTime(util::time::systemNow());
    auto filepath = folder / fmt::format("{}-{}.bin", packet->getPacketId(), datetime);

    std::ofstream fs(filepath, std::ios::binary);
    ByteBuffer data;
    packet->encode(data);

    const auto& vec = data.data();
    fs.write(reinterpret_cast<const char*>(vec.data()), vec.size());
}

bool NetworkManager::connected() {
    return gameSocket.isConnected();
}

bool NetworkManager::handshaken() {
    return _handshaken;
}

bool NetworkManager::established() {
    return _loggedin;
}

bool NetworkManager::standalone() {
    return _connectingStandalone;
}

void NetworkManager::suspend() {
    _suspended = true;
}

void NetworkManager::resume() {
    _suspended = false;
}
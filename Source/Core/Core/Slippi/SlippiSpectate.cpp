#include "SlippiSpectate.h"
#include "Common/Logging/Log.h"
#include "Common/CommonTypes.h"
#include <Core/ConfigManager.h>

// Networking
#ifdef _WIN32
#include <share.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#endif

SlippiSpectateServer* SlippiSpectateServer::getInstance()
{
    static SlippiSpectateServer instance; // Guaranteed to be destroyed.
                                    // Instantiated on first use.
    return &instance;
}

void SlippiSpectateServer::write(u8 *payload, u32 length)
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    u32 cursor = (u32)m_event_buffer.size();
    u64 offset = m_cursor_offset;
    m_event_buffer_mutex.unlock();

    slippicomm::SlippiMessage wrapper;
    slippicomm::GameEvent *game_event = new slippicomm::GameEvent();
    game_event->set_cursor(offset+cursor);
    game_event->set_next_cursor(offset+cursor+1);
    game_event->set_payload(payload, length);

    // wrapper takes ownership of the pointer here. So don't delete the object
    wrapper.set_allocated_game_event(game_event);
    std::string buffer;
    wrapper.SerializeToString(&buffer);

    // Put this message into the event buffer
    //  This will queue the message up to go out for all clients
    m_event_buffer_mutex.lock();
    m_event_buffer.push_back(buffer);
    m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::writeMenuEvent(u8 *payload, u32 length)
{
  if(!SConfig::GetInstance().m_slippiNetworkingOutput)
  {
      return;
  }

  // Reset the sent menu flag for each peer
  std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
  for(; it != m_sockets.end(); it++)
  {
      it->second->m_sent_menu = false;
  }

  slippicomm::SlippiMessage wrapper;
  slippicomm::MenuEvent *menu_event = new slippicomm::MenuEvent();
  menu_event->set_payload(payload, length);

  // wrapper takes ownership of the pointer here. So don't delete the object
  wrapper.set_allocated_menu_event(menu_event);

  // Put this message into the menu event buffer
  //  This will queue the message up to go out for all clients
  m_event_buffer_mutex.lock();
  wrapper.SerializeToString(&m_menu_event);
  m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::writeEvents(u16 peer_id)
{
    m_event_buffer_mutex.lock();
    bool in_game = m_in_game;
    m_event_buffer_mutex.unlock();

    if(in_game)
    {
        // Get the cursor for this socket
        u64 cursor = m_sockets[peer_id]->m_cursor;

        // Loop through each event that needs to be sent
        //  send all the events starting at their cursor
        m_event_buffer_mutex.lock();
        for(u64 i = cursor; i < m_event_buffer.size(); i++)
        {
            ENetPacket *packet = enet_packet_create(m_event_buffer[i].data(),
                                                    m_event_buffer[i].size(),
                                                    ENET_PACKET_FLAG_RELIABLE);
            // Batch for sending
            enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
            m_sockets[peer_id]->m_cursor++;
        }
        m_event_buffer_mutex.unlock();

    }
    else {
        if(!m_sockets[peer_id]->m_sent_menu)
        {
            // We're in a menu, so send the menu event
            m_event_buffer_mutex.lock();
            ENetPacket *packet = enet_packet_create(m_menu_event.data(),
                                                    m_menu_event.length(),
                                                    ENET_PACKET_FLAG_RELIABLE);
            // Batch for sending
            enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);
            // Record for the peer that it was sent
            m_sockets[peer_id]->m_sent_menu = true;
            m_event_buffer_mutex.unlock();
        }
    }
}

// We assume, for the sake of simplicity, that all clients have finished reading
//  from the previous game event buffer by now. At least many seconds will have passed
//  by now, so if a listener is still stuck getting events from the last game,
//  they will get erroneous data.
void SlippiSpectateServer::startGame()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    if(m_event_buffer.size() > 0)
    {
        m_cursor_offset += m_event_buffer.size();
    }
    m_event_buffer.clear();
    m_in_game = true;

    // TODO m_sockets is probably unsafe here :/
    std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    for(; it != m_sockets.end(); it++)
    {
        it->second->m_cursor = 0;
    }

    m_event_buffer_mutex.unlock();
}

void SlippiSpectateServer::endGame()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    m_menu_event.clear();
    m_in_game = false;
    m_event_buffer_mutex.unlock();
}

SlippiSpectateServer::SlippiSpectateServer()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    // Init some timestamps
    m_last_broadcast_time = std::chrono::system_clock::now();

    // Spawn thread for socket listener
    m_stop_socket_thread = false;
    m_socketThread = std::thread(&SlippiSpectateServer::SlippicommSocketThread, this);
}

SlippiSpectateServer::~SlippiSpectateServer()
{
    // The socket thread will be blocked waiting for input
    // So to wake it up, let's connect to the socket!
    m_stop_socket_thread = true;
}

void SlippiSpectateServer::writeBroadcast()
{
    sendto(m_broadcast_socket, (char*)m_broadcast_message.data(), m_broadcast_message.length(), 0,
        (struct sockaddr *)&m_broadcastAddr, sizeof(m_broadcastAddr));
    m_last_broadcast_time = std::chrono::system_clock::now();
}

void SlippiSpectateServer::handleMessage(u8 *buffer, u32 length, u16 peer_id)
{
    // Unpack the message
    slippicomm::SlippiMessage message;
    if(message.ParseFromArray(buffer, length))
    {
        // Check what type of message this is
        if(message.has_connect_request())
        {
            // Get the requested cursor
            slippicomm::ConnectRequest connect_request = message.connect_request();
            u32 requested_cursor = connect_request.cursor();
            u32 sent_cursor = 0;
            // Set the user's cursor position
            m_event_buffer_mutex.lock();
            if(requested_cursor >= m_cursor_offset)
            {
                // If the requested cursor is past what events we even have, then just tell them to start over
                if(requested_cursor > m_event_buffer.size() + m_cursor_offset)
                {
                  m_sockets[peer_id]->m_cursor = 0;
                }
                // Requested cursor is in the middle of a live match, events that we have
                else
                {
                  m_sockets[peer_id]->m_cursor = requested_cursor - m_cursor_offset;
                }
            }
            else
            {
                // The client requested a cursor that was too low. Bring them up to the present
                m_sockets[peer_id]->m_cursor = 0;
            }

            sent_cursor = m_sockets[peer_id]->m_cursor + m_cursor_offset;
            m_event_buffer_mutex.unlock();

            slippicomm::SlippiMessage reply;
            slippicomm::ConnectReply *connect_reply = new slippicomm::ConnectReply();
            connect_reply->set_nick(SConfig::GetInstance().m_slippiConsoleName);
            connect_reply->set_version("1.9.0-dev-2");
            connect_reply->set_cursor(sent_cursor);
            // Reply takes ownership of the pointer here. So don't delete the object
            reply.set_allocated_connect_reply(connect_reply);

            std::string packet_buffer;
            reply.SerializeToString(&packet_buffer);

            ENetPacket *packet = enet_packet_create(packet_buffer.data(),
                          packet_buffer.length(),
                          ENET_PACKET_FLAG_RELIABLE);

            // Batch for sending
            enet_peer_send(m_sockets[peer_id]->m_peer, 0, packet);

        }
    }


    // Put the client in the right in_game state
    m_sockets[peer_id]->m_shook_hands = true;
}

void SlippiSpectateServer::SlippicommSocketThread(void)
{
    // Setup the broadcast advertisement message and socket
    m_broadcast_socket = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable=1;
    if(setsockopt(m_broadcast_socket, SOL_SOCKET, SO_BROADCAST,
       (char*)&broadcastEnable, sizeof(broadcastEnable)))
    {
        WARN_LOG(SLIPPI, "Failed configuring Slippi braodcast socket");
        return;
    }
    memset(&m_broadcastAddr, 0, sizeof(m_broadcastAddr));
    m_broadcastAddr.sin_family = AF_INET;
    m_broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    m_broadcastAddr.sin_port = htons(20582);

    // Setup the broadcast message
    //  It never changes, so let's just do this once
    slippicomm::SlippiMessage wrapper;
    slippicomm::Advertisement *advert = new slippicomm::Advertisement();
    advert->set_nick(SConfig::GetInstance().m_slippiConsoleName);
    // Reply takes ownership of the pointer here. So don't delete the object
    wrapper.set_allocated_advertisement(advert);
    wrapper.SerializeToString(&m_broadcast_message);

    if (enet_initialize () != 0) {
        // TODO replace all printfs with logs
        printf("An error occurred while initializing ENet.\n");
        return;
    }

    ENetAddress server_address = {0};
    server_address.host = ENET_HOST_ANY;
    server_address.port = SLIPPI_PORT;

    /* create a server */
    ENetHost *server = enet_host_create(&server_address, MAX_CLIENTS, 2, 0, 0);

    if (server == NULL) {
        printf("An error occurred while trying to create an ENet server host.\n");
        return;
    }

    printf("Started a server...\n");
    // Main slippicomm server loop
    while(1)
    {
        // If we're told to stop, then quit
        if(m_stop_socket_thread)
        {
            enet_host_destroy(server);
            enet_deinitialize();
            return;
        }

        std::map<u16, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
        for(; it != m_sockets.end(); it++)
        {
            if(it->second->m_shook_hands)
            {
                writeEvents(it->first);
            }
        }

        // Write any broadcast messages if we haven't in two seconds
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        if(now - std::chrono::seconds(2) > m_last_broadcast_time)
        {
            writeBroadcast();
        }

        ENetEvent event;
        while (enet_host_service (server, &event, 1) > 0)
        {
            switch (event.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    printf ("A new client connected from %x:%u.\n",
                            event.peer -> address.host,
                            event.peer -> address.port);
                    std::shared_ptr<SlippiSocket> newSlippiSocket(new SlippiSocket());
                    newSlippiSocket->m_peer = event.peer;
                    m_sockets[event.peer->outgoingPeerID] = newSlippiSocket;

                    // New incoming client connection
                    //  I don't think there's any special logic that we need to do here
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    handleMessage(event.packet->data, event.packet->dataLength, event.peer->outgoingPeerID);
                    /* Clean up the packet now that we're done using it. */
                    enet_packet_destroy (event.packet);

                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    printf ("%s disconnected.\n", event.peer -> data);
                    /* Reset the peer's client information. */
                    event.peer -> data = NULL;
                    break;
                }
                default:
                {
                    printf ("%s Unknown message came in.\n", event.peer -> data);
                    break;
                }
            }
        }
    }

    enet_host_destroy(server);
    enet_deinitialize();
}

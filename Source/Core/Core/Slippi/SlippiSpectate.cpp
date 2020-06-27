#include "SlippiSpectate.h"
#include "Common/Logging/Log.h"
#include "nlohmann/json.hpp"
#include "Common/CommonTypes.h"
#include <Core/ConfigManager.h>

// Networking
#ifdef _WIN32
#include <share.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#endif

SlippicommServer* SlippicommServer::getInstance()
{
    static SlippicommServer instance; // Guaranteed to be destroyed.
                                    // Instantiated on first use.
    return &instance;
}

void SlippicommServer::write(u8 *payload, u32 length)
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    // Keep track of the latest time we wrote data.
    //  So that we can know when to send keepalives later
    m_write_time_mutex.lock();
    m_last_write_time = std::chrono::system_clock::now();
    m_write_time_mutex.unlock();

    m_event_buffer_mutex.lock();
    u32 cursor = (u32)m_event_buffer.size();
    u64 offset = m_cursor_offset;
    m_event_buffer_mutex.unlock();

      // Note: This is a bit messy because nlohmann can't be used for this case.
      //  nlohmann needs the contents to be valid JSON first, and then it can
      //  read and write it to UBJSON. But arbitrary binary buffers can't be in
      //  regular JSON, so this doesn't work. :(
    std::vector<u8> ubjson_header({'{', 'i', '\x04', 't', 'y', 'p', 'e', 'U',
        '\x02', 'i', '\x07', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '{',});
    std::vector<u8> cursor_header({'i', '\x03', 'p', 'o', 's', '[','$', 'U', '#', 'U', '\x08'});
    std::vector<u8> cursor_value = uint64ToVector(offset + cursor);
    std::vector<u8> next_cursor_header({'i', '\x07', 'n', 'e', 'x', 't', 'P', 'o', 's', '[','$', 'U', '#', 'U', '\x08'});
    std::vector<u8> next_cursor_value = uint64ToVector(offset + cursor + 1);
    std::vector<u8> data_field_header({'i', '\x04', 'd', 'a', 't', 'a', '[',
        '$', 'U', '#', 'I'});
    std::vector<u8> length_vector = uint16ToVector(length);
    std::vector<u8> ubjson_footer({'}', '}'});

    // Length of the entire TCP event. Not part of the slippi message per-se
    u32 event_length = length +
      (u32)ubjson_header.size() + (u32)cursor_header.size() +
      (u32)cursor_value.size() + (u32)data_field_header.size() +
      (u32)next_cursor_value.size() + (u32)next_cursor_header.size() +
      (u32)length_vector.size() + (u32)ubjson_footer.size();
    std::vector<u8> event_length_vector = uint32ToVector(event_length);

    // Let's assemble the final buffer that gets written
    std::vector<u8> buffer;
    buffer.reserve(event_length);
    buffer.insert(buffer.end(), event_length_vector.begin(), event_length_vector.end());
    buffer.insert(buffer.end(), ubjson_header.begin(), ubjson_header.end());
    buffer.insert(buffer.end(), cursor_header.begin(), cursor_header.end());
    buffer.insert(buffer.end(), cursor_value.begin(), cursor_value.end());
    buffer.insert(buffer.end(), next_cursor_header.begin(), next_cursor_header.end());
    buffer.insert(buffer.end(), next_cursor_value.begin(), next_cursor_value.end());
    buffer.insert(buffer.end(), data_field_header.begin(), data_field_header.end());
    buffer.insert(buffer.end(), length_vector.begin(), length_vector.end());
    buffer.insert(buffer.end(), payload, payload + length);
    buffer.insert(buffer.end(), ubjson_footer.begin(), ubjson_footer.end());

    // Put this message into the event buffer
    //  This will queue the message up to go out for all clients
    m_event_buffer_mutex.lock();
    m_event_buffer.push_back(buffer);
    m_event_buffer_mutex.unlock();
}

void SlippicommServer::writeMenuEvent(u8 *payload, u32 length)
{
  if(!SConfig::GetInstance().m_slippiNetworkingOutput)
  {
      return;
  }

  // Note: This is a bit messy because nlohmann can't be used for this case.
  //  nlohmann needs the contents to be valid JSON first, and then it can
  //  read and write it to UBJSON. But arbitrary binary buffers can't be in
  //  regular JSON, so this doesn't work. :(
  std::vector<u8> ubjson_header({'{', 'i', '\x04', 't', 'y', 'p', 'e', 'U',
      '\x04', 'i', '\x07', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '{',});
  std::vector<u8> data_field_header({'i', '\x04', 'd', 'a', 't', 'a', '[',
      '$', 'U', '#', 'I'});
  std::vector<u8> length_vector = uint16ToVector(length);
  std::vector<u8> ubjson_footer({'}', '}'});

  // Length of the entire TCP event. Not part of the slippi message per-se
  u32 event_length = length +
    (u32)ubjson_header.size() + (u32)data_field_header.size() +
    (u32)length_vector.size() + (u32)ubjson_footer.size();
  std::vector<u8> event_length_vector = uint32ToVector(event_length);

  // Let's assemble the final buffer that gets written
  std::vector<u8> buffer;
  buffer.reserve(event_length);
  buffer.insert(buffer.end(), event_length_vector.begin(), event_length_vector.end());
  buffer.insert(buffer.end(), ubjson_header.begin(), ubjson_header.end());
  buffer.insert(buffer.end(), data_field_header.begin(), data_field_header.end());
  buffer.insert(buffer.end(), length_vector.begin(), length_vector.end());
  buffer.insert(buffer.end(), payload, payload + length);
  buffer.insert(buffer.end(), ubjson_footer.begin(), ubjson_footer.end());

  // Put this message into the menu event buffer
  //  This will queue the message up to go out for all clients
  m_event_buffer_mutex.lock();
  m_menu_event_buffer.push_back(buffer);
  m_event_buffer_mutex.unlock();
}

void SlippicommServer::writeEvents(SOCKET socket)
{
    // Get the cursor for this socket
    u64 cursor = m_sockets[socket]->m_cursor;

    // Loop through each event that needs to be sent
    //  send all the events starting at their cursor
    m_event_buffer_mutex.lock();

    std::vector< std::vector<u8>> *buffer_ptr;

    // Are we pulling from the game events buffer or the menu events buffer?
    if(!m_sockets[socket]->m_in_game)
    {
      buffer_ptr = &m_menu_event_buffer;
    }
    else
    {
      buffer_ptr = &m_event_buffer;
    }

    for(u64 i = cursor; i < (*buffer_ptr).size(); i++)
    {
        u32 fragment_index = m_sockets[socket]->m_outgoing_fragment_index;
        int32_t byteswritten = send(socket,
          (char*)(*buffer_ptr)[i].data() + fragment_index,
          (int)(*buffer_ptr)[i].size() - fragment_index, 0);

        // There are three possible results from a send() call.
        //  1) All the data was sent.
        //      Keep the data coming
        //  2) Partial data was sent, and this would block.
        //      Stop sending data for now. Save the partial fragment
        //  3) The socket is broken
        //      Kill the socket

        // We didn't send all the data. This means result #2 or #3
        if(byteswritten < (int32_t)((*buffer_ptr)[i].size() - fragment_index))
        {
            // Is this just a blocking error?
            if(errno == EWOULDBLOCK || byteswritten >= 0)
            {
                // Update the index to represent the bytes that DID get sent
                m_sockets[socket]->m_outgoing_fragment_index += byteswritten;
            }
            else {
                // Kill the socket
                sockClose(socket);
                m_sockets.erase(socket);
            }
            // In both error cases, we have to return. No more data to send
            m_event_buffer_mutex.unlock();
            return;
        }
        // Result #1. Keep the data coming with a new event
        m_sockets[socket]->m_outgoing_fragment_index = 0;
        m_sockets[socket]->m_cursor++;
    }

    // If the socket is all caught up, sync them up the right in_game status
    if(m_sockets[socket]->m_cursor >= (*buffer_ptr).size())
    {
      m_sockets[socket]->m_in_game = m_in_game;
    }

    m_event_buffer_mutex.unlock();
}

// Helper for closing sockets in a cross-compatible way
int SlippicommServer::sockClose(SOCKET sock)
{
    int status = 0;

    #ifdef _WIN32
        status = shutdown(sock, SD_BOTH);
    #else
        status = close(sock);
    #endif

    return status;
}

SOCKET SlippicommServer::buildFDSet(fd_set *read_fds, fd_set *write_fds)
{
    // Keep track of the currently largest FD
    SOCKET maxFD = m_server_fd;
    FD_ZERO(read_fds);
    FD_ZERO(write_fds);
    FD_SET(m_server_fd, read_fds);

    // Read from the sockets list
    std::map<SOCKET, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    while(it != m_sockets.end())
    {
        FD_SET(it->first, read_fds);
        maxFD = std::max(maxFD, it->first);

        // Only add a socket to the write list if it's behind on events
        m_event_buffer_mutex.lock();
        u32 event_count = (u32)m_event_buffer.size();
        if(!m_sockets[it->first]->m_in_game)
        {
          event_count = (u32)m_menu_event_buffer.size();
        }

        m_event_buffer_mutex.unlock();
        // reset cursor if it's > event buffer size
        //  This will happen when a new game starts
        //  or some weird error. In both cases, starting over is right
        if(m_sockets[it->first]->m_cursor > event_count)
        {
            m_sockets[it->first]->m_cursor = 0;
        }
        if(m_sockets[it->first]->m_cursor < event_count)
        {
            FD_SET(it->first, write_fds);
        }
        it++;
    }

    return maxFD;
}

std::vector<u8> SlippicommServer::uint16ToVector(u16 num)
{
    u8 byte0 = num >> 8;
    u8 byte1 = num & 0xFF;

    return std::vector<u8>({byte0, byte1});
}

std::vector<u8> SlippicommServer::uint64ToVector(u64 num)
{
    u8 byte0 = num >> 56;
    u8 byte1 = (num >> 48) & 0xFF;
    u8 byte2 = (num >> 40) & 0xFF;
    u8 byte3 = (num >> 32) & 0xFF;
    u8 byte4 = (num >> 24) & 0xFF;
    u8 byte5 = (num >> 16) & 0xFF;
    u8 byte6 = (num >> 8) & 0xFF;
    u8 byte7 = num & 0xFF;

    return std::vector<u8>{byte0, byte1, byte2, byte3, byte4, byte5, byte6, byte7};
}

std::vector<u8> SlippicommServer::uint32ToVector(u32 num)
{
    u8 byte0 = num >> 24;
    u8 byte1 = (num & 0xFF0000) >> 16;
    u8 byte2 = (num & 0xFF00) >> 8;
    u8 byte3 = num & 0xFF;

    return std::vector<u8>({byte0, byte1, byte2, byte3});
}


// We assume, for the sake of simplicity, that all clients have finished reading
//  from the previous game event buffer by now. At least many seconds will have passed
//  by now, so if a listener is still stuck getting events from the last game,
//  they will get erroneous data.
void SlippicommServer::startGame()
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

    // If the socket is all caught up, sync them up the right in_game status
    std::map<SOCKET, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    for(; it != m_sockets.end(); it++)
    {
      if(it->second->m_cursor >= m_menu_event_buffer.size())
      {
        it->second->m_in_game = m_in_game;
        it->second->m_cursor = m_cursor_offset;
      }
    }

    m_event_buffer_mutex.unlock();
}

void SlippicommServer::endGame()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    m_event_buffer_mutex.lock();
    m_menu_event_buffer.clear();
    m_in_game = false;

    // If the socket is all caught up, sync them up the right in_game status
    std::map<SOCKET, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    for(; it != m_sockets.end(); it++)
    {
      if(it->second->m_cursor >= m_event_buffer.size())
      {
        it->second->m_in_game = m_in_game;
      }
    }

    m_event_buffer_mutex.unlock();
}

SlippicommServer::SlippicommServer()
{
    if(!SConfig::GetInstance().m_slippiNetworkingOutput)
    {
        return;
    }

    #ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2,2), &wsa_data);
    #endif

    // Init some timestamps
    m_last_write_time = std::chrono::system_clock::now();
    m_last_broadcast_time = std::chrono::system_clock::now();

    // Spawn thread for socket listener
    m_stop_socket_thread = false;
    m_socketThread = std::thread(&SlippicommServer::SlippicommSocketThread, this);
}

SlippicommServer::~SlippicommServer()
{
    // The socket thread will be blocked waiting for input
    // So to wake it up, let's connect to the socket!
    m_stop_socket_thread = true;

    SOCKET sock = 0;
    struct sockaddr_in serv_addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        WARN_LOG(SLIPPI, "Failed to shut down Slippi networking thread");
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SLIPPI_PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)
    {
        WARN_LOG(SLIPPI, "Failed to shut down Slippi networking thread");
        return;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        WARN_LOG(SLIPPI, "Failed to shut down Slippi networking thread");
        return;
    }

    m_socketThread.join();
    #ifdef _WIN32
    WSACleanup();
    #endif
}

void SlippicommServer::writeKeepalives()
{
    // nlohmann::json keepalive = {{"type", KEEPALIVE_TYPE}};
    // std::vector<u8> ubjson_keepalive = nlohmann::json::to_ubjson(keepalive);
    //
    // // Write the data to each open socket
    // std::map<SOCKET, std::shared_ptr<SlippiSocket>>::iterator it = m_sockets.begin();
    // for(; it != m_sockets.end(); it++)
    // {
    //     // Don't send a keepalive in the middle of an event message
    //     if(it->second->m_outgoing_fragment_index > 0)
    //     {
    //         continue;
    //     }
    //
    //     // Keepalives only get sent when no other data was sent for two whole seconds
    //     //  So the chances of the network buffer being full here are pretty low.
    //     //   It's fine to just loop the send(), effectively blocking on the call
    //     send(it->first, (char *)&m_keepalive_len, sizeof(m_keepalive_len), 0);
    //
    //     int32_t byteswritten = 0;
    //     while ((uint32_t)byteswritten < ubjson_keepalive.size())
    //     {
    //         int32_t ret = send(it->first, (char *)ubjson_keepalive.data() + byteswritten,
    //                             (int)ubjson_keepalive.size() - byteswritten, 0);
    //         // -1 means the socket is closed
    //         if (ret == -1)
    //         {
    //             m_sockets.erase(it->first);
    //             break;
    //         }
    //         byteswritten += ret;
    //     }
    // }
}

void SlippicommServer::writeBroadcast()
{
    // Broadcast message structure
    struct broadcast_msg broadcast;

    const char* nickname = SConfig::GetInstance().m_slippiConsoleName.c_str();
    char cmd[] = "SLIP_READY";

    memcpy(broadcast.cmd, cmd, sizeof(broadcast.cmd));
    memset(broadcast.mac_addr, 0, sizeof(broadcast.mac_addr));
    strncpy(broadcast.nickname, nickname, sizeof(broadcast.nickname));

    sendto(m_broadcast_socket, (char*)&broadcast, sizeof(broadcast), 0,
        (struct sockaddr *)&m_broadcastAddr, sizeof(m_broadcastAddr));
}

void SlippicommServer::handleMessage(char *buffer, u32 length, u16 peer_id)
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
        else {
            std::cout << "ERROR: GOT a weird message type!" << std::endl;
        }
    }


    // Put the client in the right in_game state
    m_sockets[peer_id]->m_in_game = m_in_game;
    m_sockets[peer_id]->m_shook_hands = true;
}

void SlippicommServer::SlippicommSocketThread(void)
{
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

        ENetEvent event;
        while (enet_host_service (server, &event, 0) > 0)
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
                    handleMessage(event.peer->data, event.peer->dataLength, event.peer->outgoingPeerID);
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


    //     // We timed out. So take this moment to send any keepalives that need sending
    //     if(numActiveSockets == 0)
    //     {
    //         std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    //         m_write_time_mutex.lock();
    //         std::chrono::system_clock::time_point last_write = m_last_write_time;
    //         m_write_time_mutex.unlock();
    //         if(now - std::chrono::seconds(2) > last_write)
    //         {
    //             writeKeepalives();
    //             m_write_time_mutex.lock();
    //             m_last_write_time = now;
    //             m_write_time_mutex.unlock();
    //         }
    //
    //         // Broadcasts are on their own timer. Send one every 2 seconds-ish
    //         // In a perfect world, we'd have these setup on a signal-based timer but...
    //         if(now - std::chrono::seconds(2) > m_last_broadcast_time)
    //         {
    //             writeBroadcast();
    //             m_last_broadcast_time = now;
    //         }
    //         continue;
    //     }
    //
    //     // For each new read socket that has activity, handle it
    //     for(int sock = 0; sock <= maxFD; ++sock)
    //     {
    //         if(FD_ISSET(sock, &read_fds))
    //         {
    //             // If the socket that just got activity is the server FD, then we have a
    //             // whole new connection ready to come in. accept() it
    //             if(sock == m_server_fd)
    //             {
    //                 SOCKET new_socket;
    //                 if ((new_socket = accept(m_server_fd,
    //                                    (struct sockaddr *)&address,
    //                                    (socklen_t*)&addrlen))<0)
    //                 {
    //                     WARN_LOG(SLIPPI, "Failed listening to Slippi streaming socket");
    //                     return;
    //                 }
    //
    //                 #ifdef _WIN32
    //                 u_long mode = 1;
    //                 ioctlsocket(new_socket, FIONBIO, &mode);
    //                 #else
    //                 fcntl(new_socket, F_SETFL, fcntl(new_socket, F_GETFL, 0) | O_NONBLOCK);
    //                 #endif
    //
    //                 // Add the new socket to the list
    //                 std::shared_ptr<SlippiSocket> newSlippiSocket(new SlippiSocket());
    //                 m_sockets[new_socket] = newSlippiSocket;
    //             }
    //             else
    //             {
    //                 // The socket that has activity must be new data coming in on
    //                 // an existing connection. Handle that message
    //                 handleMessage(sock);
    //             }
    //         }
    //     }
    //
    //     // For each write socket that is available, write to it
    //     for(int sock = 0; sock <= maxFD; ++sock)
    //     {
    //         if(FD_ISSET(sock, &write_fds))
    //         {
    //             if(m_sockets.find(sock) != m_sockets.end() && m_sockets[sock]->m_shook_hands)
    //             {
    //                 writeEvents(sock);
    //             }
    //         }
    //     }
    }

    enet_host_destroy(server);
    enet_deinitialize();
}

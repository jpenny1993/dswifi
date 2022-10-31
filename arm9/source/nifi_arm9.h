#ifndef NIFI_ARM9_H
#define NIFI_ARM9_H

#include <nds/ndstypes.h>

#define WIFI_TRANSMIT_RATE 0x0014           // Data transfer rate (2mbits/10)
#define WIFI_FRAME_OFFSET 32                // The size of a WiFi frame
#define WIFI_TTL 120                        // Number of server ticks a packet should be retryable for
#define WIFI_TTL_RATE 20                    // Number of server ticks before a message should be retried

#define RAW_PACKET_LENGTH 256U              // Total packet size, around 250 characters
#define READ_PARAM_LENGTH 32U               // Max length of each parameter in a packet
#define READ_PARAM_COUNT 14U                // Max allowed parameters in a packet, both defined and custom

#define REQUEST_GAMEID_INDEX 0U             // Index of unique game identifier
#define REQUEST_ROOMID_INDEX 1U             // Index of unique room identifier
#define REQUEST_COMMAND_INDEX 2U            // Index of the message type
#define REQUEST_ACK_INDEX 3U                // Index of the acknowledgement flag
#define REQUEST_MESSAGEID_INDEX 4U          // Index of the message identifier
#define REQUEST_TO_INDEX 5U                 // Index of the target client ID
#define REQUEST_FROM_INDEX 6U               // Index of the sending client ID
#define REQUEST_MAC_INDEX 7U                // Index of the sending NDS machine address
#define REQUEST_DATA_START_INDEX 8U         // Index of the first custom data parameter
#define REQUEST_DATA_PARAM_COUNT (READ_PARAM_COUNT - REQUEST_DATA_START_INDEX) // Max number of custom data parameters in a packet

#define INDEX_UNKNOWN -1                    // Unknown array index
#define ID_EMPTY 0U                         // Default identifier value when using uint8
#define ID_ANY 127U                         // MAX value of a uint8

#define CMD_ROOM_SEARCH "SCAN"              // Request that nearby rooms announce their presence
#define CMD_ROOM_ANNOUNCE "ROOM"            // Announce room presence and info
#define CMD_ROOM_JOIN "JOIN"                // Request to join an existing room
#define CMD_ROOM_CONFIRM_JOIN "ACCEPT"      // Approve a join room request
#define CMD_ROOM_DECLINE_JOIN "DENY"        // Decline the join room request
#define CMD_ROOM_LEAVE "QUIT"               // Announce client disconnect to host
#define CMD_ROOM_DISCONNECTED "LEFT"        // Host announces client disconnect to other clients
#define CMD_HOST_MIGRATE "MIGRATE"          // Announce host migration to one of the clients
#define CMD_HOST_ANNOUNCE "HOST"            // Announce self as new host
#define CMD_CLIENT_ANNOUNCE "CLIENT"        // Annouce  client ID and name
#define CMD_CLIENT_POSITION "POSITION"      // Announce client position
#define CMD_CLIENT_SCORE "SCORE"            // Announce client score
#define CMD_CLIENT_ACTION "ACT"             // Announce client action

#define CLIENT_MAX 6U                       // Total room members including the server
#define COMMAND_LENGTH 9U                   // Length of the command parameter in a packet
#define GAME_ID_LENGTH 5U                   // Length of the unique game identifier
#define MAC_ADDRESS_LENGTH 13U              // Length of an NDS MAC Address
#define PROFILE_NAME_LENGTH 10U             // Length of an NDS Profile Name

typedef struct {
    bool isProcessed;                       // Message has been acknowledged by other devices
    bool isAcknowledgement;                 // Receipt that a message was received
    u16 messageId;                          // Message ID for acknowledgements (up to 65534)
    u8 timeToLive;                          // Time to live before the packet is dropped
    u8 toClientId;                          // Client ID in the joined room to talk to. (1 - 126 is Player, 127 is ALL)
    u8 fromClientId;                        // Client ID in the joined room the message is coming from
    char command[COMMAND_LENGTH];           // Game Command i.e. client, score, position, action
    char macAddress[MAC_ADDRESS_LENGTH];    // MAC address of the NDS that sent the message
    char data[REQUEST_DATA_PARAM_COUNT][READ_PARAM_LENGTH]; // Message content, up to 6 params with up to 32 chars each
} NiFiPacket;

typedef struct {
    u8 clientId;                            // Used to identify client in the room (1 - 126 is Player, 0 is EMPTY)
    char macAddress[MAC_ADDRESS_LENGTH];    // Used to register and verify messages
    char playerName[PROFILE_NAME_LENGTH];   // Player name from their NDS profile
    u16 lastMessageId;                      // Last received message ID acknowledgement syncing (up to 65534)
} NiFiClient;

typedef struct {
    char macAddress[MAC_ADDRESS_LENGTH];    // Used to register and verify messages
    char roomName[PROFILE_NAME_LENGTH];     // Player name from their NDS profile
    u8 roomSize;                            // Total allowed members in the room
    u8 memberCount;                         // Total members currently in the room
} NiFiRoom;

typedef struct {
    int x;
    int y;
    int z;
} Position;

enum DebugMessageType {
    DBG_Information = 0,
    DBG_Error = 1,
    DBG_RawPacket = 2,
    DBG_SentPacket = 3,
    DBG_ReceivedPacket = 4,
    DBG_Acknowledgement = 5
};

typedef void (*DebugMessageHandler)(int, char *);

typedef void (*RoomHandler)(NiFiRoom);

typedef void (*ClientHandler)(u8, NiFiClient);

typedef void (*DisconnectHandler)();

typedef void (*PositionHandler)(Position, u8, NiFiClient);

typedef void (*GamePacketHandler)(NiFiPacket);

extern NiFiClient clients[CLIENT_MAX];
extern NiFiClient *localClient;
extern NiFiClient *host;

extern void NiFi_Init(int wifiChannel, int timerId, char gameIdentifier[GAME_ID_LENGTH]);

extern void NiFi_ResetBuffers();

extern void NiFi_Shutdown();

extern void NiFi_SetDebugOutput(DebugMessageHandler handler);

extern void NiFi_OnRoomAnnounced(RoomHandler handler);

extern void NiFi_OnJoinAccepted(RoomHandler handler);

extern void NiFi_OnJoinDeclined(RoomHandler handler);

extern void NiFi_OnClientConnected(ClientHandler handler);

extern void NiFi_OnClientDisconnected(ClientHandler handler);

extern void NiFi_OnDisconnected(DisconnectHandler handler);

extern void NiFi_OnHostMigration(ClientHandler handler);

extern void NiFi_OnPositionUpdated(PositionHandler handler);

extern void NiFi_OnGamePacket(GamePacketHandler handler);

extern void NiFi_CreateRoom();

extern void NiFi_ScanRooms();

extern void NiFi_JoinRoom(char roomMacAddress[MAC_ADDRESS_LENGTH]);

extern void NiFi_LeaveRoom();

extern void NiFi_BroadcastPosition(Position position);

extern void NiFi_QueuePacket(NiFiPacket *packet);

extern void NiFi_QueueBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]);

extern void NiFi_SendPacket(NiFiPacket *packet);

extern void NiFi_SendBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]);

#endif // NIFI_ARM9_H
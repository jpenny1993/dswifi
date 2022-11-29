#include <stdio.h>
#include <nds.h>
#include <dswifi9.h>
#include "nifi_arm9.h"

// RAW WiFi data, contains packets from any nearby wireless devices
int WiFi_ReceivedLength;               // The size of the incoming data from the WiFi receiver
char WiFi_ReceivedBuffer[1024];        // A raw buffer of the incoming data from the WiFi receiver (DO NOT USE DIRECTLY!)
#define IncomingData (WiFi_ReceivedBuffer + WIFI_FRAME_OFFSET) // Safe accessor for WiFi_ReceivedBuffer array

// Unprocessed incoming NiFi data
char EncodedPacketBuffer[1024];        // Packets waiting to be processed
char TempBuffer[1024];                 // Incomplete packets that couldn't be processed yet

// Encoded packet buffers for incoming/outgoing messages
char DecodePacketBuffer[READ_PARAM_COUNT][READ_PARAM_LENGTH] = {0}; // The decode buffer for identified packets

// Staging objects for decoding packets and creating acknowledgement packets
NiFiPacket IncomingPacket;          // The incoming packet used by the packet decoder
NiFiPacket AcknowledgementPacket;   // The outgoing packet used to prepare acknowledgements
u8 PktRoomId;                       // The room ID of the incoming packet
u8 PktToClientId;                   // The TO Client ID of the incoming packet

// Decoded packet buffers for incoming/outgoing messages
u8 ipIndex, opIndex, akIndex, spIndex;
NiFiPacket IncomingPackets[12], OutgoingPackets[18];

int TimerId = 0;
char GameIdentifier[GAME_ID_LENGTH] = "NiFi";
bool IsHost;
u16 CurrentMessageId;
u8 MyRoomId;
u8 LastClientId;

NiFiClient clients[CLIENT_MAX];
NiFiClient *localClient = &clients[0];
NiFiClient *host;

DebugMessageHandler debugMessageHander = 0;
RoomHandler roomAnnouncedHandler = 0;
RoomHandler joinAcceptedHandler = 0;
RoomHandler joinDeclinedHandler = 0;
ClientHandler clientConnectHandler = 0;
ClientHandler clientDisconnectHandler = 0;
DisconnectHandler disconnectHandler = 0;
ClientHandler hostMigrationHandler = 0;
PositionHandler positionHandler = 0;
GamePacketHandler gamePacketHandler = 0;

/// @brief Outputs a message to the debug message handler.
void PrintDebug(int type, char *message) {
   if (debugMessageHander == 0) return;
   (*debugMessageHander)(type, message);
}

/// @brief Generates a random number
/// @return A value between 1 and 127
u8 RandomByte() {
   return (rand() % 126 + 1) & 0xff;
}

/// @brief Gets the machine address of the current NDS
/// @param macAddress A string to place the copied MAC address
void GetMacAddress(char macAddress[MAC_ADDRESS_LENGTH]) {
   memset(macAddress, 0, MAC_ADDRESS_LENGTH);

   // Get the mac address from the DS
   u8 macAddressUnsigned[6];
   Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, macAddressUnsigned);

   // Convert unsigned values to hexa values
   int pos = 0;
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[0]);
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[1]);
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[2]);
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[3]);
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[4]);
   pos += sprintf(&macAddress[pos], "%02X", macAddressUnsigned[5]);
}

/// @brief Gets the DS owners profile name and copies it onto the given string
/// @param profileName A string to place the copied profile name
void GetProfileName(char profileName[PROFILE_NAME_LENGTH]) {
   memset(profileName, 0, PROFILE_NAME_LENGTH);
   u16 nameLength = PersonalData->nameLen;
   // Fallback for no profile name
   if (nameLength <= 0) {
      strcpy(profileName,  "Player");
      return;
   }
   // Cast chars from the UTF-16 name stored on DS
   for (u8 index = 0; index < nameLength; index++) {
      profileName[index] = (char)PersonalData->name[index] & 255;
   }
}

/// @brief Finds the first index of a client using the same client ID
/// @param clientId Client identifier in the current room
/// @return Client array index, or -1 when not found
int8 IndexOfClientUsingId(u8 clientId) {
   if (clientId == ID_ANY) return INDEX_UNKNOWN;
   for (int8 index = 0; index < CLIENT_MAX; index++) {
      if (clients[index].clientId != clientId) continue;
      return index;
   }
   return INDEX_UNKNOWN;
}

/// @brief Finds the first index of a client using the same mac address
/// @param macAddress An NDS machine address
/// @return Client array index, or -1 when not found
int8 IndexOfClientUsingMacAddress(char macAddress[MAC_ADDRESS_LENGTH]) {
   for (int8 index = 0; index < CLIENT_MAX; index++) {
      if (clients[index].clientId == ID_EMPTY) continue;
      if (strcmp(clients[index].macAddress, macAddress) != 0) continue;
      return index;
   }
   return INDEX_UNKNOWN;
}

/// @brief Counts the total active clients
/// @return Total active clients including the host
u8 CountActiveClients() {
   u8 counter = 0;
   for (u8 i = 0; i < CLIENT_MAX; i++) {
      if (clients[i].clientId != 0) {
         counter++;
      }
   }
   return counter;
}

/// @brief Generates a new client ID
/// @return An incrementing value from 2 to 126
u8 NewClientId() {
   bool used = false;
   do {
      // Increment
      LastClientId++;
      // Reset to first client
      if (LastClientId >= ID_ANY) {
         LastClientId = 2; // 1 is always the room host
      }
      // Retry until unique client ID
      used = IndexOfClientUsingId(LastClientId) != INDEX_UNKNOWN;
   } while (used);
   return LastClientId;
}

/// @brief Attempts to find an empty slot and initialise a NiFi client
/// @param clientId Client ID to set, also used to check for existing
/// @param macAddress Mac address to set, also used to check for existing
/// @param playerName Player name to set
/// @return clientId if configured or existing, INDEX_UNKNOWN if player slots full
int8 SetupNiFiClient(u8 clientId, char macAddress[MAC_ADDRESS_LENGTH], char playerName[PROFILE_NAME_LENGTH]) {
   // Disallow reserved values
   if (clientId == ID_EMPTY || clientId == ID_ANY)
      return INDEX_UNKNOWN;
   // Try find out if the client already exists
   int8 clientIndex = IndexOfClientUsingId(clientId);
   if (clientIndex != INDEX_UNKNOWN) return clientIndex;
   clientIndex = IndexOfClientUsingMacAddress(macAddress);
   if (clientIndex != INDEX_UNKNOWN) return clientIndex;
   // Otherwise find an empty slot to populate
   clientIndex = IndexOfClientUsingId(ID_EMPTY);
   // Back out if no empty slot available 
   if (clientIndex == INDEX_UNKNOWN) return clientIndex;
   // Setup player data on empty client
   clients[clientIndex].clientId = clientId;
   strncpy(clients[clientIndex].macAddress, macAddress, MAC_ADDRESS_LENGTH);
   strncpy(clients[clientIndex].playerName, playerName, PROFILE_NAME_LENGTH);
   return clientIndex;
}

/// @brief Identifies whether the incoming packet should be queued for processing
/// @param params The decoded packet as a string array
/// @return true if the NDS might need to process the packet
bool IsPacketIntendedForMe(char (*params)[READ_PARAM_LENGTH]) {
   // Ignore messages from other applications
   if (strcmp(params[REQUEST_GAMEID_INDEX], GameIdentifier) != 0) {
      return false;
   }
   // Ignore packets from myself
   if (strcmp(params[REQUEST_MAC_INDEX], localClient->macAddress) == 0) {
       return false;
   }
   // Accept ACKs, but ONLY for my own packets
   bool isAck = strcmp(params[REQUEST_ACK_INDEX], "1") == 0; 
   if (isAck && strcmp(params[REQUEST_DATA_START_INDEX], localClient->macAddress) == 0) {
       return true;
   }
   // 1-126 is room data, 127 is a scan
   sscanf(params[REQUEST_ROOMID_INDEX], "%hhd", &PktRoomId);
   // Accept instruction packets ONLY for the room I'm inside
   if (!isAck && PktRoomId == MyRoomId && MyRoomId != ID_ANY) {
      // Accept scans and announcements from new rooms using the same ID
      if (IsHost && (strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_SEARCH) == 0 ||
                    strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_ANNOUNCE) == 0)) {
         return true;
      }
      // Ignore game packets that aren't directed at me
      sscanf(params[REQUEST_TO_INDEX], "%hhd", &PktToClientId);
      if (PktToClientId != localClient->clientId) {
         return false;
      }
      // Ignore packets from unknown clients
      return IndexOfClientUsingMacAddress(params[REQUEST_MAC_INDEX]) != INDEX_UNKNOWN;
   }
   // Accept searches and join requests for the room I'm hosting
   if (IsHost && PktRoomId == ID_ANY) {
      return strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_SEARCH) == 0 ||
             (strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_JOIN) == 0 &&
              strcmp(params[REQUEST_DATA_START_INDEX], localClient->macAddress) == 0);
   }
   // Accept room announcements and join responses when I'm looking for a room
   if (MyRoomId == ID_ANY && PktRoomId != MyRoomId &&
       strcmp(params[REQUEST_DATA_START_INDEX], localClient->macAddress) == 0) {
      if (strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_CONFIRM_JOIN) == 0) {
         // Copy room ID early, then handle the confirm packet later as a client
         MyRoomId = PktRoomId;
         return true;
      }
      return strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_ANNOUNCE) == 0 ||
             strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_CONFIRM_JOIN) == 0 ||
             strcmp(params[REQUEST_COMMAND_INDEX], CMD_ROOM_DECLINE_JOIN) == 0;
   }
   return false;
}

/// @brief Copies packet onto the incoming packet buffer, if the buffer is full then the oldest message is overwritten
/// @param packet incoming NiFi packet
void EnqueueIncomingPacket(NiFiPacket *packet) {
   // Warn if needed
   if (!IncomingPackets[ipIndex].isProcessed) {
      PrintDebug(DBG_Error, "Overwriting incoming packet");
   }

   // Copy packet onto array
   IncomingPackets[ipIndex].isProcessed = packet->isProcessed;
   IncomingPackets[ipIndex].isAcknowledgement = packet->isAcknowledgement;
   IncomingPackets[ipIndex].messageId = packet->messageId;
   IncomingPackets[ipIndex].timeToLive = packet->timeToLive;
   IncomingPackets[ipIndex].toClientId = packet->toClientId;
   IncomingPackets[ipIndex].fromClientId = packet->fromClientId;
   memset(IncomingPackets[ipIndex].command, 0, sizeof(IncomingPackets[ipIndex].command));
   strncpy(IncomingPackets[ipIndex].command, packet->command, sizeof(IncomingPackets[ipIndex].command));
   memset(IncomingPackets[ipIndex].macAddress, 0, sizeof(IncomingPackets[ipIndex].macAddress));
   strncpy(IncomingPackets[ipIndex].macAddress, packet->macAddress, sizeof(IncomingPackets[ipIndex].macAddress));
   memset(IncomingPackets[ipIndex].data, 0, sizeof(IncomingPackets[ipIndex].data));
   for (u8 di = 0; di < REQUEST_DATA_PARAM_COUNT; di++) {
      strncpy(IncomingPackets[ipIndex].data[di], packet->data[di], READ_PARAM_LENGTH);
   }

   // Find next available packet index to use
   // either the next packet to have been processed or the oldest packet
   u8 arraySize = (sizeof(IncomingPackets) / sizeof(NiFiPacket));
   u8 counter = 0;
   do {
      ipIndex += 1;
      if (ipIndex == arraySize) ipIndex = 0;
      if (IncomingPackets[ipIndex].isProcessed) break;
   } while(counter++ < arraySize);
}

void DecodePacket(NiFiPacket *packet, u8 readParams) {
   // FORMAT: {GID;RID;CMD;ACK;MID;TO;FROM;MAC;DATA}
   // We can ignore GID and RID at this point
   packet->isProcessed = false;

   // Read the packet headers
   strcpy(packet->command, DecodePacketBuffer[REQUEST_COMMAND_INDEX]);
   packet->isAcknowledgement = strcmp(DecodePacketBuffer[REQUEST_ACK_INDEX], "1") == 0;
   sscanf(DecodePacketBuffer[REQUEST_MESSAGEID_INDEX], "%hd", &packet->messageId);
   sscanf(DecodePacketBuffer[REQUEST_TO_INDEX], "%hhd", &packet->toClientId);
   sscanf(DecodePacketBuffer[REQUEST_FROM_INDEX], "%hhd", &packet->fromClientId);
   strcpy(packet->macAddress, DecodePacketBuffer[REQUEST_MAC_INDEX]);

   // Zero the message content
   memset(packet->data, 0, sizeof(packet->data));

   // Copy the received message content
   for (int paramIndex = REQUEST_DATA_START_INDEX; paramIndex < REQUEST_DATA_PARAM_COUNT; paramIndex++) {
      if (paramIndex > readParams) break;
      if (strlen(DecodePacketBuffer[paramIndex]) > 0) {
         int dataIndex = paramIndex - REQUEST_DATA_START_INDEX;
         strcpy(packet->data[dataIndex], DecodePacketBuffer[paramIndex]);
      }
   }
}

/// @brief Decodes the NiFi packet between the given indices
/// @param startPosition start index for the packet
/// @param endPosition end index for the packet
void ProcessEncodedPacketBuffer(int startPosition, int endPosition) {
   // Get the string packet inbetween the curly braces
   char currentPacket[RAW_PACKET_LENGTH] = "";
   strncpy(currentPacket, EncodedPacketBuffer + startPosition, endPosition - startPosition);
   if (debugMessageHander) {
      memset(TempBuffer, 0, sizeof(TempBuffer)); 
      sprintf(TempBuffer, "{%s}", currentPacket);
      PrintDebug(DBG_RawPacket, TempBuffer);
   }

   // Get the index of the parameter delimiter
   char *ptr = strtok(currentPacket, ";");
   int splitCount = 0;

   // Copy the value of each parameter into an array for decoding
   memset(DecodePacketBuffer, 0, sizeof(DecodePacketBuffer));
   while (ptr != NULL) {
      strcpy(DecodePacketBuffer[splitCount], ptr);
      splitCount++;
      ptr = strtok(NULL, ";");
   }

   // Decode packet and copy onto processing buffer for later
   if (IsPacketIntendedForMe(DecodePacketBuffer)) {
      DecodePacket(&IncomingPacket, splitCount);
      EnqueueIncomingPacket(&IncomingPacket);
      if (debugMessageHander) {
         if (IncomingPacket.isAcknowledgement)
            PrintDebug(DBG_ReceivedAcknowledgement, TempBuffer);
         else
            PrintDebug(DBG_ReceivedPacket, TempBuffer);
      }
   }
}

/// @brief The event handler for incoming WiFi packets.
/// Needs to complete as fast as possible so that we can continue to receive WiFi data.
/// We should not be forwarding packets into game code from this routine.
/// @param packetID locates the packet specified in the internal buffer
/// @param readlength the number of bytes to read
void OnRawPacketReceived(int packetID, int readlength) {
   // Read the received packet
   Wifi_RxRawReadPacket(packetID, readlength, (unsigned short *)WiFi_ReceivedBuffer);
   WiFi_ReceivedLength = readlength - WIFI_FRAME_OFFSET;

   // Ensure that all data contained in the packet are ASCII characters
   int receivedDataLength = strlen(IncomingData);
   if (WiFi_ReceivedLength != receivedDataLength + 1) {
      return;
   }

   // It's not guaranteed that we will receive a full packet in a single read
   strcat(EncodedPacketBuffer, IncomingData);

   // Try process any encoded packets that we can identify
   // Then add all remaining characters after current data packet back onto the data buffer
   int startPosition, endPosition;
   while ((startPosition = strstr(EncodedPacketBuffer, "{") - EncodedPacketBuffer + 1) > 0 &&
            (endPosition = strstr(EncodedPacketBuffer + startPosition, "}") - EncodedPacketBuffer) > 0) {
      ProcessEncodedPacketBuffer(startPosition, endPosition);
      memset(TempBuffer, 0, sizeof(TempBuffer));
      strcat(TempBuffer, EncodedPacketBuffer + endPosition + 1);
      strcpy(EncodedPacketBuffer, TempBuffer);
   }
}

/// @brief Encodes and writes the given packet onto the specified buffer
/// @param packet NiFi packet to write
/// @param buffer outgoing or print buffer
/// @return length of the written packet
int WritePacketToBuffer(NiFiPacket *packet, char buffer[RAW_PACKET_LENGTH]) {
   // FORMAT: {GID;RID;CMD;ACK;MID;TO;FROM;MAC;DATA}
   memset(buffer, 0, RAW_PACKET_LENGTH);
   int pos = 0;
   buffer[pos++] = '{';
   pos += sprintf(&buffer[pos], "%s", GameIdentifier);
   pos += sprintf(&buffer[pos], ";%hhd", MyRoomId);
   pos += sprintf(&buffer[pos], ";%s", packet->command);
   pos += sprintf(&buffer[pos], ";%d", packet->isAcknowledgement);
   pos += sprintf(&buffer[pos], ";%d", packet->messageId);
   pos += sprintf(&buffer[pos], ";%hhd", packet->toClientId);
   pos += sprintf(&buffer[pos], ";%hhd", packet->fromClientId);
   pos += sprintf(&buffer[pos], ";%s", localClient->macAddress);
   for (int i = 0; i < REQUEST_DATA_PARAM_COUNT; i++) {
      if (strlen(packet->data[i]) == 0) continue;
      pos += sprintf(&buffer[pos], ";%s", packet->data[i]);
   }
   buffer[pos++] = '}';
   buffer[pos++] = '\0'; // TODO sort issues with inconsistent memory and wifi frame drops
   return pos;
}

/// @brief Clears all data in the packet and sets the command
/// @param packet The NiFi packet to configure
/// @param commandCode The command code / packet type
void NiFi_SetPacket(NiFiPacket *packet, char commandCode[COMMAND_LENGTH]) {
   // FORMAT: {GID;RID;CMD;MID;ACK;TO;FROM;MAC;DATA}
   // GID and RID will only be included during send
   packet->isProcessed = false;
   packet->timeToLive = WIFI_TTL;
   memset(packet->command, 0, sizeof(packet->command));
   strcpy(packet->command, commandCode);
   packet->messageId = ++CurrentMessageId;
   packet->isAcknowledgement = false;
   memset(packet->macAddress, 0, sizeof(packet->macAddress));
   strcpy(packet->macAddress, localClient->macAddress);
   packet->toClientId = ID_ANY;
   packet->fromClientId = localClient->clientId;
   memset(packet->data, 0, sizeof(packet->data));
   if (CurrentMessageId == U16_MAX) {
      CurrentMessageId = 0; // MessageID rollover
   }
}

/// @brief Sends a NiFi packet over the WiFi adapter, sent packets will not expect acknowledgements
/// @param packet outgoing NiFi packet to be sent
void NiFi_SendPacket(NiFiPacket *packet) {
   char outgoingBuffer[RAW_PACKET_LENGTH]; // needs to be a separate buffer to account for game devs using send instead of queue
   int packetLength = WritePacketToBuffer(packet, outgoingBuffer);
   if (debugMessageHander) {
      if (packet->isAcknowledgement)
         PrintDebug(DBG_SentAcknowledgement, outgoingBuffer);
      else
         PrintDebug(DBG_SentPacket, outgoingBuffer);
   }

   int packetSent = Wifi_RawTxFrame(packetLength, WIFI_TRANSMIT_RATE, (unsigned short *)outgoingBuffer);
   if (packetSent == -1) {
      // I'm doubtful this will work if there's no available memory
      PrintDebug(DBG_Error, "Unable to send RawTxFrame over WiFi due to memory limitations");
   }
}

/// @brief Creates and sends an acknowledgement for the received packet
/// @param packet incoming NiFi packet to be acknowledged
void SendAcknowledgement(NiFiPacket *receivedPacket)
{
   AcknowledgementPacket.isAcknowledgement = true;
   // Set the current MAC address as the sender
   memset(AcknowledgementPacket.macAddress, 0, sizeof(AcknowledgementPacket.macAddress));
   strcpy(AcknowledgementPacket.macAddress, localClient->macAddress);
   // Reverse the packet direction for response
   AcknowledgementPacket.toClientId = receivedPacket->fromClientId;
   AcknowledgementPacket.fromClientId = localClient->clientId;
   // Include the message ID for validation
   AcknowledgementPacket.messageId = receivedPacket->messageId;
   // Include the command message for validation
   memset(AcknowledgementPacket.command, 0, sizeof(AcknowledgementPacket.command));
   strcpy(AcknowledgementPacket.command, receivedPacket->command);
   // Include senders MAC address for validation
   memset(AcknowledgementPacket.data, 0, sizeof(AcknowledgementPacket.data));
   strcpy(AcknowledgementPacket.data[0], receivedPacket->macAddress);
   // GID and RID are then included when sending
   NiFi_SendPacket(&AcknowledgementPacket);
}

/// @brief Sends a NiFi packet to all clients, sent packets will not expect acknowledgements
/// @param packet outgoing NiFi packet to be broadcasted
/// @param ignoreClientIds client IDs that should not receive the packet
void NiFi_SendBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]) {
   bool contains;
   for (u8 i = 0; i < CLIENT_MAX; i++) {
      if (clients[i].clientId == ID_EMPTY) continue;
      if (clients[i].clientId == localClient->clientId) continue;
      if (ignoreClientIds != NULL) {
         contains = false;
         for (u8 j = 0; j < (sizeof(*ignoreClientIds) / sizeof(u8)); j++) {
            if (clients[i].clientId == ignoreClientIds[j]) {
               contains = true;
               break;
            }
         }
         if (contains) continue;
      }
      packet->toClientId = clients[i].clientId;
      NiFi_SendPacket(packet);
   }
}

/// @brief Queues a NiFi packet to be sent, sent packets will expect acknowledgements.
/// If the outgoing buffer is full then the oldest message is overwritten.
/// @param packet outgoing NiFi packet to be sent
void NiFi_QueuePacket(NiFiPacket *packet) {
   // Warn if needed
   if (!OutgoingPackets[opIndex].isProcessed) {
      PrintDebug(DBG_Error, "Overwriting outgoing packet");
   }

   // Copy packet onto array
   OutgoingPackets[opIndex].isProcessed = packet->isProcessed;
   OutgoingPackets[opIndex].isAcknowledgement = packet->isAcknowledgement;
   OutgoingPackets[opIndex].messageId = packet->messageId;
   OutgoingPackets[opIndex].timeToLive = packet->timeToLive;
   OutgoingPackets[opIndex].toClientId = packet->toClientId;
   OutgoingPackets[opIndex].fromClientId = packet->fromClientId;
   memset(OutgoingPackets[opIndex].command, 0, sizeof(OutgoingPackets[opIndex].command));
   strncpy(OutgoingPackets[opIndex].command, packet->command, sizeof(OutgoingPackets[opIndex].command));
   memset(OutgoingPackets[opIndex].macAddress, 0, sizeof(OutgoingPackets[opIndex].macAddress));
   strncpy(OutgoingPackets[opIndex].macAddress, packet->macAddress, sizeof(OutgoingPackets[opIndex].macAddress));
   memset(OutgoingPackets[opIndex].data, 0, sizeof(OutgoingPackets[opIndex].data));
   for (u8 di = 0; di < REQUEST_DATA_PARAM_COUNT; di++) {
      strncpy(OutgoingPackets[opIndex].data[di], packet->data[di], READ_PARAM_LENGTH);
   }

   // Find next available index to write a packet
   u8 arraySize = (sizeof(OutgoingPackets) / sizeof(NiFiPacket));
   u8 counter = 0;
   do {
      opIndex += 1;
      if (opIndex == arraySize) opIndex = 0;
      if (OutgoingPackets[opIndex].isProcessed) break;
   } while(counter++ < arraySize);
}

/// @brief Queues a NiFi packet to be sent to all clients, sent packets will expect acknowledgements.
/// If the outgoing buffer is full then the oldest message is overwritten.
/// @param packet outgoing NiFi packet to be broadcasted
/// @param ignoreClientIds client IDs that should not receive the packet
void NiFi_QueueBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]) {
   bool contains;
   for (u8 i = 0; i < CLIENT_MAX; i++) {
      if (clients[i].clientId == ID_EMPTY) continue;
      if (clients[i].clientId == localClient->clientId) continue;
      if (ignoreClientIds != NULL) {
         contains = false;
         for (u8 j = 0; j < (sizeof(*ignoreClientIds) / sizeof(u8)); j++) {
            if (clients[i].clientId == ignoreClientIds[j]) {
               contains = true;
               break;
            }
         }
         if (contains) continue;
      }
      packet->toClientId = clients[i].clientId;
      NiFi_QueuePacket(packet);
   }
}

/// @brief Host a room for others to join
void NiFi_CreateRoom() {
   if (MyRoomId != ID_ANY) return;
   IsHost = true;
   MyRoomId = RandomByte();
   localClient->clientId = LastClientId = 1;
   host = localClient;
   if (debugMessageHander > 0) {
      char debugMessage[50];
      sprintf(debugMessage, "HOSTING A ROOM %d as %s\n", MyRoomId, localClient->playerName);
      PrintDebug(DBG_Information, debugMessage);
   }
   // Search for other rooms using the same room ID
   NiFiPacket p;
   NiFi_SetPacket(&p, CMD_ROOM_SEARCH);
   NiFi_SendPacket(&p);
}

/// @brief Request nearby rooms to announce their presence
void NiFi_ScanRooms() {
   if (MyRoomId != ID_ANY) return;
   PrintDebug(DBG_Information, "Searching for rooms");
   NiFiPacket p;
   MyRoomId = ID_ANY;
   localClient->clientId = ID_ANY;
   NiFi_SetPacket(&p, CMD_ROOM_SEARCH);
   NiFi_SendPacket(&p);
}

/// @brief Request to join a nearby room
/// @param roomMacAddress MAC Address of the room
void NiFi_JoinRoom(char roomMacAddress[MAC_ADDRESS_LENGTH]) {
   if (MyRoomId != ID_ANY) return;
   PrintDebug(DBG_Information, "Attempting to join room");
   NiFiPacket r;
   NiFi_SetPacket(&r, CMD_ROOM_JOIN);
   strcpy(r.data[0], roomMacAddress);
   strcpy(r.data[1], localClient->playerName); // My display name
   NiFi_SendPacket(&r);
}

/// @brief Announce intent to leave the current room, hosts perform a host migration
void NiFi_LeaveRoom() {
   if (MyRoomId == ID_ANY) return;
   NiFiPacket p;
   u8 hostId = host->clientId;
   u8 activeClients = CountActiveClients();

   if (IsHost && activeClients == 1) {
      PrintDebug(DBG_Information, "Closing empty room");
      NiFi_ResetBuffers();
      return;
   }

   if (IsHost && activeClients > 1) {
      PrintDebug(DBG_Information, "Migrating room ownership");
      // Find the next known client
      for (u8 i = 1; i < CLIENT_MAX; i++) {
         if (clients[i].clientId != ID_EMPTY) {
            hostId = clients[i].clientId;
            break;
         }
      }
      NiFi_SetPacket(&p, CMD_HOST_MIGRATE);
      sprintf(p.data[0], "%hhd", MyRoomId);
      sprintf(p.data[1], "%hhd", hostId);
      // Migrate local host reference
      host = &clients[hostId];
      IsHost = false;
      // Broadcast migration to all clients
      NiFi_QueueBroadcast(&p, NULL);
   }

   PrintDebug(DBG_Information, "Leaving room");
   NiFi_SetPacket(&p, CMD_ROOM_LEAVE);
   p.toClientId = hostId;
   NiFi_QueuePacket(&p);
}

/// @brief Broadcasts the player's position to other room members
/// @param position xyz coordinates
void NiFi_BroadcastPosition(Position position) {
   if (MyRoomId == ID_ANY) return;
   PrintDebug(DBG_Information, "Broadcasting position");
   NiFiPacket p;
   NiFi_SetPacket(&p, CMD_CLIENT_POSITION);
   sprintf(p.data[0], "%d", position.x);
   sprintf(p.data[1], "%d", position.y);
   sprintf(p.data[2], "%d", position.z);
   NiFi_QueueBroadcast(&p, NULL);
}

/// @brief Finds the matching outgoing packet and marks it as acknowledged
/// @param p Incoming NiFi packet
void CompleteAcknowledgedPacket(NiFiPacket *p) {
   for (u8 oi = 0; oi < (sizeof(OutgoingPackets) / sizeof(NiFiPacket)); oi++) {
      if (OutgoingPackets[oi].isProcessed) continue;
      if (OutgoingPackets[oi].messageId != p->messageId) continue;
      if (OutgoingPackets[oi].toClientId != p->fromClientId) continue;
      if (strcmp(OutgoingPackets[oi].command, p->command) != 0) continue;
      OutgoingPackets[oi].isProcessed = true;
      PrintDebug(DBG_Information, "Acknowledgement confirmed");
      break;
   }
}

void NotifyPositionUpdate(NiFiPacket *p, u8 clientIndex) {
   if (clientIndex == INDEX_UNKNOWN) return;
   Position pos = { 0, 0, 0 };
   if (strlen(p->data[0]) > 0)
      sscanf(p->data[0], "%d", &(pos.x));
   if (strlen(p->data[1]) > 0)
      sscanf(p->data[1], "%d", &(pos.y));
   if (strlen(p->data[2]) > 0)
      sscanf(p->data[2], "%d", &(pos.z));
   if (positionHandler)
      (*positionHandler)(pos, clientIndex, clients[clientIndex]);
}

void HandlePacketAsSearching(NiFiPacket *p) {
   if (strcmp(p->command, CMD_ROOM_ANNOUNCE) == 0) {
      if (debugMessageHander > 0) {
         char debugMessage[128];
         sprintf(debugMessage, "Room: %s, (%s/%s)\n", p->data[1], p->data[2], p->data[3]);
         PrintDebug(DBG_Information, debugMessage);
      }
      NiFiRoom room;
      strcpy(room.macAddress, p->macAddress);
      strcpy(room.roomName, p->data[1]);
      sscanf(p->data[2], "%hhd", &(room.memberCount));
      sscanf(p->data[3], "%hhd", &(room.roomSize));
      if (roomAnnouncedHandler) {
         (*roomAnnouncedHandler)(room);
      }
      return;
   }
   if (strcmp(p->command, CMD_ROOM_DECLINE_JOIN) == 0) {
      PrintDebug(DBG_Information, "Declined access to room");
      NiFiRoom room;
      strcpy(room.macAddress, p->macAddress);
      strcpy(room.roomName, p->data[1]);
      sscanf(p->data[2], "%hhd", &(room.memberCount));
      sscanf(p->data[3], "%hhd", &(room.roomSize));
      if (joinDeclinedHandler) {
         (*joinDeclinedHandler)(room);
      }
      return;
   }
}

void HandlePacketAsClient(NiFiPacket *p, u8 cIndex) {
   // When another player has announced their position 
   if (strcmp(p->command, CMD_CLIENT_POSITION) == 0) {
      NotifyPositionUpdate(p, cIndex);
      return;
   }
   // When the host updates you on client changes
   if (strcmp(p->command, CMD_CLIENT_ANNOUNCE) == 0) {
      u8 clientId;
      sscanf(p->data[0], "%hhd", &clientId);
      u8 clientIndex = SetupNiFiClient(clientId, p->data[1], p->data[2]);
      if (clientIndex != INDEX_UNKNOWN && clientConnectHandler > 0) {
         (*clientConnectHandler)(clientIndex, clients[clientIndex]);
      }
      return;
   }
   // When I've been disconnected, or I've been notified of someone else disconnecting
   if (strcmp(p->command, CMD_ROOM_DISCONNECTED) == 0) {
      u8 disconnectedId;
      sscanf(p->data[0], "%hhd", &disconnectedId);
      // Current party leader has announced me leaving
      if (disconnectedId == localClient->clientId) {
         NiFi_ResetBuffers();
         if (disconnectHandler) {
            (*disconnectHandler)();
         }
         return;
      }
      // Some other client has been announced as leaving
      u8 dcIndex = IndexOfClientUsingId(disconnectedId);
      if (dcIndex == INDEX_UNKNOWN) return;
      if (clientDisconnectHandler) {
         (*clientDisconnectHandler)(dcIndex, clients[dcIndex]);
      }
      // Only clearing ID so player stats might be retained when rejoining
      clients[dcIndex].clientId = ID_EMPTY;
      return;
   }
   // When the host accepts you into the room, setup their client
   if (strcmp(p->command, CMD_ROOM_CONFIRM_JOIN) == 0) {
      // Setup my client ID
      localClient->clientId = p->toClientId;
      // Setup the host client
      u8 clientIndex = SetupNiFiClient(p->fromClientId, p->macAddress, p->data[1]);
      if (clientIndex == INDEX_UNKNOWN) return;
      host = &clients[clientIndex];
      // Announce join to game devs
      NiFiRoom room;
      strcpy(room.macAddress, p->macAddress);
      strcpy(room.roomName, p->data[1]);
      sscanf(p->data[2], "%hhd", &(room.memberCount));
      sscanf(p->data[3], "%hhd", &(room.roomSize));
      if (joinAcceptedHandler) {
         (*joinAcceptedHandler)(room);
      }
      return;
   }
   // When the host migrates due to room ID conflict, go along with it
   if (strcmp(p->command, CMD_HOST_MIGRATE) == 0) {
      if (cIndex == INDEX_UNKNOWN) return;
      u8 newRoomId, newHostId;
      // People don't need to know when a room ID changes
      sscanf(p->data[0], "%hhd", &newRoomId);
      if (newRoomId != MyRoomId) {
         MyRoomId = newRoomId;
      }
      // People do need to know when the room leader changes
      sscanf(p->data[1], "%hhd", &newHostId);
      if (host->clientId != newHostId) {
         u8 hostIndex = IndexOfClientUsingId(newHostId);
         if (hostIndex != INDEX_UNKNOWN)
            host = &clients[hostIndex];
         IsHost = hostIndex == 0;
         if (hostMigrationHandler) {
            (*hostMigrationHandler)(hostIndex, *host);
         }
      }
      return;
   }
   // If unknown then assume it's from a game dev
   if (gamePacketHandler) {
      (*gamePacketHandler)(*p);
   }
}

void HandlePacketAsHost(NiFiPacket *p, u8 cIndex) {
   NiFiPacket r;
   // When client is searching for a room, announce room presence
   if (strcmp(p->command, CMD_ROOM_SEARCH) == 0) {
      PrintDebug(DBG_Information, "Announcing prescence to searcher");
      NiFi_SetPacket(&r, CMD_ROOM_ANNOUNCE);
      strcpy(r.data[0], p->macAddress);                  // Return MAC
      strcpy(r.data[1], localClient->playerName);        // Room name
      sprintf(r.data[2], "%hhd", CountActiveClients());  // Current clients
      sprintf(r.data[3], "%d", CLIENT_MAX);              // Total clients
      NiFi_SendPacket(&r);
      return;
   }
   // When the client is attempting to join the room, only allow if space
   if (strcmp(p->command, CMD_ROOM_JOIN) == 0) { 
      u8 memberCount = CountActiveClients(); 
      if (memberCount < CLIENT_MAX) {
         u8 newClientId = NewClientId();
         u8 newClientIndex = SetupNiFiClient(newClientId, p->macAddress, p->data[1]);
         if (newClientIndex == INDEX_UNKNOWN) return;
         PrintDebug(DBG_Information, "New client connecting");
         // Join confirmation
         NiFi_SetPacket(&r, CMD_ROOM_CONFIRM_JOIN);
         r.toClientId = newClientId; 
         strcpy(r.data[0], p->macAddress);
         strcpy(r.data[1], localClient->playerName);
         sprintf(p->data[2], "%hhd", memberCount + 1);
         sprintf(p->data[3], "%hhd", CLIENT_MAX);
         NiFi_QueuePacket(&r);
         // Announce new client to existing clients
         NiFi_SetPacket(&r, CMD_CLIENT_ANNOUNCE);
         sprintf(r.data[0], "%hhd", newClientId);
         strcpy(r.data[1], p->macAddress);
         strcpy(r.data[2], p->data[1]);
         u8 ignoreIds[1] = { newClientId };
         NiFi_QueueBroadcast(&r, ignoreIds);
         // Annouce existing clients to new client
         for (u8 i = 0; i < CLIENT_MAX; i++) {
            if (clients[i].clientId == ID_EMPTY) continue;
            if (clients[i].clientId == localClient->clientId) continue;
            if (clients[i].clientId == newClientId) continue;  // TODO: Send client announce to joined player maybe?
            NiFi_SetPacket(&r, CMD_CLIENT_ANNOUNCE);
            r.toClientId = newClientId;
            sprintf(r.data[0], "%hhd", clients[i].clientId);
            strcpy(r.data[1], clients[i].macAddress);
            strcpy(r.data[2], clients[i].playerName);
            NiFi_QueuePacket(&r);
         }
         // Finally let the game dev know
         if (clientConnectHandler) {
            (*clientConnectHandler)(newClientIndex, clients[newClientIndex]);
         }
      }
      else if (IndexOfClientUsingMacAddress(p->macAddress) == INDEX_UNKNOWN) {
         PrintDebug(DBG_Information, "New client blocked");
         NiFi_SetPacket(&r, CMD_ROOM_DECLINE_JOIN);
         strcpy(r.data[0], p->macAddress);
         strcpy(r.data[1], localClient->playerName);
         sprintf(p->data[2], "%hhd", memberCount);
         sprintf(p->data[3], "%hhd", CLIENT_MAX);
         NiFi_SendPacket(&r);
      }
      return;
   }
   // When another room accounces it's presence, migrate room ID and retry
   if (strcmp(p->command, CMD_ROOM_ANNOUNCE) == 0) {
      u8 newRoomId = RandomByte();
      if (strcmp(p->data[0], localClient->macAddress) != 0) {
         // Somehow 2 rooms with the same ID have come near each other
         // Perform a host migration to the existing clients
         PrintDebug(DBG_Information, "RoomId conflict: starting host migration");
         NiFi_SetPacket(&r, CMD_HOST_MIGRATE);
         sprintf(r.data[0], "%hhd", newRoomId);
         sprintf(r.data[1], "%hhd", localClient->clientId);
         NiFi_QueuePacket(&r);
      }
      // Room ID is taken try another
      MyRoomId = newRoomId;
      NiFi_SetPacket(&r, CMD_ROOM_SEARCH);
      NiFi_QueuePacket(&r); // Maybe only queue if ACK is required?
      return;
   }
   // When a client disconnects from the room, mark them as empty
   if (strcmp(p->command, CMD_ROOM_LEAVE) == 0) {
      if (cIndex == INDEX_UNKNOWN) return;
      PrintDebug(DBG_Information, "Client is disconnecting");
      NiFi_SetPacket(&r, CMD_ROOM_DISCONNECTED);
      sprintf(r.data[0] , "%hhd", p->fromClientId);
      NiFi_QueueBroadcast(&r, NULL);
      if (clientDisconnectHandler) {
         (*clientDisconnectHandler)(cIndex, clients[cIndex]);
      }
      clients[cIndex].clientId = ID_EMPTY;
      return;
   }
   // When another player has announced their position 
   if (strcmp(p->command, CMD_CLIENT_POSITION) == 0) {
      NotifyPositionUpdate(p, cIndex);
      return;
   }
   // If unknown then assume it's from a game dev
   if (gamePacketHandler) {
      (*gamePacketHandler)(*p);
   }
}

/// @brief Handles processing of incoming packets, sending of queued packets, and handling of acknowledgements
void Timer_Tick() {
   u8 arraySize = sizeof(IncomingPackets) / sizeof(NiFiPacket);
   u8 counter = 0;
   bool enumerate = false;

   // Handle incoming packets and forward game packets onto the developer
   do {
      if (akIndex == ipIndex) break;
      if (enumerate) {
         // Increment onto next packet
         akIndex += 1;
         // Return to the start of the array
         if (akIndex == arraySize) akIndex = 0;
         enumerate = false;
      }
      NiFiPacket *p = &IncomingPackets[akIndex];
      // Ignore processed packets
      if (p->isProcessed) {
         enumerate = true;
         continue;
      }
      // Mark packet as processed
      p->isProcessed = true;
      // Mark acknowledged outgoing packets as processed
      if (p->isAcknowledgement) {
         CompleteAcknowledgedPacket(p);
         enumerate = true;
         continue;
      }
      // Send acknowledgement ASAP for instruction packets
      SendAcknowledgement(p);
      if (MyRoomId == ID_ANY) {
         HandlePacketAsSearching(p);
         enumerate = true;
         continue;
      }
      // Skip late messages that are considered out of date
      u8 cIndex = INDEX_UNKNOWN;
      if (p->fromClientId != ID_ANY  &&
            p->fromClientId != ID_EMPTY) {
         cIndex = IndexOfClientUsingId(p->fromClientId);
         NiFiClient *client = &clients[cIndex];
         if (p->messageId < client->lastMessageId &&        // Ignore old messages
            client->lastMessageId - p->messageId < 500) {   // Unless MsgId Rollover
            enumerate = true;
            continue;
         }
         // Update the last message ID on the client
         client->lastMessageId = p->messageId;
      }
      if (IsHost) HandlePacketAsHost(p, cIndex);
      else HandlePacketAsClient(p, cIndex);
      enumerate = true;
   } while (++counter < arraySize);

   // Send outgoing packets and retry packets that weren't acknowledged
   arraySize = sizeof(OutgoingPackets) / sizeof(NiFiPacket);
   counter = 0;
   enumerate = false;
      do {
      if (spIndex == opIndex) break;
      if (enumerate) {
         // Increment onto next packet
         spIndex += 1;
         // Return to the start of the array
         if (spIndex == arraySize) spIndex = 0;
         enumerate = false;
      }
      // Ignore processed packets
      if (OutgoingPackets[spIndex].isProcessed) {
         enumerate = true;
         continue;
      }
      // Countdown until packet should be dropped
      OutgoingPackets[spIndex].timeToLive -= 1;
      // Drop packet and add timeout to strike target client
      if (OutgoingPackets[spIndex].timeToLive == 0) {
         OutgoingPackets[spIndex].isProcessed = true;
         PrintDebug(DBG_Error, "NiFi packet dropped");
         NiFiPacket b;
         NiFi_SetPacket(&b, CMD_ROOM_DISCONNECTED);
         sprintf(b.data[0] , "%hhd", OutgoingPackets[spIndex].toClientId);
         NiFi_QueueBroadcast(&b, NULL);
         enumerate = true;
         continue;
      }
      // Choose when to send the packet, currently up to 3 times, the 4th attempt will timeout first
      if ((OutgoingPackets[spIndex].timeToLive % WIFI_TTL_RATE) == 0) {
         NiFi_SendPacket(&OutgoingPackets[spIndex]);
         enumerate = true;
      }
   } while (++counter < arraySize);
}

/// @brief Resets variables used for NiFi package management.
/// Should be called when disconnected before switching between host/client modes.
void NiFi_ResetBuffers() {
   MyRoomId = ID_ANY;
   IsHost = false;
   PktRoomId = 0;
   PktToClientId = 0;
   CurrentMessageId = 0;
   LastClientId = 1;
   host = NULL;

   // Reset clients data
   for (int c = 0 ; c < CLIENT_MAX; c++) {
      clients[c].clientId = ID_EMPTY;
      clients[c].lastMessageId = 0;
      if (c > 0) { // Skip resetting local client details
         memset(clients[c].macAddress, 0, MAC_ADDRESS_LENGTH);
         memset(clients[c].playerName, 0, PROFILE_NAME_LENGTH);
      }
   }

   // Mark all packets as processed to prevent the timer function from trying to process them
   for (int i = 0 ; i < (sizeof(IncomingPackets) / sizeof(NiFiPacket)); i++) {
      IncomingPackets[i].isProcessed = true;
   }

   for (int o = 0 ; o < (sizeof(OutgoingPackets) / sizeof(NiFiPacket)); o++) {
      OutgoingPackets[o].isProcessed = true;
   }

   // Reset buffers
   memset(WiFi_ReceivedBuffer, 0, sizeof(WiFi_ReceivedBuffer));
   memset(EncodedPacketBuffer, 0, sizeof(EncodedPacketBuffer));
   memset(TempBuffer, 0, sizeof(TempBuffer));

   WiFi_ReceivedLength = 0;
   ipIndex = 0;
   opIndex = 0;
   akIndex = 0;
   spIndex = 0;
}

/// @brief Initialises the NiFi system
/// @param wifiChannel Select a 2.4Ghz RF channel from 1 - 13
/// @param timerId The hardware timer to use from 0 - 3
/// @param gameIdentifier a short but unique code to differentiate your game from others
void NiFi_Init(int wifiChannel, int timerId, char gameIdentifier[GAME_ID_LENGTH]) {
   PrintDebug(DBG_Information, "Initialising NiFi");
   // Replace default GID when possible
   if (gameIdentifier != NULL) {
      memset(GameIdentifier, 0, GAME_ID_LENGTH);
      strcpy(GameIdentifier, gameIdentifier);
   }

   // Replace default timer when in bounds
   if (TimerId > -1 && TimerId < 4) {
      TimerId = timerId;
   }

   // Default to 1 when WiFi channel number is incorrect
   if (wifiChannel < 1 || wifiChannel > 13) {
      wifiChannel = 1;
   }

   // Clear the internal buffers
   NiFi_ResetBuffers();

   // Changes how incoming packets are handled
   Wifi_SetRawPacketMode(PACKET_MODE_NIFI);

   // Init WiFi without automatic settings
   Wifi_InitDefault(false);
   Wifi_EnableWifi();

   // Configure a custom packet handler
   Wifi_RawSetPacketHandler(OnRawPacketReceived);

   // Force specific channel for communication
   Wifi_SetChannel(wifiChannel);

   // Get MAC address of the Nintendo DS
   GetMacAddress(localClient->macAddress);

   // Get player name from NDS profile
   GetProfileName(localClient->playerName);

   // Start timer to handle packets
   timerStart(TimerId, ClockDivider_1024, TIMER_FREQ_1024(240), Timer_Tick);
}

/// @brief Disables NiFi system and restores default configuration to the WiFi module
void NiFi_Shutdown() {
   PrintDebug(DBG_Information, "Stopping NiFi");
   timerStop(TimerId);
   // If NiFi is shutdown when the room is still active, then kick all remaining players
   if (IsHost && CountActiveClients() > 1) {
      NiFiPacket b;
      NiFi_SetPacket(&b, CMD_ROOM_DISCONNECTED);
      for (u8 i = 1; i < CLIENT_MAX; i++) {
         if (clients[i].clientId == ID_EMPTY) continue;
         b.toClientId = clients[i].clientId;
         sprintf(b.data[0] , "%hhd", clients[i].clientId);
         NiFi_SendPacket(&b);
      }
   }
   Wifi_DisableWifi();
   Wifi_SetRawPacketMode(PACKET_MODE_WIFI);
   Wifi_RawSetPacketHandler(0);
   NiFi_ResetBuffers();
   PrintDebug(DBG_Information, "NiFi shutdown complete");
}

/// @brief Configures a handler for game devs to see debug information
void NiFi_SetDebugOutput(DebugMessageHandler handler) {
   debugMessageHander = handler;
}

void NiFi_OnRoomAnnounced(RoomHandler handler) {
   roomAnnouncedHandler = handler;
}

void NiFi_OnJoinAccepted(RoomHandler handler) {
   joinAcceptedHandler = handler;
}

void NiFi_OnJoinDeclined(RoomHandler handler) {
   joinDeclinedHandler = handler;
}

void NiFi_OnClientConnected(ClientHandler handler) {
   clientConnectHandler = handler;
}

void NiFi_OnClientDisconnected(ClientHandler handler) {
   clientDisconnectHandler = handler;
}

void NiFi_OnDisconnected(DisconnectHandler handler) {
   disconnectHandler = handler;
}

void NiFi_OnHostMigration(ClientHandler handler) {
   hostMigrationHandler = handler;
}

void NiFi_OnPositionUpdated(PositionHandler handler) {
   positionHandler = handler;
}

void NiFi_OnGamePacket(GamePacketHandler handler) {
   gamePacketHandler = handler;
}

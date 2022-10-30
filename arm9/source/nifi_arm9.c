#include <nds.h>
#include <dswifi9.h>
#include <stdio.h>
// #include <string.h>
// #include <ctype.h>

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
char OutgoingPacketBuffer[RAW_PACKET_LENGTH]; // The send buffer for the currently outgoing packet

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

/// @brief Outputs a message to the debug message handler.
void Debug(int type, char *message) {
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
/// @return true if player setup, false if already exists or no empty player slots
bool TrySetupNiFiClient(u8 clientId, char macAddress[MAC_ADDRESS_LENGTH], char playerName[PROFILE_NAME_LENGTH]) {
   // Skip existing clients
   int8 clientIndex = IndexOfClientUsingId(clientId);
   if (clientIndex != INDEX_UNKNOWN) return false;
   // Skip duplicate attempts to join
   clientIndex = IndexOfClientUsingMacAddress(macAddress);
   if (clientIndex != INDEX_UNKNOWN) return false;
   // Back out of responding when the room is full room
   clientIndex = IndexOfClientUsingId(ID_EMPTY);
   if (clientIndex == INDEX_UNKNOWN) return false;
   // Set player data on the empty client
   clients[clientIndex].clientId = clientId;
   strncpy(clients[clientIndex].macAddress, macAddress, MAC_ADDRESS_LENGTH);
   strncpy(clients[clientIndex].playerName, playerName, PROFILE_NAME_LENGTH);
   return true;
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
      Debug(DBG_Error, "Overwriting incoming packet");
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

/// @brief Decodes the NiFi packet between the given indices
/// @param startPosition start index for the packet
/// @param endPosition end index for the packet
void ProcessEncodedPacketBuffer(int startPosition, int endPosition) {
   // Get the string packet inbetween the curly braces
   char currentPacket[RAW_PACKET_LENGTH] = "";
   strncpy(currentPacket, EncodedPacketBuffer + startPosition, endPosition - startPosition);
   Debug(DBG_RawPacket, currentPacket);

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
      Debug(IncomingPacket.isAcknowledgement ? DBG_Acknowledgement : DBG_ReceivedPacket, currentPacket);
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
   for (u8 i = 0; i < REQUEST_DATA_PARAM_COUNT; i++) {
      if (strlen(packet->data[i]) > 0)
         pos += sprintf(&buffer[pos], ";%s", packet->data[i]);
   }
   buffer[pos++] = '}';
   return pos;
}

/// @brief Sends a NiFi packet over the WiFi adapter, sent packets will not expect acknowledgements
/// @param packet outgoing NiFi packet to be sent
void SendPacket(NiFiPacket *packet) {
   int packetLength = WritePacketToBuffer(packet, OutgoingPacketBuffer);
   int packetSent = Wifi_RawTxFrame(packetLength, WIFI_TRANSMIT_RATE, (unsigned short *)OutgoingPacketBuffer);
   Debug(DBG_SentPacket, OutgoingPacketBuffer);
   if (packetSent == -1) {
      Debug(DBG_Error, "Unable to send RawTxFrame over WiFi due to space limitations");
   }
}

/// @brief Creates and sends an acknowledgement for the received packet
/// @param packet incoming NiFi packet to be acknowledged
void SendAcknowledgement(NiFiPacket *receivedPacket)
{
   AcknowledgementPacket.isAcknowledgement = true;
   // Set the current MAC address as the sender
   memset(AcknowledgementPacket.macAddress, 0, MAC_ADDRESS_LENGTH);
   strcpy(AcknowledgementPacket.macAddress, localClient->macAddress);
   // Reverse the packet direction for response
   AcknowledgementPacket.toClientId = receivedPacket->fromClientId;
   AcknowledgementPacket.fromClientId = localClient->clientId;
   // Include the message ID for validation
   AcknowledgementPacket.messageId = receivedPacket->messageId;
   // Include the command message for validation
   memset(AcknowledgementPacket.command, 0, COMMAND_LENGTH);
   strcpy(AcknowledgementPacket.command, receivedPacket->command);
   // Include senders MAC address for validation
   memset(AcknowledgementPacket.data, 0, sizeof(AcknowledgementPacket.data));
   strcpy(AcknowledgementPacket.data[0], receivedPacket->macAddress);
   // GID and RID are then included when sending
   SendPacket(&AcknowledgementPacket);
}

/// @brief Sends a NiFi packet to all clients, sent packets will not expect acknowledgements
/// @param packet outgoing NiFi packet to be broadcasted
/// @param ignoreClientIds client IDs that should not receive the packet
void SendBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]) {
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
      SendPacket(packet);
   }
}

/// @brief Queues a NiFi packet to be sent, sent packets will expect acknowledgements.
/// If the outgoing buffer is full then the oldest message is overwritten.
/// @param packet outgoing NiFi packet to be sent
void QueuePacket(NiFiPacket *packet) {
   // Warn if needed
   if (!OutgoingPackets[opIndex].isProcessed) {
      Debug(DBG_Error, "Overwriting outgoing packet");
   }

   // Copy packet onto array
   OutgoingPackets[opIndex].isProcessed = packet->isProcessed;
   OutgoingPackets[opIndex].isAcknowledgement = packet->isAcknowledgement;
   OutgoingPackets[opIndex].messageId = packet->messageId;
   OutgoingPackets[opIndex].timeToLive = packet->timeToLive;
   OutgoingPackets[opIndex].toClientId = packet->toClientId;
   OutgoingPackets[opIndex].fromClientId = packet->fromClientId;
   memset(OutgoingPackets[opIndex].command, 0, COMMAND_LENGTH);
   strncpy(OutgoingPackets[opIndex].command, packet->command, COMMAND_LENGTH);
   memset(OutgoingPackets[opIndex].macAddress, 0, MAC_ADDRESS_LENGTH);
   strncpy(OutgoingPackets[opIndex].macAddress, packet->macAddress, MAC_ADDRESS_LENGTH);
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
void QueueBroadcast(NiFiPacket *packet, u8 ignoreClientIds[]) {
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
      QueuePacket(packet);
   }
}

/// @brief Resets variables used for NiFi package management.
/// Should be called when disconnected before switching between host/client modes.
void NiFi_ResetBuffers() {
   Debug(DBG_Information, "NiFi buffers resetting");
   // Mark all packets as processed to prevent the timer function from trying to process them
   for (int i = 0 ; i < (sizeof(IncomingPackets) / sizeof(NiFiPacket)); i++) {
      IncomingPackets[i].isProcessed = true;
   }

   for (int o = 0 ; o < (sizeof(OutgoingPackets) / sizeof(NiFiPacket)); o++) {
      OutgoingPackets[o].isProcessed = true;
   }

   // Reset clients data
   for (int c = 0 ; c < CLIENT_MAX; c++) {
      clients[c].clientId = ID_EMPTY;
      clients[c].lastMessageId = 0;
      if (c > 0) { // Skip resetting local client details
         memset(clients[c].macAddress, 0, MAC_ADDRESS_LENGTH);
         memset(clients[c].playerName, 0, PROFILE_NAME_LENGTH);
      }
   }

   // Reset buffers
   memset(WiFi_ReceivedBuffer, 0, sizeof(WiFi_ReceivedBuffer));
   memset(EncodedPacketBuffer, 0, sizeof(EncodedPacketBuffer));
   memset(TempBuffer, 0, sizeof(TempBuffer));

   // Reset variables
   WiFi_ReceivedLength = 0;
   ipIndex = 0;
   opIndex = 0;
   akIndex = 0;
   spIndex = 0;
   PktRoomId = 0;
   PktToClientId = 0;
   CurrentMessageId = 0;
   IsHost = false;
   MyRoomId = ID_ANY;
   LastClientId = 1;
   host = NULL;
   Debug(DBG_Information, "NiFi buffers reset");
}

/// @brief Initialises the NiFi system
/// @param wifiChannel Select a 2.4Ghz RF channel from 1 - 13
/// @param timerId The hardware timer to use from 0 - 3
/// @param gameIdentifier a short but unique code to differentiate your game from others
void NiFi_Init(int wifiChannel, int timerId, char gameIdentifier[GAME_ID_LENGTH]) {
   Debug(DBG_Information, "Initialising NiFi");
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
   Debug(DBG_Information, "Initialised NiFi");
}

/// @brief Disables NiFi system and restores default configuration to the WiFi module
void NiFi_Shutdown() {
   Debug(DBG_Information, "Stopping NiFi");
   // TODO: broadcast host shutdown message
   timerStop(TimerId);
   Wifi_DisableWifi();
   Wifi_SetRawPacketMode(PACKET_MODE_WIFI);
   Wifi_RawSetPacketHandler(0);
   NiFi_ResetBuffers();
   Debug(DBG_Information, "NiFi shutdown complete");
}

/// @brief Configures a handler for game devs to see debug information
void NiFi_SetDebugOutput(DebugMessageHandler handler) {
   debugMessageHander = handler;
}

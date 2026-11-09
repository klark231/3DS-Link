#pragma once
#include "network/room.h"
namespace Network {
struct WifiPacket {
    enum class PacketType : u8 { Beacon, Data, Authentication, AssociationResponse, Deauthentication, NodeMap };
    PacketType type;
    std::vector<u8> data;
    MacAddress transmitter_address;
    MacAddress destination_address;
    u8 channel;
};
} // namespace Network

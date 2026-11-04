// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1

#include <algorithm>
#include <cstring>
#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_data.h"

namespace Service::NWM {

using MacAddress = std::array<u8, 6>;

/*
 * Generates a SNAP-enabled 802.2 LLC header for the specified protocol.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateLLCHeader(EtherType protocol) {
    LLCHeader header{};
    header.protocol = protocol;

    std::vector<u8> buffer(sizeof(header));
    std::memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Generates a Nintendo UDS SecureData header with the specified parameters.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateSecureDataHeader(u16 data_size, u8 channel, u16 dest_node_id,
                                                u16 src_node_id, u16 sequence_number) {
    SecureDataHeader header{};
    header.protocol_size = data_size + sizeof(SecureDataHeader);
    // Note: This size includes everything except the first 4 bytes of the structure,
    // reinforcing the hypotheses that the first 4 bytes are actually the header of
    // another container protocol.
    header.securedata_size = data_size + sizeof(SecureDataHeader) - 4;
    // Frames sent by the emulated application are never UDS management frames
    header.is_management = 0;
    header.data_channel = channel;
    header.sequence_number = sequence_number;
    header.dest_node_id = dest_node_id;
    header.src_node_id = src_node_id;

    std::vector<u8> buffer(sizeof(header));
    std::memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Calculates the CTR used for the AES-CTR process that calculates
 * the CCMP crypto key for data frames.
 * @returns The CTR used for data frames crypto key generation.
 */
// (frame encryption helpers removed: neither Citra nor the bridge encrypts data frames)
std::vector<u8> GenerateDataPayload(std::span<const u8> data, u8 channel, u16 dest_node,
                                    u16 src_node, u16 sequence_number) {
    std::vector<u8> buffer = GenerateLLCHeader(EtherType::SecureData);
    std::vector<u8> securedata_header = GenerateSecureDataHeader(
        static_cast<u16>(data.size()), channel, dest_node, src_node, sequence_number);

    buffer.insert(buffer.end(), securedata_header.begin(), securedata_header.end());
    buffer.insert(buffer.end(), data.begin(), data.end());
    return buffer;
}

SecureDataHeader ParseSecureDataHeader(std::span<const u8> data) {
    SecureDataHeader header;

    // Skip the LLC header
    std::memcpy(&header, data.data() + sizeof(LLCHeader), sizeof(header));

    return header;
}

std::vector<u8> GenerateEAPoLStartFrame(u16 association_id, ConnectionType conn_type,
                                        const NodeInfo& node_info) {
    EAPoLStartPacket eapol_start{};
    eapol_start.association_id = association_id;
    eapol_start.connection_type = conn_type;
    eapol_start.node.friend_code_seed = node_info.friend_code_seed;

    std::copy(node_info.username.begin(), node_info.username.end(),
              eapol_start.node.username.begin());

    // Note: The network_node_id and unknown bytes seem to be uninitialized in the NWM module.
    // TODO(B3N30): The last 8 bytes seem to have a fixed value of 07 88 15 00 04 e9 13 00 in
    // EAPoL-Start packets from different 3DSs to the same host during a Super Smash Bros. 4 game.
    // Find out what that means.

    std::vector<u8> eapol_buffer(sizeof(EAPoLStartPacket));
    std::memcpy(eapol_buffer.data(), &eapol_start, sizeof(eapol_start));

    std::vector<u8> buffer = GenerateLLCHeader(EtherType::EAPoL);
    buffer.reserve(buffer.size() + sizeof(EAPoLStartPacket));
    buffer.insert(buffer.end(), eapol_buffer.begin(), eapol_buffer.end());
    return buffer;
}

EtherType GetFrameEtherType(std::span<const u8> frame) {
    LLCHeader header;
    std::memcpy(&header, frame.data(), sizeof(header));
    return header.protocol;
}

u16 GetEAPoLFrameType(std::span<const u8> frame) {
    // Ignore the LLC header
    u16_be eapol_type;
    std::memcpy(&eapol_type, frame.data() + sizeof(LLCHeader), sizeof(eapol_type));
    return eapol_type;
}

NodeInfo DeserializeNodeInfoFromFrame(std::span<const u8> frame) {
    EAPoLStartPacket eapol_start;

    // Skip the LLC header
    std::memcpy(&eapol_start, frame.data() + sizeof(LLCHeader), sizeof(eapol_start));

    return DeserializeNodeInfo(eapol_start.node);
}

NodeInfo DeserializeNodeInfo(const EAPoLNodeInfo& node) {
    NodeInfo node_info{};
    node_info.friend_code_seed = node.friend_code_seed;
    node_info.network_node_id = node.network_node_id;

    std::copy(node.username.begin(), node.username.end(), node_info.username.begin());

    return node_info;
}

std::vector<u8> GenerateEAPoLLogoffFrame(const MacAddress& mac_address, u16 network_node_id,
                                         const NodeList& nodes, u8 max_nodes, u8 total_nodes) {
    EAPoLLogoffPacket eapol_logoff{};
    eapol_logoff.assigned_node_id = network_node_id;
    eapol_logoff.connected_nodes = total_nodes;
    eapol_logoff.max_nodes = max_nodes;

    for (std::size_t index = 0; index < max_nodes; ++index) {
        const auto& node_info = nodes[index];
        auto& node = eapol_logoff.nodes[index];

        node.friend_code_seed = node_info.friend_code_seed;
        node.network_node_id = node_info.network_node_id;

        std::copy(node_info.username.begin(), node_info.username.end(), node.username.begin());
    }

    std::vector<u8> eapol_buffer(sizeof(EAPoLLogoffPacket));
    std::memcpy(eapol_buffer.data(), &eapol_logoff, sizeof(eapol_logoff));

    std::vector<u8> buffer = GenerateLLCHeader(EtherType::EAPoL);
    buffer.reserve(buffer.size() + sizeof(EAPoLStartPacket));
    buffer.insert(buffer.end(), eapol_buffer.begin(), eapol_buffer.end());
    return buffer;
}

EAPoLLogoffPacket ParseEAPoLLogoffFrame(std::span<const u8> frame) {
    EAPoLLogoffPacket eapol_logoff;

    // Skip the LLC header
    std::memcpy(&eapol_logoff, frame.data() + sizeof(LLCHeader), sizeof(eapol_logoff));
    return eapol_logoff;
}

EAPoLStartPacket DeserializeEAPolStartPacket(std::span<const u8> frame) {
    EAPoLStartPacket eapol_start;

    std::memcpy(&eapol_start, frame.data() + sizeof(LLCHeader), sizeof(eapol_start));
    return eapol_start;
}

ParsedEAPoLStart ParseCompatibleEAPoLStart(std::span<const u8> frame) {
    ParsedEAPoLStart result{};

    // Step 1: raw safe memcpy (NOT strict parser)
    EAPoLStartPacket raw{};
    std::memcpy(&raw, frame.data() + sizeof(LLCHeader), sizeof(raw));

    result.packet = raw;

    // Step 2: compatibility normalization
    u8 raw_type = static_cast<u8>(raw.connection_type);

    switch (raw_type) {
        case 0:
        case 1:
            result.packet.connection_type = ConnectionType::Client;
            result.legacy = true;
            break;

        case 2:
            result.packet.connection_type = ConnectionType::Spectator;
            result.legacy = true;
            break;

        default:
            result.packet.connection_type = ConnectionType::Client;
            result.legacy = true;
            break;
    }

    return result;
}

} // namespace Service::NWM

#include <cthun-client/protocol/chunks.hpp>

namespace CthunClient {

//
// MessageChunk
//

MessageChunk::MessageChunk() : descriptor { 0 },
                               size { 0 },
                               content {} {
}

MessageChunk::MessageChunk(uint8_t _descriptor,
                           uint32_t _size,
                           std::string _content)
        : descriptor { _descriptor },
          size { _size },
          content { _content } {
}

MessageChunk::MessageChunk(uint8_t _descriptor,
                           std::string _content)
        : MessageChunk(_descriptor, _content.size(), _content) {
}

bool MessageChunk::operator==(const MessageChunk& other_msg_chunk) const {
    return descriptor == other_msg_chunk.descriptor
           && size == other_msg_chunk.size
           && content == other_msg_chunk.content;
}

void MessageChunk::serializeOn(SerializedMessage& buffer) const {
    serialize<uint8_t>(descriptor, 1, buffer);
    serialize<uint32_t>(size, 4, buffer);
    serialize<std::string>(content, size, buffer);
}

std::string MessageChunk::toString() const {
     return "size: " + std::to_string(size) + " bytes - content: " + content;
}

//
// ParsedChunks
//

// Default ctor
ParsedChunks::ParsedChunks()
        : envelope {},
          has_data { false },
          data_type { ContentType::Json },
          data {},
          binary_data { "" },
          debug {} {
}

// No data ctor
ParsedChunks::ParsedChunks(DataContainer _envelope,
                           std::vector<std::string> _debug)
        : envelope { _envelope },
          has_data { false },
          data_type { ContentType::Json },
          data {},
          binary_data { "" },
          debug { _debug } {
}

// JSON data ctor
ParsedChunks::ParsedChunks(DataContainer _envelope,
                           DataContainer _data,
                           std::vector<std::string> _debug)
        : envelope { _envelope },
          has_data { true },
          data_type { ContentType::Json },
          data { _data },
          binary_data { "" },
          debug { _debug } {
}

// Binary data ctor
ParsedChunks::ParsedChunks(DataContainer _envelope,
                           std::string _binary_data,
                           std::vector<std::string> _debug)
        : envelope { _envelope },
          has_data { true },
          data_type { ContentType::Binary },
          data {},
          binary_data { _binary_data },
          debug { _debug } {
}

std::string ParsedChunks::toString() const {
    auto s = "ENVELOPE: " + envelope.toString();

    if (has_data) {
        s += "\nDATA: ";
        if (data_type == ContentType::Json) {
            s += data.toString();
        } else {
            s += binary_data;
        }
    }

    for (auto& d : debug) {
        s += ("\nDEBUG: " + d);
    }

    return s;
}

}  // namespace CthunClient

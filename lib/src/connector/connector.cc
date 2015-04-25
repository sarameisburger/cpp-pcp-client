#include <cthun-client/connector/connector.hpp>
#include <cthun-client/connector/uuid.hpp>
#include <cthun-client/protocol/message.hpp>
#include <cthun-client/protocol/schemas.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE CTHUN_CLIENT_LOGGING_PREFIX".connector"

#include <leatherman/logging/logging.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <cstdio>
#include <chrono>

// TODO(ale): disable assert() once we're confident with the code...
// To disable assert()
// #define NDEBUG
#include <cassert>

namespace CthunClient {

//
// Constants
//

static const uint CONNECTION_CHECK_S { 15 };  // [s]
static const int DEFAULT_MSG_TIMEOUT { 10 };  // [s]

static const std::string MY_SERVER_URI { "cth:///server" };

//
// Utility functions
//

// TODO(ale): move this to leatherman
std::string getISO8601Time(unsigned int modifier_in_seconds) {
    boost::posix_time::ptime t = boost::posix_time::microsec_clock::universal_time()
                                 + boost::posix_time::seconds(modifier_in_seconds);
    return boost::posix_time::to_iso_extended_string(t) + "Z";
}

// TODO(ale): move plural from the common StringUtils in leatherman
template<typename T>
std::string plural(std::vector<T> things);

std::string plural(int num_of_things) {
    return num_of_things > 1 ? "s" : "";
}

//
// Public api
//

Connector::Connector(const std::string& server_url,
                     const std::string& client_type,
                     const std::string& ca_crt_path,
                     const std::string& client_crt_path,
                     const std::string& client_key_path)
        : server_url_ { server_url },
          client_metadata_ { client_type,
                             ca_crt_path,
                             client_crt_path,
                             client_key_path },
          connection_ptr_ { nullptr },
          validator_ {},
          schema_callback_pairs_ {},
          mutex_ {},
          cond_var_ {},
          is_destructing_ { false },
          is_monitoring_ { false },
          is_associated_ { false } {
    // Add Cthun schemas to the Validator instance member
    validator_.registerSchema(Protocol::EnvelopeSchema());
    validator_.registerSchema(Protocol::DebugSchema());

    // Register Cthun callbacks
    registerMessageCallback(
        Protocol::AssociateResponseSchema(),
        [this](const ParsedChunks& parsed_chunks) {
            associateResponseCallback(parsed_chunks);
        });
}

Connector::~Connector() {
    if (connection_ptr_ != nullptr) {
        // reset callbacks to avoid breaking the Connection instance
        // due to callbacks having an invalid reference context
        LOG_INFO("Resetting the WebSocket event callbacks");
        connection_ptr_->resetCallbacks();
    }

    {
        std::lock_guard<std::mutex> the_lock { mutex_ };
        is_destructing_ = true;
        cond_var_.notify_one();
    }
}

// Register schemas and onMessage callbacks

void Connector::registerMessageCallback(const Schema schema,
                                        MessageCallback callback) {
    validator_.registerSchema(schema);
    auto p = std::pair<std::string, MessageCallback>(schema.getName(), callback);
    schema_callback_pairs_.insert(p);
}

// Manage the connection state

void Connector::connect(int max_connect_attempts) {
    if (connection_ptr_ == nullptr) {
        // Initialize the WebSocket connection
        connection_ptr_.reset(new Connection(server_url_, client_metadata_));

        // Set WebSocket callbacks
        connection_ptr_->setOnMessageCallback(
            [this](std::string message) {
                processMessage(message);
            });

        connection_ptr_->setOnOpenCallback(
            [this]() {
                associateSession();
            });
    }

    try {
        // Open the WebSocket connection
        connection_ptr_->connect(max_connect_attempts);
    } catch (connection_processing_error& e) {
        // NB: connection_fatal_errors are propagated whereas
        //     connection_processing_errors are converted to
        //     connection_config_errors (they can be thrown after
        //     websocketpp::Endpoint::connect() or ::send() failures)
        LOG_ERROR("Failed to connect: %1%", e.what());
        throw connection_config_error { e.what() };
    }
}

bool Connector::isConnected() const {
    return connection_ptr_ != nullptr
           && connection_ptr_->getConnectionState() == ConnectionStateValues::open;
}

bool Connector::isAssociated() const {
    return isConnected() && is_associated_.load();
}

void Connector::monitorConnection(int max_connect_attempts) {
    checkConnectionInitialization();

    if (!is_monitoring_) {
        is_monitoring_ = true;
        startMonitorTask(max_connect_attempts);
    } else {
        LOG_WARNING("The monitorConnection has already been called");
    }
}

// Send messages

void Connector::send(const Message& msg) {
    checkConnectionInitialization();
    auto serialized_msg = msg.getSerialized();
    LOG_DEBUG("Sending message of %1% bytes:\n%2%",
              serialized_msg.size(), msg.toString());
    connection_ptr_->send(&serialized_msg[0], serialized_msg.size());
}

void Connector::send(const std::vector<std::string>& targets,
                     const std::string& message_type,
                     unsigned int timeout,
                     const DataContainer& data_json,
                     const std::vector<DataContainer>& debug) {
    sendMessage(targets,
                message_type,
                timeout,
                false,
                data_json.toString(),
                debug);
}

void Connector::send(const std::vector<std::string>& targets,
                     const std::string& message_type,
                     unsigned int timeout,
                     const std::string& data_binary,
                     const std::vector<DataContainer>& debug) {
    sendMessage(targets,
                message_type,
                timeout,
                false,
                data_binary,
                debug);
}

void Connector::send(const std::vector<std::string>& targets,
                     const std::string& message_type,
                     unsigned int timeout,
                     bool destination_report,
                     const DataContainer& data_json,
                     const std::vector<DataContainer>& debug) {
    sendMessage(targets,
                message_type,
                timeout,
                destination_report,
                data_json.toString(),
                debug);
}

void Connector::send(const std::vector<std::string>& targets,
                     const std::string& message_type,
                     unsigned int timeout,
                     bool destination_report,
                     const std::string& data_binary,
                     const std::vector<DataContainer>& debug) {
    sendMessage(targets,
                message_type,
                timeout,
                destination_report,
                data_binary,
                debug);
}

//
// Private interface
//

// Utility functions

void Connector::checkConnectionInitialization() {
    if (connection_ptr_ == nullptr) {
        throw connection_not_init_error { "connection not initialized" };
    }
}

MessageChunk Connector::createEnvelope(const std::vector<std::string>& targets,
                                       const std::string& message_type,
                                       unsigned int timeout,
                                       bool destination_report) {
    auto msg_id = UUID::getUUID();
    auto expires = getISO8601Time(timeout);
    LOG_INFO("Creating message with id %1% for %2% receiver%3%",
             msg_id, targets.size(), plural(targets.size()));

    DataContainer envelope_content {};

    envelope_content.set<std::string>("id", msg_id);
    envelope_content.set<std::string>("message_type", message_type);
    envelope_content.set<std::vector<std::string>>("targets", targets);
    envelope_content.set<std::string>("expires", expires);
    envelope_content.set<std::string>("sender", client_metadata_.uri);

    if (destination_report) {
        envelope_content.set<bool>("destination_report", true);
    }

    return MessageChunk { ChunkDescriptor::ENVELOPE, envelope_content.toString() };
}

void Connector::sendMessage(const std::vector<std::string>& targets,
                            const std::string& message_type,
                            unsigned int timeout,
                            bool destination_report,
                            const std::string& data_txt,
                            const std::vector<DataContainer>& debug) {
    auto envelope_chunk = createEnvelope(targets, message_type, timeout,
                                         destination_report);
    MessageChunk data_chunk { ChunkDescriptor::DATA, data_txt };
    Message msg { envelope_chunk, data_chunk };

    for (auto debug_content : debug) {
        MessageChunk d_c { ChunkDescriptor::DEBUG, debug_content.toString() };
        msg.addDebugChunk(d_c);
    }

    send(msg);
}

// WebSocket onOpen callback - will send the associate session request

void Connector::associateSession() {
    // Envelope
    auto envelope = createEnvelope(std::vector<std::string> { MY_SERVER_URI },
                                   Protocol::ASSOCIATE_REQ_TYPE,
                                   DEFAULT_MSG_TIMEOUT,
                                   false);

    // Create and send message
    Message msg { envelope };
    LOG_INFO("Sending Associate Session request");
    send(msg);
}

// WebSocket onMessage callback

void Connector::processMessage(const std::string& msg_txt) {
    LOG_DEBUG("Received message of %1% bytes - raw message:\n%2%",
              msg_txt.size(), msg_txt);

    // Deserialize the incoming message
    std::unique_ptr<Message> msg_ptr;
    try {
        msg_ptr.reset(new Message(msg_txt));
    } catch (message_error& e) {
        LOG_ERROR("Failed to deserialize message: %1%", e.what());
        return;
    }

    // Parse message chunks
    ParsedChunks parsed_chunks;
    try {
        parsed_chunks = msg_ptr->getParsedChunks(validator_);
    } catch (validation_error& e) {
        LOG_ERROR("Invalid message - bad content: %1%", e.what());
        return;
    } catch (data_parse_error& e) {
        LOG_ERROR("Invalid message - invalid JSON content: %1%", e.what());
        return;
    } catch (schema_not_found_error& e) {
        LOG_ERROR("Invalid message - unknown schema: %1%", e.what());
        return;
    }

    // Execute the callback associated with the data schema
    auto schema_name = parsed_chunks.envelope.get<std::string>("message_type");

    if (schema_callback_pairs_.find(schema_name) != schema_callback_pairs_.end()) {
        auto c_b = schema_callback_pairs_.at(schema_name);
        LOG_TRACE("Executing callback for a message with '%1%' schema",
                  schema_name);
        c_b(parsed_chunks);
    } else {
        LOG_WARNING("No message callback has be registered for '%1%' schema",
                    schema_name);
    }
}

// Associate session response callback

void Connector::associateResponseCallback(const ParsedChunks& parsed_chunks) {
    assert(parsed_chunks.has_data);
    assert(parsed_chunks.data_type == CthunClient::ContentType::Json);

    auto response_id = parsed_chunks.envelope.get<std::string>("id");
    auto server_uri = parsed_chunks.envelope.get<std::string>("sender");

    auto request_id = parsed_chunks.data.get<std::string>("id");
    auto success = parsed_chunks.data.get<bool>("success");

    std::string msg { "Received associate session response " + response_id
                      + " from " + server_uri + " for request " + request_id };

    if (success) {
        LOG_INFO("%1%: success", msg);
        is_associated_ = true;
    } else {
        if (parsed_chunks.data.includes("reason")) {
            auto reason = parsed_chunks.data.get<std::string>("reason");
            LOG_WARNING("%1%: failure - %2%", msg, reason);
        } else {
            LOG_WARNING("%1%: failure", msg);
        }
    }
}

// Monitor task

void Connector::startMonitorTask(int max_connect_attempts) {
    assert(connection_ptr_ != nullptr);

    while (true) {
        std::unique_lock<std::mutex> the_lock { mutex_ };
        auto now = std::chrono::system_clock::now();

        cond_var_.wait_until(the_lock,
                             now + std::chrono::seconds(CONNECTION_CHECK_S));

        if (is_destructing_) {
            // The dtor has been invoked
            LOG_INFO("Stopping the monitor task");
            is_monitoring_ = false;
            the_lock.unlock();
            return;
        }

        try {
            if (!isConnected()) {
                LOG_WARNING("WebSocket connection to Cthun server lost; retrying");
                is_associated_ = false;
                connection_ptr_->connect(max_connect_attempts);
            } else {
                LOG_DEBUG("Sending heartbeat ping");
                connection_ptr_->ping();
            }
        } catch (connection_processing_error& e) {
            // Connection::connect() or ping() failure - keep trying
            LOG_ERROR("Connection monitor failure: %1%", e.what());
        } catch (connection_fatal_error& e) {
            // Failed to reconnect after max_connect_attempts - stop
            LOG_ERROR("The connection monitor task will stop - failure: %1%",
                      e.what());
            is_monitoring_ = false;
            the_lock.unlock();
            throw;
        }

        the_lock.unlock();
    }
}

}  // namespace CthunClient

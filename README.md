# CthunClient

## Introduction

CthunClient is a C++ client library for the [Cthun](https://github.com/puppetlabs/cthun)
messaging framework. It includes a collection of abstractions which can be used
to initiate connections to a Cthun server, wrapping the Cthun message format and
performing schema validation for message bodies.

A tutorial on how to create a Cthun agent / controller pair with CthunClient is
[here][1].

## Building the library

### Requirements
 - a C++11 compiler (clang/gcc 4.7)
 - gnumake
 - cmake (2.8.12 and newer)
 - boost (1.54 and newer)

### Build

Building the library is simple, just run:

    make

Tests can be run with:

    make test

## Usage

##### Table of Contents
- [Important Data Structures](#data_structures)
- [Creating Connections](#connections)
- [Message Schemas and Callbacks](#receiving_messages)
- [Sending Messages](#sending_messages)
- [Data Validation](#validation)

<a name="data_structures"/>
###Important Data Structures

Before we start to look at creating connections and sending/receiving messages, it
is important to look at some of the data structures used by CthunClient.

__DataContainer__

The DataContainer class is used frequently by the CthunClient library as a simplified
abstraction around complex JSON c++ libraries. It has the following constructors:

    DataContainer() // Creates an empty container
    DataContainer(std::string json_txt) // creates a DataContainer from a JSON string

Consider the following JSON string wrapped in a DataContainer object, data.

```
    {
      "module" : "puppet",
      "action" : "run",
      "params" : {
        "first" : "--module-path=/home/alice/modules"
      }
    }
```

You can construct a DataContainer as follows:

```
    DataContainer data { jsons_string };
```

The DataContainer's constructor can throw the following exception:

 - data_parse_error - This error is thrown when invalid JSON is passed to the constructor.

The following calls to the _get_ method will retrieve values from the DataContainer.

```
    data.get<std::string>("module"); // == "puppet"
    data.get<std::string>({ "params", "first" }); // == "--module-path=/home/alice/modules"
```

Note that when the _get_ method is invoked with an initialiser list it will use
each argument to descend a level into the object tree.

The supported scalar types are: int, double, bool, std::string, and DataContainer.
Elements of such types can be grouped in an array, represented by a std::vector
instance.

In case _get_ is invoked with an unknown key, no exception is thrown; depending
on the requested value type, the method returns:

 - 0 (int)
 - 0.0 (double)
 - false (bool)
 - an empty std::string
 - an empty DataContainer
 - an empty std::vector of the requested type

The _get_ method throws an assertion error in case the specified type does not
match the one of the requested value. You can verify if the type is correct by
using the _type_ method (see below).

You can also set the value of fields and create new fields with the _set_ method.
```
    data.set<int>("foo", 42);
    data.set<bool>({ "params", "second" }, false);
```

This will change the internal JSON representation to

```
    {
      "module" : "puppet",
      "action" : "run",
      "params" : {
        "first" : "--module-path=/home/alice/modules",
        "second" : false
      },
      "foo" : 42
    }
```

Note that the _set_ method uses the initialiser list in the same way as the _get_
method. Each argument to the list is one level to descend.

The _set_ method can throw the following exception:

 - data_key_error - thrown when a nested message key is invalid (i.e. the
 associated value is not a valid JSON object, so that is not possible to
 iterate the remaining nested keys) or when the root element is not a valid
 JSON object, so that is not possible to set the specified key-value entry.

You can use the _type_ method to retrieve the type of a given value. As done for
_get_ and _set_, you can specify the value's key with an initialiser list, in
order to navigate multiple levels within a JSON object.

The _type_ method returns a value of the DataType enumeration, defined as:

```
    enum DataType { Object, Array, String, Int, Bool, Double, Null };
```

The _type_ method can throw the following exception:

 - data_key_error - thrown when the specified key is unknown.

__ParsedChunks__

The _Parsed_Chunks_ struct is a simplification of a parsed Cthun message. It allows
for direct access of a message's Envelope, Data and Debug chunks as DataContainer
or string objects.

The ParsedChunks struct is defined as:

```
    struct ParsedChunks {
        // Envelope
        DataContainer envelope;

        // Data
        bool got_data;
        ContentType data_type;
        DataContainer data;
        std::string binary_data;

        // Debug
        std::vector<DataContainer> debug;
    }
```

<a name="connections"/>
###Creating Connections

The first step to interacting with a Cthun server is creating a connection. To
achieve this we must first create an instance of the Connector object.

The constructor of the Connector class is defined as:

```
    Connector(const std::string& server_url,
              const std::string& client_type,
              const std::string& ca_crt_path,
              const std::string& client_crt_path,
              const std::string& client_key_path)
```

The parameters are described as:

 - client_type - A free form value used to group applications connected to a Cthun server.
 For example, an applications connected with the type _potato_ will all be addressable
 as the Cthun endpoint cth://*/potato. The only value that you cannot use is "server",
 which is reserved for Cthun servers (please refer to the URI section in the
 [Cthun specifications][2]).
 - server_url - The URL of the Cthun server. For example, _wss://localhost:8090/cthun/_.
 - ca_crt_path - The path to your CA certificate file.
 - client_crt_path - The path to a client certificate file generated by your CA.
 - client_key_path - The path to a client public key file generated by you CA.

This means that you can instantiate a Connector object as follows:

```
    Connector connector { "wss://localhost:8090/cthun/", "controller",
                          "/etc/puppet/ssl/ca/ca_crt.pem",
                          "/etc/puppet/ssl/certs/client_crt.pem",
                          "/etc/puppet/ssl/public_keys/client_key.pem" };
```

When you have created a Connector object you are ready to connect. The Connector's
connect method is defined as:

```
    void connect(int max_connect_attempts = 0) throws (connection_config_error,
                                                       connection_fatal_error)
```

The parameters are described as:

 - max_connect_attempts - The amount of times the Connector will try and establish
 a connection to the Cthun server if a problem occurs. It will try to connect
 indefinately when set to 0. Defaults to 0.

The connect method can throw the following exceptions:

```
    class connection_config_error : public connection_error
```

This exception will be thrown if a Connector is misconfigured. Misconfiguration
includes specifying an invalid server url or a path to a file that doesn't exist.
Note that if this exception has been thrown no attempt at creating a network
connection has yet been made.

```
    class connection_fatal_error : public connection_error
```

This exception wil be thrown if the connection cannot be established after the
Connector has tried _max_connect_attempts_ times.

A connection can be established as follows:

```
    try {
        connector.connect(5);
    } catch (connection_config_error e) {
        ...
    } catch (connection_fatal_error) {
        ...
    }
```

If no exceptions are thrown it means that a connection has been sucessfuly
established. You can check on the status of a connection with the Connector's
isConnected method.

The isConnected method is defined as:

```
    bool isConnected()
```

And it can be used as follows:

```
    if (connector.isConnected()) {
        ...
    } else {
        ...
    }
```

By default a connection is non persistent. For instance, in case WebSocket is
used as the underlying transport layer, ping messages must be sent periodically
to keep the connection alive. Also, the connection may drop due to communication
errors.  You can enable connection persistence by calling the _monitorConnection_
method that will periodically check the state of the underlying connection. It
will send keepalive messages to the server and attempt to re-establish the
connection in case it has been dropped.

_monitorConnection_ is defined as:

```
    void monitorConnection(int max_connect_attempts = 0)
```

The parameters are described as:

- max_connect_attemps -The number of times the Connector will try to reconnect
 a connection to the Cthun server if a problem occurs. It will try to connect
 indefinately when set to 0. Defaults to 0.

Note that if the Connector fails to re-establish the connection after the
specified number of attempts, a _connection_fatal_error_ will be thrown.
Also, calling _monitorConnection_ will block the execution thread as the
monitoring task will not be executed on a separate thread. On the other hand,
the caller can safely execute _monitorConnection_ on a separate thread since
the function returns once the _Connector_ destructor is invoked.

```
    connector.monitorConnection(5);
```

<a name="receiving_messages"/>
### Message Schemas and Callbacks

Every message sent over the Cthun server has to specify a value for the _data_schema_
field in the message envelope. These data schema's determine how a message's data
section is validated. To process messages received from a Cthun server you must
first create a schema object for a specific _data_schema_ value.

The constructor for the Schema class is defined as:

```
    Schema(const std::string& name, ContentType content_type)
```

The parameters are described as:

 - name - The name of the schema. This should be the same as the value found in
 a message's data_schema field.
 - content_type - Defines the content type of the schema. Valid options are
 ContentType::Binary and ContentType::Json

A Schema object can be created as follows:

```
    Schema cnc_request_schema { "cnc_request", ContentType::Json};
```

You can now start to add constraints to the Schema. Consider the following JSON-schema:

```
  {
    "title": "cnc_request",
    "type": "object",
    "properties": {
      "module": {
        "type": "string"
      },
      "action": {
        "type": "string"
      },
    },
    "required": ["module"]
  }
```
You can reproduce its constraints by using the addConstraint method which
is defined as follows:

```
    void addConstraint(std::string field, TypeConstraint type, bool required)
```

The parameters are described as follows:

 - field - The name of the field you wish to add the constraint to.
 - type - The type constraint to put on the field. Valid types are TypeConstraint::Bool,
 TypeConstraint::Int, TypeConstraint::Bool, TypeConstraint::Double, TypeConstraint::Array,
 TypeConstraint::Object, TypeConstraint::Null and TypeConstraint::Any
 - required - Specify whether the field is required to be present or not. If not specified
 it will default to false.

```
    cnc_request_schema.addConstraint("module", TypeConstraint::String, true);
    cnc_request_schema.addConstraint("action", TypeConstraint::String);
```

With a schema defined we can now start to process messages of that type by registering
message specific callbacks. This is done with the Connector's registerMessageCallback
method which is defined as follows:

```
    void registerMessageCallback(const Schema schema, MessageCallback callback)
```

The parameters are described as follows:

 - schema - A previously created schema object
 - callback - A callback function with the signature void(const ParsedChunks& msg_content)

For example:

```
    void cnc_requestCallback(const ParsedChunks& msg_content) {
      std::cout << "Message envelope: " << msg_content.envelope.toString() << std::endl;

      if (msg_content.has_data()) {
        if (msg_content.data_type == ContentType::Json) {
          std::cout << "Content Type: JSON" << std::endl;
          std::cout << msg_content.data.toString() << std::endl;
        } else {
          std::cout << "Content Type: Binary" << std::endl;
          std::cout << msg_content.binary_data << std::endl;
        }
      }

      for (const auto& debug_chunk : msg_content) {
        std::cout << "Data Chunk: " << debug_chunk << std::endl;
      }
    }

    ...
    connector.registerMessageCallback(cnc_request_schema, cnc_requestCallback);
```

Now that the callback has been regsitered, every time a message is received where
the data_schema field is _cnc_request_, the content of the Data chunk will be
validated against the schema and if it passes, the above defined function will be called.
If a message is received which doesn't have a registered data_schema the message
will be ignored.

Using this method of registering schema/callback pairs we can handle each message
in a unique manner,

```
    connector.registerMessageCallback(cnc_request_schema, cnc_requestCallback);
    connector.registerMessageCallback(puppet_request_schema, puppet_requestCallback);
    connector.registerMessageCallback(puppet_db_request_schema, puppet_db_requestCallback);
```

or you can assign one callback to a lot of different schemas,

```
    connector.registerMessageCallback(schema_1, genericCallback);
    ...
    connector.registerMessageCallback(schema_n, genericCallback);
    connector.registerMessageCallback(schema_n1, genericCallback);
```

<a name="sending_messages"/>
### Sending Messages

Once you have established a connection to the Cthun server you can send messages
using the _send_ function. There are two overloads for the function that are
defined as:

```
    void send(std::vector<std::string> targets,
              std::string data_schema,
              unsigned int timeout,
              DataContainer data_json,
              std::vector<DataContainer> debug = std::vector<DataContainer> {})
                        throws (connection_processing_error, connection_not_init_error)
```

With the parameters are described as follows:

 - targets - A vector of the destinations the message will be sent to
 - data_schema - The Schema that identifies the message type
 - timeout - Duration the message will be valid on the fabric
 - data_json - A DataContainer representing the data chunk of the message
 - debug - A vector of strings representing the debug chunks of the message (defaults to empty)


```
    void send(std::vector<std::string> targets,
              std::string data_schema,
              unsigned int timeout,
              std::string data_binary,
              std::vector<DataContainer> debug = std::vector<DataContainer> {})
                        throws (connection_processing_error, connection_not_init_error)

```

With the parameters are described as follows:

 - targets - A vector of the destinations the message will be sent to
 - data_schema - The Schema that identifies the message type
 - timeout - Duration the message will be valid on the fabric
 - data_binary - A string representing the data chunk of the message
 - debug - A vector of strings representing the debug chunks of the message (defaults to empty)

The _send_ methods can throw the following exceptions:

```
    class connection_processing_error : public connection_error
```

This exception is thrown when an error occurs during at the underlying WebSocket
layer.

```
    class connection_not_init_error : public connection_error
```

This exception is thrown when trying to send a message when there is no active
connection to the server.

Example usage:

```
    DataContainer data {};
    data.set<std::string>("foo", "bar");
    try {
      connector.send({"cth://*/potato"}, "potato_schema", 42, data);
    } catch (connection_not_init_error e) {
      std::cout << "Cannot send message without being connected to the server" << std::endl;
    } catch (connection_processing_error e) {
      std::cout << "An error occured at the WebSocket layer: " << e.what() << std::endl;
   }
```

<a name="validation"/>
### Data Validation

As mentioned in the [Message Schemas and Callbacks](#receiving_messages), messages
received from the Cthun server will be matched against a Schema that you defined.
The Connector object achieves this functionality by using an instance of the Validator
class. It is possible to instantiate your own instance of the Validator class and
use schema's to validate other, non message, data structures.

The Validator is limited to a no-args constructor:

```
    Validator()
```

You can register a Schema by using the _registerSchema_ method, defined as:

```
    void registerSchema(const Schema& schema) throws (schema_redefinition_error)
```

The parameters are described as follows:

 - schema - A schema object that desribes a set of constraints.

When a Schema has been registered you can use the _validate_ method to validate
a DataContainer object. The _validate_ method is defined as follows:

```
    void validate(DataContainer& data, std::string schema_name) const throws (validation_error)
```

The parameters are described as follows:

 - data - A DataContainer you want to validate.
 - schema_name - The name of the schema you want to validate against.

Example usage:

```
    Validator validator {};
    Schema s {"test-schema", ContentType::Json };
    s.addConstraint("foo", TypeConstraint::Int);
    validator.registerSchema(s);

    DataContainer d {};
    d.set<int>("foo", 42);

    try {
      Validator.validate(d, "test-schema");
    } catch (validation_error) {
      std::cout << "Validation failed" << std::endl;
    }
```

[1]: https://github.com/puppetlabs/cthun-client/tree/master/tutorial
[2]: https://github.com/puppetlabs/cthun-specifications

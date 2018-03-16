#ifndef BROKER_FWD_HH
#define BROKER_FWD_HH

#include <cstdint>

namespace broker {

class blocking_endpoint;
class context;
class data;
class endpoint;
class internal_command;
class nonblocking_endpoint;
class publisher;
class status;
class store;
class subscriber;
class topic;
struct add_command;
struct erase_command;
struct put_command;
struct put_unique_command;
struct set_command;
struct snapshot_command;
struct subtract_command;

struct network_info;

/// A monotonic identifier to represent a specific lookup request.
using request_id = uint64_t;

// Arithmetic data types
using boolean = bool;
using count = uint64_t;
using integer = int64_t;
using real = double;

namespace detail {

class flare_actor;
class mailbox;

} // namespace detail

} // namespace broker

#endif // BROKER_FWD_HH
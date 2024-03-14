// This suite is an integration test. It does not check for a specific feature,
// but makes sure the system behaves correctly in different use cases. The
// system always consists of at least three nodes. Messages are not checked
// individually. Rather, the system runs to a predetermined point before
// checking for an expected outcome.
#include "broker/broker-test.test.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <caf/actor_system.hpp>
#include <caf/behavior.hpp>
#include <caf/defaults.hpp>
#include <caf/downstream.hpp>
#include <caf/error.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/logger.hpp>
#include <caf/scheduler/test_coordinator.hpp>
#include <caf/timestamp.hpp>

#include "broker/configuration.hh"
#include "broker/endpoint.hh"
#include "broker/error.hh"
#include "broker/internal/core_actor.hh"
#include "broker/internal/endpoint_access.hh"
#include "broker/internal/type_id.hh"
#include "broker/peer_info.hh"
#include "broker/peer_status.hh"
#include "broker/status.hh"
#include "broker/status_subscriber.hh"
#include "broker/subscriber.hh"
#include "broker/topic.hh"

namespace atom = broker::internal::atom;

using namespace broker;

using caf::unit_t;

namespace {

struct peer_fixture;

// Holds state shared by all peers. There exists exactly one global fixture.
struct global_fixture {
  // Maps host names to peers.
  using peers_map = std::map<std::string, peer_fixture*>;

  peers_map peers;

  ~global_fixture() {
    // Make sure peers vector is empty before destructors of children might
    // attempt accessing it.
    peers.clear();
  }

  // Makes sure all handles are distinct.
  uint64_t next_handle_id = 1;

  // Tries progressesing actors messages or network traffic.
  bool try_exec();

  // Progresses actors messages and network traffic as much as possible.
  void exec_loop();
};

// Holds state for individual peers. We use one fixture per simulated peer.
struct peer_fixture {
  // Pointer to the global state.
  global_fixture* parent;

  // Identifies this fixture in the parent's `peers` map.
  std::string name;

  // Each peer is an endpoint.
  endpoint ep;

  // Convenient access to `ep.system()`.
  caf::actor_system& sys;

  // Convenient access to `sys.scheduler()` with proper type.
  caf::scheduler::test_coordinator& sched;

  // Stores all received items for subscribed topics.
  std::vector<data_message> data;

  // Stores the interval between two credit rounds.
  caf::timespan credit_round_interval;

  // Returns the core manager for given core actor.
  auto& state(caf::actor hdl) {
    auto ptr = caf::actor_cast<caf::abstract_actor*>(hdl);
    return dynamic_cast<internal::core_actor&>(*ptr).state;
  }

  // Initializes this peer and registers it at parent.
  peer_fixture(global_fixture* parent_ptr, std::string peer_name)
    : parent(parent_ptr),
      name(std::move(peer_name)),
      ep(base_fixture::make_config()),
      sys(broker::internal::endpoint_access{&ep}.sys()),
      sched(dynamic_cast<caf::scheduler::test_coordinator&>(sys.scheduler())) {
    // Register at parent.
    parent->peers.emplace(name, this);
    // Run initialization code
    exec_loop();
    // Register at parent.
    parent->peers.emplace(name, this);
  }

  ~peer_fixture() {
    CAF_SET_LOGGER_SYS(&sys);
    MESSAGE("shut down " << name);
    loop_after_all_enqueues();
  }

  std::vector<peer_info> peers() {
    sched.inline_next_enqueue();
    return ep.peers();
  }

  // Subscribes to a topic, storing all incoming tuples in `data`.
  void subscribe_to(topic t) {
    ep.subscribe(
      {t},
      [](unit_t&) {
        // nop
      },
      [=](unit_t&, data_message x) { this->data.emplace_back(std::move(x)); },
      [](unit_t&, const error&) {
        // nop
      });
    parent->exec_loop();
  }

  // Publishes all `(t, xs)...` tuples.
  template <class... Ts>
  void publish(topic t, Ts... xs) {
    (ep.publish(make_data_message(t, std::move(xs))), ...);
    parent->exec_loop();
  }

  // Tries to advance actor messages or network data on this peer.
  bool try_exec() {
    return sched.try_run_once() || mpx.try_read_data()
           || mpx.try_exec_runnable() || mpx.try_accept_connection();
  }

  // Advances actor messages and network data on this peer as much as possible.
  void exec_loop() {
    while (try_exec())
      ; // rinse and repeat
  }

  void loop_after_next_enqueue() {
    sched.after_next_enqueue([=] { parent->exec_loop(); });
  }

  void loop_after_all_enqueues_helper() {
    exec_loop();
    sched.after_next_enqueue([=] { loop_after_all_enqueues_helper(); });
  }

  void loop_after_all_enqueues() {
    sched.after_next_enqueue([=] { loop_after_all_enqueues_helper(); });
  }
};

bool global_fixture::try_exec() {
  return std::any_of(peers.begin(), peers.end(),
                     [](const peers_map::value_type& kvp) {
                       return kvp.second->try_exec();
                     });
}

void global_fixture::exec_loop() {
  auto try_trigger_timeout = [this] {
    std::vector<caf::actor_clock::duration_type> ts;
    for (auto& kvp : peers) {
      auto& tac = kvp.second->sched.clock();
      if (!tac.schedule().empty())
        ts.emplace_back(tac.schedule().begin()->first - tac.now());
    }
    if (!ts.empty()) {
      auto dt = std::min_element(ts.begin(), ts.end());
      for (auto& kvp : peers)
        kvp.second->sched.clock().advance_time(*dt);
      return true;
    } else {
      return false;
    }
  };
  auto exec = [](auto& kvp) { return kvp.second->try_exec(); };
  while (std::any_of(peers.begin(), peers.end(), exec) || try_trigger_timeout())
    ; // rinse and repeat
}

// A fixture for simple setups consisting of three nodes.
struct triangle_fixture : global_fixture {
  peer_fixture mercury;
  peer_fixture venus;
  peer_fixture earth;

  triangle_fixture()
    : mercury(this, "mercury"), venus(this, "venus"), earth(this, "earth") {}

  // Connect mercury to venus and earth.
  void connect_peers() {
    MESSAGE("prepare connections");
    auto server_handle = mercury.make_accept_handle();
    mercury.mpx.prepare_connection(server_handle,
                                   mercury.make_connection_handle(), venus.mpx,
                                   "mercury", 4040,
                                   venus.make_connection_handle());
    mercury.mpx.prepare_connection(server_handle,
                                   mercury.make_connection_handle(), earth.mpx,
                                   "mercury", 4040,
                                   earth.make_connection_handle());
    MESSAGE("start listening on mercury:4040");
    // We need to connect venus and earth while mercury is blocked on
    // ep.listen() in order to avoid a "deadlock" in `ep.listen()`.
    mercury.sched.after_next_enqueue([&] {
      exec_loop();
      MESSAGE("peer venus to mercury:4040");
      venus.loop_after_next_enqueue();
      venus.ep.peer("mercury", 4040);
      MESSAGE("peer earth to mercury:4040");
      earth.loop_after_next_enqueue();
      earth.ep.peer("mercury", 4040);
    });
    // mercury.sched.inline_next_enqueue();
    mercury.ep.listen("", 4040);
  }
};

} // namespace

CAF_TEST_FIXTURE_SCOPE(triangle_use_cases, triangle_fixture)

// -- prefix-based data forwarding in Broker -----------------------------------

// Checks whether topic subscriptions are prefix-based using the asynchronous
// `endpoint::subscribe_nosync` API to subscribe to topics.
CAF_TEST(topic_prefix_matching_async_subscribe) {
  connect_peers();
  MESSAGE("assume two peers for mercury");
  mercury.loop_after_next_enqueue();
  auto mercury_peers = mercury.ep.peers();
  CAF_REQUIRE_EQUAL(mercury_peers.size(), 2u);
  CAF_CHECK_EQUAL(mercury_peers.front().status, peer_status::peered);
  CAF_CHECK_EQUAL(mercury_peers.back().status, peer_status::peered);
  MESSAGE("assume two peers for venus");
  venus.loop_after_next_enqueue();
  auto venus_peers = venus.ep.peers();
  CAF_REQUIRE_EQUAL(venus_peers.size(), 1u);
  CAF_CHECK_EQUAL(venus_peers.front().status, peer_status::peered);
  MESSAGE("assume two peers for earth");
  earth.loop_after_next_enqueue();
  auto earth_peers = earth.ep.peers();
  CAF_REQUIRE_EQUAL(earth_peers.size(), 1u);
  CAF_CHECK_EQUAL(earth_peers.front().status, peer_status::peered);
  MESSAGE("subscribe to 'zeek/events' on venus");
  venus.subscribe_to("zeek/events");
  MESSAGE("subscribe to 'zeek/events/failures' on earth");
  earth.subscribe_to("zeek/events/errors");
  MESSAGE("verify subscriptions");
  mercury.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(mercury.ep.peer_subscriptions(),
                  filter_type({"zeek/events"}));
  venus.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(venus.ep.peer_subscriptions(),
                  filter_type({"zeek/events/errors"}));
  earth.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(earth.ep.peer_subscriptions(), filter_type({"zeek/events"}));
  MESSAGE("publish to 'zeek/events/(data|errors)' on mercury");
  mercury.publish("zeek/events/errors", "oops", "sorry!");
  mercury.publish("zeek/events/data", 123, 456);
  MESSAGE("verify published data");
  CAF_CHECK_EQUAL(mercury.data, data_msgs({}));
  CAF_CHECK_EQUAL(venus.data, data_msgs({{"zeek/events/errors", "oops"},
                                         {"zeek/events/errors", "sorry!"},
                                         {"zeek/events/data", 123},
                                         {"zeek/events/data", 456}}));
  CAF_CHECK_EQUAL(earth.data, data_msgs({{"zeek/events/errors", "oops"},
                                         {"zeek/events/errors", "sorry!"}}));
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  earth.loop_after_next_enqueue();
  earth.ep.unpeer("mercury", 4040);
}

// Checks whether topic subscriptions are prefix-based using the synchronous
// `endpoint::make_subscriber` API to subscribe to topics.
CAF_TEST(topic_prefix_matching_make_subscriber) {
  connect_peers();
  MESSAGE("assume two peers for mercury");
  mercury.loop_after_next_enqueue();
  auto mercury_peers = mercury.ep.peers();
  CAF_REQUIRE_EQUAL(mercury_peers.size(), 2u);
  CAF_CHECK_EQUAL(mercury_peers.front().status, peer_status::peered);
  CAF_CHECK_EQUAL(mercury_peers.back().status, peer_status::peered);
  MESSAGE("assume two peers for venus");
  venus.loop_after_next_enqueue();
  auto venus_peers = venus.ep.peers();
  CAF_REQUIRE_EQUAL(venus_peers.size(), 1u);
  CAF_CHECK_EQUAL(venus_peers.front().status, peer_status::peered);
  MESSAGE("assume two peers for earth");
  earth.loop_after_next_enqueue();
  auto earth_peers = earth.ep.peers();
  CAF_REQUIRE_EQUAL(earth_peers.size(), 1u);
  CAF_CHECK_EQUAL(earth_peers.front().status, peer_status::peered);
  MESSAGE("subscribe to 'zeek/events' on venus");
  auto venus_s1 = venus.ep.make_subscriber({"zeek/events"});
  auto venus_s2 = venus.ep.make_subscriber({"zeek/events"});
  exec_loop();
  MESSAGE("subscribe to 'zeek/events/errors' on earth");
  auto earth_s1 = earth.ep.make_subscriber({"zeek/events/errors"});
  auto earth_s2 = earth.ep.make_subscriber({"zeek/events/errors"});
  exec_loop();
  MESSAGE("verify subscriptions");
  mercury.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(mercury.ep.peer_subscriptions(),
                  filter_type({"zeek/events"}));
  venus.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(venus.ep.peer_subscriptions(),
                  filter_type({"zeek/events/errors"}));
  earth.loop_after_next_enqueue();
  CAF_CHECK_EQUAL(earth.ep.peer_subscriptions(), filter_type({"zeek/events"}));
  MESSAGE("publish to 'zeek/events/(data|errors)' on mercury");
  mercury.publish("zeek/events/errors", "oops", "sorry!");
  mercury.publish("zeek/events/data", 123, 456);
  MESSAGE("verify published data");
  CAF_CHECK_EQUAL(venus_s1.poll(), data_msgs({{"zeek/events/errors", "oops"},
                                              {"zeek/events/errors", "sorry!"},
                                              {"zeek/events/data", 123},
                                              {"zeek/events/data", 456}}));
  CAF_CHECK_EQUAL(venus_s2.poll(), data_msgs({{"zeek/events/errors", "oops"},
                                              {"zeek/events/errors", "sorry!"},
                                              {"zeek/events/data", 123},
                                              {"zeek/events/data", 456}}));
  CAF_CHECK_EQUAL(earth_s1.poll(),
                  data_msgs({{"zeek/events/errors", "oops"},
                             {"zeek/events/errors", "sorry!"}}));
  CAF_CHECK_EQUAL(earth_s2.poll(),
                  data_msgs({{"zeek/events/errors", "oops"},
                             {"zeek/events/errors", "sorry!"}}));
  exec_loop();
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  earth.loop_after_next_enqueue();
  earth.ep.unpeer("mercury", 4040);
}

// -- unpeering of nodes and emitted status/error messages ---------------------

struct code {
  code(ec x) : value(x) {
    // nop
  }

  code(sc x) : value(x) {
    // nop
  }

  code(const event_value& x) {
    if (is<error>(x))
      value = static_cast<ec>(caf::get<error>(x).code());
    else
      value = get<status>(x).code();
  }

  std::variant<sc, ec> value;
};

std::string to_string(const code& x) {
  return is<sc>(x.value) ? to_string(std::get<sc>(x.value))
                         : to_string(std::get<ec>(x.value));
}

bool operator==(const code& x, const code& y) {
  return x.value == y.value;
}

std::vector<code> event_log(std::initializer_list<code> xs) {
  return {xs};
}

std::vector<code> event_log(const std::vector<event_value>& xs,
                            bool make_unique = false) {
  // For the purpose of this test, we only care about the peer_* statuses.
  auto predicate = [](const auto& x) {
    if constexpr (std::is_same_v<std::decay_t<decltype(x)>, status>) {
      auto c = x.code();
      return c == sc::peer_added || c == sc::peer_removed || c == sc::peer_lost;
    } else {
      return true;
    }
  };
  std::vector<code> ys;
  ys.reserve(xs.size());
  for (auto& x : xs)
    if (std::visit(predicate, x))
      ys.emplace_back(x);
  if (make_unique)
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  return ys;
}

CAF_TEST(unpeering) {
  MESSAGE("get events from all peers");
  auto mercury_es = mercury.ep.make_status_subscriber(true);
  auto venus_es = venus.ep.make_status_subscriber(true);
  auto earth_es = earth.ep.make_status_subscriber(true);
  exec_loop();
  connect_peers();
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()),
                  event_log({sc::peer_added, sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({sc::peer_added}));
  MESSAGE("disconnect venus from mercury");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_removed}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect venus again (raises ec::peer_invalid)");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({ec::peer_invalid}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect venus from sun (invalid peer)");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("sun", 123);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({ec::peer_invalid}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({}));
  MESSAGE("disconnect earth from mercury");
  earth.loop_after_next_enqueue();
  earth.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({}));
  CAF_CHECK_EQUAL(event_log(earth_es.poll()), event_log({sc::peer_removed}));
  CAF_CHECK(mercury.peers().empty());
  CAF_CHECK(venus.peers().empty());
  CAF_CHECK(earth.peers().empty());
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  earth.loop_after_next_enqueue();
  earth.ep.unpeer("mercury", 4040);
}

CAF_TEST(unpeering_without_connections) {
  MESSAGE("get events from all peers");
  auto venus_es = venus.ep.make_status_subscriber(true);
  exec_loop();
  MESSAGE("disconnect venus from non-existing peer");
  venus.loop_after_next_enqueue();
  exec_loop();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({ec::peer_invalid}));
}

CAF_TEST(connection_retry) {
  MESSAGE("get events from mercury and venus");
  auto mercury_es = mercury.ep.make_status_subscriber(true);
  auto venus_es = venus.ep.make_status_subscriber(true);
  exec_loop();
  MESSAGE("initiate peering from venus to mercury (will fail)");
  venus.ep.peer_nosync("mercury", 4040, std::chrono::seconds(1));
  MESSAGE("spawn helper that starts listening on mercury:4040 eventually");
  mercury.sys.spawn([&](caf::event_based_actor* self) -> caf::behavior {
    self->delayed_send(self, std::chrono::seconds(2), atom::ok_v);
    return {[&](caf::ok_atom) {
      MESSAGE("start listening on mercury:4040");
      auto server_handle = mercury.make_accept_handle();
      mercury.mpx.prepare_connection(server_handle,
                                     mercury.make_connection_handle(),
                                     venus.mpx, "mercury", 4040,
                                     venus.make_connection_handle());
      // We need to connect venus while mercury is blocked on ep.listen() in
      // order to avoid a "deadlock" in `ep.listen()`.
      mercury.sched.after_next_enqueue([&] {
        MESSAGE("peer venus to mercury:4040 by triggering the retry timeout");
        exec_loop();
      });
      mercury.ep.listen("", 4040);
    }};
  });
  exec_loop();
  MESSAGE("check event logs");
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_added}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll(), true),
                  event_log({ec::peer_unavailable, sc::peer_added}));
  MESSAGE("disconnect venus from mercury");
  venus.loop_after_next_enqueue();
  venus.ep.unpeer("mercury", 4040);
  CAF_CHECK_EQUAL(event_log(mercury_es.poll()), event_log({sc::peer_lost}));
  CAF_CHECK_EQUAL(event_log(venus_es.poll()), event_log({sc::peer_removed}));
}

CAF_TEST_FIXTURE_SCOPE_END()

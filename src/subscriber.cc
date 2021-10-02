#include "broker/logger.hh" // Must come before any CAF include.
#include "broker/subscriber.hh"

#include <chrono>
#include <cstddef>
#include <future>
#include <numeric>
#include <utility>

#include <caf/async/bounded_buffer.hpp>
#include <caf/async/consumer.hpp>
#include <caf/scheduled_actor.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/send.hpp>

#include "broker/atoms.hh"
#include "broker/detail/assert.hh"
#include "broker/detail/flare.hh"
#include "broker/endpoint.hh"
#include "broker/filter_type.hh"

namespace broker::detail {

struct subscriber_queue : public caf::ref_counted, public caf::async::consumer {
public:
  using buffer_type = caf::async::bounded_buffer<data_message>;

  using buffer_ptr = caf::async::bounded_buffer_ptr<data_message>;

  using guard_type = std::unique_lock<std::mutex>;

  explicit subscriber_queue(buffer_ptr buf) : buf_(std::move(buf)) {
    // nop
  }

  ~subscriber_queue() {
    if (buf_)
      buf_->cancel();
  }

  void on_producer_ready() override {
    // nop
  }

  void on_producer_wakeup() override {
    guard_type guard{mtx_};
    if (!ready_) {
      fx_.fire();
      ready_ = true;
    }
  }

  void wait() {
    guard_type guard{mtx_};
    if (!ready_)
      fx_.await_one();
  }

  bool wait_until(timestamp abs_timeout) {
    guard_type guard{mtx_};
    return ready_ || fx_.await_one(abs_timeout);
  }

  void ref_consumer() const noexcept override {
    this->ref();
  }

  void deref_consumer() const noexcept override {
    this->deref();
  }

  auto fd() const noexcept {
    return fx_.fd();
  }

  void cancel() {
    if (buf_)
      buf_->cancel();
  }

  void extinguish() {
    guard_type guard{mtx_};
    if (ready_) {
      ready_ = false;
      fx_.extinguish();
    }
  }

  void pull(std::vector<data_message>& dst_buf, size_t num) {
    BROKER_ASSERT(num > 0);
    struct cb {
      subscriber_queue* qptr;
      std::vector<data_message>* dst;
      void on_next(caf::span<const data_message> xs) {
        dst->insert(dst->end(), xs.begin(), xs.end());
      }
      void on_complete() {
        qptr->extinguish();
      }
      void on_error(const caf::error&) {
        qptr->extinguish();
      }
    };
    cb consumer{this, &dst_buf};
    if (buf_) {
      if (!buf_->pull(caf::async::delay_errors, num, consumer).second) {
        buf_ = nullptr;
      } else if (buf_->available() == 0) {
        guard_type guard{mtx_};
        if (ready_ && buf_->available() == 0) {
          ready_ = false;
          fx_.extinguish();
        }
      }
    }
  }

  size_t capacity() const noexcept {
    return buf_ ? buf_->capacity() : size_t{0};
  }

  size_t available() const noexcept {
    return buf_ ? buf_->available() : size_t{0};
  }

  friend void intrusive_ptr_add_ref(const subscriber_queue* ptr) noexcept {
    ptr->ref();
  }

  friend void intrusive_ptr_release(const subscriber_queue* ptr) noexcept {
    ptr->deref();
  }

private:
  /// Provides access to the shared buffer.
  buffer_ptr buf_;

  /// Guards access to other member variables.
  mutable std::mutex mtx_;

  /// Signals to users when data can be read or written.
  mutable detail::flare fx_;

  /// Stores whether we have data available.
  bool ready_ = false;
};

namespace {

auto* dptr(detail::opaque_type* ptr) {
  auto bptr = reinterpret_cast<caf::ref_counted*>(ptr);
  return static_cast<subscriber_queue*>(bptr);
}

auto* dptr(const detail::opaque_ptr& ptr) {
  return dptr(ptr.get());
}

detail::opaque_ptr make_opaque(caf::intrusive_ptr<subscriber_queue> ptr) {
  caf::ref_counted* raw = ptr.release();
  return {reinterpret_cast<detail::opaque_type*>(raw), false};
}

} // namespace

} // namespace broker::detail

using broker::detail::dptr;

namespace broker {

subscriber::subscriber(detail::opaque_ptr queue,
                       std::shared_ptr<filter_type> filter, caf::actor core)
  : queue_(std::move(queue)),
    core_(std::move(core)),
    core_filter_(std::move(filter)) {
  // nop
}

subscriber::~subscriber() {
  reset();
}

subscriber subscriber::make(endpoint& ep, filter_type filter, size_t) {
  BROKER_INFO("creating subscriber for topic(s)" << filter);
  using caf::async::make_bounded_buffer_resource;
  auto fptr = std::make_shared<filter_type>(std::move(filter));
  auto [con_res, prod_res] = make_bounded_buffer_resource<data_message>();
  caf::anon_send(ep.core(), fptr, std::move(prod_res));
  auto buf = con_res.try_open();
  BROKER_ASSERT(buf != nullptr);
  auto qptr = caf::make_counted<detail::subscriber_queue>(buf);
  buf->set_consumer(qptr);
  return subscriber{detail::make_opaque(std::move(qptr)), std::move(fptr),
                    ep.core()};
}

data_message subscriber::get() {
  auto tmp = get(1);
  BROKER_ASSERT(tmp.size() == 1);
  auto x = std::move(tmp.front());
  BROKER_DEBUG("received" << x);
  return x;
}

std::vector<data_message> subscriber::get(size_t num) {
  BROKER_ASSERT(num > 0);
  auto q = dptr(queue_);
  std::vector<data_message> buf;
  buf.reserve(num);
  q->pull(buf, num);
  while (buf.size() < num) {
    wait();
    q->pull(buf, num - buf.size());
  }
  return buf;
}

std::vector<data_message> subscriber::do_get(size_t num,
                                             timestamp abs_timeout) {
  std::vector<data_message> buf;
  do_get(buf, num, abs_timeout);
  return buf;
}

void subscriber::do_get(std::vector<data_message>& buf, size_t num,
                        timestamp abs_timeout) {
  auto q = dptr(queue_);
  buf.clear();
  buf.reserve(num);
  q->pull(buf, num);
  while (buf.size() < num && wait_until(abs_timeout))
    q->pull(buf, num - buf.size());
}

std::vector<data_message> subscriber::poll() {
  // The Queue may return a capacity of 0 if the producer has closed the flow.
  std::vector<data_message> buf;
  auto q = dptr(queue_);
  auto max_size = q->capacity();
  if (max_size > 0) {
    buf.reserve(max_size);
    q->pull(buf, max_size);
  }
  return buf;
}

size_t subscriber::available() const noexcept {
  return dptr(queue_)->available();
}

detail::native_socket subscriber::fd() const noexcept {
  return dptr(queue_)->fd();
}

void subscriber::add_topic(topic x, bool block) {
  BROKER_INFO("adding topic" << x << "to subscriber");
  update_filter(std::move(x), true, block);
}

void subscriber::remove_topic(topic x, bool block) {
  BROKER_INFO("removing topic" << x << "from subscriber");
  update_filter(std::move(x), false, block);
}

void subscriber::reset() {
  if (queue_) {
    dptr(queue_)->cancel();
    queue_ = nullptr;
    core_ = nullptr;
  }
}

void subscriber::update_filter(topic what, bool add, bool block) {
  if (!block) {
    caf::anon_send(core_, core_filter_, std::move(what), add,
                   std::shared_ptr<std::promise<void>>{nullptr});
  } else {
    auto sync = std::make_shared<std::promise<void>>();
    auto vfut = sync->get_future();
    caf::anon_send(core_, core_filter_, std::move(what), add, std::move(sync));
    vfut.get();
  }
}

void subscriber::wait() {
  dptr(queue_)->wait();
}

bool subscriber::wait_for(timespan rel_timeout) {
  return wait_until(now() + rel_timeout);
}

bool subscriber::wait_until(timestamp abs_timeout) {
  return dptr(queue_)->wait_until(abs_timeout);
}

} // namespace broker

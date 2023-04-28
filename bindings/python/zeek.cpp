
#include <stdexcept>
#include <string>
#include <utility>

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#include "broker/data.hh"
#include "broker/zeek.hh"

namespace py = pybind11;
using namespace pybind11::literals;

void init_zeek(py::module& m) {
  py::class_<broker::zeek::Message>(m, "Message")
    .def("as_data",
         static_cast<const broker::data& (broker::zeek::Message::*) () const>(
           &broker::zeek::Message::as_data));

  py::class_<broker::zeek::Event, broker::zeek::Message>(m, "Event")
    .def(py::init(
      [](broker::data data) { return broker::zeek::Event(std::move(data)); }))
    .def(py::init([](std::string name, broker::data args,
                     std::optional<double> ts) {
      if (ts)
        return broker::zeek::Event(std::move(name),
                                   std::move(broker::get<broker::vector>(args)),
                                   broker::to_timestamp(*ts));
      else
        return broker::zeek::Event(
          std::move(name), std::move(broker::get<broker::vector>(args)));
    }))
    .def("valid",
         [](const broker::zeek::Event& ev) -> bool {
           auto t = broker::zeek::Message::type(ev.as_data());
           if (t != broker::zeek::Message::Type::Event)
             return false;
           return ev.valid();
         })
    .def("name",
         [](const broker::zeek::Event& ev) -> const std::string& {
           auto t = broker::zeek::Message::type(ev.as_data());
           if (t != broker::zeek::Message::Type::Event) {
             throw std::invalid_argument("invalid Event data/type");
           }
           if (!ev.valid()) {
             throw std::invalid_argument("invalid Event data");
           }
           return ev.name();
         })
    .def("timestamp",
         [](const broker::zeek::Event& ev) -> const std::optional<double> {
           auto t = broker::zeek::Message::type(ev.as_data());
           if (t != broker::zeek::Message::Type::Event) {
             throw std::invalid_argument("invalid Event data/type");
           }
           if (!ev.valid()) {
             throw std::invalid_argument("invalid Event data");
           }
           if (auto ev_ts = ev.ts()) {
             double ts;
             broker::convert(*ev_ts, ts);
             return ts;
           }

           return std::nullopt;
         })
    .def("args", [](const broker::zeek::Event& ev) -> const broker::vector& {
      auto t = broker::zeek::Message::type(ev.as_data());
      if (t != broker::zeek::Message::Type::Event) {
        throw std::invalid_argument("invalid Event data/type");
      }
      if (!ev.valid()) {
        throw std::invalid_argument("invalid Event data");
      }
      return ev.args();
    });
}

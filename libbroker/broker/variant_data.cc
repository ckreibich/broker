#include "broker/variant_data.hh"

#include "broker/data.hh"
#include "broker/detail/type_traits.hh"
#include "broker/format/bin.hh"

#include <caf/detail/ieee_754.hpp>
#include <caf/detail/network_order.hpp>

#include <cstring>

namespace broker {

namespace {

/// Global instance of `variant_data` that represents a `nil` value.
const variant_data nil_instance;

template <variant_tag Tag, class Pred, class T1, class T2>
auto stl_visit(Pred& pred, const T1& lhs, const T2& rhs) {
  static constexpr auto index = static_cast<size_t>(Tag);
  return pred(std::get<index>(lhs.stl_value()),
              std::get<index>(rhs.stl_value()));
}

/// Visits two `data` and/or `variant` objects by calling `pred(lhs, rhs)` if
/// the types of `lhs` and `rhs` are the same. Otherwise, returns
/// `pred(lhs.type(), rhs.type())`.
template <class Predicate, class T1, class T2>
auto visit_if_same_type(Predicate&& pred, const T1& lhs, const T2& rhs) {
  // Note: we could do std::visit here, but that would require the Predicate
  //       to support all possible combinations of types. Instead, we only
  //       require the Predicate to support all combinations of types that
  //       can actually occur.
  auto lhs_type = lhs.get_tag();
  auto rhs_type = rhs.get_tag();
  if (lhs_type != rhs_type)
    return pred(lhs_type, rhs_type);
  using type = variant_tag;
  switch (lhs_type) {
    default: // type::none:
      return stl_visit<type::none>(pred, lhs, rhs);
    case type::boolean:
      return stl_visit<type::boolean>(pred, lhs, rhs);
    case type::integer:
      return stl_visit<type::integer>(pred, lhs, rhs);
    case type::count:
      return stl_visit<type::count>(pred, lhs, rhs);
    case type::real:
      return stl_visit<type::real>(pred, lhs, rhs);
    case type::string:
      return stl_visit<type::string>(pred, lhs, rhs);
    case type::address:
      return stl_visit<type::address>(pred, lhs, rhs);
    case type::subnet:
      return stl_visit<type::subnet>(pred, lhs, rhs);
    case type::port:
      return stl_visit<type::port>(pred, lhs, rhs);
    case type::timestamp:
      return stl_visit<type::timestamp>(pred, lhs, rhs);
    case type::timespan:
      return stl_visit<type::timespan>(pred, lhs, rhs);
    case type::enum_value:
      return stl_visit<type::enum_value>(pred, lhs, rhs);
    case type::set:
      return stl_visit<type::set>(pred, lhs, rhs);
    case type::table:
      return stl_visit<type::table>(pred, lhs, rhs);
    case type::vector:
      return stl_visit<type::vector>(pred, lhs, rhs);
  }
}

/// Compares two `data` and/or `variant` objects for equality.
struct eq_predicate {
  template <class T1, class T2>
  bool operator()(const T1& lhs, const T2& rhs) const {
    if constexpr (std::is_pointer_v<T1>)
      return (*this)(*lhs, rhs);
    else if constexpr (std::is_pointer_v<T2>)
      return (*this)(lhs, *rhs);
    else if constexpr (detail::has_begin_v<T1>)
      return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), *this);
    else if constexpr (detail::is_pair<T1>)
      return (*this)(lhs.first, rhs.first) && (*this)(lhs.second, rhs.second);
    else
      return lhs == rhs;
  }
};

} // namespace

const variant_data* variant_data::nil() noexcept {
  return &nil_instance;
}

data variant_data::to_data() const {
  auto f = [](const auto& val) -> broker::data {
    using val_type = std::decay_t<decltype(val)>;
    if constexpr (std::is_same_v<std::string_view, val_type>) {
      return broker::data{std::string{val}};
    } else if constexpr (std::is_same_v<enum_value_view, val_type>) {
      return broker::data{enum_value{std::string{val.name}}};
    } else if constexpr (std::is_same_v<variant_data::set*, val_type>) {
      broker::set result;
      for (const auto& x : *val)
        result.emplace(x.to_data());
      return broker::data{std::move(result)};
    } else if constexpr (std::is_same_v<variant_data::table*, val_type>) {
      broker::table result;
      for (const auto& [key, val] : *val)
        result.emplace(key.to_data(), val.to_data());
      return broker::data{std::move(result)};
    } else if constexpr (std::is_same_v<variant_data::list*, val_type>) {
      broker::vector result;
      result.reserve(val->size());
      for (const auto& x : *val)
        result.emplace_back(x.to_data());
      return broker::data{std::move(result)};
    } else {
      return broker::data{val};
    }
  };
  return std::visit(f, value);
}

namespace {

template <class T>
using mbr_allocator = detail::monotonic_buffer_resource::allocator<T>;

struct decoder_handler_value;

struct decoder_handler_list;

struct decoder_handler_set;

struct decoder_handler_table;

// Consumes events from a decoder and produces a data object.
struct decoder_handler_value {
  detail::monotonic_buffer_resource* buf;
  variant_data* result;

  template <class T>
  void value(const T& arg) {
    result->value = arg;
  }

  decoder_handler_list begin_list();

  void end_list(decoder_handler_list&);

  decoder_handler_set begin_set();

  void end_set(decoder_handler_set&);

  decoder_handler_table begin_table();

  void end_table(decoder_handler_table&);
};

// Consumes events from a decoder and produces a list of data objects.
struct decoder_handler_list {
  detail::monotonic_buffer_resource* buf;
  variant_data::list* result;

  explicit decoder_handler_list(detail::monotonic_buffer_resource* res)
    : buf(res) {
    using vec_allocator = mbr_allocator<variant_data>;
    using vec_type = variant_data::list;
    mbr_allocator<vec_type> allocator{buf};
    result = new (allocator.allocate(1)) vec_type(vec_allocator{buf});
  }

  template <class T>
  void value(const T& arg) {
    auto& item = result->emplace_back();
    item.value = arg;
  }

  auto begin_list() {
    return decoder_handler_list{buf};
  }

  void end_list(decoder_handler_list& other) {
    result->emplace_back().value = other.result;
  }

  decoder_handler_set begin_set();

  void end_set(decoder_handler_set&);

  decoder_handler_table begin_table();

  void end_table(decoder_handler_table&);
};

// Consumes events from a decoder and produces a set of data objects.
struct decoder_handler_set {
  detail::monotonic_buffer_resource* buf;
  variant_data::set* result;

  explicit decoder_handler_set(detail::monotonic_buffer_resource* res)
    : buf(res) {
    using set_allocator = mbr_allocator<variant_data>;
    using set_type = variant_data::set;
    mbr_allocator<set_type> allocator{buf};
    result = new (allocator.allocate(1)) set_type(set_allocator{buf});
  }

  template <class T>
  void value(const T& arg) {
    variant_data item;
    item.value = arg;
    result->insert(std::move(item));
  }

  auto begin_list() {
    return decoder_handler_list{buf};
  }

  void end_list(decoder_handler_list& other) {
    variant_data item;
    item.value = other.result;
    result->insert(std::move(item));
  }

  auto begin_set() {
    return decoder_handler_set{buf};
  }

  void end_set(decoder_handler_set& other) {
    variant_data item;
    item.value = other.result;
    result->insert(std::move(item));
  }

  decoder_handler_table begin_table();

  void end_table(decoder_handler_table&);
};

struct decoder_handler_table {
  detail::monotonic_buffer_resource* buf;
  variant_data::table* result;
  std::optional<variant_data> key;

  explicit decoder_handler_table(detail::monotonic_buffer_resource* res)
    : buf(res) {
    using table_allocator = variant_data::table_allocator;
    using table_type = variant_data::table;
    mbr_allocator<table_type> allocator{buf};
    result = new (allocator.allocate(1)) table_type(table_allocator{buf});
  }

  template <class T>
  void add(T&& arg) {
    if (!key) {
      key.emplace();
      key->value = arg;
    } else {
      variant_data val;
      val.value = arg;
      result->emplace(std::move(*key), std::move(val));
      key.reset();
    }
  }

  template <class T>
  void value(const T& arg) {
    add(arg);
  }

  auto begin_list() {
    return decoder_handler_list{buf};
  }

  void end_list(decoder_handler_list& other) {
    add(other.result);
  }

  auto begin_set() {
    return decoder_handler_set{buf};
  }

  void end_set(decoder_handler_set& other) {
    add(other.result);
  }

  auto begin_table() {
    return decoder_handler_table{buf};
  }

  void end_table(decoder_handler_table& other) {
    add(other.result);
  }

  void begin_key_value_pair() {
    // nop
  }

  void end_key_value_pair() {
    // nop
  }
};

decoder_handler_list decoder_handler_value::begin_list() {
  return decoder_handler_list{buf};
}

void decoder_handler_value::end_list(decoder_handler_list& other) {
  result->value = other.result;
}

decoder_handler_set decoder_handler_value::begin_set() {
  return decoder_handler_set{buf};
}

void decoder_handler_value::end_set(decoder_handler_set& other) {
  result->value = other.result;
}

decoder_handler_table decoder_handler_value::begin_table() {
  return decoder_handler_table{buf};
}

void decoder_handler_value::end_table(decoder_handler_table& other) {
  result->value = other.result;
}

decoder_handler_set decoder_handler_list::begin_set() {
  return decoder_handler_set{buf};
}

void decoder_handler_list::end_set(decoder_handler_set& other) {
  auto& item = result->emplace_back();
  item.value = other.result;
}

decoder_handler_table decoder_handler_list::begin_table() {
  return decoder_handler_table{buf};
}

void decoder_handler_list::end_table(decoder_handler_table& other) {
  auto& item = result->emplace_back();
  item.value = other.result;
}

decoder_handler_table decoder_handler_set::begin_table() {
  return decoder_handler_table{buf};
}

void decoder_handler_set::end_table(decoder_handler_table& other) {
  variant_data item;
  item.value = other.result;
  result->insert(std::move(item));
}

} // namespace

std::pair<bool, const std::byte*>
variant_data::parse_shallow(detail::monotonic_buffer_resource& buf,
                            const std::byte* begin, const std::byte* end) {
  decoder_handler_value handler{&buf, this};
  auto [ok, pos] = format::bin::v1::decode(begin, end, handler);
  if (!ok || pos != end) {
    return {false, pos};
  }
  return {true, pos};
}

// -- free functions -----------------------------------------------------------

bool operator==(const data& lhs, const variant_data& rhs) {
  return visit_if_same_type(eq_predicate{}, lhs, rhs);
}

bool operator==(const variant_data& lhs, const data& rhs) {
  return visit_if_same_type(eq_predicate{}, lhs, rhs);
}

bool operator==(const variant_data& lhs, const variant_data& rhs) {
  return visit_if_same_type(eq_predicate{}, lhs, rhs);
}

bool operator<(const variant_data& lhs, const variant_data& rhs) {
  if (lhs.value.index() != rhs.value.index())
    return lhs.value.index() < rhs.value.index();
  return std::visit(
    [&rhs](const auto& x) -> bool {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_pointer_v<T>) {
        return *x < *std::get<T>(rhs.value);
      } else {
        return x < std::get<T>(rhs.value);
      }
    },
    lhs.value);
}

} // namespace broker

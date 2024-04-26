/*
 * Copyright (c) 2023, Alibaba Group Holding Limited;
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <async_simple/Future.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <array>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <ylt/easylog.hpp>

#include "asio/buffer.hpp"
#include "asio/dispatch.hpp"
#include "asio/registered_buffer.hpp"
#include "async_simple/Executor.h"
#include "async_simple/Promise.h"
#include "common_service.hpp"
#include "context.hpp"
#include "expected.hpp"
#include "protocol/coro_rpc_protocol.hpp"
#include "ylt/coro_io/coro_io.hpp"
#include "ylt/coro_io/io_context_pool.hpp"
#include "ylt/coro_rpc/impl/errno.h"
#include "ylt/struct_pack.hpp"
#include "ylt/struct_pack/util.h"
#include "ylt/util/function_name.h"
#include "ylt/util/type_traits.h"
#include "ylt/util/utils.hpp"
#ifdef UNIT_TEST_INJECT
#include "inject_action.hpp"
#endif

#ifdef GENERATE_BENCHMARK_DATA
#include <fstream>
#endif
namespace coro_io {
template <typename T, typename U>
class client_pool;
}

namespace coro_rpc {

#ifdef GENERATE_BENCHMARK_DATA
std::string benchmark_file_path = "./";
#endif

class coro_connection;

template <typename T>
struct rpc_return_type {
  using type = T;
};
template <>
struct rpc_return_type<void> {
  using type = std::monostate;
};

struct rpc_resp_buffer {
  std::string read_buf_;
  std::string resp_attachment_buf_;
};

template <typename T>
struct async_rpc_result {
  T result_;
  rpc_resp_buffer buffer_;
};

template <>
struct async_rpc_result<void> {
  rpc_resp_buffer buffer_;
};

template <typename T>
using rpc_return_type_t = typename rpc_return_type<T>::type;
/*!
 * ```cpp
 * #include <ylt/coro_rpc/coro_rpc_client.hpp>
 *
 * using namespace coro_rpc;
 * using namespace async_simple::coro;
 *
 * Lazy<void> show_rpc_call(coro_rpc_client &client) {
 *   auto ec = co_await client.connect("127.0.0.1", "8801");
 *   assert(!ec);
 *   auto result = co_await client.call<hello_coro_rpc>();
 *   if (!result) {
 *     std::cout << "err: " << result.error().msg << std::endl;
 *   }
 *   assert(result.value() == "hello coro_rpc"s);
 * }
 *
 * int main() {
 *   coro_rpc_client client;
 *   syncAwait(show_rpc_call(client));
 * }
 * ```
 */
class coro_rpc_client {
  using coro_rpc_protocol = coro_rpc::protocol::coro_rpc_protocol;

 public:
  const inline static rpc_error connect_error = {errc::io_error,
                                                 "client has been closed"};
  struct config {
    uint32_t client_id = 0;
    std::chrono::milliseconds timeout_duration =
        std::chrono::milliseconds{5000};
    std::string host;
    std::string port;
#ifdef YLT_ENABLE_SSL
    std::filesystem::path ssl_cert_path;
    std::string ssl_domain;
#endif
  };

  /*!
   * Create client with io_context
   * @param io_context asio io_context, async event handler
   */
  coro_rpc_client(asio::io_context::executor_type executor,
                  uint32_t client_id = 0)
      : control_(std::make_shared<control_t>(executor, false)),
        timer_(std::make_unique<coro_io::period_timer>(executor)) {
    config_.client_id = client_id;
  }

  /*!
   * Create client with io_context
   * @param io_context asio io_context, async event handler
   */
  coro_rpc_client(
      coro_io::ExecutorWrapper<> &executor = *coro_io::get_global_executor(),
      uint32_t client_id = 0)
      : control_(
            std::make_shared<control_t>(executor.get_asio_executor(), false)),
        timer_(std::make_unique<coro_io::period_timer>(executor.get_asio_executor())) {
    config_.client_id = client_id;
  }

  std::string_view get_host() const { return config_.host; }

  std::string_view get_port() const { return config_.port; }

  [[nodiscard]] bool init_config(const config &conf) {
    config_ = conf;
#ifdef YLT_ENABLE_SSL
    if (!config_.ssl_cert_path.empty())
      return init_ssl_impl();
    else
#endif
      return true;
  };

  /*!
   * Check the client closed or not
   *
   * @return true if client closed, otherwise false.
   */
  [[nodiscard]] bool has_closed() { return control_->has_closed_; }

  /*!
   * Reconnect server
   *
   * If connect hasn't been closed, it will be closed first then connect to
   * server, else the client will connect to server directly
   *
   * @param host server address
   * @param port server port
   * @param timeout_duration RPC call timeout
   * @return error code
   */
  [[nodiscard]] async_simple::coro::Lazy<coro_rpc::err_code> reconnect(
      std::string host, std::string port,
      std::chrono::steady_clock::duration timeout_duration =
          std::chrono::seconds(5)) {
    config_.host = std::move(host);
    config_.port = std::move(port);
    config_.timeout_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
    reset();
    return connect(is_reconnect_t{true});
  }

  [[nodiscard]] async_simple::coro::Lazy<coro_rpc::err_code> reconnect(
      std::string endpoint,
      std::chrono::steady_clock::duration timeout_duration =
          std::chrono::seconds(5)) {
    auto pos = endpoint.find(':');
    config_.host = endpoint.substr(0, pos);
    config_.port = endpoint.substr(pos + 1);
    config_.timeout_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
    reset();
    return connect(is_reconnect_t{true});
  }
  /*!
   * Connect server
   *
   * If connect hasn't been closed, it will be closed first then connect to
   * server, else the client will connect to server directly
   *
   * @param host server address
   * @param port server port
   * @param timeout_duration RPC call timeout
   * @return error code
   */
  [[nodiscard]] async_simple::coro::Lazy<coro_rpc::err_code> connect(
      std::string host, std::string port,
      std::chrono::steady_clock::duration timeout_duration =
          std::chrono::seconds(5)) {
    config_.host = std::move(host);
    config_.port = std::move(port);
    config_.timeout_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
    return connect();
  }
  [[nodiscard]] async_simple::coro::Lazy<coro_rpc::err_code> connect(
      std::string_view endpoint,
      std::chrono::steady_clock::duration timeout_duration =
          std::chrono::seconds(5)) {
    auto pos = endpoint.find(':');
    config_.host = endpoint.substr(0, pos);
    config_.port = endpoint.substr(pos + 1);
    config_.timeout_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
    return connect();
  }

#ifdef YLT_ENABLE_SSL

  [[nodiscard]] bool init_ssl(std::string_view cert_base_path,
                              std::string_view cert_file_name,
                              std::string_view domain = "localhost") {
    config_.ssl_cert_path =
        std::filesystem::path(cert_base_path).append(cert_file_name);
    config_.ssl_domain = domain;
    return init_ssl_impl();
  }
#endif

  ~coro_rpc_client() { close(); }

  /*!
   * Call RPC function with default timeout (5 second)
   *
   * @tparam func the address of RPC function
   * @tparam Args the type of arguments
   * @param args RPC function arguments
   * @return RPC call result
   */
  template <auto func, typename... Args>
  async_simple::coro::Lazy<
      rpc_result<decltype(get_return_type<func>()), coro_rpc_protocol>>
  call(Args&&... args) {
    return call_for<func>(std::chrono::seconds(5), std::forward<Args>(args)...);
  }

  /*!
   * Call RPC function
   *
   * Timeout must be set explicitly.
   *
   * @tparam func the address of RPC function
   * @tparam Args the type of arguments
   * @param duration RPC call timeout
   * @param args RPC function arguments
   * @return RPC call result
   */
  template <auto func, typename... Args>
  async_simple::coro::Lazy<
      rpc_result<decltype(get_return_type<func>()), coro_rpc_protocol>>
  call_for(auto duration, Args&&... args) {
    is_waiting_for_response_=true;
    using return_type=decltype(get_return_type<func>());
    auto result = co_await send_request_for<func,Args...>(duration, std::forward<Args>(args)...);
    if (result) {
      auto async_result = co_await result.value();
      if (async_result) {
        if constexpr (std::is_same_v<return_type, void>) {
          co_return expected<return_type,rpc_error>{};
        }
        else {
          co_return expected<return_type,rpc_error>{std::move(async_result.value().result_)};
        }
      }
      else {
        co_return expected<return_type,rpc_error>{unexpect_t{},std::move(async_result.error())};
      }
    }
    else {
      co_return expected<return_type,rpc_error>{unexpect_t{},std::move(result.error())};
    }
  }

  /*!
   * Get inner executor
   */
  auto &get_executor() { return control_->executor_; }

  uint32_t get_client_id() const { return config_.client_id; }

  void close() {
    ELOGV(INFO, "client_id %d close", config_.client_id);
    close_socket(control_);
  }

  bool set_req_attachment(std::string_view attachment) {
    if (attachment.size() > UINT32_MAX) {
      ELOGV(ERROR, "too large rpc attachment");
      return false;
    }
    req_attachment_ = attachment;
    return true;
  }

  std::string_view get_resp_attachment() const { return control_->resp_buffer_.resp_attachment_buf_; }

  std::string release_resp_attachment() {
    return std::move(control_->resp_buffer_.resp_attachment_buf_);
  }

  template <typename T, typename U>
  friend class coro_io::client_pool;

 private:
  // the const char * will convert to bool instead of std::string_view
  // use this struct to prevent it.
  struct is_reconnect_t {
    bool value = false;
  };

  void reset() {
    close_socket(control_);
    control_->socket_ =
        asio::ip::tcp::socket(control_->executor_.get_asio_executor());
    control_->is_timeout_ = false;
    control_->has_closed_ = false;
  }
  static bool is_ok(coro_rpc::err_code ec) noexcept { return !ec; }
  [[nodiscard]] async_simple::coro::Lazy<coro_rpc::err_code> connect(
      is_reconnect_t is_reconnect = is_reconnect_t{false}) {
#ifdef YLT_ENABLE_SSL
    if (!ssl_init_ret_) {
      std::cout << "ssl_init_ret_: " << ssl_init_ret_ << std::endl;
      co_return errc::not_connected;
    }
#endif
    if (!is_reconnect.value && control_->has_closed_)
      AS_UNLIKELY {
        ELOGV(ERROR,
              "a closed client is not allowed connect again, please use "
              "reconnect function or create a new "
              "client");
        co_return errc::io_error;
      }
    control_->has_closed_ = false;

    ELOGV(INFO, "client_id %d begin to connect %s", config_.client_id,
          config_.port.data());
    timeout(*this->timer_,config_.timeout_duration, "connect timer canceled")
        .start([](auto &&) {
        });

    std::error_code ec = co_await coro_io::async_connect(
        &control_->executor_, control_->socket_, config_.host, config_.port);
    std::error_code err_code;
    timer_->cancel(err_code);

    if (ec) {
      if (control_->is_timeout_) {
        co_return errc::timed_out;
      }
      co_return errc::not_connected;
    }

    if (control_->is_timeout_) {
      ELOGV(WARN, "client_id %d connect timeout", config_.client_id);
      co_return errc::timed_out;
    }

    control_->socket_.set_option(asio::ip::tcp::no_delay(true), ec);

#ifdef YLT_ENABLE_SSL
    if (!config_.ssl_cert_path.empty()) {
      assert(control_->ssl_stream_);
      auto shake_ec = co_await coro_io::async_handshake(
          control_->ssl_stream_, asio::ssl::stream_base::client);
      if (shake_ec) {
        ELOGV(WARN, "client_id %d handshake failed: %s", config_.client_id,
              shake_ec.message().data());
        co_return errc::not_connected;
      }
    }
#endif

    co_return coro_rpc::err_code{};
  };
#ifdef YLT_ENABLE_SSL
  [[nodiscard]] bool init_ssl_impl() {
    try {
      ssl_init_ret_ = false;
      ELOGV(INFO, "init ssl: %s", config_.ssl_domain.data());
      auto &cert_file = config_.ssl_cert_path;
      ELOGV(INFO, "current path %s",
            std::filesystem::current_path().string().data());
      if (file_exists(cert_file)) {
        ELOGV(INFO, "load %s", cert_file.string().data());
        ssl_ctx_.load_verify_file(cert_file);
      }
      else {
        ELOGV(INFO, "no certificate file %s", cert_file.string().data());
        return ssl_init_ret_;
      }
      ssl_ctx_.set_verify_mode(asio::ssl::verify_peer);
      ssl_ctx_.set_verify_callback(
          asio::ssl::host_name_verification(config_.ssl_domain));
      control_->ssl_stream_ =
          std::make_unique<asio::ssl::stream<asio::ip::tcp::socket &>>(
              control_->socket_, ssl_ctx_);
      ssl_init_ret_ = true;
    } catch (std::exception &e) {
      ELOGV(ERROR, "init ssl failed: %s", e.what());
    }
    return ssl_init_ret_;
  }
#endif

  async_simple::coro::Lazy<bool> timeout(coro_io::period_timer &timer,
                                         auto duration, std::string err_msg) {
    timer.expires_after(duration);
    std::weak_ptr socket_watcher = control_;
    bool is_timeout = co_await timer.async_await();
    if (!is_timeout) {
      co_return false;
    }
    if (auto self = socket_watcher.lock()) {
      self->is_timeout_ = is_timeout;
      close_socket(self);
      co_return true;
    }
    co_return false;
  }

  template <auto func, typename... Args>
  void static_check() {
    using Function = decltype(func);
    using param_type = util::function_parameters_t<Function>;
    if constexpr (!std::is_void_v<param_type>) {
      using First = std::tuple_element_t<0, param_type>;
      constexpr bool is_conn = requires { typename First::return_type; };

      if constexpr (std::is_member_function_pointer_v<Function>) {
        using Self = util::class_type_t<Function>;
        if constexpr (is_conn) {
          static_assert(
              util::is_invocable<Function, Self, First, Args...>::value,
              "called rpc function and arguments are not match");
        }
        else {
          static_assert(util::is_invocable<Function, Self, Args...>::value,
                        "called rpc function and arguments are not match");
        }
      }
      else {
        if constexpr (is_conn) {
          static_assert(util::is_invocable<Function, First, Args...>::value,
                        "called rpc function and arguments are not match");
        }
        else {
          static_assert(util::is_invocable<Function, Args...>::value,
                        "called rpc function and arguments are not match");
        }
      }
    }
    else {
      if constexpr (std::is_member_function_pointer_v<Function>) {
        using Self = util::class_type_t<Function>;
        static_assert(util::is_invocable<Function, Self, Args...>::value,
                      "called rpc function and arguments are not match");
      }
      else {
        static_assert(util::is_invocable<Function, Args...>::value,
                      "called rpc function and arguments are not match");
      }
    }
  }

  /*
   * buffer layout
   * ┌────────────────┬────────────────┐
   * │req_header      │args            │
   * ├────────────────┼────────────────┤
   * │REQ_HEADER_LEN  │variable length │
   * └────────────────┴────────────────┘
   */
  template <auto func, typename... Args>
  std::vector<std::byte> prepare_buffer(uint32_t& id, Args &&...args) {
    std::vector<std::byte> buffer;
    std::size_t offset = coro_rpc_protocol::REQ_HEAD_LEN;
    if constexpr (sizeof...(Args) > 0) {
      using arg_types = util::function_parameters_t<decltype(func)>;
      pack_to<arg_types>(buffer, offset, std::forward<Args>(args)...);
    }
    else {
      buffer.resize(offset);
    }

    auto &header = *(coro_rpc_protocol::req_header *)buffer.data();
    header = {};
    header.magic = coro_rpc_protocol::magic_number;
    header.function_id = func_id<func>();
    header.attach_length = req_attachment_.size();
    id = request_id_++;
    ELOG_TRACE<<"send request ID:"<<id<<".";
    header.seq_num = id;
    
#ifdef UNIT_TEST_INJECT
    if (g_action == inject_action::client_send_bad_magic_num) {
      header.magic = coro_rpc_protocol::magic_number + 1;
    }
    if (g_action == inject_action::client_send_header_length_0) {
      header.length = 0;
    }
    else {
#endif
      auto sz = buffer.size() - coro_rpc_protocol::REQ_HEAD_LEN;
      if (sz > UINT32_MAX) {
        ELOGV(ERROR, "too large rpc body");
        return {};
      }
      header.length = sz;
#ifdef UNIT_TEST_INJECT
    }
#endif
    return buffer;
  }

  template <typename T>
  static rpc_result<T, coro_rpc_protocol> handle_response_buffer(std::string_view buffer,
                                                          uint8_t rpc_errc,bool& should_close) {
    rpc_return_type_t<T> ret;
    struct_pack::err_code ec;
    rpc_error err;
    if (rpc_errc == 0)
      AS_LIKELY {
        ec = struct_pack::deserialize_to(ret, buffer);
        if SP_LIKELY (!ec) {
          if constexpr (std::is_same_v<T, void>) {
            return {};
          }
          else {
            return std::move(ret);
          }
        }
      }
    else {
      if (rpc_errc != UINT8_MAX) {
        err.val() = rpc_errc;
        ec = struct_pack::deserialize_to(err.msg, buffer);
        if SP_LIKELY (!ec) {
          should_close = true;
          return rpc_result<T, coro_rpc_protocol>{unexpect_t{}, std::move(err)};
        }
      }
      else {
        ec = struct_pack::deserialize_to(err, buffer);
        if SP_LIKELY (!ec) {
          return rpc_result<T, coro_rpc_protocol>{unexpect_t{}, std::move(err)};
        }
      }
    }
    should_close = true;
    // deserialize failed.
    ELOGV(WARNING, "deserilaize rpc result failed");
    err = {errc::invalid_rpc_result, "failed to deserialize rpc return value"};
    return rpc_result<T, coro_rpc_protocol>{unexpect_t{}, std::move(err)};
  }

  template <typename FuncArgs>
  auto get_func_args() {
    using First = std::tuple_element_t<0, FuncArgs>;
    constexpr bool has_conn_v = requires { typename First::return_type; };
    return util::get_args<has_conn_v, FuncArgs>();
  }

  template <typename... FuncArgs, typename Buffer, typename... Args>
  void pack_to_impl(Buffer &buffer, std::size_t offset, Args &&...args) {
    struct_pack::serialize_to_with_offset(
        buffer, offset, std::forward<FuncArgs>(std::forward<Args>(args))...);
  }

  template <typename Tuple, size_t... Is, typename Buffer, typename... Args>
  void pack_to_helper(std::index_sequence<Is...>, Buffer &buffer,
                      std::size_t offset, Args &&...args) {
    pack_to_impl<std::tuple_element_t<Is, Tuple>...>(
        buffer, offset, std::forward<Args>(args)...);
  }

  template <typename FuncArgs, typename Buffer, typename... Args>
  void pack_to(Buffer &buffer, std::size_t offset, Args &&...args) {
    using tuple_pack = decltype(get_func_args<FuncArgs>());
    pack_to_helper<tuple_pack>(
        std::make_index_sequence<std::tuple_size_v<tuple_pack>>{}, buffer,
        offset, std::forward<Args>(args)...);
  }

  struct async_rpc_raw_result_value_type {
    std::variant<rpc_resp_buffer,std::string_view> buffer_;
    uint8_t errc_;
  };

  using async_rpc_raw_result=std::variant<async_rpc_raw_result_value_type,std::error_code>;

  struct control_t;

  struct handler_t {
    std::unique_ptr<coro_io::period_timer> timer_;
    control_t* control_;
    async_simple::Promise<async_rpc_raw_result> promise_;
    handler_t(std::unique_ptr<coro_io::period_timer>&& timer,control_t* control,async_simple::Promise<async_rpc_raw_result> &&promise):timer_(std::move(timer)), control_(control),promise_(std::move(promise)) {}
    void operator()(rpc_resp_buffer&& buffer,uint8_t rpc_errc) {
      timer_->cancel();
      if (control_) /*is waiting for response*/ {
        promise_.setValue(async_rpc_raw_result{async_rpc_raw_result_value_type{std::string_view{control_->resp_buffer_.read_buf_},rpc_errc}});
      }
      else {
        promise_.setValue(async_rpc_raw_result{async_rpc_raw_result_value_type{std::move(buffer),rpc_errc}});
      }
    }
    void local_error (std::error_code& ec) {
      timer_->cancel();
      promise_.setValue(async_rpc_raw_result{ec});
    } 
  };
  struct control_t {
#ifdef YLT_ENABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket &>> ssl_stream_;
#endif
#ifdef GENERATE_BENCHMARK_DATA
  std::string func_name_;
#endif
    bool is_timeout_;
    std::atomic<bool> has_closed_ = false;
    coro_io::ExecutorWrapper<> executor_;
    std::unordered_map<uint32_t, handler_t> response_handler_table_;
    rpc_resp_buffer resp_buffer_;
    asio::ip::tcp::socket socket_;
    std::atomic<bool> is_recving_=false;
    control_t(asio::io_context::executor_type executor, bool is_timeout)
        : socket_(executor), is_timeout_(is_timeout), has_closed_(false), executor_(executor) {}
  };

  static void close_socket(
      std::shared_ptr<coro_rpc_client::control_t> control) {
    if (control->has_closed_) {
      return;
    }
    control->has_closed_ = true;
    control->executor_.schedule([control = std::move(control)]() {
      asio::error_code ignored_ec;
      control->socket_.shutdown(asio::ip::tcp::socket::shutdown_both,
                                ignored_ec);
      control->socket_.close(ignored_ec);
    });
  }

#ifdef UNIT_TEST_INJECT
 public:
  coro_rpc::err_code sync_connect(const std::string &host,
                                  const std::string &port) {
    return async_simple::coro::syncAwait(connect(host, port));
  }

  template <auto func, typename... Args>
  rpc_result<decltype(get_return_type<func>()), coro_rpc_protocol> sync_call(
      Args &&...args) {
    return async_simple::coro::syncAwait(
        call<func>(std::forward<Args>(args)...));
  }
#endif



  template <auto func, typename... Args>
  async_simple::coro::Lazy<rpc_error>
  send_request_for_impl(auto duration, uint32_t& id, Args &&...args) {
    using R = decltype(get_return_type<func>());

    if (control_->has_closed_)
      AS_UNLIKELY {
        ELOGV(ERROR, "client has been closed, please re-connect");
        co_return rpc_error{errc::io_error,
                      "client has been closed, please re-connect"};
      }

#ifdef YLT_ENABLE_SSL
    if (!ssl_init_ret_) {
      co_return rpc_error{errc::not_connected}};
    }
#endif

    static_check<func, Args...>();

    if (duration.count() > 0) {
      if (timer_ == nullptr)
        timer_ = std::make_unique<coro_io::period_timer>(control_->executor_.get_asio_executor());
      timeout(*timer_, duration, "rpc call timer canceled").start([](auto &&) {
      });
    }

#ifdef YLT_ENABLE_SSL
    if (!config_.ssl_cert_path.empty()) {
      assert(control_->ssl_stream_);
      co_return co_await send_impl<func>(*control_->ssl_stream_, id, std::forward<Args>(args)...);
    }
    else {
#endif
      co_return co_await send_impl<func>(control_->socket_, id, std::forward<Args>(args)...);
#ifdef YLT_ENABLE_SSL
    }
#endif
  }


  static void send_err_response(control_t* controller, std::error_code& errc) {
    rpc_error ec;
    for (auto &e:controller->response_handler_table_) {
      e.second.local_error(errc);
    }
    controller->response_handler_table_.clear();
  }
  template<typename Socket>
  static async_simple::coro::Lazy<void> recv(std::shared_ptr<control_t> controller, Socket& socket) {
    std::pair<std::error_code,std::size_t> ret;
    do {
      coro_rpc_protocol::resp_header header;
      ret = co_await coro_io::async_read(
          socket,
          asio::buffer((char *)&header, coro_rpc_protocol::RESP_HEAD_LEN));
      if (ret.first) {
        ELOG_ERROR<<"read rpc head failed, error msg:"<<ret.first.message()<<". close the socket.value="<<ret.first.value();
        break;
      }
      uint32_t body_len = header.length;
      struct_pack::detail::resize(controller->resp_buffer_.read_buf_, body_len);
      if (header.attach_length == 0) {
        ret = co_await coro_io::async_read(
            socket, asio::buffer(controller->resp_buffer_.read_buf_.data(), body_len));
        controller->resp_buffer_.resp_attachment_buf_.clear();
      }
      else {
        struct_pack::detail::resize(controller->resp_buffer_.resp_attachment_buf_,
                                    header.attach_length);
        std::array<asio::mutable_buffer, 2> iov{
            asio::mutable_buffer{controller->resp_buffer_.read_buf_.data(), body_len},
            asio::mutable_buffer{controller->resp_buffer_.resp_attachment_buf_.data(),
                                  controller->resp_buffer_.resp_attachment_buf_.size()}};
        ret = co_await coro_io::async_read(socket, iov);
      }
      if (ret.first) {
        ELOG_ERROR<<"read rpc body failed, error msg:"<<ret.first.message()<<". close the socket.";
        break;
      }
#ifdef GENERATE_BENCHMARK_DATA
      std::ofstream file(
          benchmark_file_path + controller->func_name_ + ".out",
          std::ofstream::binary | std::ofstream::out);
      file << std::string_view{(char *)&header,
                                coro_rpc_protocol::RESP_HEAD_LEN};
      file << controller->resp_buffer_.read_buf_;
      file << controller->resp_buffer_.resp_attachment_buf_;
      file.close();
#endif
      if (auto iter=controller->response_handler_table_.find(header.seq_num);iter!=controller->response_handler_table_.end()) {
        ELOG_TRACE<<"find request ID:"<<header.seq_num<<". start notify response handler";
        iter->second(std::move(controller->resp_buffer_), header.err_code);
        controller->response_handler_table_.erase(iter);
      }
      else {
        ELOG_ERROR<<"unexists request ID:"<<header.seq_num<<". close the socket.";
        break;
      }
      if (controller->response_handler_table_.size() == 0) {
        controller->is_recving_= false;
        co_return;
      }
    } while (true);
    controller->is_recving_ = false;
    close_socket(controller); 
    send_err_response(controller.get(),ret.first);
    co_return;
  }

  template <typename T>
  static async_simple::coro::Lazy<expected<async_rpc_result<T>,rpc_error>> get_deserializer(async_simple::Future<async_rpc_raw_result> future,std::weak_ptr<control_t> watcher) {
    auto executor = co_await async_simple::CurrentExecutor();
    auto executorFuture = std::move(future).via(executor);
    auto ret_ = co_await std::move(executorFuture);
    
    if (ret_.index() ==1) [[unlikely]] { // local error
      auto& ret=std::get<1>(ret_);
      if (ret.value()==static_cast<int>(std::errc::operation_canceled) || ret.value()==static_cast<int>(std::errc::timed_out)) {
        co_return coro_rpc::unexpected<rpc_error>{rpc_error{errc::timed_out,ret.message()}};
      }
      else {
        co_return coro_rpc::unexpected<rpc_error>{rpc_error{errc::io_error,ret.message()}};
      }
    }
    
    bool should_close=false;
    std::string_view buffer_view;
    auto &ret=std::get<0>(ret_);
    if (ret.buffer_.index()==0) {
      buffer_view = std::get<0>(ret.buffer_).read_buf_;
    }
    else {
      buffer_view = std::get<1>(ret.buffer_);
    }
    auto result = handle_response_buffer<T>(buffer_view,ret.errc_,should_close); 
    if (should_close) {
      if (auto w=watcher.lock();w) {
        close_socket(std::move(w));
      }
    }
    if (result) {
      if constexpr (std::is_same_v<T, void>) {
        if (ret.buffer_.index()==0) {
          co_return async_rpc_result<T>{std::move(std::get<0>(ret.buffer_))};
        }
        else {
          co_return async_rpc_result<T>{};
        }
      }
      else {
        if (ret.buffer_.index()==0) {
          co_return async_rpc_result<T>{result.value(),std::move(std::get<0>(ret.buffer_))};
        }
        else {
          co_return async_rpc_result<T>{result.value()};
        }
      }
    } else {
      co_return coro_rpc::unexpected<rpc_error>{result.error()};
    }
  }
public:

    template <auto func, typename... Args>
  async_simple::coro::Lazy<coro_rpc::expected<
      async_simple::coro::Lazy<coro_rpc::expected<
          async_rpc_result<decltype(get_return_type<func>())>, rpc_error>>,
      rpc_error>>
  send_request(Args &&...args) {
    return send_request_for<func>(std::chrono::seconds{5}, std::forward<Args>(args)...);
  }

  template <auto func, typename... Args>
  async_simple::coro::Lazy<coro_rpc::expected<
      async_simple::coro::Lazy<coro_rpc::expected<
          async_rpc_result<decltype(get_return_type<func>())>, rpc_error>>,
      rpc_error>>
  send_request_for(auto duration, Args &&...args) {
    uint32_t id;
    auto result = co_await send_request_for_impl<func>(duration, id, std::forward<Args>(args)...);
    auto &control = *control_;
    if (!result) {
      async_simple::Promise<async_rpc_raw_result> promise;
      auto future = promise.getFuture();
      bool is_waiting_for_response=is_waiting_for_response_;
      is_waiting_for_response_=false;
      auto &&[_, is_ok] = control.response_handler_table_.try_emplace(
        id, std::move(timer_), is_waiting_for_response?control_.get():nullptr,std::move(promise));
      if (!is_ok) [[unlikely]] {
        close();
        err_code ec=errc::serial_number_conflict;
        co_return coro_rpc::unexpected<coro_rpc::rpc_error>{ec};
      }
      else {
        if (!control.is_recving_) {
          control.is_recving_ = true;        
#ifdef YLT_ENABLE_SSL
          if (!config_.ssl_cert_path.empty()) {
            assert(control.ssl_stream_);
            recv(control_,*control.ssl_stream_).start([](auto&&){});
          }
          else {
#endif
            recv(control_,control.socket_).start([](auto&&){});
#ifdef YLT_ENABLE_SSL
          }
#endif  
        }
        co_return get_deserializer<decltype(get_return_type<func>())>(std::move(future),std::weak_ptr<control_t>{control_});
      }
    }
    else {
      co_return coro_rpc::unexpected<rpc_error>{std::move(result)};
    }
  }
private:


  template <auto func, typename Socket, typename... Args>
  async_simple::coro::Lazy<rpc_error>
  send_impl(Socket &socket, uint32_t& id, Args&&... args) {
    auto buffer = prepare_buffer<func>(id, std::forward<Args>(args)...);
    if (buffer.empty()) {
      co_return rpc_error{errc::message_too_large};
      }
#ifdef GENERATE_BENCHMARK_DATA
    control_->func_name_ = get_func_name<func>();
    std::ofstream file(
        benchmark_file_path + control_->func_name_ + ".in",
        std::ofstream::binary | std::ofstream::out);
    file << std::string_view{(char *)buffer.data(), buffer.size()};
    file.close();
#endif
    std::pair<std::error_code, size_t> ret;
#ifdef UNIT_TEST_INJECT
    if (g_action == inject_action::client_send_bad_header) {
      buffer[0] = (std::byte)(uint8_t(buffer[0]) + 1);
    }
    if (g_action == inject_action::client_close_socket_after_send_header) {
      ret = co_await coro_io::async_write(
          socket,
          asio::buffer(buffer.data(), coro_rpc_protocol::REQ_HEAD_LEN));
      ELOGV(INFO, "client_id %d close socket", config_.client_id);
      close();
      co_return rpc_error{errc::io_error, ret.first.message()};
    }
    else if (g_action ==
              inject_action::client_close_socket_after_send_partial_header) {
      ret = co_await coro_io::async_write(
          socket,
          asio::buffer(buffer.data(), coro_rpc_protocol::REQ_HEAD_LEN - 1));
      ELOGV(INFO, "client_id %d close socket", config_.client_id);
      close();
      co_return rpc_error{errc::io_error, ret.first.message()};
    }
    else if (g_action ==
              inject_action::client_shutdown_socket_after_send_header) {
      ret = co_await coro_io::async_write(
          socket,
          asio::buffer(buffer.data(), coro_rpc_protocol::REQ_HEAD_LEN));
      ELOGV(INFO, "client_id %d shutdown", config_.client_id);
      control_->socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
      co_return rpc_error{errc::io_error, ret.first.message()};
    }
    else {
#endif
      if (req_attachment_.empty()) {
        ret = co_await coro_io::async_write(
            socket, asio::buffer(buffer.data(), buffer.size()));
      }
      else {
        std::array<asio::const_buffer, 2> iov{
            asio::const_buffer{buffer.data(), buffer.size()},
            asio::const_buffer{req_attachment_.data(),
                                req_attachment_.size()}};
        ret = co_await coro_io::async_write(socket, iov);
        req_attachment_ = {};
      }
#ifdef UNIT_TEST_INJECT
    }
#endif
#ifdef UNIT_TEST_INJECT
    if (g_action == inject_action::force_inject_client_write_data_timeout) {
      control_->is_timeout_ = true;
    }
#endif
#ifdef UNIT_TEST_INJECT
    if (g_action == inject_action::client_close_socket_after_send_payload) {
      ELOGV(INFO, "client_id %d client_close_socket_after_send_payload",
            config_.client_id);
      close();
      co_return rpc_error{errc::io_error, ret.first.message()};
    }
#endif
    if (ret.first) {
      close();
      if (control_->is_timeout_) {
        co_return rpc_error{errc::timed_out};
      }
      else {
        co_return rpc_error{errc::io_error, ret.first.message()};
      }
    }
    co_return rpc_error{};
  }

 private:
  std::atomic<bool> is_waiting_for_response_=false;
  std::atomic<uint32_t> request_id_{0};
  std::unique_ptr<coro_io::period_timer> timer_;
  std::shared_ptr<control_t> control_;
  std::string_view req_attachment_;
  config config_;
  constexpr static std::size_t default_read_buf_size_ = 256;
#ifdef YLT_ENABLE_SSL
  asio::ssl::context ssl_ctx_{asio::ssl::context::sslv23};
  bool ssl_init_ret_ = true;
#endif
};
}  // namespace coro_rpc

// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ClientSocketPoolManager manages access to all ClientSocketPools.  It's a
// simple container for all of them.  Most importantly, it handles the lifetime
// and destruction order properly.

#ifndef NET_SOCKET_CLIENT_SOCKET_POOL_MANAGER_H_
#define NET_SOCKET_CLIENT_SOCKET_POOL_MANAGER_H_

#include <string>

#include "net/base/completion_once_callback.h"
#include "net/base/net_export.h"
#include "net/base/request_priority.h"
#include "net/http/http_network_session.h"
#include "net/socket/client_socket_pool.h"

namespace base {
class Value;
namespace trace_event {
class ProcessMemoryDump;
}
}

namespace net {

typedef base::Callback<int(const AddressList&, const NetLogWithSource& net_log)>
    OnHostResolutionCallback;

class ClientSocketHandle;
class HostPortPair;
class HttpNetworkSession;
class HttpProxyClientSocketPool;
class HttpRequestHeaders;
class NetLogWithSource;
class ProxyInfo;
class ProxyServer;
class TransportClientSocketPool;

struct SSLConfig;

// This should rather be a simple constant but Windows shared libs doesn't
// really offer much flexiblity in exporting contants.
enum DefaultMaxValues { kDefaultMaxSocketsPerProxyServer = 32 };

class NET_EXPORT_PRIVATE ClientSocketPoolManager {
 public:
  enum SocketGroupType {
    SSL_GROUP,     // For all TLS sockets.
    NORMAL_GROUP,  // For normal HTTP sockets.
    FTP_GROUP      // For FTP sockets (over an HTTP proxy).
  };

  ClientSocketPoolManager();
  virtual ~ClientSocketPoolManager();

  // The setter methods below affect only newly created socket pools after the
  // methods are called. Normally they should be called at program startup
  // before any ClientSocketPoolManagerImpl is created.
  static int max_sockets_per_pool(HttpNetworkSession::SocketPoolType pool_type);
  static void set_max_sockets_per_pool(
      HttpNetworkSession::SocketPoolType pool_type,
      int socket_count);

  static int max_sockets_per_group(
      HttpNetworkSession::SocketPoolType pool_type);
  static void set_max_sockets_per_group(
      HttpNetworkSession::SocketPoolType pool_type,
      int socket_count);

  static int max_sockets_per_proxy_server(
      HttpNetworkSession::SocketPoolType pool_type);
  static void set_max_sockets_per_proxy_server(
      HttpNetworkSession::SocketPoolType pool_type,
      int socket_count);

  static base::TimeDelta unused_idle_socket_timeout(
      HttpNetworkSession::SocketPoolType pool_type);
  static void set_unused_idle_socket_timeout(
      HttpNetworkSession::SocketPoolType pool_type,
      base::TimeDelta timeout);

  virtual void FlushSocketPoolsWithError(int error) = 0;
  virtual void CloseIdleSockets() = 0;
  // Returns the socket pool for direct HTTP and SSL connections.
  virtual TransportClientSocketPool* GetTransportSocketPool() = 0;
  // Returns the socket pool used for both SOCKS and SSL over SOCKS.
  // TODO(https://crbug.com/929714): Merge this with
  // GetSocketPoolForHTTPLikeProxy(), once GetSocketPoolForSSLWithProxy() and
  // GetSocketPoolForHTTPLikeProxy() have been merged.
  virtual TransportClientSocketPool* GetSocketPoolForSOCKSProxy(
      const ProxyServer& socks_proxy) = 0;
  // Returns the HttpProxyClientSocketPool for a ProxyServer that uses an
  // "HTTP-like" scheme, as defined by ProxyServer::is_http_like().
  virtual TransportClientSocketPool* GetSocketPoolForHTTPLikeProxy(
      const ProxyServer& http_proxy) = 0;
  // Creates a Value summary of the state of the socket pools.
  virtual std::unique_ptr<base::Value> SocketPoolInfoToValue() const = 0;

  // Dumps memory allocation stats. |parent_dump_absolute_name| is the name
  // used by the parent MemoryAllocatorDump in the memory dump hierarchy.
  virtual void DumpMemoryStats(
      base::trace_event::ProcessMemoryDump* pmd,
      const std::string& parent_dump_absolute_name) const = 0;
};

// A helper method that uses the passed in proxy information to initialize a
// ClientSocketHandle with the relevant socket pool. Use this method for
// HTTP/HTTPS requests. |ssl_config_for_origin| is only used if the request
// uses SSL and |ssl_config_for_proxy| is used if the proxy server is HTTPS.
// |resolution_callback| will be invoked after the the hostname is
// resolved.  If |resolution_callback| does not return OK, then the
// connection will be aborted with that value.
int InitSocketHandleForHttpRequest(
    ClientSocketPoolManager::SocketGroupType group_type,
    const HostPortPair& endpoint,
    const HttpRequestHeaders& request_extra_headers,
    int request_load_flags,
    RequestPriority request_priority,
    HttpNetworkSession* session,
    const ProxyInfo& proxy_info,
    quic::QuicTransportVersion quic_version,
    const SSLConfig& ssl_config_for_origin,
    const SSLConfig& ssl_config_for_proxy,
    PrivacyMode privacy_mode,
    const SocketTag& socket_tag,
    const NetLogWithSource& net_log,
    ClientSocketHandle* socket_handle,
    const OnHostResolutionCallback& resolution_callback,
    CompletionOnceCallback callback,
    const ClientSocketPool::ProxyAuthCallback& proxy_auth_callback);

// A helper method that uses the passed in proxy information to initialize a
// ClientSocketHandle with the relevant socket pool. Use this method for
// HTTP/HTTPS requests for WebSocket handshake.
// |ssl_config_for_origin| is only used if the request
// uses SSL and |ssl_config_for_proxy| is used if the proxy server is HTTPS.
// |resolution_callback| will be invoked after the the hostname is
// resolved.  If |resolution_callback| does not return OK, then the
// connection will be aborted with that value.
// This function uses WEBSOCKET_SOCKET_POOL socket pools.
int InitSocketHandleForWebSocketRequest(
    ClientSocketPoolManager::SocketGroupType group_type,
    const HostPortPair& endpoint,
    const HttpRequestHeaders& request_extra_headers,
    int request_load_flags,
    RequestPriority request_priority,
    HttpNetworkSession* session,
    const ProxyInfo& proxy_info,
    const SSLConfig& ssl_config_for_origin,
    const SSLConfig& ssl_config_for_proxy,
    PrivacyMode privacy_mode,
    const NetLogWithSource& net_log,
    ClientSocketHandle* socket_handle,
    const OnHostResolutionCallback& resolution_callback,
    CompletionOnceCallback callback,
    const ClientSocketPool::ProxyAuthCallback& proxy_auth_callback);

// Deprecated: Please do not use this outside of //net and //services/network.
// A helper method that uses the passed in proxy information to initialize a
// ClientSocketHandle with the relevant socket pool. Use this method for
// a raw socket connection to a host-port pair (that needs to tunnel through
// the proxies).
NET_EXPORT int InitSocketHandleForRawConnect(
    const HostPortPair& host_port_pair,
    HttpNetworkSession* session,
    int request_load_flags,
    RequestPriority request_priority,
    const ProxyInfo& proxy_info,
    const SSLConfig& ssl_config_for_origin,
    const SSLConfig& ssl_config_for_proxy,
    PrivacyMode privacy_mode,
    const NetLogWithSource& net_log,
    ClientSocketHandle* socket_handle,
    CompletionOnceCallback callback,
    const ClientSocketPool::ProxyAuthCallback& proxy_auth_callback);

// Deprecated: Please do not use this outside of //net and //services/network.
// A helper method that uses the passed in proxy information to initialize a
// ClientSocketHandle with the relevant socket pool. Use this method for
// a raw socket connection with TLS negotiation to a host-port pair (that needs
// to tunnel through the proxies).
NET_EXPORT int InitSocketHandleForTlsConnect(
    const HostPortPair& host_port_pair,
    HttpNetworkSession* session,
    int request_load_flags,
    RequestPriority request_priority,
    const ProxyInfo& proxy_info,
    const SSLConfig& ssl_config_for_origin,
    const SSLConfig& ssl_config_for_proxy,
    PrivacyMode privacy_mode,
    const NetLogWithSource& net_log,
    ClientSocketHandle* socket_handle,
    CompletionOnceCallback callback,
    const ClientSocketPool::ProxyAuthCallback& proxy_auth_callback);

// Similar to InitSocketHandleForHttpRequest except that it initiates the
// desired number of preconnect streams from the relevant socket pool.
int PreconnectSocketsForHttpRequest(
    ClientSocketPoolManager::SocketGroupType group_type,
    const HostPortPair& endpoint,
    const HttpRequestHeaders& request_extra_headers,
    int request_load_flags,
    RequestPriority request_priority,
    HttpNetworkSession* session,
    const ProxyInfo& proxy_info,
    const SSLConfig& ssl_config_for_origin,
    const SSLConfig& ssl_config_for_proxy,
    PrivacyMode privacy_mode,
    const NetLogWithSource& net_log,
    int num_preconnect_streams);

}  // namespace net

#endif  // NET_SOCKET_CLIENT_SOCKET_POOL_MANAGER_H_

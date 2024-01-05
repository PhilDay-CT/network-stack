#include <FreeRTOS.h>
#include <FreeRTOS_IP.h>
#include <FreeRTOS_ND.h>

#include "FreeRTOS_IP_Private.h"
#include "NetAPI.h"
#include "cdefs.h"
#include "cheri.hh"
#include "compartment-macros.h"
#include "firewall.h"
#include "network-internal.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <debug.hh>
#include <limits>
#include <locks.hh>
#include <platform-ethernet.hh>
#include <token.h>

using Debug            = ConditionalDebug<true, "Network stack wrapper">;
constexpr bool UseIPv6 = CHERIOT_RTOS_OPTION_IPv6;

using CHERI::Capability;
using CHERI::check_pointer;
using CHERI::Permission;
using CHERI::PermissionSet;

namespace
{
	/**
	 * The sealed wrapper around a FreeRTOS socket.
	 */
	struct SealedSocket
	{
		/**
		 * The network address that this socket is connected to.  This is used
		 * to close the firewall hole when the socket is closed.
		 */
		NetworkAddress address;
		/**
		 * The lock protecting this socket.
		 */
		FlagLockPriorityInherited socketLock;
		/**
		 * The FreeRTOS socket.  It would be nice if this didn't require a
		 * separate allocation but FreeRTOS+TCP isn't designed to support that
		 * use case.
		 */
		FreeRTOS_Socket_t *socket;
	};

	/**
	 * Returns the key with which SealedSocket instances are sealed.
	 */
	__always_inline SKey socket_key()
	{
		return STATIC_SEALING_TYPE(Socket);
	}

	/**
	 * Unseal `sealedSocket` and, if it is sealed with the correct type, pass
	 * it to `operation`.  If the unsealing fails, return `-EINVAL`.  This is a
	 * helper function used for operations on sockets.
	 */
	int with_sealed_socket(auto operation, Sealed<SealedSocket> sealedSocket)
	{
		auto *socket = token_unseal(socket_key(), sealedSocket);
		if (socket == nullptr)
		{
			Debug::log("Failed to unseal socket");
			return -EINVAL;
		}
		return operation(socket);
	}

	/**
	 * Wrapper around `with_sealed_socket` that locks the socket before calling
	 * the operation.  This is used by everything except the close operation
	 * (which must not try to release the lock after the lock has been
	 * deallocated).
	 */
	int with_sealed_socket(Timeout             *timeout,
	                       auto                 operation,
	                       Sealed<SealedSocket> sealedSocket)
	{
		with_sealed_socket(
		  [&](SealedSocket *socket) {
			  if (LockGuard g{socket->socketLock, timeout})
			  {
				  return operation(socket);
			  }
			  return -ETIMEDOUT;
		  },
		  sealedSocket);
	}

	/**
	 * RAII helper for claiming heap objects.  This can be used with heap and
	 * non-heap objects and will maintain a claim on the object until it goes
	 * out of scope.  If the object is not heap-allocated then this silently
	 * pretends to have added a claim (non-heap objects cannot be deallocated).
	 */
	template<typename T>
	class Claim
	{
		/**
		 * True if the object can be guaranteed not to be deallocated for the
		 * lifetime of this object.
		 */
		bool isValid;
		/**
		 * True if the object is heap-allocated.
		 */
		bool isHeapObject;
		/**
		 * The object that we are claiming.
		 */
		T *value;
		/**
		 * The capability that we are using to claim the object (and to
		 * subsequently drop the claim).
		 */
		SObj mallocCapability;

		public:
		/**
		 * Create a claim on `value` using `mallocCapability`.
		 */
		__always_inline Claim(SObj mallocCapability, T *value)
		  : value{value}, mallocCapability{mallocCapability}
		{
			if (!heap_address_is_valid(value))
			{
				isValid      = true;
				isHeapObject = false;
			}
			else
			{
				isValid      = heap_claim(mallocCapability, value) > 0;
				isHeapObject = true;
			}
		}

		/**
		 * Drop the claim on the object.
		 */
		__always_inline ~Claim()
		{
			if (isValid && isHeapObject)
			{
				heap_free(mallocCapability, value);
			}
		}

		/**
		 * Drop ownership of the claim and return the claimed object.  The
		 * caller is responsible for releasing the claim.
		 */
		__always_inline T *release()
		{
			isValid = false;
			return value;
		}

		/**
		 * Implicit conversion to bool.  True if the claim is valid.
		 */
		__always_inline operator bool() const
		{
			return isValid;
		}
	};

} // namespace

namespace
{
	NetworkAddress host_resolve(const char *hostname, bool useIPv6 = UseIPv6)
	{
		// This structure is *huge* (>300 bytes!) don't allocate it on the
		// stack. The FreeRTOS code uses only one field of this structure, which
		// is quite depressing.  We might be able to allocate something smaller
		// that has the same initial layout.
		static struct freertos_addrinfo hints;
		hints.ai_family = useIPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET;
		struct freertos_addrinfo *results = nullptr;
		Debug::log("Trying DNS request (hints is {} bytes!)", sizeof(hints));
		firewall_permit_dns();
		auto ret = FreeRTOS_getaddrinfo(hostname, nullptr, &hints, &results);
		firewall_permit_dns(false);
		if (ret != 0)
		{
			Debug::log("DNS request returned: {}", ret);
			if (useIPv6)
			{
				return host_resolve(hostname, false);
			}
			return {0, NetworkAddress::AddressKindInvalid};
		}

		NetworkAddress address;
		bool           isIPv6 = false;
		for (freertos_addrinfo *r = results; r != nullptr; r = r->ai_next)
		{
			Debug::log("Canonical name: {}", r->ai_canonname);
			if (r->ai_family == FREERTOS_AF_INET6)
			{
				memcpy(
				  address.ipv6, r->ai_addr->sin_address.xIP_IPv6.ucBytes, 16);
				address.kind = NetworkAddress::AddressKindIPv6;
				Debug::log("Got IPv6 address");
			}
			else
			{
				address.ipv4 = r->ai_addr->sin_address.ulIP_IPv4;
				address.kind = NetworkAddress::AddressKindIPv4;
				Debug::log("Got IPv4 address");
				Debug::log("Got address: {}.{}.{}.{}",
				           (int)r->ai_addr->sin_address.ulIP_IPv4 & 0xff,
				           (int)r->ai_addr->sin_address.ulIP_IPv4 >> 8 & 0xff,
				           (int)r->ai_addr->sin_address.ulIP_IPv4 >> 16 & 0xff,
				           (int)r->ai_addr->sin_address.ulIP_IPv4 >> 24 & 0xff);
			}
		}
		return address;
	}

} // namespace

NetworkAddress network_host_resolve(const char *hostname, bool useIPv6)
{
	return host_resolve(hostname, useIPv6);
}

SObj network_socket_create_and_connect(Timeout       *timeout,
                                       SObj           mallocCapability,
                                       NetworkAddress address,
                                       ConnectionType            type,
                                       short          port)
{
	bool     isIPv6 = address.kind == NetworkAddress::AddressKindIPv6;
	Socket_t socket =
	  FreeRTOS_socket(isIPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET,
	                  type == ConnectionTypeTCP ? FREERTOS_SOCK_STREAM  :
	                                              FREERTOS_SOCK_DGRAM,
	                  type == ConnectionTypeTCP ? FREERTOS_IPPROTO_TCP:
	                                              FREERTOS_IPPROTO_UDP);
	if (socket == nullptr)
	{
		Debug::log("Failed to create socket");
		return nullptr;
	}
	// Claim the socket so that it counts towards the caller's quota.  The
	// network stack also keeps a claim to it.  We will drop this claim on
	// deallocation.
	Claim c{mallocCapability, socket};
	if (!c)
	{
		Debug::log("Failed to claim socket");
		FreeRTOS_closesocket(socket);
		return nullptr;
	}
	// TODO: This should have nice RAII wrappers!
	auto [socketWrapper, sealedSocket] =
	  token_allocate<SealedSocket>(timeout, mallocCapability, socket_key());
	if (socketWrapper == nullptr)
	{
		Debug::log("Failed to allocate socket wrapper");
		FreeRTOS_closesocket(socket);
		return nullptr;
	}
	socketWrapper->address = address;
	socketWrapper->socket  = socket;
	struct freertos_sockaddr server;
	memset(&server, 0, sizeof(server));
	server.sin_len  = sizeof(server);
	server.sin_port = FreeRTOS_htons(port);
	if (isIPv6)
	{
		server.sin_family = FREERTOS_AF_INET6;
		memcpy(server.sin_address.xIP_IPv6.ucBytes, address.ipv6, 16);
	}
	else
	{
		server.sin_family            = FREERTOS_AF_INET;
		server.sin_address.ulIP_IPv4 = address.ipv4;
	}
	Debug::log("Trying to connect to server");
	if (int ret = FreeRTOS_connect(socket, &server, sizeof(server)); ret != 0)
	{
		// TODO: Retry until the timeout expires.
		Debug::log("Failed to connect to server.  Error: {}.", ret);
		token_obj_destroy(mallocCapability, socket_key(), sealedSocket);
		// FIXME: Remove the firewall rule if we didn't manage to connect.
		return nullptr;
	}
	Debug::log("Successfully connected to server");
	c.release();
	return sealedSocket;
}

int network_socket_close(Timeout *t, SObj mallocCapability, SObj sealedSocket)
{
	return with_sealed_socket(
	  [=](SealedSocket *socket) {
		  // Don't use a lock guard here, we don't want to release the lock if
		  // it's been deallocated.  This must be released on all return paths
		  // except the one for success.
		  if (!socket->socketLock.try_lock(t))
		  {
			  return -ETIMEDOUT;
		  }
		  // Drop the caller's claim on the socket.
		  if (heap_free(mallocCapability, socket->socket) != 0)
		  {
			  Debug::log("Failed to free socket");
			  // Release the lock so that we don't leak it.
			  socket->socketLock.unlock();
			  return -EINVAL;
		  }
		  bool isTCP = socket->socket->ucProtocol == FREERTOS_IPPROTO_TCP;
		  // Shut down the socket before closing the firewall.
		  FreeRTOS_shutdown(socket->socket, FREERTOS_SHUT_RDWR);
		  if (socket->address.kind == NetworkAddress::AddressKindIPv4)
		  {
			  if (isTCP)
			  {
				  firewall_remove_tcpipv4_endpoint(socket->address.ipv4);
			  }
			  else
			  {
				  firewall_remove_udpipv4_endpoint(socket->address.ipv4);
			  }
		  }
		  // Close the socket.  Another thread will actually clean up the
		  // memory.
		  FreeRTOS_closesocket(socket->socket);
		  token_obj_destroy(mallocCapability, socket_key(), sealedSocket);
		  return 0;
	  },
	  sealedSocket);
}

NetworkReceiveResult
network_socket_receive(Timeout *timeout, SObj mallocCapability, SObj socket)
{
	uint8_t *buffer = nullptr;
	ssize_t  result = with_sealed_socket(
	   [&](SealedSocket *socket) {
          do
          {
              // TODO: It would be nice to use FREERTOS_ZERO_COPY here, but
              // unfortunately that copies into a ring buffer and returns a
              // pointer into the ring buffer.  If the FreeRTOS network stack is
              // ever extended to store lists of incoming packets then we could
              // use that.
              //
              // Read how may bytes are available.
              int available =
                FreeRTOS_recv(socket->socket,
			                   nullptr,
			                   std::numeric_limits<size_t>::max(),
			                   FREERTOS_MSG_PEEK | FREERTOS_MSG_DONTWAIT);
              if (available > 0)
              {
                  do
                  {
                      buffer = static_cast<uint8_t *>(
                        heap_allocate(timeout, mallocCapability, available));
                      if (buffer == nullptr)
                      {
                          // If allocation failed and the timeout is zero, give
                          // up now.
                          if (!timeout->may_block())
                          {
                              return -ETIMEDOUT;
                          }
                          // If there's time left, let's try allocating a
                          // smaller buffer.
                          auto quota = heap_quota_remaining(mallocCapability);
                          // Subtract 16 bytes to account for the allocation
                          // header.
                          if ((quota < available) && (quota > 16))
                          {
                              available = quota - 16;
                              continue;
                          }
                          return -ENOMEM;
                      }
                  } while (buffer == nullptr);
                  // Now do the real receive.
                  int received = FreeRTOS_recv(
				     socket->socket, buffer, available, FREERTOS_MSG_DONTWAIT);
                  return received;
              }
              if (available < 0)
              {
                  if (available == -pdFREERTOS_ERRNO_ENOTCONN)
                  {
                      Debug::log("Connection closed, not receiving");
                      return -ENOTCONN;
                  }
                  // Something went wrong?
                  Debug::log("Receive failed with unexpected error: {}",
				              available);
                  return -EINVAL;
              }
              if (!timeout->may_block())
              {
                  return -ETIMEDOUT;
              }
              // It's annoying that we end up querying the time twice here, but
              // FreeRTOS's timeout API is not designed for composition.
              auto       startTick = thread_systemtick_get();
              TickType_t remaining = timeout->remaining;
              FreeRTOS_setsockopt(
			     socket->socket, 0, FREERTOS_SO_RCVTIMEO, &remaining, 0);
              // Wait for at least one byte to be available.
              auto ret =
                FreeRTOS_recv(socket->socket, nullptr, 1, FREERTOS_MSG_PEEK);
              Debug::log("Blocking call returned {}", ret);
              auto endTick = thread_systemtick_get();
              timeout->elapse((((uint64_t)endTick.hi << 32) | endTick.lo) -
			                   (((uint64_t)startTick.hi << 32) | startTick.lo));
          } while (timeout->may_block());
          return -ETIMEDOUT;
	   },
	   socket);
	return {result, buffer};
}

ssize_t
network_socket_send(Timeout *timeout, SObj socket, void *buffer, size_t length)
{
	return with_sealed_socket(
	  [&](SealedSocket *socket) {
		  // Ensure that the buffer is valid for the duration of the send.
		  Claim claim{MALLOC_CAPABILITY, buffer};
		  if (!claim)
		  {
			  Debug::log("Failed to claim buffer");
			  return -ENOMEM;
		  }
		  // At this point, we know that the buffer can't go away from under us,
		  // so it's safe to do the checks.
		  if (!CHERI::check_pointer<PermissionSet{Permission::Load}>(buffer,
		                                                             length))
		  {
			  return -EPERM;
		  }
		  // FIXME: This should use the socket options to set / update
		  // the timeout.
		  auto ret = FreeRTOS_send(socket->socket, buffer, length, 0);
		  if (ret >= 0)
		  {
			  return ret;
		  }
		  if (ret == -pdFREERTOS_ERRNO_ENOTCONN)
		  {
			  return -ENOTCONN;
		  }
		  // Catchall
		  Debug::log("Send failed with unexpected error: {}", ret);
		  return -EINVAL;
	  },
	  socket);
}
// Lightweight header exposing mDNS socket helpers
#pragma once

#include <cstddef>
#include <cstdint>
// Provide POSIX ssize_t on platforms that declare it in <sys/types.h>
#include <sys/types.h>

#include <string>
#include <set>

// Configuration passed to the mDNS socket task.
// Contains the socket descriptor and a pointer to a set for discovered IPv4
// addresses. The task will insert discovered IPv4 addresses into the set
// pointed to by `set_ip`. This allows the caller to provide a shared
// set (for example a global) that the task updates.
struct TaskConfiguration {
	int sock_mdns;
	std::set<std::string>* found_elgato_devices_ips; // pointer to caller-owned set

	// Optional filter: only insert A records whose DNS name matches this
	// qname. If empty, all A records are accepted. The value should be a
	// fully-qualified name or a service name like "_elg._tcp.local".
	std::string qname;
	
	// For responder functionality
	std::string mdns_hostname; // Our hostname to respond to queries for
	std::string our_ip;       // Our IP address to respond with
};

// Returns a bound UDP socket file descriptor joined to the mDNS multicast
// group on all interfaces, or -1 on error.
int mdns_setup_socket();

// Send a PTR query for the provided `qname` using the provided socket.
// If `qname` is empty the default `_elg._tcp.local` is used.
// Returns number of bytes sent or -1 on error.
ssize_t send_mdns_ptr_query(const int &sock_mdns, const std::string &qname);

// Broadcast an mDNS service announcement (unsolicited response).
// This advertises your service on the network using the same mDNS socket.
// service_type: e.g., "_http._tcp.local"
// instance_name: e.g., "My ESP32 Device"
// hostname: e.g., "esp32.local"
// ipv4_addr: your device's IP address as string (e.g., "192.168.1.100")
// port: service port number
// txt_records: optional vector of "key=value" strings for TXT record metadata
// Returns number of bytes sent or -1 on error.
ssize_t send_mdns_announcement(const int &sock_mdns, const std::string &service_type,
                                const std::string &instance_name, const std::string &hostname,
                                const std::string &ipv4_addr, uint16_t port,
                                const std::vector<std::string> &txt_records);

// Broadcast a simple mDNS A record announcement (hostname -> IP).
// This is useful for basic hostname resolution without service discovery.
// hostname: e.g., "esp32.local"
// ipv4_addr: your device's IP address as string (e.g., "192.168.1.100")
// Returns number of bytes sent or -1 on error.
ssize_t send_mdns_a_record(const int &sock_mdns, const std::string &hostname, const std::string &ipv4_addr);

// Unified mDNS socket task that handles both service discovery and query responses.
// This ensures a single thread processes all mDNS socket traffic without conflicts.
// sock_mdns: the mDNS socket
// qname: service type to discover (e.g., "_elg._tcp.local")
// set_ip: set to store discovered IP addresses
// our_hostname: our hostname to respond to queries for (e.g., "esp32-elights.local")
// our_ip: our IP address to respond with
void mdns_socket_task(const int &sock_mdns, const std::string &qname, std::set<std::string> &set_ip,
                      const std::string &our_hostname, const std::string &our_ip);
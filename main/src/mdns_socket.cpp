#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <vector>
#include <string>
#include <set>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "mdns_socket.h"

static const char* MDNS_MULTICAST_IP = "224.0.0.251";
static const int MDNS_PORT = 5353;
static const char* TAG = "mdns_socket";

int mdns_setup_socket()
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }   

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (sock < 0) {
        ESP_LOGE(TAG, "mDNS socket setup failed");
    }

    return sock;
}

ssize_t send_mdns_ptr_query(const int &sock_mdns, const std::string &qname)
{
    if (qname.empty() || sock_mdns < 0) {
        return -1;
    }
    std::vector<uint8_t> query;
    query.resize(12); // DNS header
    // ID = 0 for mDNS
    query[0] = 0; query[1] = 0;
    // Flags = 0 (standard query)
    query[2] = 0; query[3] = 0;
    // QDCOUNT = 1
    query[4] = 0; query[5] = 1;
    // ANCOUNT, NSCOUNT, ARCOUNT = 0
    query[6] = 0; query[7] = 0; query[8] = 0; query[9] = 0; query[10] = 0; query[11] = 0;

    auto push_label = [&](const std::string &s) {
        uint8_t len = (uint8_t)s.size();
        query.push_back(len);
        query.insert(query.end(), s.begin(), s.end());
    };

    // Split the qname on '.' and push each label
    size_t start = 0;
    while (start < qname.size()) {
        size_t dot = qname.find('.', start);
        std::string label;
        if (dot == std::string::npos) {
            label = qname.substr(start);
            start = qname.size();
        } else {
            label = qname.substr(start, dot - start);
            start = dot + 1;
        }
        if (!label.empty()) push_label(label);
    }
    query.push_back(0); // term
    // QTYPE = PTR(12)
    query.push_back(0); query.push_back(12);
    // QCLASS = IN (1)
    query.push_back(0); query.push_back(1);

    struct sockaddr_in mcast_addr;
    bzero(&mcast_addr, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MULTICAST_IP, &mcast_addr.sin_addr);

    ssize_t sent = sendto(sock_mdns, query.data(), query.size(), 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    return sent;
}

// Broadcast an mDNS service announcement (unsolicited response)
// service_type: e.g. "_http._tcp.local"
// instance_name: e.g. "My Device"
// hostname: e.g. "mydevice.local"
// ipv4_addr: your device's IP address as a string
// port: service port number
// txt_records: optional key=value pairs for TXT record (can be empty)
ssize_t send_mdns_announcement(const int &sock_mdns, const std::string &service_type,
                                const std::string &instance_name, const std::string &hostname,
                                const std::string &ipv4_addr, uint16_t port,
                                const std::vector<std::string> &txt_records)
{
    if (sock_mdns < 0 || service_type.empty() || instance_name.empty() || 
        hostname.empty() || ipv4_addr.empty()) {
        return -1;
    }

    std::vector<uint8_t> response;
    response.resize(12); // DNS header

    // ID = 0 for mDNS
    response[0] = 0; response[1] = 0;
    // Flags = 0x8400 (response, authoritative)
    response[2] = 0x84; response[3] = 0x00;
    // QDCOUNT = 0
    response[4] = 0; response[5] = 0;
    // ANCOUNT = 3 (PTR, SRV, TXT)
    response[6] = 0; response[7] = 3;
    // NSCOUNT = 0, ARCOUNT = 1 (A record)
    response[8] = 0; response[9] = 0; response[10] = 0; response[11] = 1;

    auto push_label = [&](const std::string &s) {
        uint8_t len = (uint8_t)s.size();
        response.push_back(len);
        response.insert(response.end(), s.begin(), s.end());
    };

    auto push_name = [&](const std::string &name) {
        size_t start = 0;
        while (start < name.size()) {
            size_t dot = name.find('.', start);
            std::string label;
            if (dot == std::string::npos) {
                label = name.substr(start);
                start = name.size();
            } else {
                label = name.substr(start, dot - start);
                start = dot + 1;
            }
            if (!label.empty()) push_label(label);
        }
        response.push_back(0); // terminator
    };

    auto push_u16 = [&](uint16_t val) {
        response.push_back((val >> 8) & 0xFF);
        response.push_back(val & 0xFF);
    };

    auto push_u32 = [&](uint32_t val) {
        response.push_back((val >> 24) & 0xFF);
        response.push_back((val >> 16) & 0xFF);
        response.push_back((val >> 8) & 0xFF);
        response.push_back(val & 0xFF);
    };

    std::string full_instance = instance_name + "." + service_type;

    // 1. PTR Record: service_type -> full_instance
    push_name(service_type);
    push_u16(12);  // TYPE = PTR
    push_u16(0x0001);  // CLASS = IN (no cache-flush for PTR)
    push_u32(4500);  // TTL = 4500 seconds
    size_t ptr_rdlen_pos = response.size();
    push_u16(0);  // RDLENGTH placeholder
    size_t ptr_rdata_start = response.size();
    push_name(full_instance);
    uint16_t ptr_rdlen = response.size() - ptr_rdata_start;
    response[ptr_rdlen_pos] = (ptr_rdlen >> 8) & 0xFF;
    response[ptr_rdlen_pos + 1] = ptr_rdlen & 0xFF;

    // 2. SRV Record: full_instance -> hostname:port
    push_name(full_instance);
    push_u16(33);  // TYPE = SRV
    push_u16(0x8001);  // CLASS = IN with cache-flush bit
    push_u32(120);  // TTL
    size_t srv_rdlen_pos = response.size();
    push_u16(0);  // RDLENGTH placeholder
    size_t srv_rdata_start = response.size();
    push_u16(0);  // Priority
    push_u16(0);  // Weight
    push_u16(port);  // Port
    push_name(hostname);
    uint16_t srv_rdlen = response.size() - srv_rdata_start;
    response[srv_rdlen_pos] = (srv_rdlen >> 8) & 0xFF;
    response[srv_rdlen_pos + 1] = srv_rdlen & 0xFF;

    // 3. TXT Record: full_instance -> txt data
    push_name(full_instance);
    push_u16(16);  // TYPE = TXT
    push_u16(0x8001);  // CLASS = IN with cache-flush bit
    push_u32(4500);  // TTL
    size_t txt_rdlen_pos = response.size();
    push_u16(0);  // RDLENGTH placeholder
    size_t txt_rdata_start = response.size();
    if (txt_records.empty()) {
        response.push_back(0);  // Empty TXT record
    } else {
        for (const auto &txt : txt_records) {
            uint8_t txt_len = (uint8_t)txt.size();
            response.push_back(txt_len);
            response.insert(response.end(), txt.begin(), txt.end());
        }
    }
    uint16_t txt_rdlen = response.size() - txt_rdata_start;
    response[txt_rdlen_pos] = (txt_rdlen >> 8) & 0xFF;
    response[txt_rdlen_pos + 1] = txt_rdlen & 0xFF;

    // 4. A Record: hostname -> IPv4 address (in Additional Records section)
    push_name(hostname);
    push_u16(1);  // TYPE = A
    push_u16(0x8001);  // CLASS = IN with cache-flush bit
    push_u32(120);  // TTL
    push_u16(4);  // RDLENGTH = 4 bytes for IPv4
    struct in_addr addr;
    if (inet_pton(AF_INET, ipv4_addr.c_str(), &addr) == 1) {
        response.push_back((addr.s_addr >> 0) & 0xFF);
        response.push_back((addr.s_addr >> 8) & 0xFF);
        response.push_back((addr.s_addr >> 16) & 0xFF);
        response.push_back((addr.s_addr >> 24) & 0xFF);
    } else {
        return -1;  // Invalid IP address
    }

    struct sockaddr_in mcast_addr;
    bzero(&mcast_addr, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MULTICAST_IP, &mcast_addr.sin_addr);

    ssize_t sent = sendto(sock_mdns, response.data(), response.size(), 0, 
                          (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    return sent;
}

// Broadcast a simple mDNS A record announcement (hostname -> IP)
ssize_t send_mdns_a_record(const int &sock_mdns, const std::string &hostname, const std::string &ipv4_addr)
{
    if (sock_mdns < 0 || hostname.empty() || ipv4_addr.empty()) {
        return -1;
    }

    std::vector<uint8_t> response;
    response.resize(12); // DNS header

    // ID = 0 for mDNS
    response[0] = 0; response[1] = 0;
    // Flags = 0x8400 (response, authoritative)
    response[2] = 0x84; response[3] = 0x00;
    // QDCOUNT = 0
    response[4] = 0; response[5] = 0;
    // ANCOUNT = 1 (just A record)
    response[6] = 0; response[7] = 1;
    // NSCOUNT = 0, ARCOUNT = 0
    response[8] = 0; response[9] = 0; response[10] = 0; response[11] = 0;

    auto push_label = [&](const std::string &s) {
        uint8_t len = (uint8_t)s.size();
        response.push_back(len);
        response.insert(response.end(), s.begin(), s.end());
    };

    auto push_name = [&](const std::string &name) {
        size_t start = 0;
        while (start < name.size()) {
            size_t dot = name.find('.', start);
            std::string label;
            if (dot == std::string::npos) {
                label = name.substr(start);
                start = name.size();
            } else {
                label = name.substr(start, dot - start);
                start = dot + 1;
            }
            if (!label.empty()) push_label(label);
        }
        response.push_back(0); // terminator
    };

    auto push_u16 = [&](uint16_t val) {
        response.push_back((val >> 8) & 0xFF);
        response.push_back(val & 0xFF);
    };

    auto push_u32 = [&](uint32_t val) {
        response.push_back((val >> 24) & 0xFF);
        response.push_back((val >> 16) & 0xFF);
        response.push_back((val >> 8) & 0xFF);
        response.push_back(val & 0xFF);
    };

    // A Record: hostname -> IPv4 address
    push_name(hostname);
    push_u16(1);  // TYPE = A
    push_u16(0x8001);  // CLASS = IN with cache-flush bit
    push_u32(120);  // TTL = 120 seconds
    push_u16(4);  // RDLENGTH = 4 bytes for IPv4

    struct in_addr addr;
    if (inet_pton(AF_INET, ipv4_addr.c_str(), &addr) == 1) {
        response.push_back((addr.s_addr >> 0) & 0xFF);
        response.push_back((addr.s_addr >> 8) & 0xFF);
        response.push_back((addr.s_addr >> 16) & 0xFF);
        response.push_back((addr.s_addr >> 24) & 0xFF);
    } else {
        return -1;  // Invalid IP address
    }

    struct sockaddr_in mcast_addr;
    bzero(&mcast_addr, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MULTICAST_IP, &mcast_addr.sin_addr);

    ssize_t sent = sendto(sock_mdns, response.data(), response.size(), 0, 
                          (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    return sent;
}


// Helper to read big-endian 16/32-bit values from buffer
static uint16_t read_u16(const uint8_t* buf) {
    return (uint16_t)buf[0] << 8 | buf[1];
}

static uint32_t read_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
}

// Parse a domain name from an mDNS/DNS message handling compression pointers.
// msg_start: pointer to beginning of message; msg_len total length; offset is
// advanced past the name (except when a pointer is encountered, pointer
// bytes are consumed). Returns a std::string name (without trailing dot).
static std::string parse_name(const uint8_t* msg_start, size_t msg_len, size_t &offset) {
    std::string name;
    if (offset >= msg_len) return name;

    size_t i = offset;
    bool jumped = false;
    size_t jumps = 0;

    while (i < msg_len && jumps < msg_len) {
        uint8_t len = msg_start[i];
        // pointer?
        if ((len & 0xC0) == 0xC0) {
            if (i + 1 >= msg_len) return name;
            uint16_t pointer = ((len & 0x3F) << 8) | msg_start[i + 1];
            if (pointer >= msg_len) return name;
            // Advance the top-level offset only once for the pointer
            if (!jumped) {
                offset = i + 2;
            }
            i = pointer;
            jumped = true;
            jumps++;
            continue;
        }

        if (len == 0) {
            // end of name
            if (!jumped) offset = i + 1;
            break;
        }

        // label
        if (i + 1 + len > msg_len) return name;
        if (!name.empty()) name += ".";
        name.append((const char*)(msg_start + i + 1), len);
        if (!jumped) offset = i + 1 + len;
        i += 1 + len;
    }

    return name;
}

// Normalize DNS/mDNS names for matching keys: lowercase and remove a
// trailing dot if present. This avoids mismatches between records that use
// different case or include/exclude the final dot.
static std::string normalize_dns_name(const std::string &s) {
    std::string out = s;
    if (!out.empty() && out.back() == '.') out.pop_back();
    for (auto &c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Unified mDNS socket task that handles both:
// 1. Listening for service discovery responses (PTR/SRV/A records)
// 2. Responding to mDNS queries for our hostname
// This ensures a single thread processes all mDNS socket traffic without conflicts.
void mdns_socket_task(const int &sock_mdns, const std::string &qname, std::set<std::string> &set_ip, 
                      const std::string &our_hostname, const std::string &our_ip) {
    std::string normalized_qname = normalize_dns_name(qname);
    std::string normalized_hostname = normalize_dns_name(our_hostname);

    // buffer for incoming packets
    const size_t BUF_SZ = 1500;
    uint8_t buf[BUF_SZ];

    // receive ONE packet (caller will call us in a loop)
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    ssize_t len = recvfrom(sock_mdns, buf, BUF_SZ, 0, (struct sockaddr*)&src, &slen);
    if (len <= 0) {
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom() error: %s", strerror(errno));
        }
        return; // Timeout or error, return to caller
    }

    if (len < 12) return; // Too short to be valid DNS

    // Parse DNS header
    uint16_t flags = read_u16(buf + 2);
    uint16_t qdcount = read_u16(buf + 4);
    uint16_t ancount = read_u16(buf + 6);
    uint16_t nscount = read_u16(buf + 8);
    uint16_t arcount = read_u16(buf + 10);

    bool is_query = !(flags & 0x8000); // QR bit = 0 means query
    size_t offset = 12;

    // Process questions (for queries and responses)
    if (is_query && qdcount > 0) {
        // This is a query - check if someone is asking for our hostname
        for (int q = 0; q < qdcount; ++q) {
            if (offset >= (size_t)len) break;

            std::string qname_parsed = parse_name(buf, len, offset);
            if (offset + 4 > (size_t)len) break;

            uint16_t qtype = read_u16(buf + offset); offset += 2;
            uint16_t qclass = read_u16(buf + offset); offset += 2;

            // Check if this is an A record query for our hostname
            if ((qtype == 1 || qtype == 255) && // A record or ANY
                (qclass == 1 || qclass == 255) && // IN class or ANY
                normalize_dns_name(qname_parsed) == normalized_hostname) {

                ESP_LOGI(TAG, "Received mDNS A query for %s, responding with %s", qname_parsed.c_str(), our_ip.c_str());

                // Send A record response
                send_mdns_a_record(sock_mdns, our_hostname, our_ip);
                break; // Done processing this packet
            }
        }
    } else if (!is_query) {
        // This is a response - process answers for service discovery
        // Skip questions first
        for (int q = 0; q < qdcount; ++q) {
            std::string qname_parsed = parse_name(buf, len, offset);
            if (offset + 4 > (size_t)len) { offset = len; break; }
            offset += 4; // qtype + qclass
        }

        bool found_matching_qname = false;
        // Iterate answers + authorities + additionals
        int rr_total = ancount + nscount + arcount;
        for (int rr = 0; rr < rr_total; ++rr) {
            if (offset >= (size_t)len) break;
            size_t name_off = offset;
            (void)name_off;
            std::string name = parse_name(buf, len, offset);
            if (offset + 10 > (size_t)len) break;
            uint16_t type = read_u16(buf + offset); offset += 2;
            uint16_t clas = read_u16(buf + offset); offset += 2;
            uint32_t ttl = read_u32(buf + offset); offset += 4;
            (void)ttl;
            uint16_t rdlen = read_u16(buf + offset); offset += 2;

            found_matching_qname |= (normalize_dns_name(name) == normalized_qname);
            if (offset + rdlen > (size_t)len) break;

            // We are only interested in the A record, the IPv4 of the device on the network
            if ((clas == 1 || clas == 32769) && type == 1) { // A
                if (rdlen == 4) {
                    char ipstr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, buf + offset, ipstr, sizeof(ipstr));

                    if (found_matching_qname) {
                        set_ip.insert(std::string(ipstr));
                    }
                }
            }

            offset += rdlen;
        }
    }
}
#include "clsDNSLookup.h"
#include "clsLog.h"
#include "clsEpollReactor.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <strings.h>
#include <sys/random.h>

DNSLookup::DNSLookup(EpollReactor* reactor, size_t cache_ttl_sec, size_t cache_max_size) :
    m_pReactor(reactor),
    m_cache_max_size(cache_max_size),
    m_cache_ttl_sec(cache_ttl_sec) {
    setTimeout(3);
    setMaxRetries(1);
    init_request_pool(1000);
    load_dns_servers();
    load_hosts_file();

    if (m_dns_servers.empty()) {
        m_dns_servers = {"8.8.8.8", "8.8.4.4"};
    }

    for (const std::string& s : m_dns_servers) {
        struct in_addr a{};
        inet_pton(AF_INET, s.c_str(), &a);
        m_dns_server_addrs.push_back(a);
    }

    // query ID tasadofi (dar barabare javab haye jaali)
    if (getrandom(&m_rngState, sizeof(m_rngState), GRND_NONBLOCK) != (ssize_t)sizeof(m_rngState) || m_rngState == 0) {
        m_rngState = now_ms() ^ (uint64_t)(uintptr_t)this ^ 0x9E3779B97F4A7C15ULL;
    }

    reset_socket();
}

DNSLookup::~DNSLookup() {
    m_shuttingDown = true;
    close();

    // darkhast haye montazer ba natije khali javab dade mishan
    std::vector<DNSRequest*> pending;
    pending.reserve(m_pending.size());
    for (auto& p : m_pending)
        pending.push_back(p.second);
    m_pending.clear();

    for (DNSRequest* req : pending) {
        if (req->cb)
            invoke_callback(req->cb, req->hostname.c_str(), {}, req->qtype, req->user_data);
    }
}

uint64_t DNSLookup::now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int DNSLookup::fd() const {
    return m_SocketContext.fd;
}

void DNSLookup::close() {
    if (m_SocketContext.fd != -1) {
        m_pReactor->del_fd(m_SocketContext.fd, true);
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }
}

void DNSLookup::reset_socket() {
    close();

    m_SocketContext.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_SocketContext.fd == -1) {
#ifdef DEBUG
        NR_PERROR("DNS socket creation failed");
#endif
        return;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    if (bind(m_SocketContext.fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
#ifdef DEBUG
        NR_PERROR("DNS bind failed");
#endif
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
        return;
    }

    m_SocketContext.ev.events = EPOLLIN;
    bool ret = m_pReactor->register_fd(fd(), &m_SocketContext.ev, IS_DNS_LOOKUP_SOCKET, this);
    if (!ret) {
#ifdef DEBUG
        NR_PERROR("Add DNS fd to epoll failed");
#endif
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
        return;
    }

#ifdef DEBUG
    NR_LOG(netrector::LOG_INFO, "DNSLookup initialized with fd %d", fd());
#endif
}

void DNSLookup::setTimeout(uint16_t newTimeout) {
    m_Timeout = newTimeout;
}

void DNSLookup::setCache_ttl_sec(size_t newCache_ttl_sec) {
    m_cache_ttl_sec = newCache_ttl_sec;
}

void DNSLookup::setMaxRetries(uint16_t newMaxRetries) {
    m_max_retries = newMaxRetries;
}

void DNSLookup::init_request_pool(size_t pool_size) {
    m_request_pool.resize(pool_size);
    m_free_requests.reserve(pool_size);
    for (auto& req : m_request_pool) {
        m_free_requests.push_back(&req);
    }
}

DNSLookup::DNSRequest* DNSLookup::acquire_request() {
    if (m_free_requests.empty()) {
        return nullptr;
    }
    DNSRequest* req = m_free_requests.back();
    m_free_requests.pop_back();
    return req;
}

void DNSLookup::release_request(DNSRequest* req) {
    req->hostname.clear();
    req->retry_count = 0;
    req->server_idx = 0;
    req->sent_ms = 0;
    req->cb = nullptr;
    req->user_data = nullptr;
    m_free_requests.push_back(req);
}

void DNSLookup::invoke_callback(callback_t cb, const char *hostname, const std::vector<std::string> &ips_vec, QUERY_TYPE qtype, void *user_data)
{
    size_t count = ips_vec.size();
    char** ips = nullptr;
    if (count > 0) {
        ips = new char*[count];
        for (size_t i = 0; i < count; ++i) {
            ips[i] = new char[ips_vec[i].size() + 1];
            memcpy(ips[i], ips_vec[i].c_str(), ips_vec[i].size() + 1);
        }
    }

    cb(hostname, ips, count, qtype, user_data);
    free_ips(ips, count);
}

void DNSLookup::free_ips(char** ips, size_t count) {
    if (ips == nullptr)
        return;

    for (size_t i = 0; i < count; ++i) {
        delete[] ips[i];
    }

    delete[] ips;
}

bool DNSLookup::resolve(const char *hostname, callback_t cb, void *user_data, QUERY_TYPE QuryType) {
    if (!hostname || !*hostname || !cb || m_shuttingDown)
        return false;

    //age HostAddress ipaddress bod resolve nemishe
    // Check for IPv4
    if (QuryType == DNSLookup::A) {
        struct in_addr ipv4_addr;
        if (inet_pton(AF_INET, hostname, &ipv4_addr) == 1) {
            invoke_callback(cb, hostname, {std::string(hostname)}, QuryType, user_data);
            return true;
        }
    }

    // Check for IPv6
    if (QuryType == DNSLookup::AAAA) {
        struct in6_addr ipv6_addr;
        if (inet_pton(AF_INET6, hostname, &ipv6_addr) == 1) {
            char buf[INET6_ADDRSTRLEN];
            if (!inet_ntop(AF_INET6, &ipv6_addr, buf, sizeof(buf)))
                return false;
            invoke_callback(cb, hostname, {std::string(buf)}, QuryType, user_data);
            return true;
        }
    }

    std::string strhost(hostname);
    // esm ha case-insensitive hastan
    std::transform(strhost.begin(), strhost.end(), strhost.begin(), [](unsigned char c) { return (char)tolower(c); });
    time_t now = time(nullptr);

    // /etc/hosts bar DNS olaviat dare
    if (!m_hosts.empty()) {
        std::string key = strhost;
        if (!key.empty() && key.back() == '.')
            key.pop_back();

        auto hit = m_hosts.find(key);
        if (hit != m_hosts.end()) {
            // agar esm dar hosts hast ama in noe record ro nadare, natije khali barmigarde
            // (connectTo bad az A khodesh AAAA ro emtehan mikone)
            std::vector<std::string> ips = (QuryType == DNSLookup::A) ? hit->second.a_ips : hit->second.aaaa_ips;
            invoke_callback(cb, hostname, ips, QuryType, user_data);
            return true;
        }
    }

    // Check cache (faghat baraye haman noe record)
    auto it = m_cache_map.find(strhost);
    if (it != m_cache_map.end()) {
        const std::vector<std::string>& cached = (QuryType == DNSLookup::A) ? it->second.a_ips : it->second.aaaa_ips;
        time_t stamp = (QuryType == DNSLookup::A) ? it->second.a_time : it->second.aaaa_time;

        if (!cached.empty() && (size_t)(now - stamp) < m_cache_ttl_sec) {
            // LRU ghabl az callback update mishe (callback momkene cache ro taghir bede)
            m_cache_list.splice(m_cache_list.begin(), m_cache_list, it->second.lru_it);

            std::vector<std::string> ips = cached;  // copy: callback momkene entry ro avaz kone
            invoke_callback(cb, hostname, ips, QuryType, user_data);
            return true;
        }
    }

    if (m_SocketContext.fd == -1) {
        reset_socket();
        if (m_SocketContext.fd == -1)
            return false;
    }

    DNSRequest* req = acquire_request();
    if (!req)
        return false;

    req->hostname = strhost;
    req->cb = cb;
    req->user_data = user_data;
    req->qid = generate_query_id();
    req->qtype = QuryType;
    req->retry_count = 0;
    req->server_idx = 0;

    if (!send_request(req)) {
        release_request(req);
        return false;
    }

    m_pending[req->qid] = req;
    return true;
}

void DNSLookup::cancel(void *user_data)
{
    for (auto it = m_pending.begin(); it != m_pending.end(); ) {
        if (it->second->user_data == user_data) {
            release_request(it->second);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

// darkhast tamam shod (movafagh ya na): aval az list hazf mishe, bad callback
void DNSLookup::finish_request(DNSRequest *req, const std::vector<std::string> &ips)
{
    m_pending.erase(req->qid);

    callback_t cb = req->cb;
    void* user = req->user_data;
    QUERY_TYPE qtype = req->qtype;
    std::string hostname = std::move(req->hostname);
    release_request(req);

    if (cb)
        invoke_callback(cb, hostname.c_str(), ips, qtype, user);
}

bool DNSLookup::retry_request(DNSRequest *req)
{
    if (req->retry_count >= m_max_retries)
        return false;

    req->retry_count++;
    // talash badi ba server badi
    req->server_idx = (uint16_t)((req->server_idx + 1) % m_dns_servers.size());
    return send_request(req);
}

void DNSLookup::on_dns_read() {
    for (int i = 0; i < 64; i++) {
        struct sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);

        ssize_t len = recvfrom(m_SocketContext.fd, m_shared_buffer, sizeof(m_shared_buffer), 0, (struct sockaddr*)&from_addr, &from_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            if (errno == EINTR)
                continue;

#ifdef DEBUG
            NR_PERROR("DNS recvfrom failed");
#endif
            // darkhast haye montazer ba timeout dobare ersal mishan
            reset_socket();
            return;
        }

        // callback ha momkene resolve() seda bezanan ke m_shared_buffer ro avaz mikone;
        // handle_packet ghabl az seda zadane callback kar ba buffer ro tamam mikone
        handle_packet(m_shared_buffer, (size_t)len, from_addr);

        if (m_SocketContext.fd == -1)
            return;
    }
}

void DNSLookup::handle_packet(const uint8_t *packet, size_t len, const sockaddr_in &from)
{
    if (len < 12)
        return;

    // javab faghat az server haiy ke azashon porsidim ghabul mishe
    if (from.sin_port != htons(53))
        return;

    bool knownServer = false;
    for (const struct in_addr& a : m_dns_server_addrs) {
        if (a.s_addr == from.sin_addr.s_addr) {
            knownServer = true;
            break;
        }
    }
    if (!knownServer)
        return;

    uint16_t id = (uint16_t)((packet[0] << 8) | packet[1]);
    uint16_t flags = (uint16_t)((packet[2] << 8) | packet[3]);
    uint16_t rcode = flags & 0x0F;
    if (!(flags & 0x8000))
        return; // Not a DNS response (QR=0)

    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return; // Unknown query ID (ya ghablan javab dade shode)

    DNSRequest* req = it->second;

    std::vector<std::string> ips;
    if (rcode == 0) {
        // parse ham soal ro ba darkhast tatbigh mide ham javab ha ro estekhraj mikone
        m_lastMinTTL = UINT32_MAX;
        if (!parse_dns_response(packet, len, req, ips)) {
            // soal ba darkhast ma yeki nabod: nadide gerefte mishe
            return;
        }

        if (!ips.empty()) {
            // TTL-e javab rayat mishe (hadeaksar cache_ttl, hadeaghal 1 sanie)
            update_lru_cache(req->hostname, ips, req->qtype, m_lastMinTTL);
        }

        // ips khali: esm vojod dare ama in noe record ro nadare (NODATA)
        finish_request(req, ips);
        return;
    }

#ifdef DEBUG
    NR_LOG(netrector::LOG_INFO, "DNS error for %s (QTYPE=%u): RCODE=%u", req->hostname.c_str(), req->qtype, rcode);
#endif

    // NXDOMAIN javab ghatei hast; baghie khata ha (ServFail, Refused, ...) ba server badi tekrar mishan
    if (rcode != 3 && retry_request(req))
        return;

    finish_request(req, {});
}

void DNSLookup::maintenance() {
    if (m_pending.empty())
        return;

    uint64_t now = now_ms();
    uint64_t timeout_ms = (uint64_t)m_Timeout * 1000ULL;

    // dar hine peymayesh callback seda zade nemishe (callback momkene m_pending ro taghir bede)
    std::vector<DNSRequest*> failed;
    for (auto& p : m_pending) {
        DNSRequest* req = p.second;
        if (now - req->sent_ms < timeout_ms)
            continue;

        if (retry_request(req))
            continue;

        failed.push_back(req);
    }

    for (DNSRequest* req : failed) {
#ifdef DEBUG
        NR_LOG(netrector::LOG_INFO, "DNS timeout for %s (QTYPE=%u, retries=%u)", req->hostname.c_str(), req->qtype, req->retry_count);
#endif
        finish_request(req, {});
    }
}

uint16_t DNSLookup::generate_query_id() {
    for (;;) {
        // xorshift64
        m_rngState ^= m_rngState << 13;
        m_rngState ^= m_rngState >> 7;
        m_rngState ^= m_rngState << 17;

        uint16_t qid = (uint16_t)(m_rngState >> 24);
        if (qid != 0 && m_pending.find(qid) == m_pending.end())
            return qid;
    }
}

bool DNSLookup::build_dns_query(const char* hostname, QUERY_TYPE qtype, std::vector<uint8_t>& packet) {
    packet.assign(m_dns_header, m_dns_header + 12);

    size_t total = 0;
    const char* ptr = hostname;
    while (*ptr) {
        const char* dot = strchr(ptr, '.');
        size_t segment_len = dot ? (size_t)(dot - ptr) : strlen(ptr);

        if (segment_len == 0) {
            // noghte payani mojaz hast ("example.com."), label khali vasate esm na
            if (dot && *(dot + 1) == '\0')
                break;
            return false;
        }

        if (segment_len > 63)
            return false;

        total += segment_len + 1;
        if (total > 253)
            return false;

        packet.push_back(static_cast<uint8_t>(segment_len));
        packet.insert(packet.end(), ptr, ptr + segment_len);

        if (!dot)
            break;
        ptr = dot + 1;
    }

    if (total == 0)
        return false;

    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(qtype >> 8));
    packet.push_back(static_cast<uint8_t>(qtype & 0xFF));
    packet.push_back(0x00);
    packet.push_back(0x01);

    return true;
}

bool DNSLookup::send_request(DNSRequest *req) {
    if (m_dns_servers.empty() || m_SocketContext.fd == -1)
        return false;

    std::vector<uint8_t> query;
    if (!build_dns_query(req->hostname.c_str(), req->qtype, query))
        return false;

    query[0] = static_cast<uint8_t>(req->qid >> 8);
    query[1] = static_cast<uint8_t>(req->qid & 0xFF);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53);
    server_addr.sin_addr = m_dns_server_addrs[req->server_idx % m_dns_server_addrs.size()];

    req->sent_ms = now_ms();
    ssize_t sent = sendto(m_SocketContext.fd, query.data(), query.size(), MSG_DONTWAIT, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (sent < 0) {
        // EAGAIN: buffer UDP por hast; darkhast sabt mimone va ba timeout tekrar mishe
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
#ifdef DEBUG
        NR_PERROR("DNS sendto failed");
#endif
        return false;
    }
    return true;
}

// rad shodan az yek esm (ba ya bedone compression). false: packet kharab
static bool dns_skip_name(const uint8_t* packet, size_t len, size_t& pos)
{
    while (pos < len) {
        uint8_t c = packet[pos];
        if (c == 0) {
            pos += 1;
            return true;
        }

        if ((c & 0xC0) == 0xC0) {
            // pointer: esm hamin ja tamam mishe
            pos += 2;
            return pos <= len;
        }

        if (c > 63)
            return false;

        pos += (size_t)c + 1;
    }
    return false;
}

// false: packet kharab hast ya soal ba darkhast ma motabeghat nadare
bool DNSLookup::parse_dns_response(const uint8_t* packet, size_t len, const DNSRequest* req, std::vector<std::string>& ips) {
    if (len < 12)
        return false;

    uint16_t qdcount = (uint16_t)((packet[4] << 8) | packet[5]);
    uint16_t ancount = (uint16_t)((packet[6] << 8) | packet[7]);
    if (qdcount != 1)
        return false;

    // tatbighe esm soal ba darkhast (bedone compression dar bakhshe soal)
    size_t pos = 12;
    std::string qname;
    while (pos < len) {
        uint8_t c = packet[pos];
        if (c == 0) {
            pos++;
            break;
        }
        if (c > 63 || pos + 1 + c > len)
            return false;
        if (!qname.empty())
            qname.push_back('.');
        qname.append(reinterpret_cast<const char*>(packet + pos + 1), c);
        pos += (size_t)c + 1;
    }

    if (pos + 4 > len)
        return false;

    uint16_t qtype = (uint16_t)((packet[pos] << 8) | packet[pos + 1]);
    uint16_t qclass = (uint16_t)((packet[pos + 2] << 8) | packet[pos + 3]);
    pos += 4;

    std::string expected = req->hostname;
    if (!expected.empty() && expected.back() == '.')
        expected.pop_back();

    if (qtype != (uint16_t)req->qtype || qclass != 1 || strcasecmp(qname.c_str(), expected.c_str()) != 0)
        return false;

    for (uint16_t i = 0; i < ancount && pos < len; ++i) {
        if (!dns_skip_name(packet, len, pos))
            break;

        // TYPE(2) CLASS(2) TTL(4) RDLENGTH(2)
        if (pos + 10 > len)
            break;

        uint16_t type = (uint16_t)((packet[pos] << 8) | packet[pos + 1]);
        uint32_t ttl = ((uint32_t)packet[pos + 4] << 24) | ((uint32_t)packet[pos + 5] << 16) | ((uint32_t)packet[pos + 6] << 8) | packet[pos + 7];
        uint16_t rdlen = (uint16_t)((packet[pos + 8] << 8) | packet[pos + 9]);
        pos += 10;
        if (ttl < m_lastMinTTL)
            m_lastMinTTL = ttl;

        if (pos + rdlen > len)
            break;

        if (type == DNSLookup::A && rdlen == 4 && req->qtype == DNSLookup::A) {
            char ip_str[INET_ADDRSTRLEN];
            struct in_addr addr;
            memcpy(&addr.s_addr, packet + pos, 4);
            if (inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str))) {
                ips.push_back(ip_str);
            }
        } else if (type == DNSLookup::AAAA && rdlen == 16 && req->qtype == DNSLookup::AAAA) {
            char ip_str[INET6_ADDRSTRLEN];
            struct in6_addr addr;
            memcpy(&addr, packet + pos, 16);
            if (inet_ntop(AF_INET6, &addr, ip_str, sizeof(ip_str))) {
                ips.push_back(ip_str);
            }
        }
        // baghie record ha (mesle CNAME) rad mishan
        pos += rdlen;
    }

    return true;
}

void DNSLookup::load_dns_servers() {
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 10, "nameserver") != 0)
            continue;

        size_t start = line.find_first_not_of(" \t", 10);
        if (start == std::string::npos || start == 10)
            continue;

        size_t end = line.find_first_of(" \t#;", start);
        std::string server = line.substr(start, end == std::string::npos ? std::string::npos : end - start);

        // faghat server haye IPv4
        struct in_addr tmp;
        if (!server.empty() && inet_pton(AF_INET, server.c_str(), &tmp) == 1) {
            m_dns_servers.push_back(server);
            if (m_dns_servers.size() >= 3)
                break;
        }
    }
}

void DNSLookup::load_hosts_file() {
    std::ifstream file("/etc/hosts");
    std::string line;
    while (std::getline(file, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);

        // format: IP esm1 esm2 ...
        std::vector<std::string> tokens;
        size_t pos = 0;
        while (pos < line.size()) {
            size_t start = line.find_first_not_of(" \t\r", pos);
            if (start == std::string::npos)
                break;
            size_t end = line.find_first_of(" \t\r", start);
            tokens.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            pos = end;
        }

        if (tokens.size() < 2)
            continue;

        unsigned char tmp[sizeof(struct in6_addr)];
        bool isV4 = inet_pton(AF_INET, tokens[0].c_str(), tmp) == 1;
        bool isV6 = !isV4 && inet_pton(AF_INET6, tokens[0].c_str(), tmp) == 1;
        if (!isV4 && !isV6)
            continue;

        for (size_t i = 1; i < tokens.size(); i++) {
            std::string name = tokens[i];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
            HostsEntry& e = m_hosts[name];
            if (isV4)
                e.a_ips.push_back(tokens[0]);
            else
                e.aaaa_ips.push_back(tokens[0]);
        }
    }
}

void DNSLookup::update_lru_cache(const std::string& hostname, const std::vector<std::string>& ips, DNSLookup::QUERY_TYPE qtype, uint32_t ttl) {
    auto it = m_cache_map.find(hostname);
    if (it == m_cache_map.end()) {
        while (!m_cache_list.empty() && m_cache_list.size() >= m_cache_max_size) {
            m_cache_map.erase(m_cache_list.back());
            m_cache_list.pop_back();
        }

        m_cache_list.push_front(hostname);
        it = m_cache_map.emplace(hostname, CacheEntry{}).first;
        it->second.lru_it = m_cache_list.begin();
    } else {
        m_cache_list.splice(m_cache_list.begin(), m_cache_list, it->second.lru_it);
    }

    time_t now = time(nullptr);
    time_t ttlSec = (time_t)std::min<uint64_t>(ttl, m_cache_ttl_sec);
    if (ttlSec < 1) ttlSec = 1;
    // zaman-e enghezaye vaghei = now + ttl; dar lookup ba cache_ttl moghayese mishe
    time_t stamp = now - (time_t)m_cache_ttl_sec + ttlSec;
    if (qtype == DNSLookup::A) {
        it->second.a_ips = ips;
        it->second.a_time = stamp;
    } else if (qtype == DNSLookup::AAAA) {
        it->second.aaaa_ips = ips;
        it->second.aaaa_time = stamp;
    }
}

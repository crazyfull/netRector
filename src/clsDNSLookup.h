#ifndef CLSDNSLOOKUP_H
#define CLSDNSLOOKUP_H

#include "SocketContext.h"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>
#include <list>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
//#define DEBUG 1

class EpollReactor;
class DNSLookup {
public:
    enum QUERY_TYPE {
        A = 1,
        AAAA = 28
    };

    // ips faghat ta payane callback motabar hast. count == 0 yani resolve nashod.
    typedef void (*callback_t)(const char *hostname, char **ips, size_t count, DNSLookup::QUERY_TYPE qtype, void *user_data);

    struct DNSRequest {
        std::string hostname;
        callback_t cb {nullptr};
        void* user_data {nullptr};
        uint16_t qid {0};
        DNSLookup::QUERY_TYPE qtype {A};
        uint64_t sent_ms {0};
        uint16_t retry_count {0};
        uint16_t server_idx {0};
    };

    DNSLookup(EpollReactor* reactor, size_t cache_ttl_sec = 300, size_t cache_max_size = 2000);
    ~DNSLookup();

    // true: callback hatman (yek bar) seda zade mishe; momkene ghabl az bargasht-e in tabe
    //       bashe (vaghti hostname khodesh IP hast ya dar cache hast).
    // false: darkhast sabt nashod va callback seda zade nemishe.
    bool resolve(const char *hostname, callback_t cb, void *user_data, DNSLookup::QUERY_TYPE QuryType = DNSLookup::A);

    // laghve hame darkhast haye montazer ba in user_data (callback seda zade nemishe)
    void cancel(void *user_data);

    void on_dns_read();
    void maintenance();
    int fd() const;
    void close();
    void reset_socket();

    void setTimeout(uint16_t newTimeout);
    void setCache_ttl_sec(size_t newCache_ttl_sec);
    void setMaxRetries(uint16_t newMaxRetries);

private:
    EpollReactor* m_pReactor;

    struct SocketContext m_SocketContext {};
    std::unordered_map<uint16_t, DNSRequest*> m_pending;
    uint8_t m_shared_buffer[1500];
    uint8_t m_dns_header[12] = {
        0x00, 0x00,
        0x01, 0x00,
        0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00
    };

    struct CacheEntry {
        std::vector<std::string> a_ips;
        std::vector<std::string> aaaa_ips;
        time_t a_time {0};
        time_t aaaa_time {0};
        std::list<std::string>::iterator lru_it;
    };

    std::unordered_map<std::string, CacheEntry> m_cache_map;
    std::list<std::string> m_cache_list;    // LRU: jadidtarin dar ebteda
    size_t m_cache_max_size;
    size_t m_cache_ttl_sec;
    std::vector<std::string> m_dns_servers;
    std::vector<struct in_addr> m_dns_server_addrs;

    // esm haye sabet az /etc/hosts (mesle localhost); ghabl az DNS check mishan
    struct HostsEntry {
        std::vector<std::string> a_ips;
        std::vector<std::string> aaaa_ips;
    };
    std::unordered_map<std::string, HostsEntry> m_hosts;
    uint16_t m_Timeout;
    uint16_t m_max_retries;
    uint64_t m_rngState {0};
    uint32_t m_lastMinTTL {0};
    bool m_shuttingDown {false};

    // Object Pool
    std::vector<DNSRequest> m_request_pool;
    std::vector<DNSRequest*> m_free_requests;

    void init_request_pool(size_t pool_size);
    DNSRequest* acquire_request();
    void release_request(DNSRequest* req);

    uint16_t generate_query_id();
    bool build_dns_query(const char* hostname, QUERY_TYPE qtype, std::vector<uint8_t>& out);
    bool parse_dns_response(const uint8_t* packet, size_t len, const DNSRequest* req, std::vector<std::string>& ips);
    bool send_request(DNSRequest* req);
    bool retry_request(DNSRequest* req);
    void finish_request(DNSRequest* req, const std::vector<std::string>& ips);
    void handle_packet(const uint8_t* packet, size_t len, const struct sockaddr_in& from);
    void load_dns_servers();
    void load_hosts_file();
    void invoke_callback(callback_t cb, const char* hostname, const std::vector<std::string>& ips, QUERY_TYPE qtype, void* user_data);
    void free_ips(char** ips, size_t count);
    void update_lru_cache(const std::string& hostname, const std::vector<std::string>& ips, DNSLookup::QUERY_TYPE qtype, uint32_t ttl);
    static uint64_t now_ms();
};

#endif // CLSDNSLOOKUP_H

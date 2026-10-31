# استاندارد نام‌گذاری netRector

این سند قاعده‌ی واحد نام‌گذاری **کلاس‌ها و تابع‌ها** (و چیزهای هم‌خانواده‌شان: نوع‌ها، enum، عضوها، پارامترها) را مشخص می‌کند و در بخش ۳ فهرست دقیق تغییرها را می‌دهد.

- **دامنه:** `src/` و فایل‌های ریشه‌ی پروژه (`main.cpp`، `Socks5*.h`، `TunnelClient.h`، `HttpTest.h`، `TestCommon.h`). `tests/muxstress/muxstress.cpp` بررسی نشده است.
- **وضعیت:** کد هنوز تغییر نکرده؛ این سند استاندارد و جدول «فعلی ← استاندارد» است.
- **قاعده‌ی انتخاب:** هر جا کد چند سبک داشت، سبکی انتخاب شد که در کد فعلی غالب است تا کمترین تغییر لازم شود: کلاس‌ها `PascalCase`، متدها `camelCase`، عضوها `m_` + `camelCase`، ثابت‌ها `UPPER_SNAKE`.

## ۱. قوانین

| مورد | قاعده | نمونه |
|---|---|---|
| کلاس، struct، enum، alias نوع | `PascalCase`، بدون پیشوند `cls` (پیشوند فقط برای نام فایل است) | `TCPSocket`، `MultiplexedTunnel`، `SocketStatus` |
| namespace | همه حروف کوچک | `netrector` |
| تابع و متد (public/protected/private، static، آزاد) | `camelCase`، با **فعل** شروع شود | `connectTo`، `sendToStream`، `parseDNSResponse` |
| getter ساده (بدون پارامتر، ارزان) | خود اسم، **بدون** `get` | `fd()`، `reactor()`، `status()` |
| getter بولی | پیشوند `is` / `has` / `should` | `isTLS()`، `hasAsyncPrivateKey()` |
| `getX(...)` | فقط وقتی پارامتر دارد، خروجی را با out-param می‌دهد، یا ممکن است نتیجه نداشته باشد | `getPeerAddress(ip, port)`، `getStream(id)`، `getShard(index)` |
| setter | `setX(value)` | `setMaxStreams(n)` |
| hook رویداد | `onXxx` (virtual یا نقطه‌ی ورود رویداد) | `onClose()`، `onKeepAliveTick()` |
| ثبت callback | `setOnXxx(fn, arg)` | `setOnData(fn, arg)` |
| dispatcher محافظت‌شده | `handleOnXxx` (اول callback ثبت‌شده، بعد virtual) | `handleOnData` |
| پردازش داخلی ورودی | `handleXxx` | `handlePingFrame`، `handlePacket` |
| نوع function-pointer | پسوند `Fn` | `OnDataFn`، `AcceptFn`، `ResolveFn` |
| نوع `std::function` | `Callback` | `Timer::Callback` |
| جفت‌فعل‌ها | `add`/`remove`، `register`/`unregister`، `start`/`stop`، `open`/`close`، `enable`/`disable`، `push`/`pop`، `acquire`/`release` | `addFd` ↔ `removeFd` |
| حروف اولیه‌ی اختصاری (TCP، UDP، DNS، TLS، IP، EOF، ALPN، LRU، GC) | وسط یا آخر نام: همه بزرگ. اگر **اولین** کلمه‌ی نام باشد: همه کوچک | `enableTLS`، `onDNSEvent`، `freeIPs` / `tlsContext()`، `dnsPending` |
| حرف ربط داخل نام (`By`، `Of`، `To`) | یک کلمه‌ی کامل است و با حرف بزرگ شروع می‌شود | `getIPByName` (نه `getIPbyName`) |
| مخفف‌های معمولی (`fd`، `id`، `ms`، `idx`) | مثل کلمه‌ی عادی | `registerFd`، `streamId`، `genId`، `nowMs` |
| enum | نوع: `PascalCase` مفرد. مقدارها: `PascalCase`. برای کد جدید `enum class` | `CloseReason::ClosedByPeer` |
| ثابت و macro | `UPPER_SNAKE`؛ macro های پروژه با پیشوند `NR_` | `MAX_EVENTS`، `NR_LOG` |
| عضو داده‌ی private/protected | `m_` + `camelCase`، **بدون** پیشوند نوع | `m_reactor` (نه `m_pReactor`)، `m_socketContext` |
| فیلد struct عمومی | `camelCase`، بدون `m_` | `sendWindow`، `userData` |
| پارامتر و متغیر محلی | `camelCase` با حرف اول کوچک | `port`، `queryType` |
| املا | بدون غلط املایی در نام‌ها | `Cached`، `Query`، `Listener` |

مخفف‌های مبهم (`cb`، `ctx`، `p`، `r`) در **نام تابع و کلاس** نیایند. در پارامتر ترجیحاً `callback`، `context`.

## ۲. استثناها

- کد شخص ثالث دست نمی‌خورد: `src/concurrentqueue.h`، `src/tlsf.c`، `src/tlsf.h`، `lightweightsemaphore.h`.
- نام‌هایی که API بیرونی تعیین می‌کند (callback های BoringSSL، سیستم‌کال‌ها، macro های `epoll`).
- نام‌های خود پروتکل در enum: `SYN`، `ACK`، `FIN`، `RST` (Yamux) و `A`، `AAAA` (DNS).
- برای کلاس‌های ساختار داده (`IntrusiveList`، `SendQueue`، `GCList`) استثنای «سبک STL» **نداریم**: `pushBack`، `popFront`، نه `push_back`، `pop_front`. اسم‌های تک‌کلمه‌ای (`size`، `empty`، `front`، `clear`) خودبه‌خود درست‌اند.

## ۳. جدول تغییرها

اولویت: **P1** = API عمومی (روی فراخواننده‌ها اثر می‌گذارد: `main.cpp`، فایل‌های ریشه، `tests/`)، **P2** = داخلی (private/static/عضو)، **P3** = اختیاری یا تغییر امضا.

### نوع‌ها و enum

| فعلی | استاندارد | اولویت |
|---|---|---|
| `TCPSocket::socketStatus` | `TCPSocket::SocketStatus` | P1 |
| `SockTypes` (مقدارها `IS_TCP_SOCKET`، …) | `enum class SockType { None, TCPListener, TCPSocket, UDPListener, UDPSocket, Timer, TimerManager, DNSLookup, Wakeup }` | P3 |
| `DNSLookup::QUERY_TYPE` | `DNSLookup::QueryType` (مقدارهای `A` و `AAAA` می‌مانند) | P1 |
| `DNSLookup::callback_t` | `DNSLookup::ResolveFn` | P1 |
| `EpollReactor::acceptCallback` | `EpollReactor::AcceptFn` (در `Server` و `Listener` هم استفاده شده) | P1 |
| `netrector::LogLevel` (`LOG_ERROR`، `LOG_WARN`، `LOG_INFO`، `LOG_DEBUG`) | `enum class LogLevel { Error, Warn, Info, Debug }`؛ `setLogLevel`/`logMessage`/`LogHandler` هم `LogLevel` بگیرند | P3 |
| `ParseResult { Incomplete, Invalid, Ok }` در `TestCommon.h` | `enum class ParseResult` | P3 |

### EpollReactor

| فعلی | استاندارد | اولویت |
|---|---|---|
| `register_fd` | `registerFd` | P1 |
| `unregister_fd` | `unregisterFd` | P1 |
| `add_fd` | `addFd` | P1 |
| `del_fd` | `removeFd` | P1 |
| `add_listener` | `addListener` | P1 |
| `add_listener_fd` | `addListenerFd` | P1 |
| `remove_listener` | `removeListener` | P1 |
| `stop_listener` (همه‌ی listener ها را می‌بندد) | `stopListeners` | P1 |
| `getIPbyName` | `getIPByName` | P1 |
| `updateCashedTime` | `updateCachedTime` | P1 |
| `getCachedNow` | `cachedNow` | P1 |
| `shutdown_all` | `shutdownAll` | P2 |
| `make_key` / `extract_fd` / `extract_gen` | `makeKey` / `extractFd` / `extractGen` | P2 |
| `m_reactorID` | `m_reactorId` | P2 |
| `m_pTimers` / `m_pDNSLookup` / `m_pConnectionList` | `m_timerManager` / `m_dnsLookup` / `m_connectionList` | P2 |
| `m_GCList` / `m_cached_now` | `m_gcList` / `m_cachedNow` | P2 |

### TCPSocket

| فعلی | استاندارد | اولویت |
|---|---|---|
| `pause_reading` / `resume_reading` | `pauseReading` / `resumeReading` | P1 |
| `getReactor` | `reactor` | P1 |
| `getStatus` (و عضو بدون پیشوند `status`) | `status()` و عضو `m_status` | P1 |
| `getErrorCode` | `errorCode` | P1 |
| `getLocalPort` | `localPort` | P1 |
| `getLastActiveTime` | `lastActiveTime` | P1 |
| `getSocketContext` | `socketContext` | P1 |
| `getPointer` (فقط `return this` و جایی استفاده نشده) | حذف | P1 |
| `connect_cb` (static) | `connectCallback` | P2 |
| `_accepted` (زیرخط اول) | `acceptFd`؛ تفاوتش با `adoptFd` بررسی شود | P2 |
| پارامترهای `Arg`، `Data` | `arg`، `data` | P2 |
| `m_pReactor`، `m_SocketContext` | `m_reactor`، `m_socketContext` | P2 |

### UDPSocket، Timer، TimerManager

| فعلی | استاندارد | اولویت |
|---|---|---|
| `UDPSocket::getReactor` / `getLocalPort` / `getErrorCode` / `getSocketContext` | `reactor` / `localPort` / `errorCode` / `socketContext` | P1 |
| `Timer::getReactor`، `TimerManager::getReactor` | `reactor` | P1 |
| `Timer::singleShot` | `startSingleShot` (اول نام فعل باشد) | P1 |

### Server و Listener

| فعلی | استاندارد | اولویت |
|---|---|---|
| `Server::AddNewListener(int Port, …)` | `addListener(int port, …)` | P1 |
| `Server::getRoundRobinShard` | `roundRobinShard` | P1 |
| `Server::setup_signals` | `setupSignals` | P2 |
| `Listener::Addlisten` (قدیمی، جایی استفاده نشده) | حذف؛ معادل `start(port, nullptr, nullptr, nullptr)` | P1 |

### DNSLookup

| فعلی | استاندارد | اولویت |
|---|---|---|
| `on_dns_read` | `onDNSRead` | P1 |
| `reset_socket` | `resetSocket` | P1 |
| `setCache_ttl_sec` | `setCacheTtlSec` | P1 |
| پارامتر `QuryType` | `queryType` | P1 |
| `init_request_pool` / `acquire_request` / `release_request` | `initRequestPool` / `acquireRequest` / `releaseRequest` | P2 |
| `generate_query_id` | `generateQueryId` | P2 |
| `build_dns_query` / `parse_dns_response` | `buildDNSQuery` / `parseDNSResponse` | P2 |
| `send_request` / `retry_request` / `finish_request` | `sendRequest` / `retryRequest` / `finishRequest` | P2 |
| `handle_packet` | `handlePacket` | P2 |
| `load_dns_servers` / `load_hosts_file` | `loadDNSServers` / `loadHostsFile` | P2 |
| `invoke_callback` | `invokeCallback` | P2 |
| `free_ips` | `freeIPs` | P2 |
| `update_lru_cache` | `updateLRUCache` | P2 |
| `now_ms` | `nowMs` | P2 |
| `dns_skip_name` (static در `.cpp`) | `dnsSkipName` | P2 |
| فیلدهای `user_data`، `retry_count`، `sent_ms`، `server_idx`، `a_ips`، `aaaa_ips`، `a_time`، `aaaa_time`، `lru_it` | `userData`، `retryCount`، `sentMs`، `serverIdx`، `aIPs`، `aaaaIPs`، `aTime`، `aaaaTime`، `lruIt` | P2 |
| `m_cache_*`، `m_dns_*`، `m_max_retries`، `m_shared_buffer`، `m_request_pool`، `m_free_requests`، `m_Timeout` | معادل `camelCase`: `m_cacheMap`، `m_dnsServers`، `m_maxRetries`، … | P2 |

### SocketList، BufferPool، لیست‌ها، Log، TLS، Tunnel

| فعلی | استاندارد | اولویت |
|---|---|---|
| `SocketList(int MaxFD)` | `SocketList(int maxFd)` | P2 |
| `genIDCounter` / `nextGenID` / `SockInfo::genID` / `m_genIDCounter` | `genIdCounter` / `nextGenId` / `SockInfo::genId` / `m_genIdCounter` | P1 |
| `SockInfo::active_link` | `activeLink` | P2 |
| `BufferPool::pool_` | `m_pool` | P2 |
| `IntrusiveList::push_back` / `for_each` | `pushBack` / `forEach` | P1 |
| `SendQueue::pop_front` | `popFront` | P1 |
| `GCList::flush_all` / `now_s_coarse` | `flushAll` / `nowSecCoarse` | P1 / P2 |
| `netrector::logEnabled` (و macro `NR_LOG` که صدایش می‌زند) | `isLogEnabled` | P1 |
| `TLSContext::asyncPrivateKey()` | `hasAsyncPrivateKey()` | P1 |
| `TLSContext::verifyPeer()` | `shouldVerifyPeer()` | P1 |
| `MultiplexedTunnel::remoteGoAway()` | `isRemoteGoAway()` | P1 |
| `MultiplexedTunnel::remoteFin(Stream*)` | `handleRemoteFin` | P2 |
| `MultiplexedTunnel::protocolError(const char*)` | `raiseProtocolError` | P2 |
| `EPOLL_LISTINER_EVENTS` (در `epoll.h`، املا) | `EPOLL_LISTENER_EVENTS` | P2 |

### کد ریشه (برنامه‌ی تست)

نام کلاس‌ها (`Socks5Session`، `TunnelClientEndpoint`، `HttpConn`، …) و تابع‌های `main.cpp`/`Socks5*.h`/`TunnelClient.h` با استاندارد جور هستند. دو مورد اختیاری (P3): `usage()` ← `printUsage()` و `replyFromErrno()` ← `makeReplyFromErrno()` (اول نام فعل).

## ۴. اجرا

حجم تقریبی استفاده در کد (با احتساب تعریف و کامنت): `m_SocketContext` ≈ ۱۷۰ بار، `m_pReactor` ≈ ۹۰ بار، `getReactor` ≈ ۲۰، `getStatus` ≈ ۲۰، `AddNewListener` ≈ ۱۱.

ترتیب پیشنهادی:

1. یک commit برای P2 (داخلی)، چون فراخواننده‌ی بیرونی ندارد.
2. یک commit برای P1، همراه اصلاح فراخواننده‌ها در `main.cpp`، فایل‌های ریشه و `tests/`.
3. بعد از هر commit: `scripts/build.sh` و `python3 tests/run_tests.py`.
4. P3 را جدا و در صورت نیاز.

برای rename خودکار می‌شود از `clang-tidy` با `-fix` استفاده کرد؛ این پیکربندی قاعده‌های بخش ۱ را که ابزار می‌فهمد (کلاس، متد، عضو، پارامتر) چک می‌کند. این پیکربندی اجرا نشده است. قاعده‌ی حروف اختصاری و حذف `get` را ابزار چک نمی‌کند و با نگاه دستی یا `grep` باید دید.

```yaml
# .clang-tidy
Checks: '-*,readability-identifier-naming'
HeaderFilterRegex: '(src/cls.*|src/SocketContext\.h|src/constants\.h)'
CheckOptions:
  - { key: readability-identifier-naming.ClassCase,              value: CamelCase }
  - { key: readability-identifier-naming.StructCase,             value: CamelCase }
  - { key: readability-identifier-naming.EnumCase,               value: CamelCase }
  - { key: readability-identifier-naming.TypeAliasCase,          value: CamelCase }
  - { key: readability-identifier-naming.NamespaceCase,          value: lower_case }
  - { key: readability-identifier-naming.FunctionCase,           value: camelBack }
  - { key: readability-identifier-naming.MethodCase,             value: camelBack }
  - { key: readability-identifier-naming.ParameterCase,          value: camelBack }
  - { key: readability-identifier-naming.PrivateMemberPrefix,    value: m_ }
  - { key: readability-identifier-naming.ProtectedMemberPrefix,  value: m_ }
  - { key: readability-identifier-naming.PrivateMemberCase,      value: camelBack }
  - { key: readability-identifier-naming.ProtectedMemberCase,    value: camelBack }
  - { key: readability-identifier-naming.ConstexprVariableCase,  value: UPPER_CASE }
```

## ۵. نکته‌ی جانبی

مقدارهای `LOG_INFO` و `LOG_DEBUG` در `LogLevel` هم‌نام macro های `<syslog.h>` هستند. اگر برنامه‌ای که از کتابخانه استفاده می‌کند `<syslog.h>` را include کند، با کتابخانه تداخل می‌کند. تبدیل به `enum class LogLevel` (ردیف P3 بالا) این مشکل را هم برطرف می‌کند.

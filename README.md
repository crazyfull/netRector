# netRector

کتابخانه‌ی شبکه‌ی عمومی بر پایه‌ی `epoll` برای تعداد اتصال زیاد و کارایی بالا (فقط لینوکس، C++17).
کد کتابخانه در `src/` است. فایل‌های ریشه‌ی پروژه فقط برنامه‌ی تست و بنچمارک هستند؛ تونل و پروکسی
یکی از کاربردهای آن است، نه هدف اصلی.

| مسیر | محتوا |
|---|---|
| `src/` | کتابخانه: reactor، `TCPSocket` (ساده و TLS)، `MultiplexedTunnel` (Yamux)، DNS، تایمر |
| `main.cpp` و `*.h` در ریشه | برنامه‌ی تست با حالت‌های `client`، `server`، `socks5`، `http` (بنچمارک) و `udp` |
| `bench/` | بنچمارک مقابل nginx (`bench/bench.sh`) و نتایج (`bench/RESULTS.md`) |
| `yamux_server/` | سرور تست Yamux با Python (پروژه‌ی جدا، فقط standard library) |
| `tests/run_tests.py` | تست یکپارچه‌ی خودکار؛ `tests/go_yamux/` ابزار سازگاری با `hashicorp/yamux` |
| `scripts/`، `docker/` | اسکریپت‌های build و محیط لینوکسی |
| `report/` | گزارش بررسی کد و وضعیت هر یافته |
| `backup/` | نسخه‌ی اصلی فایل‌ها قبل از تغییرات |

## Build

BoringSSL لازم است. یک بار آن را بسازید:

```sh
scripts/build_boringssl.sh          # خروجی: third_party/boringssl/{include,lib}
scripts/build.sh                    # qmake + make، خروجی: build/netRector
```

اگر BoringSSL جای دیگری است: `BORINGSSL_DIR=/opt/boringssl scripts/build.sh`.
در Qt Creator هم همان `netRector.pro` باز می‌شود (متغیر محیطی `BORINGSSL_DIR` را در تنظیمات build بگذارید).

روی macOS یا ویندوز از Docker استفاده کنید:

```sh
docker build -t netrector-dev docker/
docker run --rm -it -v "$PWD":/work -w /work netrector-dev bash
# داخل کانتینر:
QT_SELECT=qt5 scripts/build.sh
```

`scripts/build_asan.sh` یک نسخه‌ی debug با AddressSanitizer و UBSan در `build-asan/` می‌سازد.

## اجرای برنامه‌ی تست

```sh
# سرور تونل (C++)؛ با --cert/--key روی TLS
build/netRector server --listen 0.0.0.0:9191 --cert cert.pem --key key.pem

# یا سرور تونل Python (معادل همان)
python3 yamux_server/server.py --listen 0.0.0.0:9191 --tls-cert cert.pem --tls-key key.pem

# کلاینت: SOCKS5 محلی، هر اتصال یک stream روی تونل
build/netRector client --server 1.2.3.4:9191 --listen 127.0.0.1:1080 --tls --insecure

# پروکسی SOCKS5 مستقیم، بدون تونل
build/netRector socks5 --listen 127.0.0.1:1080

curl --socks5-hostname 127.0.0.1:1080 https://example.com/

# سرور HTTP ساده برای بنچمارک (پاسخ ثابت یا یک فایل با sendfile)، با یا بدون TLS
build/netRector http --listen 0.0.0.0:8080 --threads 4 [--file big.bin] [--cert c --key k --tls-threads 4]

# UDP echo (تست UDPSocket)
build/netRector udp --listen 127.0.0.1:5353
```

| گزینه | معنی |
|---|---|
| `--tls` | کلاینت با TLS وصل می‌شود و گواهی سرور را بررسی می‌کند |
| `--ca FILE` | فایل CA برای بررسی گواهی (پیش‌فرض: CAهای سیستم) |
| `--sni NAME` | نام مورد انتظار در گواهی (پیش‌فرض: همان host) |
| `--insecure` | TLS بدون بررسی گواهی (برای گواهی self-signed) |
| `--threads N` | تعداد reactor. در حالت `client` هر reactor تونل خودش را دارد |
| `--keepalive SEC` | فاصله‌ی ping تونل؛ `0` یعنی خاموش (پیش‌فرض ۳۰) |
| `--stats SEC` | چاپ دوره‌ای تعداد fd و stream |
| `--tls-threads N` | عملیات کلید خصوصی handshake روی N thread جدا (`server` و `http`) |
| `--single-accept` | یک accept در هر event به‌جای حداکثر ۶۴ تا |
| `-v` | لاگ هر اتصال و هر stream |

برای ساخت گواهی آزمایشی: `yamux_server/gen_cert.sh`.

## تست خودکار

```sh
python3 tests/run_tests.py                       # همه‌ی حالت‌ها
python3 tests/run_tests.py --only tunnel,tls     # فقط بعضی حالت‌ها
python3 tests/run_tests.py --threads 4
python3 tests/run_tests.py --bin build-asan/netRector
```

حالت‌ها: `direct`، `tunnel`، `tls`، `tls-ca` (C++ به C++) و `py`، `py-tls` (کلاینت C++ به سرور Python).
هر حالت ۱۱ تست دارد: echo، مقصد IPv6، دانلود و آپلود ۶۴ مگابایتی با بررسی hash، half-close،
۱۰۰ stream هم‌زمان، خطای اتصال و DNS، backpressure، قطع ناگهانی، نشت fd و قطع و وصل تونل.

## بنچمارک مقابل nginx

```sh
docker run --rm --ulimit nofile=1048576:1048576 -v "$PWD":/work -w /work netrector-dev bench/bench.sh 4 10
```

سرورها روی ۴ هسته و `wrk` روی بقیه pin می‌شوند. نتیجه‌ی آخرین اجرا در [bench/RESULTS.md](bench/RESULTS.md).

## استفاده از TLS در کتابخانه

```cpp
#include "clsTLSContext.h"

// سرور: یک بار ساخته می‌شود و بین همه‌ی اتصال‌ها مشترک است
TLSContext* tls = TLSContext::createServer("cert.pem", "key.pem", &error);

TCPSocket* onAccept(void* ctx) {
    MySocket* s = new MySocket;
    s->enableTLS(static_cast<TLSContext*>(ctx));   // onAccepted بعد از handshake می‌آید
    return s;
}
srv.AddNewListener(443, "0.0.0.0", onAccept, tls);

// کلاینت
TLSContext* tls = TLSContext::createClient(true /*verify*/);
sock->setReactor(reactor);
sock->enableTLS(tls);                  // SNI و بررسی نام از host گرفته می‌شود
sock->connectTo("example.com", 443);   // onConnected بعد از handshake می‌آید
```

بعد از `enableTLS` بقیه‌ی API (`send`، `onReceiveData`، `close`، backpressure) بدون تغییر کار می‌کند.
`MultiplexedTunnel` هم چون از `TCPSocket` ارث می‌برد روی TLS کار می‌کند.

**handshake غیرهمزمان:** با `tls->enableAsyncPrivateKey(threads)` امضای RSA/ECDSA در handshake (چند صد
میکروثانیه) روی thread های جدا انجام می‌شود و حلقه‌ی reactor بلاک نمی‌شود. بقیه‌ی handshake (ECDHE، هش)
مثل nginx در خود reactor می‌ماند. با فعال شدن این گزینه فقط cipher های ECDHE پذیرفته می‌شوند.

## ارسال بدون کپی و فایل

```cpp
sock->send(buf, len);                       // اگر صف خالی باشد مستقیم به کرنل، وگرنه یک کپی به صف
sock->sendv(iov, n);                        // چند تکه در یک syscall (writev)
sock->sendNoCopy(ptr, len, release, arg);   // بدون کپی؛ release وقتی ارسال تمام شد (یا با close) صدا زده می‌شود
sock->sendNoCopy(staticBuf, len);           // داده‌ی ثابت (static): بدون release
sock->sendFile(fd, offset, len);            // sendfile از page cache (روی TLS با pread)
```

## UDP و Listener

```cpp
class Echo : public UDPSocket {
    void onDatagram(const uint8_t* d, size_t n, const sockaddr* from, socklen_t l) override { sendTo(d, n, from, l); }
};
Echo* u = new Echo;  u->setReactor(reactor);  u->bind("0.0.0.0", 5353, /*reusePort*/ true);
// دریافت با recvmmsg (چند datagram در یک syscall)؛ بستن با u->close() و حذف توسط reactor

Listener* l = new Listener(reactor);  l->start(8080, "0.0.0.0", acceptFactory, ctx);  l->stop();
```

## تغییرات API کتابخانه

کدی که قبلاً از کتابخانه استفاده می‌کرده باید این موارد را رعایت کند:

- **مالکیت:** شیء `TCPSocket` باید با `new` ساخته شود. بعد از accept یا `connectTo() == true` مالک آن reactor است و بعد از `onClose` خودش حذف می‌شود. اگر سوکت عضو یک کلاس دیگر است، `setAutoDelete(false)` بزنید.
- **`onClose`:** دقیقاً یک بار و به‌عنوان آخرین callback صدا زده می‌شود. خطای اتصال اول `onConnectFailed` و بعد `onClose` می‌دهد.
- **`connectTo`:** اگر `false` برگرداند هیچ callbackی صدا زده نشده و شیء هنوز مال شماست. اگر `true` برگرداند نتیجه با callback می‌آید و ممکن است قبل از برگشت تابع آمده باشد.
- **`send`:** حالا `bool` برمی‌گرداند. خطا داخل خود `send` باعث `onClose` نمی‌شود؛ بسته شدن به انتهای همان دور حلقه منتقل می‌شود.
- **`close()`:** پیش‌فرض graceful است: صف ارسال خالی می‌شود، FIN فرستاده می‌شود و تا FIN طرف مقابل (حداکثر ۵ ثانیه) صبر می‌کند. `close(true)` فوری است.
- **`onPause` / `onResume`:** یعنی صف ارسال همین سوکت از حد بالا گذشت یا به حد پایین رسید. رفتار پیش‌فرض توقف خواندن از همان سوکت است؛ پروکسی‌ها باید خواندن از طرف مقابل را متوقف کنند. `pause_reading` و `resume_reading` دیگر callback صدا نمی‌زنند.
- **half-close:** با `setAllowHalfClose(true)`، دریافت FIN به `onReadEOF` می‌رسد و سوکت برای نوشتن باز می‌ماند. `shutdownWrite()` سمت نوشتن را می‌بندد.
- **بافر خواندن:** هر reactor یک بافر مشترک ۶۴ کیلوبایتی دارد. اشاره‌گر داده فقط تا پایان `onReceiveData` معتبر است (قبلاً هم همین بود).
- **thread:** همه‌ی متدها فقط از thread همان reactor. از thread دیگر با `EpollReactor::post(...)`.
- **listener:** پارامتر `bindIP` حالا واقعاً اعمال می‌شود. `nullptr` یا `"*"` یعنی همه‌ی اینترفیس‌ها (IPv4 و IPv6). هر listener می‌تواند factory خودش را داشته باشد.
- **Yamux:** برای هر stream علاوه بر `onData` و `onClose`، callbackهای `onRemoteFin`، `onPause` و `onResume` هست. `pauseStream` و `resumeStream` جریان دریافت را کنترل می‌کنند. `closeStream(id)` یعنی FIN و `closeStream(id, true)` یعنی RST. توضیح کامل در `src/clsMultiplexedTunnel.h`.
- **timeout:** اتصال و handshake بعد از ۱۵ ثانیه قطع می‌شوند. `setIdleTimeout(sec)` برای اتصال‌های بیکار اختیاری است. بررسی timeout ها تکه‌تکه انجام می‌شود (هر ۲۵۰ms بخشی از جدول)، پس با هزاران اتصال stall دوره‌ای ندارد.
- **حافظه:** اتصال بیکار هیچ تخصیص heap ندارد (صف ارسال intrusive است و بافر خواندن مشترک). اگر pool پر شود صف از `malloc` استفاده می‌کند؛ اتصال قطع نمی‌شود. اندازه‌ی pool هر reactor با پارامتر سازنده‌ی `Server`/`EpollReactor` قابل تنظیم است.
- **syscall ها:** گزینه‌های سوکت (NODELAY, keepalive) روی listener تنظیم می‌شوند و به اتصال‌های accept شده به ارث می‌رسند (۵ setsockopt کمتر برای هر اتصال)؛ بستن fd بدون `epoll_ctl(DEL)`؛ `recv` کوتاه یعنی بافر خالی است و `recv` اضافه برای EAGAIN زده نمی‌شود؛ اتصال‌های خروجی فقط NODELAY می‌گیرند (keepalive در صورت نیاز در `onConnected` با `setSocketKeepAlive(fd(), true)`).

## کارهای انجام‌نشده

- برنامه‌ی تست SOCKS5 احراز هویت ندارد. روی IP عمومی فقط با `--listen 127.0.0.1:...` اجرا کنید.
- خواندن TLS از طریق `SSL_set_fd` است؛ BoringSSL برای هر record جدا می‌خواند. یک BIO سفارشی روی بافر مشترک reactor می‌تواند syscall های خواندن TLS را کمتر کند.
- `MSG_ZEROCOPY` کرنل استفاده نمی‌شود (فقط برای ارسال‌های بزرگ‌تر از ~۱۰KB به‌صرفه است و ردیابی completion می‌خواهد).
- DNS فقط سرورهای IPv4 از `/etc/resolv.conf` را می‌خواند و پاسخ‌های بزرگ‌تر از یک بسته‌ی UDP را دوباره با TCP نمی‌پرسد.

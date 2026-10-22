# بنچمارک netRector مقابل nginx

- تاریخ: ۱۳ اکتبر ۲۰۲۶ — `bench/bench.sh 4 10`
- محیط: Docker Desktop روی Apple Silicon (Ubuntu 24.04, arm64, 10 هسته). سرورها روی هسته‌های 0-3 (۴ worker / ۴ thread) و `wrk` روی هسته‌های 4-9 (۶ thread). هر مورد ۱۰ ثانیه.
- nginx 1.24 (بسته‌ی Ubuntu) با `multi_accept off`، `access_log off`، `sendfile on`، `return 200` برای پاسخ ثابت، TLS با session ticket.
- netRector: `netRector http --threads 4` (پاسخ ثابت با `sendNoCopy`، فایل با `sendFile`)، BoringSSL، پیش‌فرض multi-accept.
- پاسخ ثابت: ۱۷ بایت body. فایل: ۱۰۰ کیلوبایت.

| سناریو | nginx | netRector | نسبت |
|---|---|---|---|
| keep-alive، ۶۴ اتصال | ۸۲۷K req/s | ۸۸۸K req/s | ۱٫۰۷× |
| keep-alive، ۱۰۰۰ اتصال | ۸۶۸K req/s | ۱٬۰۸۴K req/s | ۱٫۲۵× |
| keep-alive، ۱۰٬۰۰۰ اتصال | ۶۹۲K req/s (avg 14.2ms) | ۷۱۹K req/s (avg 10.3ms) | ۱٫۰۴× |
| اتصال جدید برای هر درخواست (`Connection: close`)، ۲۵۶ اتصال | ۲۴۲K req/s | ۲۳۶K req/s (multi-accept) / ۲۱۲K (single-accept) | ۰٫۹۸× |
| فایل ۱۰۰KB، ۲۵۶ اتصال (sendfile) | ۲۵۵K req/s, 3.4ms | ۲۸۳K req/s, 0.8ms | ۱٫۱۱× |
| TLS keep-alive، ۱۰۰۰ اتصال | ۵۳۳K req/s | ۶۸۰K req/s | ۱٫۲۸× |
| TLS با handshake برای هر درخواست، ۶۴ اتصال | ۱۵٫۳K req/s | ۱۷٫۹K (sync) / ۱۹٫۹K (`--tls-threads 4`) | ۱٫۱۷× / ۱٫۳۰× |
| RSS بعد از همه‌ی تست‌ها | ۳۹۰ MB | ۲۲۷ MB | |

## اثر handshake غیرهمزمان بر پاسخ‌دهی

۶۴ اتصال keep-alive روی پورت ساده، هم‌زمان با طوفان handshake TLS (۶۴ اتصال، هر درخواست یک handshake) روی همان سرور:

| حالت | req/s (keep-alive) | avg | p99 | max |
|---|---|---|---|---|
| بدون طوفان (مرجع) | ۶۳۲K | 84µs | 351µs | 10ms |
| طوفان، امضا در reactor (sync) | ۴۰۹K | 285µs | **4.0ms** | 47ms |
| طوفان، `--tls-threads 4` | ۴۱۸K | 165µs | **0.72ms** | 19ms |

امضای کلید خصوصی که از reactor بیرون می‌رود، p99 اتصال‌های دیگر را ۵٫۵ برابر کم می‌کند. throughput کل تغییر زیادی نمی‌کند چون هر چهار هسته از قبل اشباع بودند.

## نکات

- `wrk` در بعضی اجراها «timeout» گزارش می‌کند در حالی که بیشینه‌ی تأخیر ثبت‌شده‌ی خودش زیر ۱۰۰ms است (timeout تنظیم‌شده ۱۰ ثانیه)؛ این مشکل حسابداری wrk زیر بار اشباع است و برای nginx هم رخ می‌دهد.
- اعداد روی لینوکس واقعی x86-64 بالاتر خواهند بود؛ نسبت‌ها اهمیت دارند، نه مقادیر مطلق.
- برای تکرار: `docker run --rm --ulimit nofile=1048576:1048576 -v "$PWD":/work -w /work netrector-dev bench/bench.sh 4 10`

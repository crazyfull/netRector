# muxstress — تست سنگین multiplexing (Yamux)

یک کلاینت و سرور کوچک روی خود کتابخانه که multiplexing را در شرایط واقعی شبکه می‌سنجد و در انتها
همه‌ی خطاها را چاپ می‌کند (exit code صفر فقط وقتی هیچ خطایی نباشد).

| فایل | کار |
|---|---|
| `muxstress.cpp` | سرور echo و کلاینت تولیدکننده/راستی‌آزما (`build.sh` می‌سازد: `muxstress` و `muxstress-asan`) |
| `chaos_relay.py` | relay که جریان TCP را به تکه‌های تصادفی (۱ بایت تا N) با تأخیر تصادفی می‌شکند |
| `fuzz.py` | فریم‌های خراب/خصمانه‌ی Yamux به سرور (`server`)، یا سرور جعلی که به کلاینت آشغال می‌دهد (`client`) |
| `run_muxstress.sh` | اجرای همه‌ی سناریوها و چاپ خلاصه |

## پروتکل داخل هر stream
هر رکورد: `len(u32) seq(u32) crc32(u32) payload`. کلاینت رکوردها را با اندازه‌های تصادفی
(۱ بایت، حول ۱۲ بایت، تصادفی، تا ۲۵۶KB یعنی یک window کامل) می‌فرستد؛ سرور هر رکورد را بررسی
و عیناً برمی‌گرداند؛ کلاینت ترتیب (`seq`)، سلامت (`crc`) و تعداد بایت‌ها را بررسی می‌کند.

رفتار stream ها (با نسبت قابل تنظیم):
- **slow**: کلاینت گاه‌به‌گاه `pauseStream` می‌زند تا window طرف مقابل و صف تونل پر شود (ستون `pauses`).
- **rst**: وسط کار `closeStream(id, true)`؛ بعد از آن هیچ callbackی نباید بیاید.
- **عادی**: بعد از آخرین رکورد `FIN`؛ باید همه‌ی echo ها و بعد FIN سرور برسد.

سرور echo با backpressure کار می‌کند: وقتی صف ارسال stream پر شود دریافت همان stream را نگه می‌دارد.

## اجرا

```sh
# داخل کانتینر توسعه (یا لینوکس با BoringSSL)
tests/muxstress/build.sh
tests/muxstress/run_muxstress.sh asan          # همه‌ی سناریوها با AddressSanitizer
tests/muxstress/run_muxstress.sh release quick # سریع، بدون sanitizer
```

سناریوها: رکوردهای کوچک/بزرگ/۱ تا ۱۳ بایتی، عبور از `chaos_relay` (هر فریم در چند خواندن با اندازه‌های
مختلف می‌رسد)، TLS با امضای غیرهمزمان، ۳۰۰۰ stream هم‌زمان در هر تونل، fuzz سرور (۲۰ نوع فریم خراب
× چند دور؛ بعد از هر مورد یک session معتبر سلامت سرور را تأیید می‌کند) و سرور جعلی مقابل کلاینت تونل
netRector (باید هر بار تونل را ببندد، زنده بماند و دوباره وصل شود).

اجرای دستی روی دو ماشین:

```sh
# ماشین A
tests/muxstress/muxstress server --listen 0.0.0.0:9300 [--cert c.pem --key k.pem --tls-threads 2]
# ماشین B (اختیاری: relay شکننده‌ی بسته‌ها بین دو طرف)
python3 tests/muxstress/chaos_relay.py --listen 127.0.0.1:9301 --target A:9300
tests/muxstress/muxstress client --server A:9300 [--tls] --streams 500 --messages 300 \
    --max-size 65536 --slow 0.3 --rst 0.1 --threads 4 --duration 300
```

گزینه‌های کلاینت: `--streams` (هر thread)، `--messages` (رکورد هر stream)، `--min-size/--max-size`،
`--slow`، `--rst`، `--inflight` (سقف بایت echo نشده‌ی هر stream)، `--rounds`، `--duration` (ثانیه؛ بعد از آن
stream های باز به‌عنوان stall گزارش می‌شوند)، `--threads` (هر thread یک تونل جدا).

در خروجی، `rejected` در سمت سرور یعنی رکورد خراب از کلاینت (در fuzz طبیعی است)؛ `ERRORS` همیشه باید صفر باشد.

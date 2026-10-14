# yamux_server

سرور تست **Yamux** (مطابق spec کتابخانهٔ `hashicorp/yamux`، نسخهٔ 0) برای کلاینت C++.
کلاینت با plain TCP یا TLS وصل می‌شود و برای هر اتصال پروکسی‌شده یک stream باز می‌کند.
روی هر stream ابتدا یک درخواست خام **SOCKS5 CONNECT** می‌آید؛ سرور به مقصد وصل می‌شود،
reply ده‌بایتی SOCKS5 را روی همان stream می‌فرستد و بعد بایت‌ها را دوطرفه relay می‌کند.

فقط standard library، پایتون 3.9 به بالا (روی 3.9، 3.11، 3.13 و 3.14 تست شده).

| فایل | کار |
|---|---|
| `server.py` | سرور |
| `gen_cert.sh` | ساخت `cert.pem` / `key.pem` به‌صورت self-signed |
| `test_selftest.py` | self-test خودکار با یک کلاینت yamux مینیمال |

## اجرا

```sh
# plain TCP
python3 server.py --listen 0.0.0.0:9191

# TLS (TLS 1.2+)
./gen_cert.sh                      # CN=localhost, SAN: localhost + 127.0.0.1, ده سال
python3 server.py --listen 0.0.0.0:9191 --tls-cert cert.pem --tls-key key.pem

# گزینه‌ها
#   --keepalive 30   هر ۳۰ ثانیه Ping|SYN؛ اگر تا ۱۰ ثانیه ACK نیاید session بسته می‌شود (پیش‌فرض 0 = خاموش)
#   -v               لاگ DEBUG در سطح frame
```

در سطح INFO برای باز/بسته شدن هر session و برای connect هر stream (مقصد و نتیجه) یک خط لاگ چاپ می‌شود.
با Ctrl-C (یا SIGTERM) سرور به هر session یک `GoAway(0)` می‌فرستد، اتصال‌ها را می‌بندد و با exit code صفر خارج می‌شود.

## self-test

```sh
python3 test_selftest.py           # برای بررسی leak:  python3 -X dev test_selftest.py
```

سرور را in-process روی پورت تصادفی بالا می‌آورد، چند سرور مقصد محلی (echo و ...) می‌سازد و همهٔ
checkها را یک بار روی plain TCP و یک بار روی TLS اجرا می‌کند (cert با `openssl` در زمان تست ساخته می‌شود؛
اگر `openssl` نصب نباشد بخش TLS با پیام واضح skip می‌شود). برای هر check یک خط `PASS` / `FAIL` چاپ می‌شود و
exit code فقط وقتی صفر است که همه pass شوند. کلاینت تست روی هر frame رفتار سرور را هم کنترل می‌کند
(رد نشدن از window، Data حداکثر 32 KiB، برنگرداندن window بیشتر از مصرف‌شده و ...).
checkهای 01 تا 10 همان فهرست خواسته‌شده‌اند و 11 تا 20 اضافه هستند.

## خلاصهٔ پروتکل (برای تطبیق با سمت C++)

### Yamux header — دوازده بایت، big-endian

| فیلد | اندازه | مقدار |
|---|---|---|
| version | 1 | همیشه `0` |
| type | 1 | `0` Data، `1` WindowUpdate، `2` Ping، `3` GoAway |
| flags | 2 | `SYN=1`، `ACK=2`، `FIN=4`، `RST=8` |
| streamID | 4 | stream‌های کلاینت فرد هستند؛ برای Ping/GoAway صفر |
| length | 4 | Data: طول payload — WindowUpdate: delta — Ping: مقدار opaque — GoAway: کد (`0` normal، `1` protocol error، `2` internal error) |

فقط frame نوع Data بعد از header، payload دارد.

### رفتار سرور

| موضوع | رفتار |
|---|---|
| باز کردن stream | کلاینت `WindowUpdate\|SYN` با delta صفر می‌فرستد (SYN روی Data هم پذیرفته می‌شود) و می‌تواند بدون انتظار برای ACK داده بفرستد. جواب سرور: `WindowUpdate\|ACK` با delta صفر |
| flagها | SYN/ACK/FIN/RST هم روی Data و هم روی WindowUpdate پردازش می‌شوند. ترتیب در یک frame: اول SYN، بعد payload یا delta، آخر FIN. اگر RST باشد بقیهٔ frame نادیده گرفته می‌شود |
| window | اولیه 256 KiB برای هر stream در هر جهت. یک Data frame می‌تواند به بزرگی کل window باشد |
| receive flow control | بعد از نوشتن روی سوکت مقصد و برگشتن `drain()`، بایت‌ها «مصرف‌شده» حساب می‌شوند؛ وقتی مجموع برنگشته به 128 KiB برسد یک WindowUpdate با همان مقدار فرستاده می‌شود. بایت‌های خود درخواست SOCKS هم مصرف‌شده حساب می‌شوند (بدون update جداگانه) |
| رد شدن از window | Data بزرگ‌تر از receive window باقی‌مانده → `GoAway(1)` و بستن session |
| send flow control | هیچ‌وقت بیشتر از send window آن stream فرستاده نمی‌شود؛ با window صفر سرور از مقصد هم نمی‌خواند. payload هر Data حداکثر 32 KiB |
| Ping | `Ping\|SYN` → جواب `Ping\|ACK` با همان مقدار. `Ping\|ACK` جواب ping خود سرور است |
| GoAway دریافتی | کد `0`: stream جدید پذیرفته نمی‌شود (SYN بعدی با `WindowUpdate\|RST` رد می‌شود)، stream‌های موجود ادامه می‌دهند. کد غیرصفر: session بسته می‌شود |
| stream ناشناس | frame بدون SYN برای stream ناشناس بی‌صدا دور ریخته می‌شود (همراه payload) |
| خطای پروتکل | SYN برای stream موجود، SYN با ID زوج، version یا type ناشناس → `GoAway(1)` و بستن session |
| half-close | FIN از کلاینت → بعد از flush داده‌های مانده، `write_eof()` روی مقصد. EOF مقصد → `WindowUpdate\|FIN` با delta صفر. stream وقتی حذف می‌شود که هر دو طرف FIN داده باشند |
| RST | RST از کلاینت → abort فوری اتصال مقصد و حذف stream. خطا/reset مقصد → `WindowUpdate\|RST` و حذف stream |
| قطع tunnel | همهٔ اتصال‌های مقصد آن session فوراً abort می‌شوند |
| TCP | `TCP_NODELAY` روی سوکت tunnel و مقصد |

### SOCKS5 داخل هر stream

greeting (method negotiation) فرستاده **نمی‌شود**؛ کلاینت آن را محلی انجام داده است.

| جهت | بایت‌ها |
|---|---|
| کلاینت → سرور | `05 01 00 ATYP ADDR PORT` — ATYP `01`: چهار بایت IPv4، `03`: یک بایت طول + domain، `04`: شانزده بایت IPv6؛ PORT دو بایت big-endian |
| سرور → کلاینت | دقیقاً ده بایت: `05 REP 00 01 00 00 00 00 00 00` |

درخواست می‌تواند بین چند Data frame تکه شده باشد و بایت‌های application می‌توانند بلافاصله (حتی در همان frame)
بعد از آن بیایند؛ این بایت‌ها buffer می‌شوند و بعد از connect موفق به مقصد می‌روند.

| REP | معنی |
|---|---|
| `00` | موفق؛ بعد از آن relay خام دوطرفه |
| `01` | general failure (نسخهٔ SOCKS غلط، domain خالی، FIN وسط درخواست، خطاهای دیگر) |
| `04` | host unreachable (خطای DNS، no route، timeout — مهلت connect پانزده ثانیه) |
| `05` | connection refused |
| `07` | command not supported (`CMD != 01`) |
| `08` | address type not supported |

در هر خطا: reply فرستاده می‌شود، بعد `WindowUpdate|FIN` (نه RST)، و stream وقتی حذف می‌شود که کلاینت FIN یا RST بفرستد.

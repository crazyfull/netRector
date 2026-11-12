#include "raiseFileDescriptorLimit.h"

#include <sys/resource.h>

int raiseFileDescriptorLimit()
{
    constexpr rlim_t MaxTableSize = 1048576;

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 1024;

    if (limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
        getrlimit(RLIMIT_NOFILE, &limit);
    }

    return static_cast<int>(limit.rlim_cur > MaxTableSize ? MaxTableSize : limit.rlim_cur);
}

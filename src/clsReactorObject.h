#ifndef CLSREACTOROBJECT_H
#define CLSREACTOROBJECT_H

// Paye-ye hame object haiy ke reactor malekeshon mishe va ba deleteLater
// (dar entehaye dor halghe) hazf mishan: TCPSocket, UDPSocket, ...
class ReactorObject {
public:
    virtual ~ReactorObject() = default;
};

#include <mutex>
class EpollReactor;
// token-e zende bodan-e reactor baraye thread haye dige: ghabl az post() ba mutex check mishe
struct ReactorLiveToken {
    std::mutex mutex;
    EpollReactor* reactor { nullptr };
};

#endif // CLSREACTOROBJECT_H

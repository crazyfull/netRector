#ifndef CLSREACTOROBJECT_H
#define CLSREACTOROBJECT_H

// Paye-ye hame object haiy ke reactor malekeshon mishe va ba deleteLater
// (dar entehaye dor halghe) hazf mishan: TCPSocket, UDPSocket, ...
class ReactorObject {
public:
    virtual ~ReactorObject() = default;
};

#endif // CLSREACTOROBJECT_H

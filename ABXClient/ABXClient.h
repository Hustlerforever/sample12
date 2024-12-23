
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include "json.hpp"
#include <fstream>
#pragma comment(lib, "Ws2_32.lib")
using json = nlohmann::json;

struct RequestPayload
{
    char CallType;
    char ResendRequest;
};

struct ResponsePayload
{
    char Symbol[4];
    char BuyOrSell;
    int Quantity;
    int Price;
    int Sequence;
};

enum LogType
{
    ERROR_LOG,
    OUTPUT_LOG
};

struct LogEntry
{
    LogType type;
    std::string msg;
};

class CABXClient
{
private:
    SOCKET m_nClientSocket;
    std::queue<LogEntry> m_logQueue;
    std::queue<int> m_qMissingSequences;
    std::unordered_map<int, ResponsePayload*> m_mapResponses;
    int m_nLastSequence;
    int m_nFirstSequence = INT_MAX;
    std::mutex m_logMutex;
    std::condition_variable m_logCV;
    std::thread m_logThread;
    bool m_exitFlag = false;

    void LogWriter();
    void WriteToLog(LogType type, const std::string& msg);

public:
    CABXClient()
    {
        m_logThread = std::thread(&CABXClient::LogWriter, this);
    }
    ~CABXClient()
    {
        m_exitFlag = true;
        m_logCV.notify_all();  // Notify the log thread to exit
        if (m_logThread.joinable())
            m_logThread.join(); // Join the log thread before cleanup

        closesocket(m_nClientSocket);
        WSACleanup();
    }

    void CreateAndBindSocket();
    void SendStreamAllRequest();
    void GetMissingPackets(unsigned char cSeqNo);
    void ReceiveResponse();
    void WriteToJSONFile();
};


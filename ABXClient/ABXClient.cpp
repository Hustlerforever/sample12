
#include "ABXClient.h"
#define SERVER_PORT 3000
#define SERVER_IP "127.0.0.1"

void CABXClient::LogWriter()
{
    try 
    {
     while (!m_exitFlag || !m_logQueue.empty())
    {
        std::unique_lock<std::mutex> lock(m_logMutex);
        m_logCV.wait(lock, [this] { return !m_logQueue.empty() || m_exitFlag; });

       if(!m_logQueue.empty())
        {
            LogEntry log = m_logQueue.front();
            m_logQueue.pop();
            std::ofstream file;
            std::string fileName = (log.type == ERROR_LOG) ? "ABXClientError.log" : "ABXClientOutput.log";
            file.open(fileName, std::ios::app);

            if (file.is_open())
            {
                file << log.msg << std::endl;
                file.close();
            }
            else
            {
                std::cerr << "Unable to open log file: " << fileName << std::endl;
            }
        }
    }
    }
    catch (const std::exception& e) {
        WriteToLog(LogType::ERROR_LOG, "LogWriter exception: " + std::string(e.what()));
    }
}

void CABXClient::WriteToLog(LogType type, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logQueue.push({ type, msg });
    m_logCV.notify_all();  // Notify the log writer thread
}

void CABXClient::SendStreamAllRequest()
{
    try
    {
        char lcPayLoad[3] = { 0 };
        RequestPayload lstPayLoad = { 0 };
        lstPayLoad.CallType = 1;
        lstPayLoad.ResendRequest = 0;
        memcpy(lcPayLoad, &lstPayLoad, 2);
        int bytesSent = send(m_nClientSocket, lcPayLoad, sizeof(lstPayLoad), 0);
        if (bytesSent == SOCKET_ERROR)
        {
            WriteToLog(ERROR_LOG, "Failed to send request: " + std::to_string(WSAGetLastError()));
        }
        else
        {
            WriteToLog(OUTPUT_LOG, "Request for Stream All sent");
        }
    }
    catch (const std::exception& e) {
        WriteToLog(LogType::ERROR_LOG, "SendStreamAllRequest exception: " + std::string(e.what()));
    }
    
}

void CABXClient::GetMissingPackets(unsigned char cSeqNo)
{
    try
    {
        char lcPayLoad[3] = { 0 };
        char lcBuffer[20] = { 0 };
        const int packetSize = 17;
        RequestPayload lstPayLoad = { 0 };
        lstPayLoad.CallType = 2;
        lstPayLoad.ResendRequest = cSeqNo;
        memcpy(lcPayLoad, &lstPayLoad, 2);
        int bytesSent = send(m_nClientSocket, lcPayLoad, sizeof(lstPayLoad), 0);
        if (bytesSent == SOCKET_ERROR)
        {
            WriteToLog(ERROR_LOG, "Failed to send request for missing packets: " + std::to_string(WSAGetLastError()));
        }
        else
        {
            int nSeqNo = (int)cSeqNo;
            WriteToLog(OUTPUT_LOG, "Request for missing packet sent for sequence number: " + std::to_string(nSeqNo));
        }

        int bytesReceived = recv(m_nClientSocket, lcBuffer, packetSize, 0);
        if (bytesReceived == SOCKET_ERROR)
        {
            WriteToLog(ERROR_LOG, "Failed to receive response: " + std::to_string(WSAGetLastError()));
        }
        else if (bytesReceived < packetSize)
        {
            WriteToLog(ERROR_LOG, "Incomplete packet received.");
        }
        else
        {
            ResponsePayload* lstResponsePayLoad = new ResponsePayload;
            lstResponsePayLoad->BuyOrSell = ((ResponsePayload*)lcBuffer)->BuyOrSell;
            lstResponsePayLoad->Quantity = ntohl(*reinterpret_cast<int*>(&lcBuffer[5]));
            lstResponsePayLoad->Price = ntohl(*reinterpret_cast<int*>(&lcBuffer[9]));
            lstResponsePayLoad->Sequence = ntohl(*reinterpret_cast<int*>(&lcBuffer[13]));
            memcpy(lstResponsePayLoad->Symbol, ((ResponsePayload*)lcBuffer)->Symbol, 4);
            m_mapResponses[lstResponsePayLoad->Sequence] = lstResponsePayLoad;
        }
    }
    catch (const std::exception& e) {
        WriteToLog(LogType::ERROR_LOG, "GetMissingPackets exception: " + std::string(e.what()));
    }
}

void CABXClient::ReceiveResponse()
{
    try
    {
        const int packetSize = 17;
        char lcBuffer[20] = { 0 };
        int PrevSequence = 0;
        int reconnAttempt = 0;

        while (true)
        {
            memset(lcBuffer, 0, sizeof(lcBuffer));
            int bytesReceived = recv(m_nClientSocket, lcBuffer, packetSize, 0);
            if (bytesReceived == 0)
            {
                closesocket(m_nClientSocket);
                WSACleanup();
                m_nLastSequence = PrevSequence;
                WriteToLog(OUTPUT_LOG, "Server closed the connection.");
                reconnAttempt++;
                if (!m_qMissingSequences.empty())
                {
                    CreateAndBindSocket();
                    while (!m_qMissingSequences.empty())
                    {
                        GetMissingPackets(m_qMissingSequences.front());
                        m_nFirstSequence = min(m_qMissingSequences.front(), m_nFirstSequence);
                        m_qMissingSequences.pop();
                    }
                }
                break;
            }
            else if (bytesReceived == SOCKET_ERROR)
            {
                WriteToLog(ERROR_LOG, "Failed to receive response: " + std::to_string(WSAGetLastError()));
                reconnAttempt++;
            }

            if (bytesReceived < packetSize)
            {
                WriteToLog(ERROR_LOG, "Incomplete packet received.");
                continue;
            }
            if (reconnAttempt > 5)
            {
                WriteToLog(ERROR_LOG, "Max no of reconnection attempts exceeded! Please connect manually.  ");
                break;
            }
            ResponsePayload* lstResponsePayLoad = new ResponsePayload;
            lstResponsePayLoad->BuyOrSell = ((ResponsePayload*)lcBuffer)->BuyOrSell;
            lstResponsePayLoad->Quantity = ntohl(*reinterpret_cast<int*>(&lcBuffer[5]));
            lstResponsePayLoad->Price = ntohl(*reinterpret_cast<int*>(&lcBuffer[9]));
            lstResponsePayLoad->Sequence = ntohl(*reinterpret_cast<int*>(&lcBuffer[13]));
            memcpy(lstResponsePayLoad->Symbol, ((ResponsePayload*)lcBuffer)->Symbol, 4);
            m_nFirstSequence = min(lstResponsePayLoad->Sequence, m_nFirstSequence);
            while ((PrevSequence + 1) < lstResponsePayLoad->Sequence)
            {
                m_qMissingSequences.push(PrevSequence + 1);
                PrevSequence++;
            }
            PrevSequence = lstResponsePayLoad->Sequence;
            m_mapResponses[lstResponsePayLoad->Sequence] = lstResponsePayLoad;
            
        }
    }
    catch (const std::exception& e) {
        WriteToLog(LogType::ERROR_LOG, "ReceiveResponse exception: " + std::string(e.what()));
    }
}

void CABXClient::CreateAndBindSocket()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        WriteToLog(ERROR_LOG, "WSAStartup failed: " + std::to_string(WSAGetLastError()));
        return;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET)
    {
        WriteToLog(ERROR_LOG, "Socket creation failed: " + std::to_string(WSAGetLastError()));
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr);

    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
    {
        WriteToLog(ERROR_LOG, "Failed to connect to server: " + std::to_string(WSAGetLastError()));
        return;
    }

    WriteToLog(OUTPUT_LOG, "Connected to server.");
    m_nClientSocket = clientSocket;
}

void CABXClient::WriteToJSONFile()
{
    ResponsePayload* lstResponse = nullptr;
    json responseArray = json::array();
    for (int i = m_nFirstSequence; i <= m_nLastSequence; i++)
    {
        lstResponse = (ResponsePayload*)m_mapResponses[i];
        char lcSymbol[5] = { 0 };
        char lcBuyOrSymbol[2] = { 0 };
        memcpy(lcSymbol, lstResponse->Symbol, 4);
        json packet = {
            {"symbol", std::string(lcSymbol)},
            {"buy_sell", std::string(1, lstResponse->BuyOrSell)},
            {"quantity", lstResponse->Quantity},
            {"price", lstResponse->Price},
            {"sequence", lstResponse->Sequence}
        };
        responseArray.push_back(packet);
        delete lstResponse;
    }
    std::ofstream file("responses.json", std::ios::app);
    try {
        file << responseArray.dump(4);
        std::string lstrMsg = "JSON data written to file successfully.";
        std::cout << lstrMsg<<std::endl;
        WriteToLog(OUTPUT_LOG, lstrMsg);
    }
    catch (const std::exception& e) {
        WriteToLog(ERROR_LOG, "Exception while writing JSON to file: " + std::string(e.what()));
    }
    file.close();
}

int main()
{
    CABXClient lABXClientObj;
   
    while (1)
    {
        std::cout << "1. Stream all responses" << std::endl;
        std::cout << "2. Exit" << std::endl;
        int choice;
        std::cin >> choice;
        switch (choice)
        {
        case 1:
            lABXClientObj.CreateAndBindSocket();
            lABXClientObj.SendStreamAllRequest();
            lABXClientObj.ReceiveResponse();
            lABXClientObj.WriteToJSONFile();
            break;
        case 2:
            return 0;
            break;
        default:
            std::cout << "Invalid choice" << std::endl;
        }
    }

    return 0;
}


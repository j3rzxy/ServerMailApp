#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <thread>
#include <mutex>
#include <sstream>
#include <filesystem>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

const int PORT = 8080;
const int BUF_SIZE = 8192;

vector<SOCKET> clients;
mutex clients_mutex;
string chat_history = "";

void save_history(const string& msg) {
    chat_history += msg + "\n";
    ofstream out("chat_history.txt", ios::app);
    out << msg << endl;
}

int count_spaces(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return -1;
    int count = 0;
    char ch;
    while (file.get(ch)) {
        if (ch == ' ') count++;
    }
    return count;
}

void handle_client(SOCKET client_sock, int client_id) {
    char buffer[BUF_SIZE];
    string client_name = "Client" + to_string(client_id);

    while (true) {
        int bytes = recv(client_sock, buffer, BUF_SIZE, 0);
        if (bytes <= 0) break;

        string cmd(buffer, bytes);
        stringstream ss(cmd);
        string action;
        ss >> action;

        if (action == "msg" || action == "broadcast") {
            string msg;
            getline(ss, msg);
            string full = client_name + ": " + msg;
            save_history(full);

            lock_guard<mutex> lock(clients_mutex);
            for (auto& c : clients) {
                if (c != client_sock) send(c, full.c_str(), full.size(), 0);
            }
            cout << full << endl;
        }
        else if (action == "send_file") {
            // Приём файла
            string filename, size_str;
            ss >> filename >> size_str;
            long long fsize = stoll(size_str);

            ofstream file("server_files/" + filename, ios::binary);
            long long received = 0;
            while (received < fsize) {
                int r = recv(client_sock, buffer, min(BUF_SIZE, (int)(fsize - received)), 0);
                if (r <= 0) break;
                file.write(buffer, r);
                received += r;
            }
            file.close();
            string resp = "File " + filename + " got.\n";
            send(client_sock, resp.c_str(), resp.size(), 0);
        }
        else if (action == "get_file") {
            string filename;
            ss >> filename;
            ifstream file("server_files/" + filename, ios::binary | ios::ate);
            if (!file.is_open()) {
                string err = "ERROR: File not found\n";
                send(client_sock, err.c_str(), err.size(), 0);
                continue;
            }
            long long fsize = file.tellg();
            file.seekg(0);

            string header = "FILE " + filename + " " + to_string(fsize) + "\n";
            send(client_sock, header.c_str(), header.size(), 0);

            char fbuf[BUF_SIZE];
            while (file) {
                file.read(fbuf, BUF_SIZE);
                int r = file.gcount();
                send(client_sock, fbuf, r, 0);
            }
            file.close();
        }
        else if (action == "count_spaces") {
            string filename;
            ss >> filename;
            int spaces = count_spaces("server_files/" + filename);
            string resp = (spaces >= 0) ?
                "SPACES: " + to_string(spaces) + "\n" :
                "ERROR: Cannot open file\n";
            send(client_sock, resp.c_str(), resp.size(), 0);
        }
        // ... другие команды
    }

    // Отключение
    closesocket(client_sock);
    lock_guard<mutex> lock(clients_mutex);
    clients.erase(remove(clients.begin(), clients.end(), client_sock), clients.end());
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_sock, SOMAXCONN);

    cout << "Server launched on port " << PORT << endl;
        filesystem::create_directory("server_files");

    int client_id = 0;
    while (true) {
        SOCKET client = accept(server_sock, nullptr, nullptr);
        {
            lock_guard<mutex> lock(clients_mutex);
            clients.push_back(client);
        }
        thread(handle_client, client, ++client_id).detach();
        cout << "Client " << client_id << " connected" << endl;
    }

    closesocket(server_sock);
    WSACleanup();
    return 0;
};
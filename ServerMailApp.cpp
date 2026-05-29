#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <thread>
#include <mutex>
#include <sstream>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

const int PORT = 8080;
const int BUF_SIZE = 8192;

// Client structure
struct Client {
    SOCKET sock;
    int    id;
    string name;
};

vector<Client> clients;
mutex          clients_mutex;

// ─── Save message to history file ────────────────────────────────────────────
void save_history(const string& msg) {
    ofstream out("chat_history.txt", ios::app);
    out << msg << "\n";
}

// ─── Broadcast to all clients (exclude one socket, INVALID_SOCKET = send to all) ─
void broadcast(const string& msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients) {
        if (c.sock != exclude)
            send(c.sock, msg.c_str(), (int)msg.size(), 0);
    }
}

// ─── Send to a single client ─────────────────────────────────────────────────
void send_to(SOCKET sock, const string& msg) {
    send(sock, msg.c_str(), (int)msg.size(), 0);
}

// ─── Get client name by socket ───────────────────────────────────────────────
string get_name(SOCKET sock) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients)
        if (c.sock == sock) return c.name;
    return "Unknown";
}

// ─── Set client name by socket ───────────────────────────────────────────────
void set_name(SOCKET sock, const string& name) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients)
        if (c.sock == sock) { c.name = name; return; }
}

// ─── Count spaces in a file ──────────────────────────────────────────────────
int count_spaces(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return -1;
    int count = 0;
    char ch;
    while (file.get(ch))
        if (ch == ' ') count++;
    return count;
}

// ─── Handle a single client ──────────────────────────────────────────────────
void handle_client(SOCKET client_sock, int client_id) {
    char   buffer[BUF_SIZE];
    string client_name = "Client" + to_string(client_id); // default until set_name

    while (true) {
        int bytes = recv(client_sock, buffer, BUF_SIZE - 1, 0);
        if (bytes <= 0) goto disconnect;
        buffer[bytes] = '\0';

        string raw(buffer, bytes);
        stringstream ss(raw);
        string action;
        ss >> action;

        // ── Set username ─────────────────────────────────────────────────────
        if (action == "set_name") {
            string new_name;
            getline(ss, new_name);
            if (!new_name.empty() && new_name[0] == ' ')
                new_name = new_name.substr(1);
            if (!new_name.empty()) {
                client_name = new_name;
                set_name(client_sock, client_name);
            }
            string join_msg = ">>> " + client_name + " joined the chat";
            cout << join_msg << "\n";
            save_history(join_msg);
            broadcast(join_msg); // including the sender
            continue;
        }

        // ── Regular message ──────────────────────────────────────────────────
        if (action == "msg" || action == "broadcast") {
            string text;
            getline(ss, text);
            if (!text.empty() && text[0] == ' ') text = text.substr(1);

            string full = client_name + ": " + text;
            cout << full << "\n";
            save_history(full);

            // Send to all except sender (sender already echoed locally)
            // To echo back to sender too, replace client_sock with INVALID_SOCKET
            broadcast(full, client_sock);
            continue;
        }

        // ── Receive file ─────────────────────────────────────────────────────
        if (action == "send_file") {
            string filename, size_str;
            ss >> filename >> size_str;
            long long fsize = stoll(size_str);

            // Find end of header line — rest of buffer is file body
            size_t nl = raw.find('\n');
            long long received = 0;

            ofstream out_file("server_files/" + filename, ios::binary);

            if (nl != string::npos && nl + 1 < raw.size()) {
                size_t body_start = nl + 1;
                size_t body_len = raw.size() - body_start;
                out_file.write(buffer + body_start, body_len);
                received += body_len;
            }

            while (received < fsize) {
                int r = recv(client_sock, buffer,
                    (int)min((long long)BUF_SIZE, fsize - received), 0);
                if (r <= 0) break;
                out_file.write(buffer, r);
                received += r;
            }
            out_file.close();

            string resp = "[File '" + filename + "' saved on server]\n";
            send_to(client_sock, resp);
            cout << "Received file '" << filename << "' from " << client_name << "\n";
            continue;
        }

        // ── Send file to client ──────────────────────────────────────────────
        if (action == "get_file") {
            string filename;
            ss >> filename;
            ifstream in_file("server_files/" + filename, ios::binary | ios::ate);
            if (!in_file.is_open()) {
                send_to(client_sock, "ERROR: File not found\n");
                continue;
            }
            long long fsize = in_file.tellg();
            in_file.seekg(0);

            string header = "FILE " + filename + " " + to_string(fsize) + "\n";
            send_to(client_sock, header);

            char fbuf[BUF_SIZE];
            while (in_file) {
                in_file.read(fbuf, BUF_SIZE);
                int r = (int)in_file.gcount();
                if (r > 0) send(client_sock, fbuf, r, 0);
            }
            in_file.close();
            continue;
        }

        // ── Count spaces ─────────────────────────────────────────────────────
        if (action == "count_spaces") {
            string filename;
            ss >> filename;
            int spaces = count_spaces("server_files/" + filename);
            string resp = (spaces >= 0)
                ? "SPACES in '" + filename + "': " + to_string(spaces) + "\n"
                : "ERROR: Cannot open file\n";
            send_to(client_sock, resp);
            continue;
        }
    }

disconnect:
    string leave_msg = ">>> " + client_name + " left the chat";
    cout << leave_msg << "\n";
    save_history(leave_msg);
    {
        lock_guard<mutex> lock(clients_mutex);
        clients.erase(
            remove_if(clients.begin(), clients.end(),
                [&](const Client& c) { return c.sock == client_sock; }),
            clients.end());
    }
    broadcast(leave_msg);
    closesocket(client_sock);
}

// ─── Server console input — lets the server send messages to all clients ─────
void server_input_loop() {
    string line;
    while (getline(cin, line)) {
        if (line.empty()) continue;
        string msg = "[Server]: " + line;
        cout << msg << "\n";
        save_history(msg);
        broadcast(msg);
    }
}

// ─── Print all local IPv4 addresses ──────────────────────────────────────────
void print_local_ips() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return;

    addrinfo hints{}, * res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return;

    cout << "  Your IP addresses (share one of these with the other PC):\n";
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char ip[INET_ADDRSTRLEN];
        sockaddr_in* sa = (sockaddr_in*)p->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        // Skip loopback
        if (string(ip) != "127.0.0.1")
            cout << "    --> " << ip << "\n";
    }
    freeaddrinfo(res);
    cout << "  (Use 127.0.0.1 only if both programs are on the same PC)\n\n";
}

// ─── Open port in Windows Firewall (requires admin rights) ───────────────────
void open_firewall_port() {
    string rule_check = "netsh advfirewall firewall show rule name=\"ChatServer_" +
        to_string(PORT) + "\" > nul 2>&1";
    if (system(rule_check.c_str()) == 0) return; // rule already exists

    string cmd = "netsh advfirewall firewall add rule "
        "name=\"ChatServer_" + to_string(PORT) + "\" "
        "protocol=TCP dir=in localport=" + to_string(PORT) +
        " action=allow > nul 2>&1";

    if (system(cmd.c_str()) == 0)
        cout << "  [Firewall] Port " << PORT << " opened successfully.\n";
    else
        cout << "  [Firewall] Could not open port automatically.\n"
        << "  Run as Administrator, or add the rule manually:\n"
        << "    netsh advfirewall firewall add rule name=\"ChatServer\" "
        << "protocol=TCP dir=in localport=" << PORT << " action=allow\n";
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cout << "Winsock initialization error!\n";
        return 1;
    }

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        cout << "Socket creation error!\n";
        WSACleanup();
        return 1;
    }

    // Allow port reuse after restart
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cout << "Bind error: " << WSAGetLastError() << "\n";
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    listen(server_sock, SOMAXCONN);
    filesystem::create_directory("server_files");

    cout << "=== Server started on port " << PORT << " ===\n\n";
    open_firewall_port();
    print_local_ips();
    cout << "Type a message and press Enter to broadcast to all clients.\n\n";

    // Thread for server-side console input
    thread(server_input_loop).detach();

    int client_id = 0;
    while (true) {
        SOCKET client = accept(server_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        {
            lock_guard<mutex> lock(clients_mutex);
            clients.push_back({ client, ++client_id, "Client" + to_string(client_id) });
        }

        cout << "[New connection] Client" << client_id << "\n";
        thread(handle_client, client, client_id).detach();
    }

    closesocket(server_sock);
    WSACleanup();
    return 0;
}
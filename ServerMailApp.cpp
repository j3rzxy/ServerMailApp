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

// Структура клиента
struct Client {
    SOCKET  sock;
    int     id;
    string  name;
};

vector<Client> clients;
mutex          clients_mutex;

// ─── Сохранение истории ───────────────────────────────────────────────────────
void save_history(const string& msg) {
    ofstream out("chat_history.txt", ios::app);
    out << msg << "\n";
}

// ─── Рассылка всем (кроме исключённого сокета, INVALID_SOCKET = всем) ─────────
void broadcast(const string& msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients) {
        if (c.sock != exclude)
            send(c.sock, msg.c_str(), (int)msg.size(), 0);
    }
}

// ─── Отправка одному клиенту ──────────────────────────────────────────────────
void send_to(SOCKET sock, const string& msg) {
    send(sock, msg.c_str(), (int)msg.size(), 0);
}

// ─── Найти имя клиента по сокету ─────────────────────────────────────────────
string get_name(SOCKET sock) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients)
        if (c.sock == sock) return c.name;
    return "Unknown";
}

// ─── Задать имя клиента ───────────────────────────────────────────────────────
void set_name(SOCKET sock, const string& name) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto& c : clients)
        if (c.sock == sock) { c.name = name; return; }
}

// ─── Подсчёт пробелов ────────────────────────────────────────────────────────
int count_spaces(const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) return -1;
    int count = 0;
    char ch;
    while (file.get(ch))
        if (ch == ' ') count++;
    return count;
}

// ─── Обработка одного клиента ────────────────────────────────────────────────
void handle_client(SOCKET client_sock, int client_id) {
    char   buffer[BUF_SIZE];
    string client_name = "Client" + to_string(client_id);   // имя до set_name

    // Ждём set_name или начинаем без имени
    while (true) {
        int bytes = recv(client_sock, buffer, BUF_SIZE - 1, 0);
        if (bytes <= 0) goto disconnect;
        buffer[bytes] = '\0';

        string raw(buffer, bytes);
        stringstream ss(raw);
        string action;
        ss >> action;

        // ── Смена имени ──────────────────────────────────────────────────────
        if (action == "set_name") {
            string new_name;
            getline(ss, new_name);
            if (!new_name.empty() && new_name[0] == ' ')
                new_name = new_name.substr(1);
            if (!new_name.empty()) {
                client_name = new_name;
                set_name(client_sock, client_name);
            }
            // Уведомление для всех
            string join_msg = ">>> " + client_name + " вошёл в чат";
            cout << join_msg << "\n";
            save_history(join_msg);
            broadcast(join_msg);                // включая самого вошедшего
            continue;
        }

        // ── Обычное сообщение ────────────────────────────────────────────────
        if (action == "msg" || action == "broadcast") {
            string text;
            getline(ss, text);
            if (!text.empty() && text[0] == ' ') text = text.substr(1);

            string full = client_name + ": " + text;
            cout << full << "\n";
            save_history(full);

            // Рассылаем ВСЕМ (включая отправителя — он уже показал локально,
            // но другие клиенты должны видеть; отправитель получит подтверждение)
            // Если не хотите дублировать у отправителя, замените INVALID_SOCKET
            // на client_sock в строке ниже.
            broadcast(full, client_sock);  // всем, кроме отправителя (он эхо показал сам)
            continue;
        }

        // ── Приём файла ──────────────────────────────────────────────────────
        if (action == "send_file") {
            string filename, size_str;
            ss >> filename >> size_str;
            long long fsize = stoll(size_str);

            // Остаток заголовка в буфере после \n — это уже начало файла
            // Найдём позицию конца заголовка
            size_t nl = raw.find('\n');
            long long received = 0;

            ofstream out_file("server_files/" + filename, ios::binary);

            // Если часть тела файла уже в буфере
            if (nl != string::npos && nl + 1 < raw.size()) {
                size_t body_start = nl + 1;
                size_t body_len = raw.size() - body_start;
                out_file.write(buffer + body_start, body_len);
                received += body_len;
            }

            // Читаем остаток
            while (received < fsize) {
                int r = recv(client_sock, buffer,
                    (int)min((long long)BUF_SIZE, fsize - received), 0);
                if (r <= 0) break;
                out_file.write(buffer, r);
                received += r;
            }
            out_file.close();

            string resp = "[Файл '" + filename + "' сохранён на сервере]\n";
            send_to(client_sock, resp);
            cout << "Получен файл '" << filename << "' от " << client_name << "\n";
            continue;
        }

        // ── Отдача файла клиенту ─────────────────────────────────────────────
        if (action == "get_file") {
            string filename;
            ss >> filename;
            ifstream in_file("server_files/" + filename, ios::binary | ios::ate);
            if (!in_file.is_open()) {
                send_to(client_sock, "ERROR: Файл не найден\n");
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

        // ── Подсчёт пробелов ─────────────────────────────────────────────────
        if (action == "count_spaces") {
            string filename;
            ss >> filename;
            int spaces = count_spaces("server_files/" + filename);
            string resp = (spaces >= 0)
                ? "SPACES в '" + filename + "': " + to_string(spaces) + "\n"
                : "ERROR: Не удаётся открыть файл\n";
            send_to(client_sock, resp);
            continue;
        }
    }

disconnect:
    // Уведомляем всех об уходе
    string leave_msg = ">>> " + client_name + " покинул чат";
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

// ─── Поток ввода сообщений от имени сервера ───────────────────────────────────
void server_input_loop() {
    string line;
    while (getline(cin, line)) {
        if (line.empty()) continue;
        string msg = "[Сервер]: " + line;
        cout << msg << "\n";
        save_history(msg);
        broadcast(msg);
    }
}

// ─── Точка входа ─────────────────────────────────────────────────────────────
int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cout << "Ошибка инициализации Winsock!\n";
        return 1;
    }

    SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) {
        cout << "Ошибка создания сокета!\n";
        WSACleanup();
        return 1;
    }

    // Разрешить повторное использование порта
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cout << "Ошибка bind: " << WSAGetLastError() << "\n";
        closesocket(server_sock);
        WSACleanup();
        return 1;
    }

    listen(server_sock, SOMAXCONN);
    filesystem::create_directory("server_files");

    cout << "=== Сервер запущен на порту " << PORT << " ===\n";
    cout << "Введите сообщение и нажмите Enter, чтобы написать всем клиентам.\n\n";

    // Поток для ввода сообщений сервером
    thread(server_input_loop).detach();

    int client_id = 0;
    while (true) {
        SOCKET client = accept(server_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        {
            lock_guard<mutex> lock(clients_mutex);
            clients.push_back({ client, ++client_id, "Client" + to_string(client_id) });
        }

        cout << "[Новое подключение] Client" << client_id << "\n";
        thread(handle_client, client, client_id).detach();
    }

    closesocket(server_sock);
    WSACleanup();
    return 0;
}
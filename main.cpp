#include <modbus/modbus.h>
#include <iostream>
#include <cstring>
#include <csignal>
#include <unistd.h>   // for close() og sleep()
#include <cstdlib>    // for exit()
#include <cerrno>     // for errno
#include <thread>     // for std::this_thread::sleep_for og std::thread
#include <chrono>     // for std::chrono::milliseconds

modbus_t *ctx;
modbus_mapping_t *mb_mapping;
int server_socket = -1;
int previous_coil_value = -1;  // For at holde styr på den tidligere værdi af Coil[0]

// Funktion til at rydde op ved CTRL+C
void cleanup(int signum) {
    std::cout << "\nCleaning up and exiting...\n";
    if (server_socket != -1) close(server_socket);
    if (ctx) modbus_free(ctx);
    if (mb_mapping) modbus_mapping_free(mb_mapping);
    exit(0);
}

// 🔁 Terminal-input tråd
void terminal_input_thread() {
    std::string input;
    while (true) {
        std::getline(std::cin, input);
        if (!input.empty()) {
            std::cout << "🧑 Terminalen skrev: " << input << std::endl;

            if (input == "start") {
                mb_mapping->tab_bits[0] = 1;
                std::cout << "✅ Coil[1] sat til 1 via terminalen\n";
            } else if (input == "stop") {
                mb_mapping->tab_bits[0] = 0;
                std::cout << "🛑 Coil[1] sat til 0 via terminalen\n";
            } else if (input == "exit") {
                cleanup(0);
            } else {
                std::cout << "ℹ️ Ukendt kommando. Brug: start, stop, exit\n";
            }
        }
    }
}

int main() {
    signal(SIGINT, cleanup);

    // Opret ny Modbus TCP server på port 502
    ctx = modbus_new_tcp("0.0.0.0", 502);
    if (ctx == nullptr) {
        std::cerr << "Unable to create Modbus context\n";
        return -1;
    }

    // Tildel én coil og ingen input/output registers
    mb_mapping = modbus_mapping_new(1, 0, 0, 0);
    if (mb_mapping == nullptr) {
        std::cerr << "Failed to allocate mapping: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return -1;
    }

    // Opret socket
    server_socket = modbus_tcp_listen(ctx, 1);
    if (server_socket == -1) {
        std::cerr << "Unable to listen on port: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        modbus_mapping_free(mb_mapping);
        return -1;
    }

    std::cout << "✅ Modbus TCP Server is running on port 502...\n";

    // Accepter forbindelse
    modbus_tcp_accept(ctx, &server_socket);

    // 🔁 Start terminal-input tråd
    std::thread t_input(terminal_input_thread);
    t_input.detach();

    // Modtag og håndtér forespørgsler
    while (true) {
        uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
        int rc = modbus_receive(ctx, query);

        if (rc > 0) {
            modbus_reply(ctx, query, rc, mb_mapping);

            // Udskriv ændring på coil[0]
            if (mb_mapping->tab_bits[0] != previous_coil_value) {
                std::cout << "📥 Coil[0] er nu: " << (int)mb_mapping->tab_bits[0] << std::endl;
                previous_coil_value = mb_mapping->tab_bits[0];
            }
        } else if (rc == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    cleanup(0);
    return 0;
}

#include <modbus/modbus.h>
#include <sqlite3.h>
#include <iostream>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <cstdlib>
#include <cerrno>
#include <thread>
#include <chrono>
#include <pigpio.h>  

/*
Kompiler:
	g++ -std=c++17 -o modbusMedSQL modbusMedSQL.cpp -lmodbus -lsqlite3 -lpthread -lpigpio
Kør: 
	sudo ./modbusMedSwitch
*/




modbus_t *ctx = nullptr;
modbus_mapping_t *mb_mapping = nullptr;
int server_socket = -1;
int previous_coil_value = -1;
int previous_input_value = -1;
sqlite3* db = nullptr;
char* errMsg = nullptr;

// GPIO konstanter
const int GPIO_24 = 24; // Åben-sensor (input)
const int GPIO_26 = 26; // Lukket-sensor (input)
const int GPIO_8 = 8;   // Motor: Åben (output)
const int GPIO_7 = 7;   // Motor: Lukke (output)

void cleanup(int signum) {
    std::cout << "\nCleaning up and exiting...\n";
    if (server_socket != -1) close(server_socket);
    if (ctx) modbus_free(ctx);
    if (mb_mapping) modbus_mapping_free(mb_mapping);
    if (db) sqlite3_close(db);
    gpioTerminate();
    exit(0);
}

void terminal_input_thread() {
    std::string input;
    while (true) {
        std::getline(std::cin, input);
        if (!input.empty()) {
            if (input == "start") {
                mb_mapping->tab_bits[0] = 1;
                std::cout << "Coil[0] sat til 1\n";
            } else if (input == "stop") {
                mb_mapping->tab_bits[0] = 0;
                std::cout << "Coil[0] sat til 0\n";
            } else if (input == "in1") {
                mb_mapping->tab_input_bits[0] = 1;
                std::cout << "Input[0] sat til 1\n";
            } else if (input == "in0") {
                mb_mapping->tab_input_bits[0] = 0;
                std::cout << "Input[0] sat til 0\n";
            } else if (input.rfind("addbox ", 0) == 0) {
                std::string size = input.substr(7);
                std::string sqlInsert = "INSERT INTO box (size) VALUES ('" + size + "');";
                int rc = sqlite3_exec(db, sqlInsert.c_str(), nullptr, nullptr, &errMsg);
                if (rc != SQLITE_OK) {
                    std::cerr << "Error during insert: " << errMsg << std::endl;
                    sqlite3_free(errMsg);
                } else {
                    std::cout << "Box inserted med size: " << size << std::endl;
                }
            } else if (input == "exit") {
                cleanup(0);
            } else {
                std::cout << "Ukendt kommando! Brug: start, stop, in1, in0, addbox <size>, exit\n";
            }
        }
    }
}

int main() {
    // ---- OPRETTELSE AF SQL-DATABASE ----
    int rc = sqlite3_open("my_database.db", &db);
    if (rc) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    // Opret tabel hvis den ikke findes
    const char* sqlCreate = "CREATE TABLE IF NOT EXISTS box("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "size TEXT NOT NULL);";
    rc = sqlite3_exec(db, sqlCreate, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating table: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return 1;
    }

    // ---- Initialiser pigpio ----
    if (gpioInitialise() < 0) {
        std::cerr << "pigpio initialization failed!" << std::endl;
        return 1;
    }
    gpioSetMode(GPIO_24, PI_INPUT);
    gpioSetMode(GPIO_26, PI_INPUT);
    gpioSetMode(GPIO_8, PI_OUTPUT);
    gpioSetMode(GPIO_7, PI_OUTPUT);

    signal(SIGINT, cleanup);

    // ---- OPRETTELSE AF TCP-SERVER ----
    ctx = modbus_new_tcp("0.0.0.0", 502);
    if (!ctx) {
        std::cerr << "Kunne ikke oprette Modbus context\n";
        return -1;
    }

    // Alloker coils og discrete inputs
    mb_mapping = modbus_mapping_new(
        1,  // 1 Discrete output (coils)
        1,  // 1 Discrete input
        0, 0
    );
    if (!mb_mapping) {
        std::cerr << "Mapping fejl: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return -1;
    }

    server_socket = modbus_tcp_listen(ctx, 1);
    if (server_socket == -1) {
        std::cerr << "Lytte-fejl: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        modbus_mapping_free(mb_mapping);
        return -1;
    }
    std::cout << "Modbus TCP Server kører på port 502...\n";
    modbus_tcp_accept(ctx, &server_socket);

    std::thread t_input(terminal_input_thread);
    t_input.detach();

    // Modtag og håndtér forespørgsler
    while (true) {
        uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
        int rc = modbus_receive(ctx, query);

        // --- GPIO styring ---
        int val24 = gpioRead(GPIO_24);  // Åben-sensor
        int val26 = gpioRead(GPIO_26);  // Lukket-sensor

        // Stop begge motorer før beslutning
        gpioWrite(GPIO_8, 0);
        gpioWrite(GPIO_7, 0);

        // Gripper-logik:
        // Coil[0] = 0 -> Åben, hvis ikke allerede åben (val26=1)
        // Coil[0] = 1 -> Luk, hvis ikke allerede lukket (val24=1)
        if (mb_mapping->tab_bits[0] == 0 && val26) {
            std::cout << "Åbner gripper\n";
            gpioWrite(GPIO_8, 0);
            gpioWrite(GPIO_7, 1);
        } else if (mb_mapping->tab_bits[0] == 1 && val24) {
            std::cout << "Lukker gripper\n";
            gpioWrite(GPIO_7, 0);
            gpioWrite(GPIO_8, 1);
        }

        // Sæt input[0] = 1 hvis én af endestop-sensorerne er LOW (dvs. vi har ramt en grænse)
        if (!val26 || !val24) {
            mb_mapping->tab_input_bits[0] = 1;
        } else {
            mb_mapping->tab_input_bits[0] = 0;
        }

        if (rc > 0) {
            modbus_reply(ctx, query, rc, mb_mapping);

            if (mb_mapping->tab_bits[0] != previous_coil_value) {
                std::cout << "Coil[0] ændret til: " << (int)mb_mapping->tab_bits[0] << std::endl;
                previous_coil_value = mb_mapping->tab_bits[0];

                std::string size = std::to_string(mb_mapping->tab_bits[0]);
                std::string sqlInsert = "INSERT INTO box (size) VALUES ('" + size + "');";
                int rc2 = sqlite3_exec(db, sqlInsert.c_str(), nullptr, nullptr, &errMsg);
                if (rc2 != SQLITE_OK) {
                    std::cerr << "Error during insert: " << errMsg << std::endl;
                    sqlite3_free(errMsg);
                } else {
                    std::cout << "Box inserted med size: " << size << std::endl;
                }
            }

            if (mb_mapping->tab_input_bits[0] != previous_input_value) {
                std::cout << "Input[0] er nu: " << (int)mb_mapping->tab_input_bits[0] << std::endl;
                previous_input_value = mb_mapping->tab_input_bits[0];

                std::string size = std::to_string(mb_mapping->tab_input_bits[0]);
                std::string sqlInsert = "INSERT INTO box (size) VALUES ('" + size + "');";
                int rc3 = sqlite3_exec(db, sqlInsert.c_str(), nullptr, nullptr, &errMsg);
                if (rc3 != SQLITE_OK) {
                    std::cerr << "Error during insert: " << errMsg << std::endl;
                    sqlite3_free(errMsg);
                } else {
                    std::cout << "Box inserted med size: " << size << std::endl;
                }
            }
        } else if (rc == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    cleanup(0);
    return 0;
}

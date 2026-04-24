#include "net_parameter.h"
#include "cache_model.h"
#include "parameters.h"
#include "serialize.h"
#include "utils.h"

class client {
public:
    local_model client_table;
    std::vector<_key_t> keys;
    std::vector<_payload_t> payloads;
    std::mutex cout_mutex;
    int retrain = 0;
    std::vector<double> error_rates;
public:
    client() {
        load_data_for_test();
        client_table = local_model();
        client_table.min_key = -50332314890.551338727041233;
        // client_table.max_key = keys[DATA_NUM - 1];
        client_table.max_key = 50332314890.551338727041233;
    }

    ~client() {}

    void run_client(const std::string& file_name) {
        const char* unix_socket_path = nullptr;
        const char* server_host = "127.0.0.1";
        int server_port = PORT_DEFAULT;
        int sockfd = (unix_socket_path != nullptr) ? init_unix_sock(unix_socket_path) : init_tcp_sock(server_host, server_port);
        if (sockfd < 0) return ;

        char recv_buf[MAX_MEM_BUFFER_SIZE];
        std::ifstream infile(file_name);
        std::string command;

        if (!infile.is_open()) {
            perror("The command is not opened!");
            return;
        }

        get_table_param(sockfd);

        int round = 0;
        size_t find_time = 0, find_error = 0;
        double error_rate = 0.0;
        size_t global_error = 0;

        std::getline(infile, command); //除去csv的表头一行
        while (std::getline(infile, command)) {
            round++;
            if (!command.empty()) {
                if (is_exit_command(command)) {
                    std::cout << "The client will be closed" << std::endl;
                    break;
                }

                std::stringstream ss(command);
                std::string token;

                std::getline(ss, token, ',');
                // std::string timestamp = std::stod(token);
    
                std::getline(ss, token, ',');
                int device_id = std::stoi(token);
            
                std::getline(ss, token, ',');
                int target_pos = std::stoi(token);

                std::getline(ss, token, ',');
                std::string operation = token;

                //溢出树这部分由于时间问题我们暂时先不给予考虑
                _key_t target_key = keys[target_pos];
                if (target_key >= client_table.min_key && target_key <= client_table.max_key) {
                    int logic_id = client_table.predict_pos(target_key);
                    command = "findid " + std::to_string(device_id) + " " + std::to_string(target_pos) + " " + std::to_string(logic_id);
                    find_time++;
                }
             
                if (write(sockfd, command.c_str(), command.length() + 1) == -1) {
                    std::cerr << "send error " << errno << ": " << strerror(errno) << std::endl;
                    exit(1);
                }

                int len = recv(sockfd, recv_buf, MAX_MEM_BUFFER_SIZE, 0);

                if (len < 0) {
                    fprintf(stderr, "Connection was broken: %s\n", strerror(errno));
                    break;
                } else if (len == 0) {
                    printf("Connection has been closed\n");
                    break;
                } else {
                    std::string msg(recv_buf);
                    memset(recv_buf, 0, MAX_MEM_BUFFER_SIZE);

                    if (msg == "Failure!") {
                        find_error++;
                        global_error++;
                        error_rate = 1.0 * find_error / find_time;
                    } else if (msg == "Update cache!") {
                        retrain++;
                        get_table_param(sockfd);
                        continue;
                    }

                    if (find_time >= 4e4) {
                        find_error = 0;
                        find_time = 0;
                        retrain++;
                        std::cout<<pthread_self()<<"count "<<retrain<<" error_rate :"<<error_rate<<std::endl;
                        error_rates.push_back(error_rate);
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(cout_mutex);
        write_error_rates_to_file();
        std::cout << "Bye" << std::endl;
        close(sockfd);
    }

private:
    void get_table_param(int sockfd) {
        std::string Init_command = "cache";
        if (write(sockfd, Init_command.c_str(), Init_command.length() + 1) == -1) {
            std::cerr << "send error " << errno << ": " << strerror(errno) << std::endl;
            exit(1);
        }

        // std::vector<t_record> received_table;
        std::vector<std::vector<Param>> received_parameter;
        char buffer[MAX_MEM_BUFFER_SIZE];
        int len = recv(sockfd, buffer, MAX_MEM_BUFFER_SIZE, 0);
        if (len > 0) {
            std::stringstream ss_received;
            ss_received.clear();
            ss_received.write(buffer, len);
            // deserialize_vector(received_table, ss_received); 
            deserialize_parameter(received_parameter, ss_received);

            // client_table.local_table = received_table;
            client_table.parameter = received_parameter;
            // write_data_to_file(received_parameter, retrain);
            // received_table.clear();
            received_parameter.clear();
        }
    }

        inline void write_data_to_file(std::vector<std::vector<Param>>& received_param, int &retrain) {
            std::string filename = "train_record" + std::to_string(pthread_self()) ;
            std::ofstream outfile(filename,std::ios_base::app); 
            if (!outfile.is_open()) {
                std::cerr << "Error opening file!" << std::endl;
                return;
            }
            outfile<<"retrain_times"<<retrain<<'\n';
            // outfile<<"leaf_nums"<<received_table.size()<<'\n';
            for (const auto& param_vector : received_param) {
                for (const auto& param : param_vector) {
                    outfile << "Intercept: " << param.intercept 
                            << ", Slope: " << param.slope
                            << ",first_key"<<param.first_key << std::endl;
                }
            }
            outfile.close();
            std::cout << "Data has been written to the file." << std::endl;
        }


    void load_data_for_test() {
        keys.resize(DATA_NUM);
        payloads.resize(DATA_NUM);
        std::ifstream infile("Data.txt");

        if (!infile) {
            std::cerr << "Failed to open file: Data.txt" << std::endl;
            return;
        }

        _key_t key;
        _payload_t payload;
        int count = 0;
        while (infile >> key >> payload) {
            keys[count] = key;
            payloads[count] = payload;
            count++;
        }
        infile.close();
        std::cout << "Loading finished!" << std::endl;
    }

    void write_error_rates_to_file() {
        // std::lock_guard<std::mutex> lock(cout_mutex);
        std::string filename = "/home/ming/桌面/PLIN-N /PLIN-N/build/train_result.txt";
        std::ofstream outfile(filename, std::ios_base::app);
        outfile << "Thread ID: " << pthread_self() << '\n';
        if (outfile.is_open()) {
            for (const auto& e : error_rates) {
                outfile << e << " ";
            }
            outfile.close();
            std::cout << "Error rates have been written to the file." << std::endl;
        } else {
            std::cerr << "Error opening file to write error rates!" << std::endl;
        }
    }

    bool is_exit_command(const std::string& cmd) {
        return cmd == "exit" || cmd == "exit;" || cmd == "bye" || cmd == "bye;";
    }

    int init_unix_sock(const char *unix_sock_path) {
        int sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (sockfd < 0) {
            fprintf(stderr, "failed to create unix socket. %s", strerror(errno));
            return -1;
        }

        struct sockaddr_un sockaddr;
        memset(&sockaddr, 0, sizeof(sockaddr));
        sockaddr.sun_family = PF_UNIX;
        snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", unix_sock_path);

        if (connect(sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr) < 0)) {
            fprintf(stderr, "failed to connect to server. unix socket path '%s'. error %s", sockaddr.sun_path, strerror(errno));
            close(sockfd);
            return -1;
        }
        return sockfd;
    }

    int init_tcp_sock(const char *server_host, int server_port) {
        struct hostent *host;
        struct sockaddr_in serv_addr;

        if ((host = gethostbyname(server_host)) == NULL) {
            fprintf(stderr, "gethostbyname failed ,errmsg = %d:%s\n", errno, strerror(errno));
            return -1;
        }

        int sockfd;
        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            fprintf(stderr, "create socket error ,errmsg = %d:%s\n", errno, strerror(errno));
            return -1;
        }

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server_port);
        serv_addr.sin_addr = *((struct in_addr *)host->h_addr);
        bzero(&(serv_addr.sin_zero), 8);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) == -1) {
            fprintf(stderr, "Failed to connect. errmsg=%d:%s\n", errno, strerror(errno));
            close(sockfd);
            return -1;
        }
        return sockfd;
    }
};

void concurrency(const std::string file_name) {
    client cl; 
    cl.run_client(file_name);
}

void test() {
    std::vector<std::string> file_name;
    file_name.push_back("command_plus.txt");
    for (int k = 0; k < TEST_THREAD/40; k++) {
        std::string file_ = "command" + std::to_string(k) + ".txt";
        file_name.push_back(file_);
    }

    std::vector<std::thread> threads;

    for (int i = 0; i < TEST_THREAD/40; ++i) {
        threads.push_back(std::thread(concurrency, file_name[i])); 
        // threads.push_back(std::thread(concurrency, "find_command_test"));
    }

    for (auto& t : threads) {
        t.join();
    }
    std::cout << "All clients have finished." << std::endl;
}

int main(void) {
    test();
    return 0;
}

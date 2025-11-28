#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <deque>
#include <ctime>

#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

using namespace std;

void log_packet(const string& log_file, const PacketHeader& header) {
    ofstream log(log_file, ios::app);
    if (log.is_open()) {
        log << (unsigned int)header.type << " " << header.seqNum << " " << header.length << " " << header.checksum << endl;
        log.close();
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        cerr << "Usage: ./wSender <receiver-IP> <receiver-port> <window-size> <input-file> <log>" << endl;
        return 1;
    }

    string receiver_ip = argv[1];
    int receiver_port = atoi(argv[2]);
    int window_size = atoi(argv[3]);
    string input_file = argv[4];
    string log_file = argv[5];

    // Clear log file
    ofstream log_clear(log_file, ios::trunc);
    log_clear.close();

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(receiver_port);
    if (inet_aton(receiver_ip.c_str(), &servaddr.sin_addr) == 0) {
        cerr << "Invalid IP address" << endl;
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50000; // 50ms receive timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    // 1. Handshake START
    srand(time(NULL));
    unsigned int start_seq = rand();
    PacketHeader start_pkt;
    start_pkt.type = PacketType::START;
    start_pkt.seqNum = start_seq;
    start_pkt.length = 0;
    start_pkt.checksum = 0; // No data

    bool connected = false;
    int retry_count = 0;
    while (!connected && retry_count < 10) {
        if (sendto(sockfd, &start_pkt, sizeof(start_pkt), 0, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        {
            perror("sendto failed");
        }
        log_packet(log_file, start_pkt);
        cerr << "[SENDER] Sent START packet, waiting for ACK..." << endl;

        PacketHeader ack_pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        auto start_time = chrono::steady_clock::now();
        while (chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count() < 500) {
            int n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), MSG_DONTWAIT, (struct sockaddr *)&from_addr, &from_len);
            if (n > 0) {
                cerr << "[SENDER] Received packet type " << (unsigned int)ack_pkt.type << " with seqNum " << ack_pkt.seqNum << endl;
                log_packet(log_file, ack_pkt);
                if (ack_pkt.type == PacketType::ACK && ack_pkt.seqNum == start_seq) {
                    connected = true;
                    cerr << "[SENDER] Connection established!" << endl;
                    break;
                }
            }
            usleep(10000); // 10ms sleep to avoid busy waiting
        }
        if (!connected) {
            cerr << "[SENDER] Timeout waiting for ACK, retrying... (attempt " << retry_count+1 << "/10)" << endl;
            retry_count++;
        }
    }
    
    if (!connected) {
        cerr << "[SENDER] Failed to establish connection!" << endl;
        close(sockfd);
        return 1;
    }

    // 2. Read File
    ifstream infile(input_file, ios::binary);
    if (!infile.is_open()) {
        cerr << "Failed to open input file" << endl;
        return 1;
    }

    vector<vector<char>> packets;
    char buffer[WTP_MDS];
    while (infile.read(buffer, WTP_MDS) || infile.gcount() > 0) {
        vector<char> packet_data(buffer, buffer + infile.gcount());
        packets.push_back(packet_data);
        if (infile.eof()) break; // Should be handled by read condition but just in case
    }
    infile.close();

    // 3. Send Data
    int base = 0;
    int next_seq_num = 0;
    int total_packets = packets.size();
    auto timer_start = chrono::steady_clock::now();
    bool timer_running = false;

    while (base < total_packets) {
        // Send packets within window
        while (next_seq_num < base + window_size && next_seq_num < total_packets) {
            size_t data_len = packets[next_seq_num].size();
            size_t packet_len = sizeof(PacketHeader) + data_len;
            vector<char> packet_buf(packet_len);
            
            PacketHeader* hdr = (PacketHeader*)packet_buf.data();
            hdr->type = PacketType::DATA;
            hdr->seqNum = next_seq_num;
            hdr->length = data_len;
            hdr->checksum = crc32(packets[next_seq_num].data(), data_len);
            memcpy(hdr->data, packets[next_seq_num].data(), data_len);

            sendto(sockfd, packet_buf.data(), packet_len, 0, (const struct sockaddr *)&servaddr, sizeof(servaddr));
            log_packet(log_file, *hdr);

            if (base == next_seq_num) {
                timer_start = chrono::steady_clock::now();
                timer_running = true;
            }
            next_seq_num++;
        }

        // Wait for ACKs or Timeout
        PacketHeader ack_pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        int n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&from_addr, &from_len);
        if (n > 0) {
            log_packet(log_file, ack_pkt);
            if (ack_pkt.type == PacketType::ACK) {
                if (ack_pkt.seqNum > base) {
                    base = ack_pkt.seqNum;
                    if (base < next_seq_num) {
                        timer_start = chrono::steady_clock::now();
                        timer_running = true;
                    } else {
                        timer_running = false;
                    }
                }
            }
        }

        // Check Timeout
        if (timer_running && chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - timer_start).count() >= 500) {
            // Timeout, resend window
            // "window moves forward to the first packet that needs to be re-transmitted" -> This is just 'base'
            // "send all packets in the current window"
            next_seq_num = base; // Reset next_seq_num to base to trigger resend loop
            timer_running = false; // Will be restarted in the send loop
        }
    }

    // 4. Handshake END
    PacketHeader end_pkt;
    end_pkt.type = PacketType::END;
    end_pkt.seqNum = start_seq;
    end_pkt.length = 0;
    end_pkt.checksum = 0;

    bool end_acked = false;
    while (!end_acked) {
        sendto(sockfd, &end_pkt, sizeof(end_pkt), 0, (const struct sockaddr *)&servaddr, sizeof(servaddr));
        log_packet(log_file, end_pkt);

        PacketHeader ack_pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        auto start_time = chrono::steady_clock::now();
        while (chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count() < 500) {
            int n = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&from_addr, &from_len);
            if (n > 0) {
                log_packet(log_file, ack_pkt);
                if (ack_pkt.type == PacketType::ACK && ack_pkt.seqNum == start_seq) {
                    end_acked = true;
                    break;
                }
            }
        }
    }

    close(sockfd);
    return 0;
}
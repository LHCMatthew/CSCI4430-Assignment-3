#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <cerrno>

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
    if (argc != 5) {
        cerr << "Usage: ./wReceiver <port-num> <window-size> <output-dir> <log>" << endl;
        return 1;
    }

    int port_num = atoi(argv[1]);
    int window_size = atoi(argv[2]);
    string output_dir = argv[3];
    string log_file = argv[4];

    // Clear log file
    ofstream log_clear(log_file, ios::trunc);
    log_clear.close();

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr, cliaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port_num);

    if (::bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        return 1;
    }

    int file_count = 0;
    char buffer[WTP_MTU]; // Buffer for receiving packet

    cerr << "[RECEIVER] Listening on port " << port_num << " with window size " << window_size << endl;

    while (true) {
        // Wait for START
        socklen_t len = sizeof(cliaddr);
        int n = recvfrom(sockfd, buffer, WTP_MTU, 0, (struct sockaddr *)&cliaddr, &len);
        if (n < 0) {
            cerr << "[RECEIVER] recvfrom error: " << errno << endl;
            continue;
        }

        PacketHeader* hdr = (PacketHeader*)buffer;
        cerr << "[RECEIVER] Received packet type " << (unsigned int)hdr->type << " seqNum " << hdr->seqNum << " from " << inet_ntoa(cliaddr.sin_addr) << endl;
        log_packet(log_file, *hdr);

        if ((unsigned int)hdr->type == (unsigned int)PacketType::START) {
            unsigned int start_seq = hdr->seqNum;
            
            // Send ACK for START
            PacketHeader ack_pkt;
            ack_pkt.type = PacketType::ACK;
            ack_pkt.seqNum = start_seq;
            ack_pkt.length = 0;
            ack_pkt.checksum = 0;
            sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (const struct sockaddr *)&cliaddr, len);
            log_packet(log_file, ack_pkt);

            // Open file
            string filename = output_dir + "/FILE-" + to_string(file_count) + ".out";
            ofstream outfile(filename, ios::binary);
            if (!outfile.is_open()) {
                cerr << "Failed to open output file: " << filename << endl;
                continue;
            }

            unsigned int expected_seq_num = 0;
            map<unsigned int, vector<char>> packet_buffer;

            while (true) {
                n = recvfrom(sockfd, buffer, WTP_MTU, 0, (struct sockaddr *)&cliaddr, &len);
                if (n < 0) continue;

                hdr = (PacketHeader*)buffer;
                log_packet(log_file, *hdr);

                // Check CRC
                if (hdr->length > 0) {
                    unsigned int calc_crc = crc32(hdr->data, hdr->length);
                    if (calc_crc != hdr->checksum) {
                        // Drop packet
                        continue;
                    }
                }

                if ((unsigned int)hdr->type == (unsigned int)PacketType::DATA) {
                    if (hdr->seqNum >= expected_seq_num + window_size) {
                        // Drop
                        continue;
                    }

                    if (hdr->seqNum == expected_seq_num) {
                        // Write to file
                        outfile.write(hdr->data, hdr->length);
                        expected_seq_num++;

                        // Check buffer
                        while (packet_buffer.count(expected_seq_num)) {
                            vector<char>& data = packet_buffer[expected_seq_num];
                            outfile.write(data.data(), data.size());
                            packet_buffer.erase(expected_seq_num);
                            expected_seq_num++;
                        }
                    } else if (hdr->seqNum > expected_seq_num) {
                        // Buffer
                        vector<char> data(hdr->data, hdr->data + hdr->length);
                        packet_buffer[hdr->seqNum] = data;
                    }
                    
                    // Send Cumulative ACK (Next Expected)
                    PacketHeader ack_pkt;
                    ack_pkt.type = PacketType::ACK;
                    ack_pkt.seqNum = expected_seq_num;
                    ack_pkt.length = 0;
                    ack_pkt.checksum = 0;
                    sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (const struct sockaddr *)&cliaddr, len);
                    log_packet(log_file, ack_pkt);

                } else if ((unsigned int)hdr->type == (unsigned int)PacketType::END) {
                    if (hdr->seqNum == start_seq) {
                        // Send ACK
                        PacketHeader ack_pkt;
                        ack_pkt.type = PacketType::ACK;
                        ack_pkt.seqNum = start_seq;
                        ack_pkt.length = 0;
                        ack_pkt.checksum = 0;
                        sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (const struct sockaddr *)&cliaddr, len);
                        log_packet(log_file, ack_pkt);
                        
                        outfile.close();
                        file_count++;
                        break; // End connection
                    }
                } else if ((unsigned int)hdr->type == (unsigned int)PacketType::START) {
                    if (hdr->seqNum == start_seq) {
                         PacketHeader ack_pkt;
                        ack_pkt.type = PacketType::ACK;
                        ack_pkt.seqNum = start_seq;
                        ack_pkt.length = 0;
                        ack_pkt.checksum = 0;
                        sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (const struct sockaddr *)&cliaddr, len);
                        log_packet(log_file, ack_pkt);
                    }
                }
            }
        }
    }

    close(sockfd);
    return 0;
}
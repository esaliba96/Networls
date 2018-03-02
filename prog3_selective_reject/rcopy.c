#include "srej.h"
#include "networks.h"
#include "cpe464.h"

typedef enum state STATE;

enum state {
	DONE, FILENAME, RECV_DATA, FILE_OK, START, END_FILE, SREJ_BLOCK
};

void run_client(char* local_file, char* remote_file, int32_t window_size, int32_t buffer_size, char* host, int16_t port);
void check_args(int argc, char** argv);
STATE start_state(char* host, uint16_t port, Connection *server);
STATE filename(char* fname, int32_t buff_size, int32_t window_size, Connection* server);
STATE file_ok(int* out_file_fd, char* out_file_name);
STATE recv_data(int32_t out_fd, Connection *server, Window* window);
STATE srej_block(uint32_t out_fd, Connection *server, Window* window);
void write_to_out(Window *window, int bottom, int current, int out_fd);

int main(int argc, char** argv) {
	char* local_file;
	char* remote_file;

	check_args(argc, argv);

	remote_file = argv[2];

	sendtoErr_init(atof(argv[5]), DROP_ON, FLIP_ON, DEBUG_OFF, RSEED_OFF);

	run_client(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]), argv[6], atoi(argv[7]));

}

void check_args(int argc, char** argv) {
	if (argc != 8) {
		printf("usage : %s <local-file> <remote-file> <window-size> <buffer-size> <error-percent> <remote-machine> <remote-port>\n", argv[0]);
		exit(-1);
	}
	if (strlen(argv[1]) > 100) {
		printf("Local filename needs to be less than 100 and is: %d\n", strlen(argv[1]));
		exit(-1);
	}
	if (strlen(argv[2]) > 100) {
		printf("Remote filename needs to be less than 100 and is: %d\n", strlen(argv[2]));
		exit(-1);
	}
	if (atoi(argv[4]) < 4 || atoi(argv[4]) > 1400) {
		printf("Buffer size needs to be between 4 and 1400 and is: %d\n", atoi(argv[4]));
		exit(-1);
	}
	if(atoi(argv[5]) < 0 || atoi(argv[5]) >= 1) {
		printf("Errore rate needs to be between 0 and less than 1 and is: %d\n", atoi(argv[5]));
		exit(-1);
	}	
}

void run_client(char* local_file, char* remote_file, int32_t window_size, int32_t buff_size, char* host, int16_t port) {
	Connection server;
	int32_t out_fd;
	STATE state = START;
	Window window;
	init_window(&window, window_size);

	while(state != DONE) {
		switch(state) {
			case START:
				state = start_state(host, port, &server);
				break;
			case FILENAME:
				state = filename(remote_file, buff_size, window_size, &server);
				break;
			case FILE_OK:
				state = file_ok(&out_fd, local_file);
				break;
			case RECV_DATA:
				state = recv_data(out_fd, &server, &window);
				break;
			case SREJ_BLOCK:
				state = srej_block(out_fd, &server, &window);
				break;
			default:
				printf("ERROR - default STATE\n");
				break;
		}
	}
	close(out_fd);
}

STATE start_state(char* host, uint16_t port, Connection *server) {
	STATE state = START;
	uint8_t packet[MAX_LEN];
	uint8_t flag = 0;
	int32_t seq_num = 0;
	int32_t recv_check = 0;
	static int retry_count = 0;

	if (server->sk_num > 0) {
		close(server->sk_num);
	}
	if (udp_client_setup(host, port, server) < 0) {
		state = DONE;
	} else {
		send_buf(0, 0, server, CONNECT, 0, packet);
		if ((state = process_select(server, &retry_count, START, FILENAME, DONE)) == FILENAME) {
			recv_check = recv_buf(packet, MAX_LEN, server->sk_num, server, &flag, &seq_num);

			if (recv_check == CRC_ERROR) {
				state = START;
			} else if (flag == ACCEPTED) {
				state = FILENAME;	
			}
		}
	}
	return state;
}

STATE filename(char* fname, int32_t buff_size, int32_t window_size, Connection* server) {
	STATE state = FILENAME;
	uint8_t packet[MAX_LEN];
	uint8_t buf[MAX_LEN];
	uint8_t flag = 0;
	int32_t seq_num = 0;
	int32_t fname_len = strlen(fname) + 1;
	int32_t recv_check = 0;
	static int retry_count = 0;

	buff_size = htonl(buff_size);
	memcpy(buf, &buff_size, SIZE_OF_BUF_SIZE);
	memcpy(buf + SIZE_OF_BUF_SIZE, &window_size, SIZE_OF_BUF_SIZE);
	memcpy(buf + 2* SIZE_OF_BUF_SIZE, fname, fname_len);

	send_buf(buf, fname_len + 2 * SIZE_OF_BUF_SIZE, server, FNAME, 0, packet);
	if ((state = process_select(server, &retry_count, FILENAME, FILE_OK, DONE)) == FILE_OK) {
		recv_check = recv_buf(packet, MAX_LEN, server->sk_num, server, &flag, &seq_num);
		if (recv_check == CRC_ERROR) {
			state = FILENAME;
		} else if (flag == FNAME_BAD) {
			printf("File %s not found\n", fname);
			state = DONE;
		}
	}

	return state;
}

STATE file_ok(int* out_file_fd, char* out_file_name) {
	if ((*out_file_fd = open(out_file_name, O_CREAT|O_TRUNC|O_WRONLY, 0600)) < 0) {
		perror("File open error");
		return DONE;
	}
	return RECV_DATA;
}

STATE recv_data(int32_t out_fd, Connection *server, Window* window) {
	int32_t seq_num = 0;
	uint8_t flag;
	int32_t data_len;
	uint8_t data_buf[MAX_LEN];
	uint8_t packet[MAX_LEN];

	if(select_call(server->sk_num, LONG_TIME, 0, NOT_NULL)  == 0) {
		printf("Timeout after 10 seconds, server is dead!\n");
		return DONE;
	}

	data_len = recv_buf(data_buf, MAX_LEN, server->sk_num, server, &flag, &seq_num);
	// printf("data recv: %s with seq_num: %d\n", data_buf, seq_num);
	// printf("window bottom: %d\n", window->bottom);

	if (flag == END_OF_FILE) {
			printf("recv eof\n");
		  	send_buf(0, 0, server, EOF_ACK, seq_num + 1, packet);
		  	return DONE;
	}
	if (data_len == CRC_ERROR) {
		window->current = window->bottom;
		send_buf(0, 0, server, SREJ, window->current, packet);
		return SREJ_BLOCK;
	}
	//printf("window->bottom: %d window->current: %d seq_num: %d\n", window->bottom, window->current, seq_num);

	if (seq_num < window->bottom) {
		//printf("bottom: %d\n", window->bottom);
		send_buf(0, 0, server, RR, window->bottom, packet);
	} else if (seq_num == window->bottom) {
		// printf("data len: %d\n", data_len);
		 //printf("data: %s\n", data_buf);
		update_window(window, window->bottom + 1);
		send_buf(0, 0, server, RR, window->bottom, packet);
		write(out_fd, data_buf, data_len);
		//printf("data intital: %s\n", data_buf);
	} else {
		printf("window bottom %d\n", window->bottom);
		add_data_to_buffer(window, data_buf, data_len, seq_num);
		window->current = window->bottom;
		//send_buf(0, 0, server, RR, window->bottom, packet);
		return SREJ_BLOCK;
	}

	return RECV_DATA;
}

STATE srej_block(uint32_t out_fd, Connection *server, Window* window) {
	int retry_count = 0;
   int return_val;
   int seq_num;
	uint8_t flag;
	int32_t data_len;
	uint8_t data_buf[MAX_LEN];
	char* data;
	uint8_t packet[MAX_LEN];
	int32_t i;
	STATE state = SREJ_BLOCK;
	int len;
//	int eof_seq;

   retry_count++;

   // if (retry_count > MAX_TRIES) {
   //    printf("Sent data %d times, no ACK, client is probably gone - I'm dead", MAX_TRIES);
   //    return DONE;
   // }

   //if (select_call(server->sk_num, SHORT_TIME, 0, NOT_NULL) == 1) {
//      retry_count = 0;
		data_len = recv_buf(data_buf, MAX_LEN, server->sk_num, server, &flag, &seq_num);

		if (data_len == CRC_ERROR) {
			return SREJ_BLOCK;
		}

		if (flag == END_OF_FILE) {
			send_buf(0, 0, server, EOF_ACK, seq_num, packet);
			return DONE;
		}

		//printf("window->bottom: %d window->top: %d seq_num: %d\n", window->bottom, window->top, seq_num);
		if (seq_num >= window->bottom && seq_num <= window->top) {
			add_data_to_buffer(window, data_buf, data_len, seq_num);
			//printf("window current: %d\n", window->current);	
			for (i = window->bottom; i <= window->top; i++) {
				window->current = i;
				if(check_if_valid(window, i) == 0) {
					//if (i != window->bottom && i != window->top && i) {
						printf("rej: %d\n", i);
						send_buf(0, 0, server, SREJ, i, packet);
					//}
					break;
				}
			}
			write_to_out(window, window->bottom, window->current, out_fd);
			//printf("sending rr for: %d\n", window->current);
			send_buf(0, 0, server, RR, window->current+1, packet);

			if (all_invalid(window)) {
				update_window(window, window->current);
				return RECV_DATA;
			}

			if(window->current > window->bottom) {
				update_window(window, window->current);
			}
			return SREJ_BLOCK;
		}
			printf("sending rr for: %d\n", window->current);

		send_buf(0, 0, server, RR, window->current, packet);

		return RECV_DATA;
   //}
//   printf("hereee\n");
  // return SREJ_BLOCK;
}

void write_to_out(Window *window, int bottom, int current, int out_fd) {
	int i;
	char* data;
	int32_t len;

	for (i = bottom; i < current; i++) {
		//printf("i: %d\n", i);
		get_data_from_buffer(window, i, &data, &len);
		//printf("data here: %d\n", len);
		remove_from_buffer(window, i);
		write(out_fd, data, len);
	}
}
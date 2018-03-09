#include <stdio.h>
// Add your system includes here.
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <arpa/inet.h>

#include "ftree.h"
#include "hash.h"

#define _GNU_SOURCE
#define MAXDIRS 100
#define MAX_CONNECTIONS 12
#define BUF_SIZE 128
#define MAX_BACKLOG 5

struct sockname {
    int sock_fd;
	int status;
	struct request req; 
};

void transfer_file(char *, struct request *, char *);

/* Accept a connection. Note that a new file descriptor is created for
 * communication with the client. The initial socket descriptor is used
 * to accept connections, but the new socket is used to communicate.
 * Return the new client's file descriptor or -1 on error.
 */
int accept_connection(int fd, struct sockname *connections) {
    
	int user_index = 0;
	int type; 

	//increment user index if the index has already been used
    while (user_index < MAX_CONNECTIONS && connections[user_index].sock_fd != -1) {
        user_index++;
    }

	//accept client call, print error if the call could not be accepted 
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        perror("server: accept");
        close(fd);
        exit(1);
    }

	//check if maximum connections has been surpassed, print error if it has
    if (user_index == MAX_CONNECTIONS) {
        fprintf(stderr, "server: max concurrent connections\n");
        close(client_fd);
        return -1;
    }

	//Read the type of request the user is making
	int num_read = read(client_fd, &type, sizeof(int));
	type = ntohl(type); //convert from endian of other machines

	//Print error if an int was not read
	if (num_read != sizeof(int)) { 
		perror("read");
	} 
	
    connections[user_index].sock_fd = client_fd; //set the socket 
	connections[user_index].req.type = type; //set the type of the client
	connections[user_index].status = AWAITING_PATH; //change the status of the client

    return client_fd; //return file descriptor of client
}

/* Reads a struct from client_index and respond with TRANSFILE if files differ, OK if they are the same, or
 * ERROR on error. Copies the differing files into the local directory and changes permissions. 
 * Return the fd if it has been closed or 0 otherwise.
 */
int read_from(int client_index, struct sockname *connections) {
	int size; 
	//set sizes according to which status the client is on
	if (connections[client_index].status == AWAITING_PATH) { 
		size = MAXPATH; //requires size of path
	} 
	else if (connections[client_index].status == AWAITING_SIZE) {
		size = sizeof(int); //requires size of int
	} 
	else if (connections[client_index].status == AWAITING_PERM) { 
		size = sizeof(int); //requires size of int
	} 
	else if (connections[client_index].status == AWAITING_HASH) { 
		size = BLOCKSIZE; //requires size of hash
	} 
	else if (connections[client_index].status == AWAITING_DATA) { 
		size = MAXDATA;  //requires size of data
	} 
	
	int fd = connections[client_index].sock_fd;
	char buf[size + 1];
	char message[size + 1];
	int num; 	
	int num_read; 
	
	//Check the status of the specific client
	//If it is awaiting path, read the path from the socket
	if (connections[client_index].status == AWAITING_PATH) { 
		num_read= read(fd, buf, size);
		buf[num_read] = '\0';
		strncpy(message, buf, size);
		strncpy(connections[client_index].req.path, message, size); //set the path

		connections[client_index].status = AWAITING_PERM; //change status to wait for next item in struct
	} 
	//If it is awaiting mode, read the mode from the socket
	else if (connections[client_index].status == AWAITING_PERM) {
		num_read= read(fd, &num, size);
		num = ntohl(num); //convert endianness to local 
		connections[client_index].req.mode = num; //set the mode

		//Check if the request type is a TRANSFILE
		if (connections[client_index].req.type != TRANSFILE) { 		
			//If the file sent is a directory, ensure that the request type is of REGDIR
 			if (S_ISDIR(connections[client_index].req.mode)) { 
 				connections[client_index].req.type = REGDIR; 
 			}	
			//If the file sent is a file, ensure that the request type is of REGFILE
 			else if(S_ISREG(connections[client_index].req.mode)) {
				connections[client_index].req.type = REGFILE; 
 			} 
		}

		connections[client_index].status = AWAITING_HASH; //change status to wait for next item in struct
	} 
	//If it is awaiting hash, read the hash from the socket
	else if (connections[client_index].status == AWAITING_HASH) { 
		num_read= read(fd, buf, size);
		buf[num_read] = '\0';
		strncpy(message, buf, size);		
		strncpy(connections[client_index].req.hash, message, size); //set the hash
		connections[client_index].status = AWAITING_SIZE; //change status to wait for next item in struct
	} 
	//If it is awaiting size, read the size from the socket
	else if (connections[client_index].status == AWAITING_SIZE) { 
		num_read = read(fd, &num, size);
		num = ntohl(num); //convert endianness to local 
		connections[client_index].req.size = num; //set the size 
		
		//Check for what the request type is
		if (connections[client_index].req.type == REGFILE) {
			//Check to see if the file already exists
			struct stat potstat; 
			if (lstat(connections[client_index].req.path, &potstat) == 0) {
				//if it exists, compare the size and hash	
				if (S_ISREG(potstat.st_mode) != 0) { 
					//open file
					FILE *file = fopen(connections[client_index].req.path, "r"); 
					//point to the end of the file 
					fseek(file, 0, SEEK_END);
					//check size 
					int size = ftell(file); 
					//point to the beginning of the file
					fseek(file, 0, SEEK_SET); 

					fclose(file); 

					//If size and hash are not equal, request for a TRANSFILE
					if (size != connections[client_index].req.size || hash(file) != connections[client_index].req.hash) {
						if (write(connections[client_index].sock_fd, "1", sizeof(char)) != sizeof(char)) {
							perror("Write"); 
							connections[client_index].sock_fd = -1;
							return fd;
						} 
					}
					//If they are equal, send an OK signal 
					else {						
						if (write(connections[client_index].sock_fd, "0", sizeof(char)) != sizeof(char)) { 
							perror("Write"); 
							connections[client_index].sock_fd = -1;
							return fd;
						} 
						chmod(connections[client_index].req.path, (connections[client_index].req.mode)); //Change the permissions
					} 
				} 
			} 
			else {
				//if the file does not exist, request for a copy
				if (write(connections[client_index].sock_fd, "1", sizeof(char)) != sizeof(char)) { 
					perror("Write"); 
					connections[client_index].sock_fd = -1;
					return fd;
				} 
			}
		}  
		else if (connections[client_index].req.type == REGDIR) { 
			//Check if the directory already exists
			struct stat potstat; 
			if (lstat(connections[client_index].req.path, &potstat) == 0) {					
				//If it exists, change it's permissions
				if (S_ISDIR(potstat.st_mode) != 0) {
					chmod(connections[client_index].req.path, (connections[client_index].req.mode)); 
				}
			} 
			else { 
				//If it does not exist, create it 
				if (mkdir(connections[client_index].req.path, (connections[client_index].req.mode)) == -1 ) { 
					perror("mkdir");
				} 	
			} 

			//Send an OK signal because no file is to be sent
			if (write(connections[client_index].sock_fd, "0", sizeof(char)) != sizeof(char)) { 
				perror("write"); 
				connections[client_index].sock_fd = -1;
				return fd;
			}
			
			connections[client_index].status = AWAITING_PATH; //Wait for the next struct to be sent 
		} 
		else if (connections[client_index].req.type == TRANSFILE) { 
			//if the file already exists, delete it
			if (access(connections[client_index].req.path, F_OK) != -1) {
				printf("we are removing this\n");
				if (remove(connections[client_index].req.path) == -1){
					perror ("remove");
					exit(1);
				}
			}
			connections[client_index].status = AWAITING_DATA; //change status to wait for data of the file
		} 
		else {
			//Write an error, since the request type is invalid
			if (write(connections[client_index].sock_fd, "2", sizeof(char)) != sizeof(char)) {
				connections[client_index].sock_fd = -1;
				return fd;
			} 
		} 
					
	} 
	//If it is awaiting data, read the data from the socket
	else if (connections[client_index].status == AWAITING_DATA) { 
		num_read = read(fd, buf, size);
		buf[num_read] = '\0'; 		
		FILE *file = fopen(connections[client_index].req.path, "a"); //open the file for appending

		//Check to see if hashes are already equal, if not, copy
		if (hash(file) != connections[client_index].req.hash) {
			if (file != NULL) {
				if (num_read != 0) { 
					int numw = fwrite(buf, 1, num_read, file); //write the bytes to the open file
					//print an error if the bytes are not written properly
					if (numw != num_read) { 
						perror("fwrite");
					} 
				}
				fclose(file); //close the file 
			} 	 
		} 

		//write success to the socket

		if (write(connections[client_index].sock_fd, "0", sizeof(char)) != sizeof(char)) {
			connections[client_index].sock_fd = -1;
			return fd;
		} 
		
		chmod(connections[client_index].req.path, (connections[client_index].req.mode)); //change permissions  

		printf("Copy completed.\n"); //print success message 
		//close the socket since we are done writing the file
		connections[client_index].sock_fd = -1;
		return fd;
	} 
	//If status is any other value, it is not valid
	else {
		//write an error to the socket
		if (write(connections[client_index].sock_fd, "2", sizeof(char)) != sizeof(char)) {
			connections[client_index].sock_fd = -1;
			return fd;
		} 
	} 

	return 0; //return 0 on success 
}	

/* Reads and updates files as data is being recieved from the file sender client.  
 * Prints a success message if copies were successful 
 */
void rcopy_server(unsigned short port) { 
	struct sockname connections[MAX_CONNECTIONS];
    for (int index = 0; index < MAX_CONNECTIONS; index++) {
        connections[index].sock_fd = -1;
		connections[index].status = AWAITING_TYPE;
    }

    // Create the socket FD.
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("server: socket");
        exit(1);
    }

    // Set information about the port (and IP) we want to be connected to.
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    server.sin_addr.s_addr = INADDR_ANY;

    // This should always be zero. On some systems, it won't error if you
    // forget, but on others, you'll get mysterious errors. So zero it.
    memset(&server.sin_zero, 0, 8);

    // Bind the selected port to the socket.
    if (bind(sock_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("server: bind");
        close(sock_fd);
        exit(1);
    }

    // Announce willingness to accept connections on this socket.
    if (listen(sock_fd, MAX_BACKLOG) < 0) {
        perror("server: listen");
        close(sock_fd);
        exit(1);
    }

    // The client accept - message accept loop. First, we prepare to listen to multiple
    // file descriptors by initializing a set of file descriptors.
    int max_fd = sock_fd;
    fd_set all_fds, listen_fds;
    FD_ZERO(&all_fds);
    FD_SET(sock_fd, &all_fds);

    while (1) {
        // select updates the fd_set it receives, so we always use a copy and retain the original.
        listen_fds = all_fds;
        int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
        if (nready == -1) {
            perror("server: select");
            exit(1);
        }

        // Is it the original socket? Create a new connection ...
        if (FD_ISSET(sock_fd, &listen_fds)) {
            int client_fd = accept_connection(sock_fd, connections);
            if (client_fd > max_fd) {
                max_fd = client_fd;
            }
            FD_SET(client_fd, &all_fds);
            printf("Accepted connection\n");
        }

        // Next, check the clients.
        // NOTE: We could do some tricks with nready to terminate this loop early.
        for (int index = 0; index < MAX_CONNECTIONS; index++) {
            if (connections[index].sock_fd > -1 && FD_ISSET(connections[index].sock_fd, &listen_fds)) {
                // Note: never reduces max_fd
                int client_closed = read_from(index, connections);
                if (client_closed > 0) {
                    FD_CLR(client_closed, &all_fds);
                    printf("Client %d disconnected\n", client_closed);
                } 
            }
        }
    }


}

int make_socket(char *host){
	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("client: socket");
		exit(1);
	}
	
	// Set the IP and port of the server to connect to.
	struct sockaddr_in server;
	struct hostent *hp;
	
	server.sin_family = PF_INET;
	server.sin_port = htons(PORT);
	
	hp = gethostbyname(host);
	if (hp == NULL){
		fprintf(stderr, "rcopy_client: %s unknown host\n", host);
		exit(1);		
	}
	
	server.sin_addr = *((struct in_addr *)hp->h_addr);
		
	// Connect to the server.
	if (connect(sock_fd, (struct sockaddr *)&server, sizeof(server)) == -1) {
		perror("client: connect");
		close(sock_fd);
		exit(1);
	}	
	
	return sock_fd;
}


void make_request(char *source, struct request *cur_request, char *base_dir){
	struct stat source_stat;
	FILE *fp;
	
	if (lstat(source, &source_stat) != 0){	
		perror("lstat");
		exit(1);
	}
	
	if (S_ISDIR(source_stat.st_mode)){
		cur_request->type = htonl(REGDIR);
	}
	else if (S_ISREG(source_stat.st_mode)){
		cur_request->type = htonl(REGFILE);
	}
	
	strncpy(cur_request->path, base_dir, strlen(base_dir)); 
	cur_request->path[strlen(base_dir)] = '\0';
	
	cur_request->mode = htonl(source_stat.st_mode);
	
	if ((fp = fopen(source, "r")) == NULL){
		perror("fopen");
		exit(1);
	}
	
	strncpy(cur_request->hash, hash(fp), BLOCKSIZE);
	
	if (fclose(fp) != 0){
		perror("fclose");
		exit (1);
	}
	
	cur_request->size = htonl(source_stat.st_size);	
}

void write_to_server(int sock_fd, struct request *cur_request){
	//write to socket
	int num_write = write(sock_fd, cur_request->path, MAXPATH);
	if (num_write != MAXPATH){
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
	printf("path: %s\n", cur_request->path);
	
	num_write = write(sock_fd, &cur_request->mode, sizeof(mode_t));
	if (num_write != sizeof(mode_t)){
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
	printf("mode: %o\n", ntohl(cur_request->mode)&0777);
	
	num_write = write(sock_fd, cur_request->hash, BLOCKSIZE);
	if (num_write != BLOCKSIZE) {
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
	
	num_write = write(sock_fd, &cur_request->size, sizeof(int));
	if (num_write != sizeof(int)){
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
	printf("size: %d\n", ntohl(cur_request->size));
	
	//write(STDOUT_FILENO, "Wrote to server", sizeof("Wrote to server"));
}

int traverse_dir (char *source, int sock_fd, char *host, char *base_dir){
	struct dirent *dp;
	DIR *dirp = opendir(source);
	int i = 0;
	int num_child = 0;
	
	if (dirp == NULL){
		perror("opendir");
		exit(1);
	}
	
	while (i < MAXDIRS){
		dp = readdir(dirp);
		
		if(dp == NULL){
			break;
		}
		else if(dp->d_name[0] != '.' && dp->d_type != DT_LNK){
			char new_path[MAXPATH];
			struct request new_request;
			char buf[MAXDATA];
			int response;
			char new_base_dir[MAXPATH];
			
			strncpy(new_base_dir, base_dir, strlen(base_dir));
			new_base_dir[strlen(base_dir)] = '\0';
			strncat(new_base_dir, "/", 1);
			strncat(new_base_dir, dp->d_name, MAXPATH - strlen(new_base_dir) - 1);
			
			strncpy(new_path, source, strlen(source));
			new_path[strlen(source)] = '\0';
			strncat(new_path, "/", 1);
			strncat(new_path, dp->d_name, MAXPATH - strlen(new_path) - 1);
			
			make_request(new_path, &new_request, new_base_dir);
			write_to_server(sock_fd, &new_request);
			
			read(sock_fd, buf, sizeof(int));
			response = strtol(buf, NULL, 10);
			printf("Response: %d\n", response);
			
			if (response == SENDFILE){
				int result;
				if ((result = fork()) == -1){
					perror("fork");
					exit(1);
				}
				
				num_child ++;
				//child process
				if (result == 0){
					transfer_file(new_path, &new_request, host);
					num_child --;
				}
			}
			else if (response == ERROR){
				fprintf(stderr, "%s: Failed to copy file!", basename(new_path));
				exit(1);
			}	
			
			//is a directory
			if (dp->d_type == DT_DIR){
				strncpy(buf, base_dir, strlen(base_dir));
				buf[strlen(base_dir)] = '\0';
				strncat(buf, "/", 1);
				strncat(buf, dp->d_name, MAXPATH - strlen(buf) - 1);
				num_child += traverse_dir(new_path, sock_fd, host, buf);
			}
		
			i ++;
		}
		
	}
	
	return num_child;
}

void transfer_file(char *source, struct request *cur_request, char *host){
	//create a new socket for the child process
	int new_sock_fd = make_socket(host);
	FILE *fp;
	int num_read;
	char buf[MAXDATA];
	
	//set request type to identify it as a file transfer
	cur_request->type = htonl(TRANSFILE);
	
	//send server the request type
	int num_write = write(new_sock_fd, &cur_request->type, sizeof(int));
	if (num_write != sizeof(int)) {
		perror("client: write");
		close(new_sock_fd);
		exit(1);
	}
	
	//send server the rest of the request information
	write_to_server(new_sock_fd, cur_request);
	
	//open file for reading
	if ((fp = fopen(source, "r")) ==  NULL){
		perror ("fopen");
		exit(1);
	}
	
	printf("writing file...\n");
	
	//reads the file at source and sends it to the server
	while ((num_read = fread(buf, 1, MAXDATA, fp)) != 0) { 
		printf("writing %s %d bytes\n", buf, num_read);
		if (write(new_sock_fd, buf, num_read) != num_read){
			perror("client: write");
			close(new_sock_fd);
			exit(1);
		}	
	}	

	printf("done\n");
	
	if (fclose(fp) != 0){
		perror("fclose");
		exit(1);
	}
	
	//reads the response from server
	read(new_sock_fd, buf, sizeof(int));
	int response = strtol(buf, NULL, 10);

	printf("Response: %d\n", response);
	
	if (close(new_sock_fd) == -1){
		perror("close");
		exit(1);
	}
	
	//file transferred over successfully
	if (response == OK){
		exit(0);
	}
	//file did not transfer sucessfully
	else if (response == ERROR){
		fprintf(stderr, "%s: Failed to copy\n", basename(source));
		exit(1);
	}
	
	//this shouldn't occur
	else{
		fprintf(stderr, "Response: %d", response);
		exit(1);
	}
}

int rcopy_client(char *source, char *host, unsigned short port){
	//create socket for main client
	int sock_fd = make_socket(host);	
	struct request cur_request;
	char buf[MAXDATA];
	int response;
	int status;
	int num_child = 0;
	
	//fill in values of request struct 
	make_request(source, &cur_request, basename(source));

	printf ("Type: %d\n", ntohl(cur_request.type));
	
	//send server type of request
	int num_write = write(sock_fd, &cur_request.type, sizeof(int));
	
	//makes sure data sent to server is of expected size
	if (num_write != sizeof(int)) {
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
	
	//send server the rest of the values of the request struct
	write_to_server(sock_fd, &cur_request);
	
	//checks response from server
	read(sock_fd, buf, sizeof(int));
	response = strtol(buf, NULL, 10);

	printf("Response: %d\n", response);
	
	//server tells client to send file data
	if (response == SENDFILE){
		int result;
		
		//fork the process
		if ((result = fork()) == -1){
			perror("fork");
			exit(1);
		}
		num_child ++;
		//let the child process deal with transfering file to server
		if (result == 0){
			transfer_file(source, &cur_request, host);
			num_child --;
		}
	}
	
	//file on server matches file sent
	else if (response == OK){
		//if original file is just a regular file, no more files to check
		if (cur_request.type == htonl(REGFILE)){
			return 0;
		}
		
	}
	
	//error on server's end
	else if (response == ERROR){
		return 1;
	}
	
	//traverses subdirectories if fil is a directory
	if (cur_request.type == htonl(REGDIR)){
		num_child += traverse_dir(source, sock_fd, host, basename(source));
	}
	
	//close the socket
	if (close(sock_fd) == -1){
		perror("close");
		exit(1);
	}
	
	while (num_child > 0){
		//if the client has no children
		if ((wait(&status)) == -1){
			perror("wait");
			exit(1);
		}
		
		//checks the exit status of children and returns 1 if a child encountered
		//an error
		if (WIFEXITED(status)){
			if (WEXITSTATUS(status) == 1){
				fprintf(stderr, "Child error\n");
				return 1;
			}
		}
		num_child --;
	}

	return 0; 
}

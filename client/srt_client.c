//
// FILE: srt_client.c
//
// Description: this file contains client states' definition, some important data structures
// and the client SRT socket interface definitions. You need to implement all these interfaces
//
// Date: April 27, 2016
// Author: Victoria Taylor (function descriptions provided by Prof. Zhou)

#include "srt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

struct client_tcb *clientTCB[MAX_TRANSPORT_CONNECTIONS];
int clientconn;

//
//
//  SRT socket API for the client side application. 
//  ===================================
//
//  In what follows, we provide the prototype definition for each call and limited pseudo code representation
//  of the function. This is not meant to be comprehensive - more a guideline. 
// 
//  You are free to design the code as you wish.
//
//  NOTE: When designing all functions you should consider all possible states of the FSM using
//  a switch statement (see the Lab3 assignment for an example). Typically, the FSM has to be
// in a certain state determined by the design of the FSM to carry out a certain action. 
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//

// This function initializes the TCB table marking all entries NULL. It also initializes 
// a global variable for the overlay TCP socket descriptor ``conn'' used as input parameter
// for snp_sendseg and snp_recvseg. Finally, the function starts the seghandler thread to 
// handle the incoming segments. There is only one seghandler for the client side which
// handles call connections for the client.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
void srt_client_init(int conn)
{
	// Mark all TCB entries NULL
	for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
		clientTCB[i] = NULL;
	}

	// Initialize global variable for TCP connection
	clientconn = conn;
	
	//start seghandler
	int err; 
	pthread_t newthread;
	err = pthread_create(&newthread, NULL, seghandler, NULL);
	if (err != 0){
		printf("Problem creating thread\n");
		exit(1);
	}
	return;
}


// This function looks up the client TCB table to find the first NULL entry, and creates
// a new TCB entry using malloc() for that entry. All fields in the TCB are initialized 
// e.g., TCB state is set to CLOSED and the client port set to the function call parameter 
// client port.  The TCB table entry index should be returned as the new socket ID to the client 
// and be used to identify the connection on the client side. If no entry in the TC table  
// is available the function returns -1.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_client_sock(unsigned int client_port)
{

  	// Find first NULL entry in TCB table and fill it with new client_tcb
	for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
		if (clientTCB[i] == NULL){
			struct client_tcb *newClient = malloc(sizeof(struct client_tcb));
			newClient->client_portNum = client_port;
			newClient->state = CLOSED;
			newClient->sendBufHead = NULL;
			newClient->sendBufTail = NULL;
			clientTCB[i] = newClient;


			// Creat mutex for client's send buffer
			pthread_mutex_t *mutex;
			mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
			if (pthread_mutex_init(mutex, NULL) != 0 ){
				printf("mutex init failed\n");
				return -1;
			}
			newClient->bufMutex = mutex;

			// return sockID (table index)
			return i;
		}
	}

	// No more room in TCB table
	return -1;
}


// This function is used to connect to the server. It takes the socket ID and the 
// server's port number as input parameters. The socket ID is used to find the TCB entry.  
// This function sets up the TCB's server port number and a SYN segment to send to
// the server using snp_sendseg(). After the SYN segment is sent, a timer is started. 
// If no SYNACK is received after SYNSEG_TIMEOUT timeout, then the SYN is 
// retransmitted. If SYNACK is received, return 1. Otherwise, if the number of SYNs 
// sent > SYN_MAX_RETRY,  transition to CLOSED state and return -1.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_client_connect(int sockfd, unsigned int server_port)
{
	struct client_tcb *client = clientTCB[sockfd];
	client->svr_portNum = server_port;

	//Set up the timespec
	struct timespec *req = malloc(sizeof(struct timespec));
	req->tv_nsec = SYN_TIMEOUT;

	//Set up segment
	seg_t synseg; 
	synseg.header.src_port = client->client_portNum;
	synseg.header.dest_port = client->svr_portNum;
	synseg.header.length = 0;
	synseg.header.type = SYN;
	synseg.header.seq_num = 0;

	//Check state of connection
	if (client->state == CLOSED){		//can't connect unless closed
		
		//Send SYN up to SYN_MAX_RETRY times
		for (int synNum = 0; synNum < SYN_MAX_RETRY; synNum++){
			//Send SYN and transition to SYNSENT
			snp_sendseg(clientconn, &synseg);
			client->state = SYNSENT;
			client->next_seqNum = 1;
			printf("%d: SYN sent\n", sockfd);

			//set timer
			nanosleep(req, NULL);

			//Check if connection  established:
			if (client->state == CONNECTED){
				printf("%d: Connected\n", sockfd);
				free(req);

				return 1;
			}
		}
		// Too many connection attempts
		if (client->state == SYNSENT){
			client->state = CLOSED;
			printf("%d: Too many connect attempts\n", sockfd);
			free(req);
			return -1;
		}
	}
	else {
		printf("%d: Connection must be closed to connect\n", sockfd);
		free(req);
		return -1;
	}
}


// Send data to a srt server. This function should use the socket ID to find the TCP entry. 
// Then It should create segBufs using the given data and append them to send buffer linked list. 
// If the send buffer was empty before insertion, a thread called sendbuf_timer 
// should be started to poll the send buffer every SENDBUF_POLLING_INTERVAL time
// to check if a timeout event should occur. If the function completes successfully, 
// it returns 1. Otherwise, it returns -1.
// 
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_client_send(int sockfd, void* data, unsigned int length)
{
	struct client_tcb *client = clientTCB[sockfd];
	length = strlen(data);
	int copy;

	pthread_mutex_lock(client->bufMutex);
	while (length){
		
		//Copy size is the min of MAX_SEG_LEN and data
		if (length > MAX_SEG_LEN){
			copy = MAX_SEG_LEN;
		}
		else{
			copy = length;
		}

		//Allocate sendBuf
		struct segBuf *buffer = malloc(sizeof(struct segBuf));
		buffer->seg.header.src_port = client->client_portNum;
		buffer->seg.header.dest_port = client->svr_portNum;
		buffer->seg.header.seq_num = client->next_seqNum;
		buffer->seg.header.length = copy;
		buffer->seg.header.type = DATA;

		//Copy data into the sendBuf
		memcpy(buffer->seg.data, data, copy);
		data += copy;
		length -= copy;

		//Update next_seqNum
		client->next_seqNum += copy;

		//If send buffer is empty, start timer thread
		if (client->sendBufHead == NULL){
			pthread_t timethread;
			pthread_create(&timethread, NULL, sendBuf_timer, client); //need to pass in client data here 
			
			//All three sendBuf pointers to first buffer
			client->sendBufHead = buffer;
			client->sendBufunSent = buffer;
			client->sendBufTail = buffer;
	
		}

		//Send buffer isn't empty- append buffer
		else{
			client->sendBufTail->next = buffer;
			client->sendBufTail = buffer;
			client->sendBufTail->next = NULL;
			if (client->sendBufunSent == NULL){
				client->sendBufunSent = buffer;
			}
		}

	}

	//All segBufs are created- now send them 
	while ((client->unAck_segNum < GBN_WINDOW) && (client->sendBufunSent != NULL)){

			if (snp_sendseg(clientconn, &client->sendBufunSent->seg)){
				
				// Record time of sent message
				struct timeval curr;
				gettimeofday(&curr, NULL);
				client->sendBufunSent->sentTime = (1000000 * curr.tv_sec) + curr.tv_usec;

				client->sendBufunSent = client->sendBufunSent->next;
				client->unAck_segNum++;
			}
			else{
				printf("%d: send failed", sockfd);
				pthread_mutex_unlock(client->bufMutex);
				return -1;
			}
	}
	pthread_mutex_unlock(client->bufMutex);
	return 1;
}


// This function is used to disconnect from the server. It takes the socket ID as 
// an input parameter. The socket ID is used to find the TCB entry in the TCB table.  
// This function sends a FIN segment to the server. After the FIN segment is sent
// the state should transition to FINWAIT and a timer started. If the 
// state == CLOSED after the timeout the FINACK was successfully received. Else,
// if after a number of retries FIN_MAX_RETRY the state is still FINWAIT then
// the state transitions to CLOSED and -1 is returned.


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_client_disconnect(int sockfd)
{

	struct client_tcb *client = clientTCB[sockfd];

	//Set up FIN segment
	seg_t finseg;
	finseg.header.src_port = client->client_portNum;
	finseg.header.dest_port = client->svr_portNum;
	finseg.header.length = 0;
	finseg.header.type = FIN;
	finseg.header.seq_num = client->next_seqNum;

	//Set up timespec
	struct timespec req; 
	req.tv_nsec = FIN_TIMEOUT;

	if (client->state == CONNECTED){

		while (client->sendBufHead != NULL){
			// Wait until all the data has been ACKed
		}

		for (int finNum = 0; finNum < FIN_MAX_RETRY; finNum++){

			//Send FIN and transition to FINWAIT
			snp_sendseg(clientconn, &finseg);
			client->state = FINWAIT;
			printf("%d: FIN sent\n", sockfd);

			//Set timer
			nanosleep(&req, NULL);

			//Check if connection has closed: (successful receipt of FINACK)
			if (client->state = CLOSED){
				printf("%d: Connection closed\n", sockfd);

				pthread_mutex_lock(client->bufMutex);
				
				//Free all segBufs
				struct segBuf *temp;
				while (client->sendBufHead != NULL){
					temp = client->sendBufHead->next;
					free(client->sendBufHead);
					client->sendBufHead = temp;

				}
				pthread_mutex_unlock(client->bufMutex);
				return 1; 
			}
		}

		// Too many FIN attempts- close connection
		if (client->state == FINWAIT){
			client->state = CLOSED;
			printf("%d: Too many disconnect attempts\n", sockfd);
			return -1;
		}
	}
	else{
		printf("%d: ERR- must be first connected to disconnect\n", sockfd);
		return -1; 
	}
}


// This function calls free() to free the TCB entry. It marks that entry in TCB as NULL
// and returns 1 if succeeded (i.e., was in the right state to complete a close) and -1 
// if fails (i.e., in the wrong state).
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_client_close(int sockfd)
{
	//Find the TCB entry
	struct client_tcb *client = clientTCB[sockfd];
	if (client == NULL){
		return -1;
	}

	if (client->state == CLOSED){
		pthread_mutex_destroy(client->bufMutex);
		free(client);
		clientTCB[sockfd] = NULL;
		return 1;
	}
	else{
		printf("%d: Client not in right state to close", sockfd);
		return -1;
	}
}


// This is a thread  started by srt_client_init(). It handles all the incoming 
// segments from the server. The design of seghanlder is an infinite loop that calls snp_recvseg(). If
// snp_recvseg() fails then the overlay connection is closed and the thread is terminated. Depending
// on the state of the connection when a segment is received  (based on the incoming segment) various
// actions are taken. See the client FSM for more details.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
void *seghandler(void* arg) {
	struct client_tcb *srtclient;
	seg_t seg;
	while (1){

		int m = snp_recvseg(clientconn, &seg);
		if (m == 1){

			// Identify the TCB the message corresponds to 
			for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
				if (clientTCB[i] != NULL){
					if ((seg.header.dest_port == clientTCB[i]->client_portNum) && (seg.header.src_port == clientTCB[i]->svr_portNum)) {
						srtclient = clientTCB[i];
					}
				}
			}


			//Check state
			switch(srtclient->state){
				case CLOSED:
					break;
				case SYNSENT:
					if (seg.header.type == SYNACK){
						srtclient->state = CONNECTED;
					}
					break;
				case CONNECTED:
					if (seg.header.type == DATAACK){
						pthread_mutex_lock(srtclient->bufMutex);
						printf("DATAACK received\n");

						int ACKseg = seg.header.seq_num;
						struct segBuf *temp;

						// Remove ACKed data segments
						while ((srtclient->sendBufHead != NULL) && (srtclient->sendBufHead->seg.header.seq_num < ACKseg)){
							temp = srtclient->sendBufHead;
							srtclient->sendBufHead = srtclient->sendBufHead->next;
							srtclient->unAck_segNum--;
							free(temp);

							//Send the next unsent data
							if (srtclient->sendBufunSent != NULL){
								snp_sendseg(clientconn, &srtclient->sendBufunSent->seg);
								srtclient->sendBufunSent = srtclient->sendBufunSent->next;
								srtclient->unAck_segNum++;
							}
						}
						pthread_mutex_unlock(srtclient->bufMutex);
					}
					break;
				case FINWAIT:
					if (seg.header.type == FINACK){
						srtclient->state = CLOSED;
					}
					break;
				default:
					printf("Connection in unkown state: %d\n", srtclient->state);
					break;

			}
		}
		else if (m == -1){
			if (srtclient->state == CLOSED){
				exit(0);
			}
			else{
				printf("receive failed");
				exit(1);
			}
		}
	}
}




// This thread continuously polls send buffer to trigger timeout events
// It should always be running when the send buffer is not empty
// If the current time -  first sent-but-unAcked segment's sent time > DATA_TIMEOUT, a timeout event occurs
// When timeout, resend all sent-but-unAcked segments
// When the send buffer is empty, this thread terminates
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
void* sendBuf_timer(void* data)
{

	struct client_tcb *client = (struct client_tcb *) data;

	struct timespec req;
	req.tv_nsec = SENDBUF_POLLING_INTERVAL;

	while (client->sendBufHead !=NULL){
		
		//sleep
		nanosleep(&req, NULL);

		//lock mutex
		pthread_mutex_lock(client->bufMutex);

		//Get current time 
		struct timeval curr, newSent;
		gettimeofday(&curr, NULL);
		unsigned int currTime = (1000000 * curr.tv_sec) + curr.tv_usec;

		//Timeout event
		if ((client->sendBufHead != NULL) && (currTime - client->sendBufHead->sentTime) > DATA_TIMEOUT){
			printf("Data timeout event\n");

			// Resend all the sent-but-not-ACKed segments
			struct segBuf *currbuf = client->sendBufHead;
			int toResend = client->unAck_segNum;
			while (toResend > 0){
				snp_sendseg(clientconn, &currbuf->seg);
				gettimeofday(&newSent, NULL);
				int sentTime = (1000000 * newSent.tv_sec) + curr.tv_usec;
				currbuf->sentTime = sentTime; 
				currbuf = currbuf->next;
				toResend--;
			}
		}

		//unlock mutex
		pthread_mutex_unlock(client->bufMutex);
	}
	return 0;
}

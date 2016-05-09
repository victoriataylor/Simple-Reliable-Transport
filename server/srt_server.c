// FILE: srt_server.c
//
// Description: this file contains server states' definition, some important
// data structures and the server SRT socket interface functions.
//
// Date: April 27, 2016
// Author: Victoria Taylor (skeleton code provided by Prof. Xia Zhou)
//
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "srt_server.h"

//
//
//  SRT socket API for the server side application. 
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

struct svr_tcb *serverTCB[MAX_TRANSPORT_CONNECTIONS];
int serverconn;

// This function initializes the TCB table marking all entries NULL. It also initializes 
// a global variable for the overlay TCP socket descriptor ``conn'' used as input parameter
// for snp_sendseg and snp_recvseg. Finally, the function starts the seghandler thread to 
// handle the incoming segments. There is only one seghandler for the server side which
// handles call connections for the client.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
void srt_server_init(int conn)
{
	//Initialize serverTCB table to NULL
	for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
		serverTCB[i] = NULL;
	}

	//Initialize the global variable for the overlay socked descriptor
	serverconn = conn;

	//Start seghandler thread
	pthread_t newthread;
	int err = pthread_create(&newthread, NULL, seghandler, NULL);
	if (err != 0){
		printf("Seghandler thread creation failed\n");
		exit(1);
	}

  	return;
}


// This function looks up the client TCB table to find the first NULL entry, and creates
// a new TCB entry using malloc() for that entry. All fields in the TCB are initialized 
// e.g., TCB state is set to CLOSED and the server port set to the function call parameter 
// server port.  The TCB table entry index should be returned as the new socket ID to the server 
// and be used to identify the connection on the server side. If no entry in the TCB table  
// is available the function returns -1.

//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_server_sock(unsigned int port)
{
	for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
		if (serverTCB[i] == NULL){
			struct svr_tcb *newClient = malloc(sizeof(struct svr_tcb));
			newClient->svr_portNum = port;
			newClient->state = CLOSED;
			serverTCB[i] = newClient;

			newClient->recvBuf = malloc(RECEIVE_BUF_SIZE);
			newClient->usedBufLen = 0;

			//Initialize mutex
			pthread_mutex_t *mutex;
			mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
			if (pthread_mutex_init(mutex, NULL) != 0){
				printf("Mutex init failed\n");
				return -1;
			}
			newClient->bufMutex = mutex;

			return i;
		}
	}
  	// No more room in TCB table
  	return -1;
}


// This function gets the TCB pointer using the sockfd and changes the state of the connection to 
// LISTENING. It then starts a timer to ``busy wait'' until the TCB's state changes to CONNECTED 
// (seghandler does this when a SYN is received). It waits in an infinite loop for the state 
// transition before proceeding and to return 1 when the state change happens, dropping out of
// the busy wait loop. You can implement this blocking wait in different ways, if you wish.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_server_accept(int sockfd)
{
	//Get TCB pointer and change state to LISTENING
	struct svr_tcb *tserver = serverTCB[sockfd];
	tserver->state = LISTENING;
	printf("server is listening\n");
	fflush(stdout);

	while (tserver->state != CONNECTED){
		//Wait forever until state is connected
	}
	return 1;
}


// Receive data from a srt client. Recall this is a unidirectional transport
// where DATA flows from the client to the server. Signaling/control messages
// such as SYN, SYNACK, etc.flow in both directions. 
// This function keeps polling the receive buffer every RECVBUF_POLLING_INTERVAL
// until the requested data is available, then it stores the data and returns 1
// If the function fails, return -1 
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_server_recv(int sockfd, void* buf, unsigned int length)
{
	struct svr_tcb *server = serverTCB[sockfd];
	length = length -1;

	//Make sure buf is empty
	memset(buf, 0, sizeof(buf));

	while (server->usedBufLen < length){	//Poll until requested info in buffer
		sleep(RECVBUF_POLLING_INTERVAL);
	}
	pthread_mutex_lock(server->bufMutex);
	memcpy(buf, server->recvBuf, length);
	memmove(server->recvBuf, server->recvBuf + length, server->usedBufLen - length);
	memset(server->recvBuf + server->usedBufLen - length, 0, RECEIVE_BUF_SIZE - server->usedBufLen + length);
	server->usedBufLen = server->usedBufLen - length;
	pthread_mutex_unlock(server->bufMutex);
	return 1;

}


// This function calls free() to free the TCB entry. It marks that entry in TCB as NULL
// and returns 1 if succeeded (i.e., was in the right state to complete a close) and -1 
// if fails (i.e., in the wrong state).
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
int srt_server_close(int sockfd)
{
	struct svr_tcb *srtserver = serverTCB[sockfd];
	while (srtserver->state != CLOSED){
		// wait for seghandler to receive FIN
	} 

	// Free TCB struct and reset table entry
	pthread_mutex_destroy(srtserver->bufMutex);
	free(srtserver->recvBuf);
	srtserver->usedBufLen = 0;
	free(srtserver);
	serverTCB[sockfd] = NULL;
	return 1;
}


// This is a thread  started by srt_server_init(). It handles all the incoming 
// segments from the client. The design of seghanlder is an infinite loop that calls snp_recvseg(). If
// snp_recvseg() fails then the overlay connection is closed and the thread is terminated. Depending
// on the state of the connection when a segment is received  (based on the incoming segment) various
// actions are taken. See the client FSM for more details.
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
void* seghandler(void* arg)
{

	while (1){
	seg_t segrec;
	seg_t segsend;

		if ( snp_recvseg(serverconn, &segrec)){

			// Identify which TCB the message corresponds to
			struct svr_tcb *srtserver;
			for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; i++){
				if (serverTCB[i] != NULL){
					if (segrec.header.dest_port == serverTCB[i]->svr_portNum){
						srtserver = serverTCB[i];
					}
				}
			}

			// Add client port to TCB
			srtserver->client_portNum = segrec.header.src_port;

			// //Set up segment
			segsend.header.src_port = srtserver->svr_portNum;
			segsend.header.dest_port = srtserver->client_portNum;


			// Handle for each state
			switch(srtserver->state){
				case CLOSED:
					break;
				case LISTENING:
					if (segrec.header.type == SYN){

						// Send SYNACK
						segsend.header.length = 0;
						segsend.header.ack_num = 1; 
						segsend.header.type = SYNACK;
						snp_sendseg(serverconn, &segsend);
						printf("SYNACK sent\n");
						
						// Transition to connected state
						srtserver->state = CONNECTED;
						srtserver->expect_seqNum = 1; 
						printf("CONNECTED\n");

					}
					break;
				case CONNECTED:
					if (segrec.header.type == SYN){
						segsend.header.type = SYNACK;
						snp_sendseg(serverconn, &segsend);
						printf("SYNACK re-sent\n");
					}
					else if (segrec.header.type == FIN){
						// Send FINACK and Transition to closewait
						segsend.header.type = FINACK;
						snp_sendseg(serverconn, &segsend);
						printf("FINACK sent\n");
						srtserver->state = CLOSEWAIT;

						//Start a closewait timer
						pthread_t cwtimer; 
						pthread_create(&cwtimer, NULL, closewait, (void*)srtserver);
					}
					else if (segrec.header.type = DATA){
						// Lock Mutex
						pthread_mutex_lock(srtserver->bufMutex);
						segsend.header.type = DATAACK;
						if ((segrec.header.seq_num == srtserver->expect_seqNum) && (srtserver->usedBufLen + segrec.header.length < RECEIVE_BUF_SIZE)){
							//strncat(srtserver->recvBuf, segrec.data, segrec.header.length);
							memmove(srtserver->recvBuf + srtserver->usedBufLen, segrec.data, segrec.header.length);
							srtserver->expect_seqNum += segrec.header.length;
							srtserver->usedBufLen += segrec.header.length;
							segsend.header.seq_num = srtserver->expect_seqNum;
							if (snp_sendseg(serverconn, &segsend)){
								printf("DATAACK sent\n");
							}

						}
						else{	
							segsend.header.seq_num = srtserver->expect_seqNum;
							snp_sendseg(serverconn, &segsend);
						}
						pthread_mutex_unlock(srtserver->bufMutex);
					}

					break;
				case CLOSEWAIT:
					if (segrec.header.type == FIN){
						//Resend FINACK
						segsend.header.type = FINACK;
						snp_sendseg(serverconn, &segsend);
						printf("FINACK re-sent\n");
					}
					break;
			}

		}
		else {
			printf("server: received message failed\n");
			exit(1);
		}
	}
}

void* closewait(void* servertcb) {
	svr_tcb_t* my_servertcb = (svr_tcb_t*)servertcb;
	sleep(CLOSEWAIT_TIMEOUT);
	my_servertcb->state = CLOSED;
	printf("CLOSED\n");
	return NULL;
}



#include "contiki.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"
#include <sys/node-id.h>

#include "simple-udp.h"


#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define UDP_PORT 1234

#define SEND_INTERVAL		(20 * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))

#define MAX_NEIGHBOURS_SAVED 16

typedef struct neighbour_s{
	int id;
	bool saved;
} neighbour;

static struct simple_udp_connection broadcast_connection;
//Array to save the neighbours encountered, initialized to NULL
static neighbour* neighbours[MAX_NEIGHBOURS_SAVED] = {NULL};

/*---------------------------------------------------------------------------*/
PROCESS(broadcast_example_process, "UDP broadcast example process");
AUTOSTART_PROCESSES(&broadcast_example_process);
/*---------------------------------------------------------------------------*/
static void neighbours_print(){
	for(int i = 0 ; i < MAX_NEIGHBOURS_SAVED; i++){
		if(neighbours[i] == NULL){
			printf("%d\n",i);
		}
	}
}
/*---------------------------------------------------------------------------*/
static  neighbour* create_neighbour(int sender_id){
	neighbour* new_neighbour = (neighbour*)malloc(sizeof(neighbour));
	//initialize the new neighbour
	new_neighbour-> id = sender_id;
	new_neighbour-> saved = false;

	return  new_neighbour;
}
/*---------------------------------------------------------------------------*/
static void
receiver(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  int sender_id = atoi((char*)data);
  printf("Data received on port %d from port %d sent by %d\n",
         receiver_port, sender_port, sender_id);
	int i;
	for(i = 0; i < MAX_NEIGHBOURS_SAVED; i++){
		if(neighbours[i] == NULL){
			neighbours[i] = create_neighbour(sender_id);
			//check if malloc failed
			if(neighbours[i] == NULL){
				//TODO log error
				printf("malloc failed\n");
			}else{
				printf("created new neigh with id: %d\n", sender_id);
			}
			break;
		}else{
			//neighbour already seen
			if(neighbours[i]->id == sender_id){
				printf("already seen neighbour: %d\n", sender_id);
				break;
			}
		}
	}
	//array of neighbours is full
	if(i == MAX_NEIGHBOURS_SAVED){
		//TODO delete last seen neighbour
		printf("can't add neighbour: %d array is full\n", sender_id);
	}
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(broadcast_example_process, ev, data)
{
  static struct etimer periodic_timer;
  static struct etimer send_timer;
  uip_ipaddr_t addr;
  static char node_id_str[3];
 

  PROCESS_BEGIN();
  neighbours_print();
  //convert node id from int to string in order to send it 
  snprintf(node_id_str, 3, "%d", node_id);
  
  simple_udp_register(&broadcast_connection, UDP_PORT,
                      NULL, UDP_PORT,
                      receiver);

  etimer_set(&periodic_timer, SEND_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
    etimer_reset(&periodic_timer);
    etimer_set(&send_timer, SEND_TIME);

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));
    printf("Sending broadcast from %s\n",node_id_str);
    uip_create_linklocal_allnodes_mcast(&addr);
    simple_udp_sendto(&broadcast_connection, node_id_str, 3, &addr);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

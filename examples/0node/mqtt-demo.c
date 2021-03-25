/*---------------------------------------------------------------------------*/
/** \addtogroup cc2538-examples
 * @{
 *
 * \defgroup cc2538-mqtt-demo CC2538 MQTT Demo Project
 *
 * Demonstrates MQTT functionality. Works with IBM Quickstart as well as
 * mosquitto.
 * @{
 *
 * \file
 * An MQTT example for the cc2538-based platforms
 */
/*---------------------------------------------------------------------------*/
#include "contiki-conf.h"
#include "rpl/rpl-private.h"
#include "mqtt.h"
#include "net/rpl/rpl.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "dev/button-sensor.h"
#include "dev/leds.h"


#include "simple-udp.h"
#include "contiki.h"
#include "lib/random.h"
#include "net/ipv6/uip-ds6.h"

#include "sys/log.h"
#define LOG_MODULE "MQTT-DEMO"
#define LOG_LEVEL LOG_LEVEL_INFO

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/node-id.h>


#define UDP_PORT 1234

#define SEND_INTERVAL		(20 * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))
#define MAX_NEIGHBOURS_SAVED 16
#define MAX_EVENT_OF_INTEREST_DELAY 80
#define MIN_EVENT_OF_INTEREST_DELAY 20
#define ALERT_ENABLED 0

static struct simple_udp_connection broadcast_connection;

//event that gets posted when there's something to publish
static process_event_t event_of_interest_event;
static process_event_t neighbour_added_event;

/*
When a random event of interest occurs,
an event gets fired. When that happens, it may occur that the finite state machine
that implements the mqtt functionalities isn't anymore in the publishing state, 
therefore the following variable is used to flag  that there's something to publish,
so when the machine will go back to the publishing state, the update won't be missed
*/
static bool event_fired;

typedef struct neighbour_s{
	int id;
	bool saved;
} neighbour;

//Array to save the neighbours encountered, initialized to NULL
static neighbour* neighbours[MAX_NEIGHBOURS_SAVED] = {NULL};

/*---------------------------------------------------------------------------*/
/*
 * Publish to a local MQTT broker (e.g. mosquitto) running on
 * the node that hosts your border router
 */
static const char *broker_ip = MQTT_DEMO_BROKER_IP_ADDR;
#define DEFAULT_ORG_ID              "mqtt-demo"
/*---------------------------------------------------------------------------*/
/*
 * A timeout used when waiting for something to happen (e.g. to connect or to
 * disconnect)
 */
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
/*---------------------------------------------------------------------------*/
/* Provide visible feedback via LEDS during various states */
/* When connecting to broker */
#define CONNECTING_LED_DURATION    (CLOCK_SECOND >> 2)

/* Each time we try to publish */
#define PUBLISH_LED_ON_DURATION    (CLOCK_SECOND)
/*---------------------------------------------------------------------------*/
/* Connections and reconnections */
#define RETRY_FOREVER              0xFF
#define RECONNECT_INTERVAL         (CLOCK_SECOND * 2)
/*---------------------------------------------------------------------------*/
/*
 * Number of times to try reconnecting to the broker.
 * Can be a limited number (e.g. 3, 10 etc) or can be set to RETRY_FOREVER
 */
#define RECONNECT_ATTEMPTS         RETRY_FOREVER
#define CONNECTION_STABLE_TIME     (CLOCK_SECOND * 5)
static struct timer connection_life;
static uint8_t connect_attempt;
/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;
#define STATE_INIT            0
#define STATE_REGISTERED      1
#define STATE_CONNECTING      2
#define STATE_CONNECTED       3
#define STATE_PUBLISHING      4
#define STATE_DISCONNECTED    5
#define STATE_NEWCONFIG       6
#define STATE_CONFIG_ERROR 0xFE
#define STATE_ERROR        0xFF
/*---------------------------------------------------------------------------*/
#define CONFIG_ORG_ID_LEN        32
#define CONFIG_TYPE_ID_LEN       32
#define CONFIG_AUTH_TOKEN_LEN    32
#define CONFIG_CMD_TYPE_LEN       8
#define CONFIG_IP_ADDR_STR_LEN   64
/*---------------------------------------------------------------------------*/
/* A timeout used when waiting to connect to a network */
#define NET_CONNECT_PERIODIC        (CLOCK_SECOND >> 2)
#define NO_NET_LED_DURATION         (NET_CONNECT_PERIODIC >> 1)
/*---------------------------------------------------------------------------*/
/* Default configuration values */
#define DEFAULT_TYPE_ID             "native"
#define DEFAULT_AUTH_TOKEN          "AUTHZ"
#define DEFAULT_SUBSCRIBE_CMD_TYPE  "+"
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (60 * CLOCK_SECOND)
#define DEFAULT_KEEP_ALIVE_TIMER    60
/*---------------------------------------------------------------------------*/
PROCESS_NAME(mqtt_demo_process);
PROCESS(broadcast_example_process, "UDP broadcast example process");
PROCESS(event_process, "Random Event Process");
AUTOSTART_PROCESSES(&mqtt_demo_process,&broadcast_example_process,&event_process);
/*---------------------------------------------------------------------------*/
/**
 * \brief Data structure declaration for the MQTT client configuration
 */
typedef struct mqtt_client_config {
  char org_id[CONFIG_ORG_ID_LEN];
  char type_id[CONFIG_TYPE_ID_LEN];
  char auth_token[CONFIG_AUTH_TOKEN_LEN];
  char broker_ip[CONFIG_IP_ADDR_STR_LEN];
  char cmd_type[CONFIG_CMD_TYPE_LEN];
  clock_time_t pub_interval;
  uint16_t broker_port;
} mqtt_client_config_t;
/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
/*---------------------------------------------------------------------------*/
/*
 * Buffers for Client ID and Topic.
 * Make sure they are large enough to hold the entire respective string
 *
 * We also need space for the null termination
 */
#define BUFFER_SIZE 64
static char client_id[BUFFER_SIZE];
static char pub_topic_neighbours[BUFFER_SIZE];
static char pub_topic_alerts[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
#define APP_BUFFER_SIZE 512
static struct mqtt_connection conn;
static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;
static struct etimer publish_periodic_timer;
static struct ctimer ct;
static char *buf_ptr;
static uint16_t seq_nr_value = 0;
/*---------------------------------------------------------------------------*/
static mqtt_client_config_t conf;
/*---------------------------------------------------------------------------*/
PROCESS(mqtt_demo_process, "MQTT Demo");
/*---------------------------------------------------------------------------*/
int
ipaddr_sprintf(char *buf, uint8_t buf_len, const uip_ipaddr_t *addr)
{
  uint16_t a;
  uint8_t len = 0;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) {
        len += snprintf(&buf[len], buf_len - len, "::");
      }
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        len += snprintf(&buf[len], buf_len - len, ":");
      }
      len += snprintf(&buf[len], buf_len - len, "%x", a);
    }
  }

  return len;
}
/*---------------------------------------------------------------------------*/
static void
pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk,
            uint16_t chunk_len)
{
  LOG_INFO("Pub handler: topic='%s' (len=%u), chunk_len=%u\n", topic, topic_len,
      chunk_len);
	int sender_id = chunk[0] - 48;
 	printf("ALERT FROM: %d\n",sender_id);

	
}
/*---------------------------------------------------------------------------*/
static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED: {
    LOG_INFO("Application has a MQTT connection!\n");
    timer_set(&connection_life, CONNECTION_STABLE_TIME);
    state = STATE_CONNECTED;
    break;
  }
  case MQTT_EVENT_DISCONNECTED: {
    LOG_INFO("MQTT Disconnect: reason %u\n", *((mqtt_event_t *)data));

    state = STATE_DISCONNECTED;
    process_poll(&mqtt_demo_process);
    break;
  }
  case MQTT_EVENT_PUBLISH: {
    msg_ptr = data;

    /*if(msg_ptr->first_chunk) {
      msg_ptr->first_chunk = 0;
      LOG_INFO("Application received a publish on topic '%s'; payload "
          "size is %i bytes\n",
          msg_ptr->topic, msg_ptr->payload_length);
    }*/

    pub_handler(msg_ptr->topic, strlen(msg_ptr->topic), msg_ptr->payload_chunk,
                msg_ptr->payload_length);
    break;
  }
  case MQTT_EVENT_SUBACK: {
    LOG_INFO("Application is subscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_UNSUBACK: {
    LOG_INFO("Application is unsubscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_PUBACK: {
    LOG_INFO("Publishing complete\n");
    break;
  }
  default:
    LOG_WARN("Application got a unhandled MQTT event: %i\n", event);
    break;
  }
}
/*---------------------------------------------------------------------------*/
static int
construct_pub_topic(void)
{
  int len = snprintf(pub_topic_neighbours, BUFFER_SIZE, MQTT_DEMO_PUBLISH_TOPIC);

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    LOG_ERR("Pub topic: %d, buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  len = snprintf(pub_topic_alerts, BUFFER_SIZE, MQTT_DEMO_PUBLISH_TOPIC_ALERT);
  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
      LOG_ERR("Pub topic: %d, buffer %d\n", len, BUFFER_SIZE);
      return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static int
construct_sub_topic(void)
{	
	//int remaining = BUFFER_SIZE;
  int len = snprintf(sub_topic, BUFFER_SIZE, "lgf/project1/%d",node_id);
	//sub_topic += len;
	//remaining -=len;
	//len = snprintf(sub_topic, remaining,"%d",node_id );
	printf("AAAAAAAAAAAAAAAAAAAAAA\n");
	printf("%s\n", sub_topic);
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
construct_client_id(void)
{
  int len = snprintf(client_id, BUFFER_SIZE, "d:%s:%s:%02x%02x%02x%02x%02x%02x",
                     conf.org_id, conf.type_id,
                     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
                     linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
                     linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    LOG_INFO("Client ID: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static void
update_config(void)
{
  if(construct_client_id() == 0) {
    /* Fatal error. Client ID larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_sub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_pub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  /* Reset the counter */
  seq_nr_value = 0;

  state = STATE_INIT;

  /*
   * Schedule next timer event ASAP
   *
   * If we entered an error state then we won't do anything when it fires
   *
   * Since the error at this stage is a config error, we will only exit this
   * error state if we get a new config
   */
  etimer_set(&publish_periodic_timer, 0);

  return;
}
/*---------------------------------------------------------------------------*/
static void
init_config()
{
  /* Populate configuration with default values */
  memset(&conf, 0, sizeof(mqtt_client_config_t));

  memcpy(conf.org_id, DEFAULT_ORG_ID, strlen(DEFAULT_ORG_ID));
  memcpy(conf.type_id, DEFAULT_TYPE_ID, strlen(DEFAULT_TYPE_ID));
  memcpy(conf.auth_token, DEFAULT_AUTH_TOKEN, strlen(DEFAULT_AUTH_TOKEN));
  memcpy(conf.broker_ip, broker_ip, strlen(broker_ip));
  memcpy(conf.cmd_type, DEFAULT_SUBSCRIBE_CMD_TYPE, 1);

  conf.broker_port = DEFAULT_BROKER_PORT;
  conf.pub_interval = DEFAULT_PUBLISH_INTERVAL;
}
/*---------------------------------------------------------------------------*/
static void
subscribe(void)
{
  mqtt_status_t status;

  status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);

  LOG_INFO("Subscribing\n");
  if(status == MQTT_STATUS_OUT_QUEUE_FULL) {
    LOG_INFO("Tried to subscribe but command queue was full!\n");
  }
}
/*---------------------------------------------------------------------------*/
static bool neighbours_to_publish(){
    //check if there are new neighbours to publish to the backend
    int i;
    for(i = 0; i < MAX_NEIGHBOURS_SAVED && neighbours[i] != NULL; i++ ){
        if(neighbours[i]->saved == false){
            break;
        }
    }
    //no new neighbours in the array
    if (i == MAX_NEIGHBOURS_SAVED || neighbours[i] == NULL){
        return false;
    }
    //there is a new neighbour to add
    return true;

}
/*---------------------------------------------------------------------------*/
static void publish_alert(void){
    int len;
    int remaining = APP_BUFFER_SIZE;

    buf_ptr = app_buffer;

    len = snprintf(buf_ptr, remaining,
                   "{"
                   "\"id\": %d}",
                   node_id);

    if(len < 0 || len >= remaining) {
        LOG_ERR("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
        return;
    }

    mqtt_publish(&conn, NULL, pub_topic_alerts, (uint8_t *)app_buffer,
                 strlen(app_buffer), MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);

    LOG_INFO("Publish for alert sent out!\n");
    event_fired = false;
}

/*---------------------------------------------------------------------------*/
static void publish_neighbours(void)
{
    //check if there's something to publish
    if(!neighbours_to_publish()){
        //LOG_INFO("No new neighbours to publish\n");
        return;
    }


  int len;
	//TODO CHECK BUFFER SIZE dimensions
  int remaining = APP_BUFFER_SIZE;

  seq_nr_value++;
  buf_ptr = app_buffer;

  len = snprintf(buf_ptr, remaining,
                   "{"
                   "\"id\": %d,"
                   "\"Seq #\":%d,"
                   "\"Uptime (sec)\":%lu,"
                   "\"neighbours\": [",
                   node_id, seq_nr_value,clock_seconds());

  if(len < 0 || len >= remaining) {
      LOG_ERR("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
      return;
  }
  remaining -= len;
  //??
  buf_ptr += len;
	
	//just because it can't be declared inside the for loop
	bool firstArrayElemet = true;
  for(int i = 0; i < MAX_NEIGHBOURS_SAVED && neighbours[i] != NULL; i++){
        if(neighbours[i]->saved == false){
            if(firstArrayElemet){
                len = snprintf(buf_ptr, remaining,"%d",neighbours[i]->id);
                firstArrayElemet = false;
            }else{
                len = snprintf(buf_ptr, remaining,",%d",neighbours[i]->id);
            }
            remaining -= len;
            //??
            buf_ptr += len;
            neighbours[i]->saved = true;
        }
  }

	len = snprintf(buf_ptr, remaining,"]}");

  if(len < 0 || len >= remaining) {
    LOG_ERR("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
    return;
  }
  
	/*remaining -= len;
  buf_ptr += len;

  // Put our Default route's string representation in a buffer
 	char def_rt_str[64];
  memset(def_rt_str, 0, sizeof(def_rt_str));
  ipaddr_sprintf(def_rt_str, sizeof(def_rt_str), uip_ds6_defrt_choose());

  len = snprintf(buf_ptr, remaining, ",\"Def Route\":\"%s\"",
                 def_rt_str);

  if(len < 0 || len >= remaining) {
    LOG_ERR("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
    return;
  }
  remaining -= len;
  buf_ptr += len;

  len = snprintf(buf_ptr, remaining, "}");

  if(len < 0 || len >= remaining) {
    LOG_ERR("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
    return;
  }*/

  mqtt_publish(&conn, NULL, pub_topic_neighbours, (uint8_t *)app_buffer,
               strlen(app_buffer), MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);

  LOG_INFO("Publish sent out!\n");
}
/*---------------------------------------------------------------------------*/
static void
connect_to_broker(void)
{
  /* Connect to MQTT server */
  mqtt_connect(&conn, conf.broker_ip, conf.broker_port,
               conf.pub_interval * 3);

  state = STATE_CONNECTING;
}
/*---------------------------------------------------------------------------*/
static void
state_machine(void)
{
  switch(state) {
  case STATE_INIT:
    /* If we have just been configured register MQTT connection */
    mqtt_register(&conn, &mqtt_demo_process, client_id, mqtt_event,
                  MAX_TCP_SEGMENT_SIZE);

    mqtt_set_username_password(&conn, "use-token-auth",
                                   conf.auth_token);

    /* _register() will set auto_reconnect; we don't want that */
    conn.auto_reconnect = 0;
    connect_attempt = 1;

    state = STATE_REGISTERED;
    LOG_INFO("Init\n");
    /* Continue */
  case STATE_REGISTERED:
    if(uip_ds6_get_global(ADDR_PREFERRED) != NULL) {
      /* Registered and with a global IPv6 address, connect! */
      LOG_INFO("Joined network! Connect attempt %u\n", connect_attempt);
      connect_to_broker();
    }
    etimer_set(&publish_periodic_timer, NET_CONNECT_PERIODIC);
    return;
    break;
  case STATE_CONNECTING:
    /* Not connected yet. Wait */
    //LOG_INFO("Connecting: retry %u...\n", connect_attempt);
    break;
  case STATE_CONNECTED:
  case STATE_PUBLISHING:
    /* If the timer expired, the connection is stable */
    if(timer_expired(&connection_life)) {
      /*
       * Intentionally using 0 here instead of 1: We want RECONNECT_ATTEMPTS
       * attempts if we disconnect after a successful connect
       */
      connect_attempt = 0;
    }

    if(mqtt_ready(&conn) && conn.out_buffer_sent) {
      /* Connected; publish */
      if(state == STATE_CONNECTED) {
        subscribe();
        state = STATE_PUBLISHING;
				//I leave it here to be sure that each time the machine passes from
				//connecting to publishing, the queue of events to be published is red
				//at least once i.e. be sure to publish after a disconnection
        etimer_set(&publish_periodic_timer, conf.pub_interval);
      } else {
          //Publish new neighbours to the specific topic
          publish_neighbours();
          //Check if there is an alert to send and possibly send it
          if(event_fired){
              publish_alert();
          }

      }
      /* Return here so we don't end up rescheduling the timer */
      return;
    } else {
      /*
       * Our publish timer fired, but some MQTT packet is already in flight
       * (either not sent at all, or sent but not fully ACKd)
       *
       * This can mean that we have lost connectivity to our broker or that
       * simply there is some network delay. In both cases, we refuse to
       * trigger a new message and we wait for TCP to either ACK the entire
       * packet after retries, or to timeout and notify us
       */
      LOG_INFO("Publishing... (MQTT state=%d, q=%u)\n", conn.state,
          conn.out_queue_full);
    }
    break;
  case STATE_DISCONNECTED:
    LOG_INFO("Disconnected\n");
    if(connect_attempt < RECONNECT_ATTEMPTS ||
       RECONNECT_ATTEMPTS == RETRY_FOREVER) {
      /* Disconnect and backoff */
      clock_time_t interval;
      mqtt_disconnect(&conn);
      connect_attempt++;

      interval = connect_attempt < 3 ? RECONNECT_INTERVAL << connect_attempt :
        RECONNECT_INTERVAL << 3;

      LOG_INFO("Disconnected: attempt %u in %lu ticks\n", connect_attempt, interval);

      etimer_set(&publish_periodic_timer, interval);

      state = STATE_REGISTERED;
      return;
    } else {
      /* Max reconnect attempts reached; enter error state */
      state = STATE_ERROR;
      LOG_ERR("Aborting connection after %u attempts\n", connect_attempt - 1);
    }
    break;
  case STATE_CONFIG_ERROR:
    /* Idle away. The only way out is a new config */
    LOG_ERR("Bad configuration.\n");
    return;
  case STATE_ERROR:
  default:
    leds_on(MQTT_DEMO_STATUS_LED);
    /*
     * 'default' should never happen
     *
     * If we enter here it's because of some error. Stop timers. The only thing
     * that can bring us out is a new config event
     */
   LOG_INFO("Default case: State=0x%02x\n", state);
    return;
  }

  /* If we didn't return so far, reschedule ourselves */
  etimer_set(&publish_periodic_timer, STATE_MACHINE_PERIODIC);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_demo_process, ev, data)
{

  PROCESS_BEGIN();

  LOG_INFO("MQTT Demo Process\n");

  init_config();
  update_config();

  /* Main loop */
  while(1) {

    PROCESS_YIELD();
    //if a timer elapses and that timer is publish periodic timer, switch state in the machine
    if (ev == PROCESS_EVENT_TIMER && data == &publish_periodic_timer) {
        state_machine();
    }else if((ev == event_of_interest_event || ev == neighbour_added_event) && state == STATE_PUBLISHING){
        printf("IN STATE PUBLISHING\n");
        state_machine();
    }

  }

  PROCESS_END();
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
  //printf("Data received on port %d from port %d sent by %d\n",receiver_port, sender_port, sender_id);
	int i;
	bool neighbour_added = false;
	for(i = 0; i < MAX_NEIGHBOURS_SAVED; i++){
		if(neighbours[i] == NULL){
			neighbours[i] = create_neighbour(sender_id);
			//check if malloc failed
			if(neighbours[i] == NULL){
				//TODO log error
				printf("malloc failed\n");
			}else{
				printf("Added new neighbour with id: %d\n", sender_id);
                neighbour_added = true;
			}
			break;
		}else{
			//neighbour already seen
			if(neighbours[i]->id == sender_id){
				//printf("Already seen neighbour: %d\n", sender_id);
				break;
			}
		}
	}
	//array of neighbours is full
	if(i == MAX_NEIGHBOURS_SAVED){
		//TODO delete last seen neighbour
		printf("can't add neighbour: %d array is full\n", sender_id);
	}

	if(neighbour_added){
        process_post(&mqtt_demo_process,neighbour_added_event, NULL);
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

  neighbour_added_event = process_alloc_event();

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
    //printf("Sending broadcast from %s\n",node_id_str);
    uip_create_linklocal_allnodes_mcast(&addr);
    simple_udp_sendto(&broadcast_connection, node_id_str, 3, &addr);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(event_process, ev, data)
{
  
  static struct etimer event_timer;
  static int event_timer_interval;

  PROCESS_BEGIN();
  event_of_interest_event = process_alloc_event();
  
  while(1) {
    event_timer_interval = (rand() % MAX_EVENT_OF_INTEREST_DELAY) + MIN_EVENT_OF_INTEREST_DELAY;
    etimer_set(&event_timer, CLOCK_SECOND * event_timer_interval);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&event_timer));
		if(ALERT_ENABLED){
			printf("EVENT OF INTEREST TRIGGERED\n");
    	event_fired = true;
    	process_post(&mqtt_demo_process,event_of_interest_event, NULL);
    }
    
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/**
 * @}
 * @}
 */

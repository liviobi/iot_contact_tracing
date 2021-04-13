Project 1 Middleware
============================

## Configurazioni preliminari:
- Avviare node red per la prima volta
    1. `docker run -it -p 1880:1880 -v node_red_data:/data --name mynodered nodered/node-red`
    2. Aprire l'[interfaccia web di Node-RED](http://127.0.0.1:1880/)
    3. Importare il flow
- Aggiungere le seguenti linee al file */etc/mosquitto/mosquitto.conf* e riavviare il broker
`
    connection bridge-01
    address mqtt.neslab.it:3200
    topic # out 0
    topic # in 0
`
## Avvio
### Node-RED:

1. `sudo docker start mynodered`
2. Aprire l'[interfaccia web di Node-RED](http://127.0.0.1:1880/)

### Cooja:

1. Aprire terminale ed inserire 
`cd iot_contact_tracing/tools/cooja/`
`ant run`
per avviare Cooja
2. Dentro Cooja avviare una nuova simulazione
*File > new simulation*
3. Impostare speed limit 100%
4. Aggiungere 1 mote rpl-border-router
5. *tasto destro sul mote > Mote tools for Contiki >Serial Socket (SERVER)*

6. Assicurarsi che la porta sia 60001 (lo è sempre se l’id del nodo è 1)
7. Cliccare start (in Serial Socket (SERVER))
8. Aprire nuovo terminale ed inserire
`cd iot_contact_tracing/examples/rpl-border-router/`
`make TARGET=cooja connect-router-cooja`

9. Verificare che compaia questa scritta verde nella console del serial socket server
10. Aggiungere i motes di tipo  *device*
11. Iniziare la simulazione cliccando start


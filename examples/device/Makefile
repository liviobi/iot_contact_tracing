DEFINES+=PROJECT_CONF_H=\"project-conf.h\"

all: mqtt-demo

CONTIKI_WITH_IPV6 = 1
MODULES += core/net/mac/tsch

APPS += mqtt
APPS += servreg-hack
CONTIKI = ../..
include $(CONTIKI)/Makefile.include

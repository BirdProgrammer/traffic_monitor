include $(RTE_SDK)/mk/rte.vars.mk

APP = trafficMonitor
SRCS-y = main.c

CFLAGS += -O3

include $(RTE_SDK)/mk/rte.extapp.mk
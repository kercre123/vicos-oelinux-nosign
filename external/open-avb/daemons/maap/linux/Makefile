BUILD_DIR=.
COMMON_DIR=../common
INCFLAGS=$(COMMON_DIR)/maap_protocol.c 
CFLAGS=$(OPT) -Wall -Werror -DUSE_GLIB `pkg-config --cflags --libs glib-2.0`
EXTRA_FLAGS=-I$(COMMON_DIR) 
FLAGS=-lpthread -lpcap
CC?=gcc

all: maap_daemon

maap_daemon: maap_linux.c
	$(CC) -o $(BUILD_DIR)/maap_daemon $(INCFLAGS) maap_linux.c $(CFLAGS) $(FLAGS) $(EXTRA_FLAGS)

clean:
	rm -rf $(BUILD_DIR)/maap_daemon

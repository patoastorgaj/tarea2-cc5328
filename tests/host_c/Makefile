# pruebas de la logica del firmware compiladas en el PC (sin ESP-IDF)
FW = ../../esp32_firmware/main
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -I$(FW)
SRCS = $(FW)/processing.c $(FW)/protocol.c $(FW)/mini_json.c

all: test_firmware
	./test_firmware

test_firmware: test_firmware.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f test_firmware

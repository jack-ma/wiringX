/*
	Copyright (c) 2014 CurlyMo <curlymoo1@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
	
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#include "wiringX.h"
#include "i2c-dev.h"
#include "raspberrypi.h"

#define	WPI_MODE_PINS		 0
#define	WPI_MODE_GPIO		 1
#define	WPI_MODE_GPIO_SYS	 2
#define	WPI_MODE_PHYS		 3
#define	WPI_MODE_PIFACE		 4
#define	WPI_MODE_UNINITIALISED	-1

#define	PI_GPIO_MASK	(0xFFFFFFC0)

#define NUM_PINS		17

#define	PI_MODEL_UNKNOWN	0
#define	PI_MODEL_A		1
#define	PI_MODEL_B		2
#define	PI_MODEL_BP		3
#define	PI_MODEL_CM		4

#define	PI_VERSION_UNKNOWN	0
#define	PI_VERSION_1		1
#define	PI_VERSION_1_1		2
#define	PI_VERSION_1_2		3
#define	PI_VERSION_2		4

#define	PI_MAKER_UNKNOWN	0
#define	PI_MAKER_EGOMAN		1
#define	PI_MAKER_SONY		2
#define	PI_MAKER_QISDA		3

#define BCM2708_PERI_BASE			   0x20000000
#define GPIO_PADS		(BCM2708_PERI_BASE + 0x00100000)
#define CLOCK_BASE		(BCM2708_PERI_BASE + 0x00101000)
#define GPIO_BASE		(BCM2708_PERI_BASE + 0x00200000)

#define	PAGE_SIZE		(4*1024)
#define	BLOCK_SIZE		(4*1024)

static int wiringPiMode = WPI_MODE_UNINITIALISED;

static volatile uint32_t *gpio;

static int pinModes[NUM_PINS] = { 0 };

static uint8_t gpioToShift[] = {
	0, 3, 6, 9, 12, 15, 18, 21, 24, 27,
	0, 3, 6, 9, 12, 15, 18, 21, 24, 27,
	0, 3, 6, 9, 12, 15, 18, 21, 24, 27,
	0, 3, 6, 9, 12, 15, 18, 21, 24, 27,
	0, 3, 6, 9, 12, 15, 18, 21, 24, 27,
};

static uint8_t gpioToGPFSEL[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};


static int *pinToGpio;

static int pinToGpioR1[64] = {
  17, 18, 21, 22, 23, 24, 25, 4,	// From the Original Wiki - GPIO 0 through 7:	wpi  0 -  7
   0,  1,							// I2C  - SDA1, SCL1				wpi  8 -  9
   8,  7,							// SPI  - CE1, CE0				wpi 10 - 11
  10,  9, 11, 						// SPI  - MOSI, MISO, SCLK			wpi 12 - 14
  14, 15,							// UART - Tx, Rx				wpi 15 - 16
// Padding:
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 31
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 47
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 63
};

static int pinToGpioR2[64] = {
  17, 18, 27, 22, 23, 24, 25, 4,	// From the Original Wiki - GPIO 0 through 7:	wpi  0 -  7
   2,  3,							// I2C  - SDA0, SCL0				wpi  8 -  9
   8,  7,							// SPI  - CE1, CE0				wpi 10 - 11
  10,  9, 11, 						// SPI  - MOSI, MISO, SCLK			wpi 12 - 14
  14, 15,							// UART - Tx, Rx				wpi 15 - 16
  28, 29, 30, 31,					// Rev 2: New GPIOs 8 though 11			wpi 17 - 20
   5,  6, 13, 19, 26,				// B+						wpi 21, 22, 23, 24, 25
  12, 16, 20, 21,					// B+						wpi 26, 27, 28, 29
   0,  1,							// B+						wpi 30, 31
// Padding:
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 47
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 63
};

static int *physToGpio;

static int physToGpioR1[64] = {
  -1,		// 0
  -1, -1,	// 1, 2
   0, -1,
   1, -1,
   4, 14,
  -1, 15,
  17, 18,
  21, -1,
  22, 23,
  -1, 24,
  10, -1,
   9, 25,
  11,  8,
  -1,  7,	// 25, 26
					 -1, -1, -1, -1, -1,	// ... 31
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 47
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,	// ... 63
} ;

static int physToGpioR2[64] = {
  -1,		// 0
  -1, -1,	// 1, 2
   2, -1,
   3, -1,
   4, 14,
  -1, 15,
  17, 18,
  27, -1,
  22, 23,
  -1, 24,
  10, -1,
   9, 25,
  11,  8,
  -1,  7,	// 25, 26
// B+
   0,  1,
   5, -1,
   6, 12,
  13, -1,
  19, 16,
  26, 20,
  -1, 21,
// the P5 connector on the Rev 2 boards:
  -1, -1,
  -1, -1,
  -1, -1,
  -1, -1,
  -1, -1,
  28, 29,
  30, 31,
  -1, -1,
  -1, -1,
  -1, -1,
  -1, -1,
};

static uint8_t gpioToGPSET[] = {
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
} ;

static uint8_t gpioToGPCLR[] = {
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
	11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,
};

static uint8_t gpioToGPLEV[] = {
	13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
	14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
};

static int sysFds[64] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static int changeOwner(char *file) {
	uid_t uid = getuid();
	uid_t gid = getgid();

	if(chown(file, uid, gid) != 0) {
		if(errno == ENOENT)	{
			fprintf(stderr, "raspberrypi->changeOwner: File not present: %s\n", file);
			return -1;
		} else {
			fprintf(stderr, "raspberrypi->changeOwner: Unable to change ownership of %s: %s\n", file, strerror (errno));
			return -1;
		}
	}

	return 0;
}

static int piBoardRev(void) {
	FILE *cpuFd;
	char line[120];
	char *c;
	static int boardRev = -1;

	if(boardRev != -1) {
		return boardRev ;
	}

	if((cpuFd = fopen("/proc/cpuinfo", "r")) == NULL) {
		fprintf(stderr, "raspberrypi->identify: Unable to open /proc/cpuinfo\n");
		return -1;
	}

	while(fgets(line, 120, cpuFd) != NULL) {
		if(strncmp (line, "Revision", 8) == 0) {
			break;
		}
	}

	fclose(cpuFd);

	if(strncmp(line, "Revision", 8) != 0) {
		fprintf(stderr, "raspberrypi->identify: No \"Revision\" line\n");
		return -1;
	}

	// Chomp trailing CR/NL

	for(c = &line[strlen(line) - 1] ; (*c == '\n') || (*c == '\r') ; --c) {
		*c = 0;
	}

	// Scan to first digit
	for(c = line; *c; ++c) {
		if(isdigit(*c)) {
			break;
		}
	}

	if(!isdigit(*c)) {
		fprintf(stderr, "raspberrypi->identify: No numeric revision string\n");
		return -1;
	}

	// Make sure its long enough

	if(strlen(c) < 4) {
		fprintf(stderr, "raspberrypi->identify: Bogus \"Revision\" line (too small)\n");
		return -1;
	}

	// Isolate  last 4 characters:

	c = c + strlen(c) - 4;

	if((strcmp(c, "0002") == 0) || (strcmp(c, "0003") == 0)) {
		boardRev = 1;
	} else {
		boardRev = 2;
	}

	return boardRev;
}

static int piBoardId(int *model, int *rev, int *mem, int *maker, int *overVolted) {
	FILE *cpuFd ;
	char line [120] ;
	char *c ;

	(void)piBoardRev();	// Call this first to make sure all's OK. Don't care about the result.

	if((cpuFd = fopen("/proc/cpuinfo", "r")) == NULL) {
		fprintf(stderr, "raspberrypi->piBoardId: Unable to open /proc/cpuinfo");
		return -1;
	}

	while(fgets (line, 120, cpuFd) != NULL) {
		if(strncmp (line, "Revision", 8) == 0) {
			break;
		}
	}

	fclose(cpuFd);

	if(strncmp(line, "Revision", 8) != 0) {
		fprintf(stderr, "raspberrypi->piBoardId: No \"Revision\" line\n");
		return -1;
	}

	// Chomp trailing CR/NL
	for(c = &line[strlen(line) - 1]; (*c == '\n') || (*c == '\r'); --c) {
		*c = 0;
	}

	// Scan to first digit
	for(c = line; *c; ++c) {
		if(isdigit(*c)) {
			break;
		}
	}

	// Make sure its long enough
	if(strlen(c) < 4) {
		fprintf(stderr, "raspberrypi->piBoardId: Bogus \"Revision\" line\n");
		return -1;
	}

	// If longer than 4, we'll assume it's been overvolted
	*overVolted = strlen(c) > 4;

	// Extract last 4 characters:
	c = c + strlen(c) - 4;

	// Fill out the replys as appropriate

	if(strcmp(c, "0002") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_1;
		*mem = 256;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "0003") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_1_1;
		*mem = 256;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "0004") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_SONY;
	} else if(strcmp(c, "0005") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_QISDA;
	} else if(strcmp(c, "0006") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "0007") == 0) {
		*model = PI_MODEL_A;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "0008") == 0) {
		*model = PI_MODEL_A;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_SONY;
	} else if(strcmp(c, "0009") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 256;
		*maker = PI_MAKER_QISDA;
	} else if(strcmp(c, "000d") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 512;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "000e") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 512;
		*maker = PI_MAKER_SONY;
	} else if(strcmp(c, "000f") == 0) {
		*model = PI_MODEL_B;
		*rev = PI_VERSION_2;
		*mem = 512;
		*maker = PI_MAKER_EGOMAN;
	} else if(strcmp(c, "0010") == 0) {
		*model = PI_MODEL_BP;
		*rev = PI_VERSION_1_2;
		*mem = 512;
		*maker = PI_MAKER_SONY;
	} else if(strcmp(c, "0011") == 0) {
		*model = PI_MODEL_CM;
		*rev = PI_VERSION_1_2;
		*mem = 512;
		*maker = PI_MAKER_SONY;
	} else {
		*model = 0;
		*rev = 0;
		*mem = 0;
		*maker = 0;
	}

	return 0;
}

static int setup(void) {
	int fd;
	int boardRev;
	int model, rev, mem, maker, overVolted;

	boardRev = piBoardRev();

	if(boardRev == 1) {
		pinToGpio =  pinToGpioR1;
		physToGpio = physToGpioR1;
	} else {
		pinToGpio =  pinToGpioR2;
		physToGpio = physToGpioR2;
	}

	if((fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC) ) < 0) {
		fprintf(stderr, "raspberrypi->setup: Unable to open /dev/mem: %s\n", strerror(errno));
		return -1;
	}

	gpio = (uint32_t *)mmap(0, BLOCK_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
	if((int32_t)gpio == -1) {
		fprintf(stderr, "raspberrypi->setup: mmap (GPIO) failed: %s\n", strerror(errno));
		return -1;
	}

	if(piBoardId(&model, &rev, &mem, &maker, &overVolted) == -1) {
		return -1;
	}
	if(model == PI_MODEL_CM) {
		wiringPiMode = WPI_MODE_GPIO;
	} else {
		wiringPiMode = WPI_MODE_PINS;
	}

	return 0;
}

static int raspberrypiDigitalRead(int pin) {
	if(pinModes[pin] != INPUT) {
		fprintf(stderr, "raspberrypi->digitalRead: Trying to write to pin %d, but it's not configured as input\n", pin);
		return -1;
	}

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpio[pin] ;
		else if (wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpio[pin] ;
		else if (wiringPiMode != WPI_MODE_GPIO)
			return -1;

		if((*(gpio + gpioToGPLEV[pin]) & (1 << (pin & 31))) != 0) {
			return HIGH;
		} else {
			return LOW;
		}
	}
	return 0;
}

static int raspberrypiDigitalWrite(int pin, int value) {
	if(pinModes[pin] != OUTPUT) {
		fprintf(stderr, "raspberrypi->digitalWrite: Trying to write to pin %d, but it's not configured as output\n", pin);
		return -1;
	}

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpio[pin] ;
		else if(wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpio[pin] ;
		else if(wiringPiMode != WPI_MODE_GPIO)
			return -1;

		if(value == LOW)
			*(gpio + gpioToGPCLR [pin]) = 1 << (pin & 31);
		else
			*(gpio + gpioToGPSET [pin]) = 1 << (pin & 31);
	}
	return 0;
}

static int raspberrypiPinMode(int pin, int mode) {
	int fSel, shift;

	if((pin & PI_GPIO_MASK) == 0) {
		if(wiringPiMode == WPI_MODE_PINS)
			pin = pinToGpio[pin];
		else if(wiringPiMode == WPI_MODE_PHYS)
			pin = physToGpio[pin];
		else if(wiringPiMode != WPI_MODE_GPIO)
			return -1;

		fSel = gpioToGPFSEL[pin];
		shift = gpioToShift[pin];

		if(mode == INPUT) {
			*(gpio + fSel) = (*(gpio + fSel) & ~(7 << shift));
		} else if(mode == OUTPUT) {
			*(gpio + fSel) = (*(gpio + fSel) & ~(7 << shift)) | (1 << shift);
		}
		pinModes[pin] = mode;
	}
	return 0;
}

static int raspberrypiISR(int pin, int mode) {
	int i = 0, fd = 0, match = 0, count = 0;
	const char *sMode = NULL;
	char path[30], c;

	pinModes[pin] = SYS;

	if(mode == INT_EDGE_FALLING) {
		sMode = "falling" ;
	} else if(mode == INT_EDGE_RISING) {
		sMode = "rising" ;
	} else if(mode == INT_EDGE_BOTH) {
		sMode = "both";
	} else {
		fprintf(stderr, "raspberrypi->isr: Invalid mode. Should be INT_EDGE_BOTH, INT_EDGE_RISING, or INT_EDGE_FALLING\n");
		return -1;
	}

	FILE *f = NULL;
	for(i=0;i<NUM_PINS;i++) {
		if(pin == i) {
			sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[i]);
			fd = open(path, O_RDWR);
			match = 1;
		}
	}

	if(!match) {
		fprintf(stderr, "raspberrypi->isr: Invalid GPIO: %d\n", pin);
		exit(0);
	}

	if(fd < 0) {
		if((f = fopen("/sys/class/gpio/export", "w")) == NULL) {
			fprintf(stderr, "raspberrypi->isr: Unable to open GPIO export interface: %s\n", strerror(errno));
			exit(0);
		}

		fprintf(f, "%d\n", pinToGpio[pin]);
		fclose(f);
	}

	sprintf(path, "/sys/class/gpio/gpio%d/direction", pinToGpio[pin]);
	if((f = fopen(path, "w")) == NULL) {
		fprintf(stderr, "raspberrypi->isr: Unable to open GPIO direction interface for pin %d: %s\n", pin, strerror(errno));
		return -1;
	}

	fprintf(f, "in\n");
	fclose(f);

	sprintf(path, "/sys/class/gpio/gpio%d/edge", pinToGpio[pin]);
	if((f = fopen(path, "w")) == NULL) {
		fprintf(stderr, "raspberrypi->isr: Unable to open GPIO edge interface for pin %d: %s\n", pin, strerror(errno));
		return -1;
	}

	if(strcasecmp(sMode, "none") == 0) {
		fprintf(f, "none\n");
	} else if(strcasecmp(sMode, "rising") == 0) {
		fprintf(f, "rising\n");
	} else if(strcasecmp(sMode, "falling") == 0) {
		fprintf(f, "falling\n");
	} else if(strcasecmp (sMode, "both") == 0) {
		fprintf(f, "both\n");
	} else {
		fprintf(stderr, "raspberrypi->isr: Invalid mode: %s. Should be rising, falling or both\n", sMode);
		return -1;
	}

	sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[pin]);
	if((sysFds[pin] = open(path, O_RDONLY)) < 0) {
		fprintf(stderr, "raspberrypi->isr: Unable to open GPIO value interface: %s\n", strerror(errno));
		return -1;
	}
	changeOwner(path);

	sprintf(path, "/sys/class/gpio/gpio%d/edge", pinToGpio[pin]);
	changeOwner(path);

	fclose(f);

	ioctl(fd, FIONREAD, &count);
	for(i=0; i<count; ++i) {
		read(fd, &c, 1);
	}
	close(fd);

	return 0;
}

static int raspberrypiWaitForInterrupt(int pin, int ms) {
	int x = 0;
	uint8_t c = 0;
	struct pollfd polls;

	if(pinModes[pin] != SYS) {
		fprintf(stderr, "raspberrypi->waitForInterrupt: Trying to read from pin %d, but it's not configured as interrupt\n", pin);
		return -1;
	}	

	if(sysFds[pin] == -1) {
		fprintf(stderr, "raspberrypi->waitForInterrupt: GPIO %d not set as interrupt\n", pin);
		return -1;
	}

	polls.fd = sysFds[pin];
	polls.events = POLLPRI;

	x = poll(&polls, 1, ms);

	(void)read(sysFds[pin], &c, 1);
	lseek(sysFds[pin], 0, SEEK_SET);

	return x;
}

static int raspberrypiGC(void) {
	int i = 0, fd = 0;
	char path[30];
	FILE *f = NULL;
	
	for(i=0;i<NUM_PINS;i++) {
		if(wiringPiMode == WPI_MODE_PINS || wiringPiMode == WPI_MODE_PHYS || wiringPiMode != WPI_MODE_GPIO) {
			pinMode(i, INPUT);
		}
		sprintf(path, "/sys/class/gpio/gpio%d/value", pinToGpio[i]);
		if((fd = open(path, O_RDWR)) > 0) {
			if((f = fopen("/sys/class/gpio/unexport", "w")) == NULL) {
				fprintf(stderr, "raspberrypi->gc: Unable to open GPIO unexport interface: %s\n", strerror(errno));
			}

			fprintf(f, "%d\n", pinToGpio[i]);
			fclose(f);
			close(fd);
		}
		if(sysFds[i] > 0) {
			close(sysFds[i]);
		}
	}

	if(gpio) {
		munmap((void *)gpio, BLOCK_SIZE);
	}
	return 0;
}

int raspberrypiI2CRead(int fd) {
	return i2c_smbus_read_byte(fd);
}

int raspberrypiI2CReadReg8(int fd, int reg) {
	return i2c_smbus_read_byte_data(fd, reg);
}

int raspberrypiI2CReadReg16(int fd, int reg) {
	return i2c_smbus_read_word_data(fd, reg);
}

int raspberrypiI2CWrite(int fd, int data) {
	return i2c_smbus_write_byte(fd, data);
}

int raspberrypiI2CWriteReg8(int fd, int reg, int data) {
	return i2c_smbus_write_byte_data(fd, reg, data);
}

int raspberrypiI2CWriteReg16(int fd, int reg, int data) {
	return i2c_smbus_write_word_data(fd, reg, data);
}

int raspberrypiI2CSetup(int devId) {
	int rev = 0, fd = 0;
	const char *device = NULL;

	if((rev = piBoardRev ()) < 0) {
		fprintf(stderr, "raspberrypi->I2CSetup: Unable to determine Pi board revision\n");
		return -1;
	}

	if(rev == 1)
		device = "/dev/i2c-0";
	else
		device = "/dev/i2c-1";

	if((fd = open(device, O_RDWR)) < 0)
		return -1;

	if(ioctl(fd, I2C_SLAVE, devId) < 0)
		return -1;

	return fd;
}

void raspberrypiInit(void) {
	device_register(&raspberrypi, "raspberrypi");
	raspberrypi->setup=&setup;
	raspberrypi->pinMode=&raspberrypiPinMode;
	raspberrypi->digitalWrite=&raspberrypiDigitalWrite;
	raspberrypi->digitalRead=&raspberrypiDigitalRead;
	raspberrypi->identify=&piBoardRev;
	raspberrypi->isr=&raspberrypiISR;
	raspberrypi->waitForInterrupt=&raspberrypiWaitForInterrupt;
	raspberrypi->I2CRead=&raspberrypiI2CRead;
	raspberrypi->I2CReadReg8=&raspberrypiI2CReadReg8;
	raspberrypi->I2CReadReg16=&raspberrypiI2CReadReg16;
	raspberrypi->I2CWrite=&raspberrypiI2CWrite;
	raspberrypi->I2CWriteReg8=&raspberrypiI2CWriteReg8;
	raspberrypi->I2CWriteReg16=&raspberrypiI2CWriteReg16;
	raspberrypi->I2CSetup=&raspberrypiI2CSetup;
	raspberrypi->gc=&raspberrypiGC;
}
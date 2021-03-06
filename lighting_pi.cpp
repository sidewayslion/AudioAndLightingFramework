/*
 * newtest.c
 *
 * Copyright (c) 2014 Jeremy Garff <jer @ jers.net>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *     1.  Redistributions of source code must retain the above copyright notice, this list of
 *         conditions and the following disclaimer.
 *     2.  Redistributions in binary form must reproduce the above copyright notice, this list
 *         of conditions and the following disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *     3.  Neither the name of the owner nor the names of its contributors may be used to endorse
 *         or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#include <getopt.h>
#include <string>
#include <iostream>
#include "version.h"

#include "ws2811.h"
#include "pattern.h"
#include "pattern_rainbow.h"
#include "pattern_pulse.h"
#include "pattern_perimeter_rainbow.h"
#include "pattern_static_color.h"
#include "log.h"

#include "beatmatch.h"
#include "beatmatchevent.h"
#include "audio/lib/libfft.h"

#include "wifi_plug.h"

/*
	PWM0, which can be set to use GPIOs 12, 18, 40, and 52.
	Only 12 (pin 32) and 18 (pin 12) are available on the B+/2B/3B
	PWM1 which can be set to use GPIOs 13, 19, 41, 45 and 53.
	Only 13 is available on the B+/2B/PiZero/3B, on pin 33
	PCM_DOUT, which can be set to use GPIOs 21 and 31.
	Only 21 is available on the B+/2B/PiZero/3B, on pin 40.
	SPI0-MOSI is available on GPIOs 10 and 38.
	Only GPIO 10 is available on all models.

	The library checks if the specified gpio is available
	on the specific model (from model B rev 1 till 3B)

*/

#define TARGET_FREQ             WS2811_TARGET_FREQ
#define GPIO_PIN_ONE            18
#define GPIO_PIN_TWO            13
#define DMA                     10
#define STRIP_TYPE              WS2811_STRIP_GRB		// WS2812/SK6812RGB integrated chip+leds


#define PI_NAME_LEN 20
#define LED_ARRAY_LEN 20

#define DEFAULT_CONTROL_PI_PORT 0
uint32_t control_pi_port = DEFAULT_CONTROL_PI_PORT;
#define DEFAULT_CONTROL_PI_IP "6.6.6.6"
char *control_pi_ip = NULL;
char *pi_name = NULL;
uint32_t led_array_length = 11;
int32_t gpio_pin = -1;
uint32_t running = 1;
uint32_t pattern[] = {0,1,2,3,4,5,6,7,8,9,10};


static void ctrl_c_handler(int signum)
{
    log_info("Control+C GET!");
	running = 0;
    (void)(signum);
}


static void setup_handlers(void)
{
    struct sigaction action;
    action.sa_handler = ctrl_c_handler;
    sigemptyset (&action.sa_mask);
    action.sa_flags=0;

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

#define MAX_IP_LENGTH 16
#define PI_NAME_LENGTH 20

struct comm_thread_params {
	const char 	 *control_pi_ip;
	uint32_t  control_pi_port;
	const char 	 *pi_name;
    uint32_t  pi_name_length;
	uint32_t  led_array_length;
	uint32_t *led_array_buf1;
	uint32_t *led_array_buf2;
	uint32_t *led_array_buf3;
	uint32_t  strip_number;
	uint32_t **receiving_array;
	uint32_t **displaying_array;
};

void print_comm_thread_params(struct comm_thread_params *params)
{
    printf("Control Pi IP:     %s\n", params->control_pi_ip);
    printf("Control Pi Port:   %d\n", params->control_pi_port);
    printf("Pi Name:           %s\n", params->pi_name);
    printf("Pi Name Length:    %d\n", params->pi_name_length);
    printf("LED Array Length:  %d\n", params->led_array_length);
    printf("LED Array Buffer1: %p\n", params->led_array_buf1);
    printf("LED Array Buffer2: %p\n", params->led_array_buf2);
    printf("LED Array Buffer3: %p\n", params->led_array_buf3);
    printf("Strip Number:      %d\n", params->strip_number);
    printf("Receive Array:     %p\n", *params->receiving_array);
    printf("Display Array:     %p\n", *params->displaying_array);
}

struct comm_thread_params*
create_comm_thread_params(const char *control_pi_ip, const uint32_t control_port,
        const char* pi_name, uint32_t strip_number, uint32_t led_array_length)
{

	struct comm_thread_params *params = (struct comm_thread_params*) malloc(sizeof(struct comm_thread_params));
    params->control_pi_ip    = control_pi_ip;
    params->control_pi_port  = control_port;
    params->pi_name          = pi_name;
    params->pi_name_length   = strlen(pi_name);
    params->led_array_length = led_array_length;
    params->led_array_buf1   = (uint32_t *) calloc(params->led_array_length, sizeof(uint32_t));
    params->led_array_buf2   = (uint32_t *) calloc(params->led_array_length, sizeof(uint32_t));
    params->led_array_buf3   = (uint32_t *) calloc(params->led_array_length, sizeof(uint32_t));
    params->strip_number     = strip_number;
    params->receiving_array  = &params->led_array_buf1;
    params->displaying_array = &params->led_array_buf2;
    return params;
}


void parseargs(int argc, char **argv)
{
	int index;
	int c;

	static struct option longopts[] =
	{
        {"help", optional_argument, 0, 'h'},
        {"port", required_argument, 0, 'P'},
        {"ip", required_argument, 0, 'I'},
        {"length", required_argument, 0, 'L'},
        {"name", required_argument, 0, 'N'},
        {"gpio", required_argument, 0, 'G'},
        {0, 0, 0, 0}
	};

	while (1)
	{

		index = 0;
		c = getopt_long(argc, argv, "hH:P:I:L:N:G:", longopts, &index);

		if (c == -1)
			break;

		switch (c)
		{
		case 0:
			/* handle flag options (array's 3rd field non-0) */
			break;
        case 'P':
            if (optarg) {
                control_pi_port = atoi(optarg);
            }
            break;
        case 'I':
            if (optarg) {
                control_pi_ip = (char*) malloc(strlen(optarg));
                strcpy(control_pi_ip, optarg);
            }
            break;
        case 'N':
            if (optarg) {
                pi_name = (char*) malloc(strlen(optarg));
                strcpy(pi_name, optarg);
            }
            break;
        case 'L':
            if (optarg) {
                led_array_length = atoi(optarg);
            }
            break;
        case 'G':
            if (optarg) {
                gpio_pin = atoi(optarg);
            }
            break;
        case 'H':
        case 'h':
            if (optarg) {
                fprintf(stderr, "P / --port: Control Pi Port Number\n");
                fprintf(stderr, "I / --ip: Control Pi IP Address\n");
                fprintf(stderr, "H / --help: This help menu\n");
                fprintf(stderr, "G / --gpio: Either the first GPIO pin or the second one");
            }
            exit(0);
        }
    }
    if (gpio_pin != 1 && gpio_pin != 2) {
        exit (-1);
    }
}

ws2811_t *
create_ledstring(uint32_t gpio_pin, uint32_t led_count)
{
    ws2811_t *ledstring; 
    ledstring = (ws2811_t*)calloc(1, sizeof(ws2811_t));
    if (ledstring == NULL) {
        printf("FARTS");
        return NULL;
    }

    ledstring->freq = TARGET_FREQ;
    ledstring->dmanum = DMA;
    if (gpio_pin == 1) {
        ledstring->channel[0].gpionum = GPIO_PIN_ONE;
        ledstring->channel[0].count = led_count;
        ledstring->channel[0].invert = 0;
        ledstring->channel[0].brightness = 100;
        ledstring->channel[0].strip_type = STRIP_TYPE;
    }
    else {
        ledstring->channel[1].gpionum = GPIO_PIN_TWO;
        ledstring->channel[1].count = led_count;
        ledstring->channel[1].invert = 0;
        ledstring->channel[1].brightness = 100;
        ledstring->channel[1].strip_type = STRIP_TYPE;
    }

    if (ws2811_init(ledstring) != WS2811_SUCCESS) {
        printf("Unable to init");
        free(ledstring);
        ledstring = NULL;
    }
    return ledstring;
}
void
stamp_ledstring(uint32_t *pattern, ws2811_t *ledstring)
{

    uint32_t channel;
    if (ledstring->channel[0].gpionum == GPIO_PIN_ONE) {
        channel = 0;
    }
    else {
        channel = 1;
    }
    uint32_t x;
    uint32_t led_count = ledstring->channel[channel].count;
    for (x = 0; x < led_count; x++)
    {
        ledstring->channel[channel].leds[x] = pattern[x];
    }
}

int main(int argc, char *argv[])
{

    parseargs(argc, argv);
    /* Handlers should only be caught in this file. And commands propogate down */
    setup_handlers();

    /* TODO: Configure one LED strip on one GPIO Pin */

    if (control_pi_ip == NULL) {
        control_pi_ip = (char*) malloc(strlen("6.6.6.6"));
        strcpy(control_pi_ip, "6.6.6.6");
    }

    if (pi_name == NULL) {
        pi_name = (char*) malloc(strlen("PI NAME UNSET"));
        strcpy(pi_name, "PI NAME UNSET");
    }
    
    /* TODO: Each of these should be in it's own thread */
    struct comm_thread_params *ctp1 = create_comm_thread_params(control_pi_ip, 
            control_pi_port, pi_name, 0, led_array_length);
    print_comm_thread_params(ctp1);

    ws2811_t *ledstring = create_ledstring(gpio_pin, led_array_length);
    while (running == 1) {
        printf("Working...%d\n", ctp1->strip_number);
        stamp_ledstring(pattern, ledstring);
        if (ws2811_render(ledstring) != WS2811_SUCCESS) {
            log_error("ws2811_render failed: ");
            running = 0;
            break;
        }
        sleep(5);
    }
    ws2811_fini(ledstring);
    return 0;
}

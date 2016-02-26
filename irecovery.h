/*
 * libirecovery.h
 * Communication to iBoot/iBSS on Apple iOS devices via USB
 *
 * Copyright (c) 2012-2013 Martin Szulecki <m.szulecki@libimobiledevice.org>
 * Copyright (c) 2010 Chronic-Dev Team
 * Copyright (c) 2010 Joshua Hill
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#ifndef LIBIRECOVERY_H
#define LIBIRECOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
	IRECV_E_SUCCESS           =  0,
	IRECV_E_NO_DEVICE         = -1,
	IRECV_E_OUT_OF_MEMORY     = -2,
	IRECV_E_UNABLE_TO_CONNECT = -3,
	IRECV_E_INVALID_INPUT     = -4,
	IRECV_E_FILE_NOT_FOUND    = -5,
	IRECV_E_USB_UPLOAD        = -6,
	IRECV_E_USB_STATUS        = -7,
	IRECV_E_USB_INTERFACE     = -8,
	IRECV_E_USB_CONFIGURATION = -9,
	IRECV_E_PIPE              = -10,
	IRECV_E_TIMEOUT           = -11,
	IRECV_E_UNKNOWN_ERROR     = -255
} irecv_error_t;

typedef enum {
	IRECV_RECEIVED            = 1,
	IRECV_PRECOMMAND          = 2,
	IRECV_POSTCOMMAND         = 3,
	IRECV_CONNECTED           = 4,
	IRECV_DISCONNECTED        = 5,
	IRECV_PROGRESS            = 6
} irecv_event_type;

typedef struct {
	int size;
	const char* data;
	double progress;
	irecv_event_type type;
} irecv_event_t;

typedef struct irecv_client_private irecv_client_private;
typedef irecv_client_private* irecv_client_t;

/* library */
const char* irecv_strerror(irecv_error_t error);
void irecv_init(void);
void irecv_exit(void);

/* device connectivity */
irecv_error_t irecv_open_with_ecid(irecv_client_t* client, unsigned long long ecid);
irecv_error_t irecv_open_with_ecid_and_attempts(irecv_client_t* pclient, unsigned long long ecid, int attempts);
irecv_error_t irecv_reset(irecv_client_t client);
irecv_error_t irecv_close(irecv_client_t client);
irecv_client_t irecv_reconnect(irecv_client_t client, int initial_pause);

/* usb helpers */
irecv_error_t irecv_usb_set_configuration(irecv_client_t client, int configuration);
irecv_error_t irecv_usb_set_interface(irecv_client_t client, int usb_interface, int usb_alt_interface);
int irecv_usb_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout);
int irecv_usb_bulk_transfer(irecv_client_t client, unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout);

/* events */
typedef int(*irecv_event_cb_t)(irecv_client_t client, const irecv_event_t* event);
irecv_error_t irecv_event_subscribe(irecv_client_t client, irecv_event_type type, irecv_event_cb_t callback, void *user_data);
irecv_error_t irecv_event_unsubscribe(irecv_client_t client, irecv_event_type type);

/*usbtmc*/
void irecv_usbtmc_init(irecv_client_t client);
int irecv_usbtmc_query(irecv_client_t client, const char *inbuf, int incount, char *outbuf, int outcount);
int irecv_usbtmc_write(irecv_client_t client, const char *buf, int count);
int irecv_usbtmc_read(irecv_client_t client, char *buf, int count);

#ifdef __cplusplus
}
#endif

#endif

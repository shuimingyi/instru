/*
 * irecovery.c
 * Communication to usbtmc devices via USB on Mac OS
 *
 * Copyright (c) 2016 shuimingyi <shuimingyi@yahoo.com>
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <pthread.h>

#define IRECV_API
#include "libirecovery.h"

struct irecv_client_private {
	int debug;
	int usb_config;
	int usb_interface;
	int usb_alt_interface;
	unsigned int mode;
	unsigned long long ecid;

	IOUSBDeviceInterface320 **handle;
	IOUSBInterfaceInterface300 **usbInterface;

	irecv_event_cb_t progress_callback;
	irecv_event_cb_t received_callback;
	irecv_event_cb_t connected_callback;
	irecv_event_cb_t precommand_callback;
	irecv_event_cb_t postcommand_callback;
	irecv_event_cb_t disconnected_callback;

	unsigned char bTag;
	unsigned char term_char; /* Termination character */
	int term_char_enabled; /* Terminate read automatically? */
	unsigned char usbtmc_last_write_bTag;
	unsigned char usbtmc_last_read_bTag;
	unsigned int number_of_bytes;
};

#define USB_TIMEOUT 10000
#define APPLE_VENDOR_ID 0x05AC

#define BUFFER_SIZE 0x1000
#define debug(...) if(libirecovery_debug) fprintf(stdout, __VA_ARGS__)

static int libirecovery_debug = 1;

static int iokit_get_string_descriptor_ascii(irecv_client_t client, uint8_t desc_index, unsigned char * buffer, int size) {

	IOReturn result;
	IOUSBDevRequest request;
	unsigned char descriptor[256];

	request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
	request.bRequest = kUSBRqGetDescriptor;
	request.wValue = (kUSBStringDesc << 8); // | desc_index;
	request.wIndex = 0; // All languages 0x409; // language
	request.wLength = sizeof(descriptor) - 1;
	request.pData = descriptor;
	request.wLenDone = 0;

	result = (*client->handle)->DeviceRequest(client->handle, &request);
	if (result == kIOReturnNoDevice)
		return IRECV_E_NO_DEVICE;
	if (result == kIOReturnNotOpen)
		return IRECV_E_USB_STATUS;
	if (result != kIOReturnSuccess)
		return IRECV_E_UNKNOWN_ERROR;

	if (descriptor[0] >= 4) { // && descriptor[2] == 0x9 && descriptor[3] == 0x4) {

		request.wValue = (kUSBStringDesc << 8) | desc_index;
		request.wIndex = descriptor[2] + (descriptor[3] << 8);
		request.wLenDone = 0;
		result = (*client->handle)->DeviceRequest(client->handle, &request);

		if (result == kIOReturnNoDevice)
			return IRECV_E_NO_DEVICE;
		if (result == kIOReturnNotOpen)
			return IRECV_E_USB_STATUS;
		if (result != kIOReturnSuccess)
			return IRECV_E_UNKNOWN_ERROR;

		int i = 2, j = 0;
		for ( ; i < descriptor[0]; i += 2, j += 1) {
			buffer[j] = descriptor[i];
		}
		buffer[j] = 0;

		return request.wLenDone;
	}
	return IRECV_E_UNKNOWN_ERROR;
}

static int check_context(irecv_client_t client) {
	if (client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	return IRECV_E_SUCCESS;
}


static int iokit_usb_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout)
{
	IOReturn result;
	IOUSBDevRequestTO req;

	bzero(&req, sizeof(req));
	req.bmRequestType     = bm_request_type;
	req.bRequest          = b_request;
	req.wValue            = OSSwapLittleToHostInt16(w_value);
	req.wIndex            = OSSwapLittleToHostInt16(w_index);
	req.wLength           = OSSwapLittleToHostInt16(w_length);
	req.pData             = data;
	req.noDataTimeout     = timeout;
	req.completionTimeout = timeout;

	result = (*client->handle)->DeviceRequestTO(client->handle, &req);
	switch (result) {
		case kIOReturnSuccess:         return req.wLenDone;
		case kIOReturnTimeout:         return IRECV_E_TIMEOUT;
		case kIOUSBTransactionTimeout: return IRECV_E_TIMEOUT;
		case kIOReturnNotResponding:   return IRECV_E_NO_DEVICE;
		case kIOReturnNoDevice:	       return IRECV_E_NO_DEVICE;
		default:
			return IRECV_E_UNKNOWN_ERROR;
	}
}

static int iokit_usb_bulk_transfer(irecv_client_t client,
						unsigned char endpoint,
						unsigned char *data,
						int length,
						int *transferred,
						unsigned int timeout) {

	IOReturn result;
	IOUSBInterfaceInterface300 **intf = client->usbInterface;
	UInt32 size = length;
	UInt8 transferDirection = endpoint & kUSBbEndpointDirectionMask;
	UInt8 numEndpoints;
	UInt8 pipeRef = 1;

	if (!intf) return IRECV_E_USB_INTERFACE;

	result = (*intf)->GetNumEndpoints(intf, &numEndpoints);

	if (result != kIOReturnSuccess || pipeRef > numEndpoints)
		return IRECV_E_USB_INTERFACE;
    
    for(UInt8 i=1; i<=numEndpoints; i++)
    {
        UInt8 direction, number, transferType;
        UInt8 interval;
        UInt16 maxPacketSize;
        
        result = (*intf)->GetPipeProperties(intf, i, &direction, &number, &transferType, &maxPacketSize, &interval);
        if (result != kIOReturnSuccess)
            return IRECV_E_USB_INTERFACE;
        if ((transferDirection == kUSBEndpointDirectionIn) && (transferType==kUSBBulk) && (direction==kUSBIn))
        {
            pipeRef = i;
            break;
        }
        else if ((transferDirection == kUSBEndpointDirectionOut) && (transferType==kUSBBulk) && (direction==kUSBOut))
        {
            pipeRef = i;
            break;
        }
    }
    

	// Just because
	result = (*intf)->GetPipeStatus(intf, pipeRef);
	switch (result) {
		case kIOReturnSuccess:  break;
		case kIOReturnNoDevice: return IRECV_E_NO_DEVICE;
		case kIOReturnNotOpen:  return IRECV_E_UNABLE_TO_CONNECT;
		default:                return IRECV_E_USB_STATUS;
	}

	// Do the transfer
	if (transferDirection == kUSBEndpointDirectionIn) {
		result = (*intf)->ReadPipeTO(intf, pipeRef, data, &size, timeout, timeout);
		if (result != kIOReturnSuccess)
			return IRECV_E_PIPE;
		*transferred = size;

		return IRECV_E_SUCCESS;
	}
	else {
		// IOUSBInterfaceClass::interfaceWritePipe (intf?, pipeRef==1, data, size=0x8000)
		result = (*intf)->WritePipeTO(intf, pipeRef, data, size, timeout, timeout);
		if (result != kIOReturnSuccess)
			return IRECV_E_PIPE;
		*transferred = size;

		return IRECV_E_SUCCESS;
	}

	return IRECV_E_USB_INTERFACE;
}

static IOReturn iokit_usb_get_interface(IOUSBDeviceInterface320 **device, uint8_t ifc, io_service_t *usbInterfacep) {

	IOUSBFindInterfaceRequest request;
	uint8_t                   current_interface;
	kern_return_t             kresult;
	io_iterator_t             interface_iterator;

	*usbInterfacep = IO_OBJECT_NULL;

	request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
	request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
	request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

	kresult = (*device)->CreateInterfaceIterator(device, &request, &interface_iterator);
	if (kresult)
		return kresult;

	for ( current_interface = 0 ; current_interface <= ifc ; current_interface++ ) {
		*usbInterfacep = IOIteratorNext(interface_iterator);
		if (current_interface != ifc)
			(void) IOObjectRelease (*usbInterfacep);
	}
	IOObjectRelease(interface_iterator);

	return kIOReturnSuccess;
}

static irecv_error_t iokit_usb_set_interface(irecv_client_t client, int usb_interface, int usb_alt_interface) {
	IOReturn result;
	io_service_t interface_service = IO_OBJECT_NULL;
	IOCFPlugInInterface **plugInInterface = NULL;
	SInt32 score;

	// Close current interface
	if (client->usbInterface) {
		result = (*client->usbInterface)->USBInterfaceClose(client->usbInterface);
		result = (*client->usbInterface)->Release(client->usbInterface);
		client->usbInterface = NULL;
	}

	result = iokit_usb_get_interface(client->handle, usb_interface, &interface_service);
	if (result != kIOReturnSuccess) {
		debug("failed to find requested interface: %d\n", usb_interface);
		return IRECV_E_USB_INTERFACE;
	}

	result = IOCreatePlugInInterfaceForService(interface_service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugInInterface, &score);
	IOObjectRelease(interface_service);
	if (result != kIOReturnSuccess) {
		debug("error creating plug-in interface: %#x\n", result);
		return IRECV_E_USB_INTERFACE;
	}

	result = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID)&client->usbInterface);
	IODestroyPlugInInterface(plugInInterface);
	if (result != kIOReturnSuccess) {
		debug("error creating interface interface: %#x\n", result);
		return IRECV_E_USB_INTERFACE;
	}

	result = (*client->usbInterface)->USBInterfaceOpen(client->usbInterface);
	if (result != kIOReturnSuccess) {
		debug("error opening interface: %#x\n", result);
		return IRECV_E_USB_INTERFACE;
	}

	if (usb_interface == 1) {
		result = (*client->usbInterface)->SetAlternateInterface(client->usbInterface, usb_alt_interface);
		if (result != kIOReturnSuccess) {
			debug("error setting alternate interface: %#x\n", result);
			return IRECV_E_USB_INTERFACE;
		}
	}

	return IRECV_E_SUCCESS;
}

IRECV_API irecv_error_t irecv_usb_set_interface(irecv_client_t client, int usb_interface, int usb_alt_interface) {
	if (check_context(client) != IRECV_E_SUCCESS)
		return IRECV_E_NO_DEVICE;

	debug("Setting to interface %d:%d\n", usb_interface, usb_alt_interface);

	if (iokit_usb_set_interface(client, usb_interface, usb_alt_interface) < 0) {
		return IRECV_E_USB_INTERFACE;
	}

	client->usb_interface = usb_interface;
	client->usb_alt_interface = usb_alt_interface;

	return IRECV_E_SUCCESS;
}

IRECV_API irecv_error_t irecv_usb_set_configuration(irecv_client_t client, int configuration) {
	if (check_context(client) != IRECV_E_SUCCESS)
		return IRECV_E_NO_DEVICE;

	IOReturn result;

	result = (*client->handle)->SetConfiguration(client->handle, configuration);
	if (result != kIOReturnSuccess) {
		debug("error setting configuration: %#x\n", result);
		return IRECV_E_USB_CONFIGURATION;
	}
	return IRECV_E_SUCCESS;
}


static irecv_error_t iokit_usb_open_service(irecv_client_t *pclient, io_service_t service) {

	IOReturn result;
	irecv_error_t error;
	irecv_client_t client;
	SInt32 score;
	UInt16 mode;
	UInt32 locationID;
	IOCFPlugInInterface **plug = NULL;
	CFStringRef serialString;

	client = (irecv_client_t) calloc( 1, sizeof(struct irecv_client_private));

	// Create the plug-in
	result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
	if (result != kIOReturnSuccess) {
		IOObjectRelease(service);
		free(client);
		return IRECV_E_UNKNOWN_ERROR;
	}

	// Cache the serial string before discarding the service. The service object
	// has a cached copy, so a request to the hardware device is not required.
	char serial_str[256];
	serial_str[0] = '\0';
	serialString = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBSerialNumberString), kCFAllocatorDefault, 0);
	if (serialString) {
		CFStringGetCString(serialString, serial_str, sizeof(serial_str), kCFStringEncodingUTF8);
        debug("%s\n", serial_str);
		CFRelease(serialString);
	}

	IOObjectRelease(service);

	// Create the device interface
	result = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&(client->handle));
	IODestroyPlugInInterface(plug);
	if (result != kIOReturnSuccess) {
		free(client);
		return IRECV_E_UNKNOWN_ERROR;
	}

	(*client->handle)->GetDeviceProduct(client->handle, &mode);
	(*client->handle)->GetLocationID(client->handle, &locationID);
	client->mode = mode;
	debug("opening device %04x:%04x @ %#010x...\n", 0x049f, client->mode, locationID);

	result = (*client->handle)->USBDeviceOpenSeize(client->handle);
	if (result != kIOReturnSuccess) {
		(*client->handle)->Release(client->handle);
		free(client);
		return IRECV_E_UNABLE_TO_CONNECT;
	}

	error = irecv_usb_set_configuration(client, 1);
	if (error != IRECV_E_SUCCESS) {
		free(client);
		return error;
	}

	error = irecv_usb_set_interface(client, 0, 0);
	if (error != IRECV_E_SUCCESS) {
		free(client);
		return error;
	}

	*pclient = client;
	return IRECV_E_SUCCESS;
}

static void iokit_cfdictionary_set_short(CFMutableDictionaryRef dict, const void *key, SInt16 value)
{
	CFNumberRef numberRef;

	numberRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &value);
	if (numberRef) {
		CFDictionarySetValue(dict, key, numberRef);
		CFRelease(numberRef);
	}
}

static io_iterator_t iokit_usb_get_iterator_for_pid(UInt16 pid) {

	IOReturn result;
	io_iterator_t iterator;
	CFMutableDictionaryRef matchingDict;

	matchingDict = IOServiceMatching(kIOUSBDeviceClassName);

	iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBVendorID), 0x049f);
	iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBProductID), pid);

	result = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iterator);
	if (result != kIOReturnSuccess)
		return IO_OBJECT_NULL;

	return iterator;
}

static irecv_error_t iokit_open_with_ecid(irecv_client_t* pclient, unsigned long long ecid) {

	io_service_t service, ret_service;
	io_iterator_t iterator;
	CFStringRef usbSerial = NULL;
	CFStringRef ecidString = NULL;
	CFRange range;

	if (pclient == NULL) {
		debug("%s: pclient parameter is null\n", __func__);
		return IRECV_E_INVALID_INPUT;
	}

	if (ecid > 0) 
	{
		ecidString = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%s"), "HTG");
		if (ecidString == NULL) 
		{
			debug("%s: failed to create ECID string\n", __func__);
			return IRECV_E_UNABLE_TO_CONNECT;
		}
	}

	*pclient = NULL;
	ret_service = IO_OBJECT_NULL;

	iterator = iokit_usb_get_iterator_for_pid(0x505b);
	if (iterator) {
		while ((service = IOIteratorNext(iterator))) {

			if (ecid == 0) {
				ret_service = service;
				break;
			}

			usbSerial = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBSerialNumberString), kCFAllocatorDefault, 0);
			if (usbSerial == NULL) {
				debug("%s: failed to create USB serial string property\n", __func__);
				IOObjectRelease(service);
				continue;
			}

			range = CFStringFind(usbSerial, ecidString, kCFCompareCaseInsensitive);
			if (range.location == kCFNotFound) 
			{
				IOObjectRelease(service);
			} 
			else 
			{
				ret_service = service;
				break;
			}
		}

		if (usbSerial) {
			CFRelease(usbSerial);
			usbSerial = NULL;
		}
		IOObjectRelease(iterator);
	}
	if (ecidString)
		CFRelease(ecidString);

	if (ret_service == IO_OBJECT_NULL)
		return IRECV_E_UNABLE_TO_CONNECT;

	return iokit_usb_open_service(pclient, ret_service);
}

static int irecv_get_string_descriptor_ascii(irecv_client_t client, uint8_t desc_index, unsigned char * buffer, int size) {
	return iokit_get_string_descriptor_ascii(client, desc_index, buffer, size);
}

IRECV_API int irecv_usb_bulk_transfer(irecv_client_t client,
							unsigned char endpoint,
							unsigned char *data,
							int length,
							int *transferred,
							unsigned int timeout) {
	return iokit_usb_bulk_transfer(client, endpoint, data, length, transferred, timeout);
}

IRECV_API int irecv_usb_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout) {
	return iokit_usb_control_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, timeout);
}

IRECV_API irecv_error_t irecv_reset(irecv_client_t client) {
	if (check_context(client) != IRECV_E_SUCCESS)
		return IRECV_E_NO_DEVICE;
	IOReturn result;

	result = (*client->handle)->ResetDevice(client->handle);
	if (result != kIOReturnSuccess && result != kIOReturnNotResponding) {
		debug("error sending device reset: %#x\n", result);
		return IRECV_E_UNKNOWN_ERROR;
	}

	return IRECV_E_SUCCESS;
}

IRECV_API irecv_error_t irecv_event_subscribe(irecv_client_t client, irecv_event_type type, irecv_event_cb_t callback, void* user_data) {
	switch(type) {
	case IRECV_RECEIVED:
		client->received_callback = callback;
		break;

	case IRECV_PROGRESS:
		client->progress_callback = callback;

	case IRECV_CONNECTED:
		client->connected_callback = callback;

	case IRECV_PRECOMMAND:
		client->precommand_callback = callback;
		break;

	case IRECV_POSTCOMMAND:
		client->postcommand_callback = callback;
		break;

	case IRECV_DISCONNECTED:
		client->disconnected_callback = callback;

	default:
		return IRECV_E_UNKNOWN_ERROR;
	}

	return IRECV_E_SUCCESS;
}

IRECV_API irecv_error_t irecv_event_unsubscribe(irecv_client_t client, irecv_event_type type) {
	switch(type) {
	case IRECV_RECEIVED:
		client->received_callback = NULL;
		break;

	case IRECV_PROGRESS:
		client->progress_callback = NULL;

	case IRECV_CONNECTED:
		client->connected_callback = NULL;

	case IRECV_PRECOMMAND:
		client->precommand_callback = NULL;
		break;

	case IRECV_POSTCOMMAND:
		client->postcommand_callback = NULL;
		break;

	case IRECV_DISCONNECTED:
		client->disconnected_callback = NULL;

	default:
		return IRECV_E_UNKNOWN_ERROR;
	}

	return IRECV_E_SUCCESS;
}

IRECV_API const char* irecv_strerror(irecv_error_t error) {
	switch (error) {
	case IRECV_E_SUCCESS:
		return "Command completed successfully";

	case IRECV_E_NO_DEVICE:
		return "Unable to find device";

	case IRECV_E_OUT_OF_MEMORY:
		return "Out of memory";

	case IRECV_E_UNABLE_TO_CONNECT:
		return "Unable to connect to device";

	case IRECV_E_INVALID_INPUT:
		return "Invalid input";

	case IRECV_E_FILE_NOT_FOUND:
		return "File not found";

	case IRECV_E_USB_UPLOAD:
		return "Unable to upload data to device";

	case IRECV_E_USB_STATUS:
		return "Unable to get device status";

	case IRECV_E_USB_INTERFACE:
		return "Unable to set device interface";

	case IRECV_E_USB_CONFIGURATION:
		return "Unable to set device configuration";

	case IRECV_E_PIPE:
		return "Broken pipe";

	case IRECV_E_TIMEOUT:
		return "Timeout talking to device";

	default:
		return "Unknown error";
	}

	return NULL;
}

IRECV_API irecv_error_t irecv_close(irecv_client_t client) {
	if (client != NULL) {
		if(client->disconnected_callback != NULL) {
			irecv_event_t event;
			event.size = 0;
			event.data = NULL;
			event.progress = 0;
			event.type = IRECV_DISCONNECTED;
			client->disconnected_callback(client, &event);
		}

		if (client->usbInterface) {
			(*client->usbInterface)->USBInterfaceClose(client->usbInterface);
			(*client->usbInterface)->Release(client->usbInterface);
			client->usbInterface = NULL;
		}
		if (client->handle) {
			(*client->handle)->USBDeviceClose(client->handle);
			(*client->handle)->Release(client->handle);
			client->handle = NULL;
		}

		free(client);
		client = NULL;
	}

	return IRECV_E_SUCCESS;
}

IRECV_API irecv_error_t irecv_open_with_ecid(irecv_client_t* pclient, unsigned long long ecid) {
	return iokit_open_with_ecid(pclient, ecid);
}

IRECV_API irecv_error_t irecv_open_with_ecid_and_attempts(irecv_client_t* pclient, unsigned long long ecid, int attempts) 
{
	int i;

	for (i = 0; i < attempts; i++) {
		if(*pclient) {
			irecv_close(*pclient);
			*pclient = NULL;
		}
		if (irecv_open_with_ecid(pclient, ecid) != IRECV_E_SUCCESS) {
			debug("Connection failed. Waiting 1 sec before retry.\n");
			sleep(1);
		} else {
			return IRECV_E_SUCCESS;
		}
	}

	return IRECV_E_UNABLE_TO_CONNECT;
}

IRECV_API irecv_client_t irecv_reconnect(irecv_client_t client, int initial_pause) 
{
	irecv_error_t error = 0;
	irecv_client_t new_client = NULL;
	irecv_event_cb_t progress_callback = client->progress_callback;

	unsigned long long ecid = client->ecid;

	if (check_context(client) == IRECV_E_SUCCESS) {
		irecv_close(client);
	}

	if (initial_pause > 0) {
		debug("Waiting %d seconds for the device to pop up...\n", initial_pause);
		sleep(initial_pause);
	}
	
	error = irecv_open_with_ecid_and_attempts(&new_client, ecid, 10);
	if(error != IRECV_E_SUCCESS) {
		return NULL;
	}

	new_client->progress_callback = progress_callback;

	return new_client;
}



IRECV_API void irecv_init(void) {

}

IRECV_API void irecv_exit(void) {

}

/* Size of driver internal buffer for regular I/O (bytes). Must be a multiple of 4 and at least as large
 * as USB parameter wMaxPacketSize (which is usually 512 bytes). */
#define USBTMC_SIZE_IOBUFFER 							4096

/* Default timeout (jiffies) */
#define USBTMC_DEFAULT_TIMEOUT 							5 * HZ

/* Maximum number of read cycles to empty bulk in endpoint during CLEAR and ABORT_BULK_IN requests.
 * Ends the loop if (for whatever reason) a short packet is not read in time. */
#define USBTMC_MAX_READS_TO_CLEAR_BULK_IN				100

/* Driver state */
#define USBTMC_DRV_STATE_CLOSED							0
#define USBTMC_DRV_STATE_OPEN							1

/* USBTMC base class status values */
#define USBTMC_STATUS_SUCCESS							0x01
#define USBTMC_STATUS_PENDING							0x02
#define USBTMC_STATUS_FAILED							0x80
#define USBTMC_STATUS_TRANSFER_NOT_IN_PROGRESS			0x81
#define USBTMC_STATUS_SPLIT_NOT_IN_PROGRESS				0x82
#define USBTMC_STATUS_SPLIT_IN_PROGRESS					0x83
/* USB488 sub class status values */
#define USBTMC_STATUS_STATUS_INTERRUPT_IN_BUSY			0x20

/* USBTMC base class bRequest values */
#define USBTMC_BREQUEST_INITIATE_ABORT_BULK_OUT			1
#define USBTMC_BREQUEST_CHECK_ABORT_BULK_OUT_STATUS		2
#define USBTMC_BREQUEST_INITIATE_ABORT_BULK_IN			3
#define USBTMC_BREQUEST_CHECK_ABORT_BULK_IN_STATUS		4
#define USBTMC_BREQUEST_INITIATE_CLEAR					5
#define USBTMC_BREQUEST_CHECK_CLEAR_STATUS				6
#define USBTMC_BREQUEST_GET_CAPABILITIES				7
#define USBTMC_BREQUEST_INDICATOR_PULSE					64
/* USB488 sub class bRequest values */
#define USBTMC_BREQUEST_READ_STATUS_BYTE				128
#define USBTMC_BREQUEST_REN_CONTROL						160
#define USBTMC_BREQUEST_GO_TO_LOCAL						161
#define USBTMC_BREQUEST_LOCAL_LOCKOUT					162

/* USBTMC MsgID values */
#define USBTMC_MSGID_DEV_DEP_MSG_OUT					1
#define USBTMC_MSGID_DEV_DEP_MSG_IN						2
#define USBTMC_MSGID_REQUEST_DEV_DEP_MSG_IN				2
#define USBTMC_MSGID_VENDOR_SPECIFIC_OUT				126
#define USBTMC_MSGID_VENDOR_SPECIFIC_IN					127
#define USBTMC_MSGID_REQUEST_VENDOR_SPECIFIC_IN			127
#define USBTMC_MSGID_TRIGGER							128

void irecv_usbtmc_init(irecv_client_t client)
{
	/* Initialize bTag and other fields */
	client->bTag = 1;
	client->term_char_enabled = 0;
	client->term_char = '\n';
}

int irecv_usbtmc_read(irecv_client_t client, char *buf, int count)
{
	int ret, actual, remaining, done, this_part;
	unsigned long int num_of_characters;
	char usbtmc_buffer[USBTMC_SIZE_IOBUFFER];

	memset(usbtmc_buffer, '\0', sizeof(usbtmc_buffer));
	/* Verify pointer and driver state */
	if (check_context(client) != IRECV_E_SUCCESS)
		return IRECV_E_NO_DEVICE;


	remaining = count;
	done = 0;
	
	while (remaining > 0)
	{
		/* Check if remaining data bytes to be read fit in the driver's buffer. Make sure there is enough
		 * space for the header (12 bytes) and alignment bytes (up to 3 bytes). */
		if (remaining > USBTMC_SIZE_IOBUFFER - 12 - 3)
		{
			this_part = USBTMC_SIZE_IOBUFFER - 12 - 3;
		}
		else
		{
			this_part = remaining;
		}
		
		/* Setup IO buffer for DEV_DEP_MSG_IN message */
		usbtmc_buffer[0x00] = USBTMC_MSGID_REQUEST_DEV_DEP_MSG_IN;
		usbtmc_buffer[0x01] = client->bTag; /* Transfer ID (bTag) */
		usbtmc_buffer[0x02] = ~(client->bTag); /* Inverse of bTag */
		usbtmc_buffer[0x03] = 0; /* Reserved */
		usbtmc_buffer[0x04] = this_part & 255; /* Max transfer (first byte) */
		usbtmc_buffer[0x05] = (this_part >> 8) & 255; /* Second byte */
		usbtmc_buffer[0x06] = (this_part >> 16) & 255; /* Third byte */
		usbtmc_buffer[0x07] = (this_part >> 24) & 255; /* Fourth byte */
		usbtmc_buffer[0x08] = client->term_char_enabled * 2;
		usbtmc_buffer[0x09] = client->term_char; /* Term character */
		usbtmc_buffer[0x0a] = 0; /* Reserved */
		usbtmc_buffer[0x0b] = 0; /* Reserved */
	
		/* Create pipe and send USB request */
		ret = irecv_usb_bulk_transfer(client, 0x04, (unsigned char*)usbtmc_buffer, 12, &actual, USB_TIMEOUT);
		
		/* Store bTag (in case we need to abort) */
		client->usbtmc_last_write_bTag = client->bTag;
	
		/* Increment bTag -- and increment again if zero */
		client->bTag++;
		if (client->bTag == 0)
			client->bTag++;
		if (ret < 0)
		{
			debug("usb_bulk_msg() returned %d\n", ret);
			return ret;
		}
	
		/* Create pipe and send USB request */
		ret = irecv_usb_bulk_transfer(client, 0x81, (unsigned char*)usbtmc_buffer, USBTMC_SIZE_IOBUFFER, &actual, 500);
		
		/* Store bTag (in case we need to abort) */
		client->usbtmc_last_read_bTag = client->bTag;
		if (ret < 0)
		{
			debug("usb_bulk_msg() read returned %d\n", ret);
			return ret;
		}
	
		/* How many characters did the instrument send? */
		num_of_characters = usbtmc_buffer[4] + (usbtmc_buffer[5] << 8) + (usbtmc_buffer[6] << 16) +
			(usbtmc_buffer[7] << 24);
	
		/* Copy buffer to user*/
        	memcpy(buf + done, &usbtmc_buffer[12], num_of_characters);
		
		done += num_of_characters;

		if (num_of_characters < this_part)
		{
			/* Short package received (less than requested amount of bytes), exit loop */
			remaining = 0;
		}
	}
	
	return done; /* Number of bytes read (total) */
}

/* This function sends a string to an instrument by wrapping it in a USMTMC DEV_DEP_MSG_OUT message. */
int irecv_usbtmc_write(irecv_client_t client, const char *buf, int count)
{
	int ret, n, actual, remaining, done, this_part;
	int num_of_bytes;
	unsigned char last_transaction;
	char usbtmc_buffer[USBTMC_SIZE_IOBUFFER];
	memset(usbtmc_buffer, '\0', sizeof(usbtmc_buffer));
	
	if (check_context(client) != IRECV_E_SUCCESS)
		return IRECV_E_NO_DEVICE;
	
	client->number_of_bytes = 0; /* In case of data left over in buffer for minor number zero */

	remaining = count;
	done = 0;
	
	while (remaining > 0) /* Still bytes to send */
	{
		if (remaining > USBTMC_SIZE_IOBUFFER - 12)
		{
			/* Use maximum size (limited by driver internal buffer size) */
			this_part = USBTMC_SIZE_IOBUFFER - 12; /* Use maximum size */
			last_transaction = 0; /* This is not the last transfer */
		}
		else
		{
			/* Can send remaining bytes in a single transaction */
			this_part = remaining;
			last_transaction = 1; /* Message ends w/ this transfer */
		}
		
		/* Setup IO buffer for DEV_DEP_MSG_OUT message */
		usbtmc_buffer[0x00] = USBTMC_MSGID_DEV_DEP_MSG_OUT;
		usbtmc_buffer[0x01] = client->bTag; /* Transfer ID (bTag) */
		usbtmc_buffer[0x02] = ~client->bTag; /* Inverse of bTag */
		usbtmc_buffer[0x03] = 0; /* Reserved */
		usbtmc_buffer[0x04] = this_part & 255; /* Transfer size (first byte) */
		usbtmc_buffer[0x05] = (this_part >> 8) & 255; /* Transfer size (second byte) */
		usbtmc_buffer[0x06] = (this_part >> 16) & 255; /* Transfer size (third byte) */
		usbtmc_buffer[0x07] = (this_part >> 24) & 255; /* Transfer size (fourth byte) */
		usbtmc_buffer[0x08] = last_transaction; /* 1 = yes, 0 = no */
		usbtmc_buffer[0x09] = 0; /* Reserved */
		usbtmc_buffer[0x0a] = 0; /* Reserved */
		usbtmc_buffer[0x0b] = 0; /* Reserved */
		
		/* Append write buffer (instrument command) to USBTMC message */
		memcpy(&usbtmc_buffer[12], buf + done, this_part);
		
		/* Add zero bytes to achieve 4-byte alignment */
		num_of_bytes = 12 + this_part;
		if (this_part % 4)
		{
			num_of_bytes += 4 - this_part % 4;
			for (n = 12 + this_part; n < num_of_bytes; n++)
				usbtmc_buffer[n] = 0;
		}
	
		ret = irecv_usb_bulk_transfer(client, 0x04, (unsigned char*)usbtmc_buffer, num_of_bytes, &actual, USB_TIMEOUT);
	
		/* Store bTag (in case we need to abort) */
		client->usbtmc_last_write_bTag = client->bTag;
		
		/* Increment bTag -- and increment again if zero */
		client->bTag++;
		if (client->bTag == 0)
			client->bTag++;
		
		if (ret < 0)
		{
			debug("usb_bulk_msg() write returned %d\n", ret);
			return ret;
		}
		
		remaining -= this_part;
		done += this_part;
	}
	
	return count;
}

int irecv_usbtmc_query(irecv_client_t client, const char *inbuf, int incount, char *outbuf, int outcount)
{
	if(irecv_usbtmc_write(client, inbuf, incount) > 0)
	{
        	return irecv_usbtmc_read(client, outbuf, outcount);
	}
	else
	{
	    debug("query write wrong\n");
	}

	return IRECV_E_PIPE;
}

#if 0
int main(int argc, char **argv)
{
	irecv_error_t error;
	irecv_client_t client;
	char buf[256];

	irecv_init();

	error = irecv_open_with_ecid(&client, 0);
	if(error < 0)
	{
		debug("open dev error\n");
		return -1;
	}

	irecv_usbtmc_init(client);
	int ret = irecv_usbtmc_query(client, "*IDN?", strlen("*IDN?"), buf, sizeof(buf));
	if(ret > 0)
	{
		debug("%s\n", buf);
	}

	irecv_close(client);
	return 0;
}
#endif



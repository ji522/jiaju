/*******************************************************************************
 * Copyright (c) 2014, 2015 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Allan Stockdill-Mander - initial API and implementation and/or initial documentation
 *    Ian Craggs - convert to FreeRTOS
 *******************************************************************************/

#include "MQTTFreeRTOS.h"
#include <stdio.h>
#include <string.h>

int ThreadStart(Thread* thread, void (*fn)(void*), void* arg)
{
	int rc = 0;
	uint16_t usTaskStackSize = (configMINIMAL_STACK_SIZE * 5);
	UBaseType_t uxTaskPriority = uxTaskPriorityGet(NULL);

	rc = xTaskCreate(fn,
		"MQTTTask",
		usTaskStackSize,
		arg,
		uxTaskPriority,
		&thread->task);

	return rc;
}

void MutexInit(Mutex* mutex)
{
	mutex->sem = xSemaphoreCreateMutex();
}

int MutexLock(Mutex* mutex)
{
	return xSemaphoreTake(mutex->sem, portMAX_DELAY);
}

int MutexUnlock(Mutex* mutex)
{
	return xSemaphoreGive(mutex->sem);
}

void TimerCountdownMS(Timer* timer, unsigned int timeout_ms)
{
	timer->xTicksToWait = timeout_ms / portTICK_PERIOD_MS;
	vTaskSetTimeOutState(&timer->xTimeOut);
}

void TimerCountdown(Timer* timer, unsigned int timeout)
{
	TimerCountdownMS(timer, timeout * 1000);
}

int TimerLeftMS(Timer* timer)
{
	xTaskCheckForTimeOut(&timer->xTimeOut, &timer->xTicksToWait);
	return (int)(timer->xTicksToWait * portTICK_PERIOD_MS);
}

char TimerIsExpired(Timer* timer)
{
	return xTaskCheckForTimeOut(&timer->xTimeOut, &timer->xTicksToWait) == pdTRUE;
}

void TimerInit(Timer* timer)
{
	timer->xTicksToWait = 0;
	memset(&timer->xTimeOut, '\0', sizeof(timer->xTimeOut));
}

int FreeRTOS_read(Network* n, unsigned char* buffer, int len, int timeout_ms)
{
	if(n->netdev->Read(n->netdev, (char*)buffer, len, timeout_ms) == 0)
	{
		return len;
	}

	return 0;
}

int FreeRTOS_write(Network* n, unsigned char* buffer, int len, int timeout_ms)
{
	int sentLen = 0;

	if(n->netdev->Write(n->netdev, (char*)buffer, len, timeout_ms) == 0)
	{
		sentLen = len;
	}

	return sentLen;
}

void FreeRTOS_disconnect(Network* n)
{
	TCP_UDP_Info tcp = {TCP_ID};
	n->netdev->Disconnect(n->netdev, (char*)&tcp, 100);
}

void NetworkDisconnect(Network* n)
{
	if(n != NULL && n->disconnect != NULL)
	{
		n->disconnect(n);
	}
}

/* Local Wi-Fi credentials used by the ESP8266 transport layer. */
const static WiFiInfo wifi = {WiFi_ID, "iPhone16proMax", "333666999"};

void NetworkInit(Network* n)
{
	n->netdev = NetDev_GetDev(ESP8266);
	if(n->netdev != NULL)
	{
		if(n->netdev->Init(n->netdev) == 0)
		{
			if(n->netdev->Connect(n->netdev, (char*)&wifi.id, 5000) == 0)
			{
				printf("Connect WiFi success.\r\n");
				n->mqttread = FreeRTOS_read;
				n->mqttwrite = FreeRTOS_write;
				n->disconnect = FreeRTOS_disconnect;
			}
			else
			{
				printf("Connect WiFi fail.\r\n");
			}
		}
		else
		{
			printf("Net Device Init fail.\r\n");
		}
	}
	else
	{
		printf("Net Device not found.\r\n");
	}
}

int NetworkConnect(Network* n, char* addr, int port)
{
	TCP_UDP_Info tcp = {TCP_ID, addr, 0, port};
	return n->netdev->Connect(n->netdev, (char*)&tcp, 8000);
}

#if 0
int NetworkConnectTLS(Network *n, char* addr, int port, SlSockSecureFiles_t* certificates, unsigned char sec_method, unsigned int cipher, char server_verify)
{
	SlSockAddrIn_t sAddr;
	int addrSize;
	int retVal;
	unsigned long ipAddress;

	retVal = sl_NetAppDnsGetHostByName(addr, strlen(addr), &ipAddress, AF_INET);
	if (retVal < 0) {
		return -1;
	}

	sAddr.sin_family = AF_INET;
	sAddr.sin_port = sl_Htons((unsigned short)port);
	sAddr.sin_addr.s_addr = sl_Htonl(ipAddress);

	addrSize = sizeof(SlSockAddrIn_t);

	n->my_socket = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, SL_SEC_SOCKET);
	if (n->my_socket < 0) {
		return -1;
	}

	SlSockSecureMethod method;
	method.secureMethod = sec_method;
	retVal = sl_SetSockOpt(n->my_socket, SL_SOL_SOCKET, SL_SO_SECMETHOD, &method, sizeof(method));
	if (retVal < 0) {
		return retVal;
	}

	SlSockSecureMask mask;
	mask.secureMask = cipher;
	retVal = sl_SetSockOpt(n->my_socket, SL_SOL_SOCKET, SL_SO_SECURE_MASK, &mask, sizeof(mask));
	if (retVal < 0) {
		return retVal;
	}

	if (certificates != NULL) {
		retVal = sl_SetSockOpt(n->my_socket, SL_SOL_SOCKET, SL_SO_SECURE_FILES, certificates->secureFiles, sizeof(SlSockSecureFiles_t));
		if (retVal < 0)
		{
			return retVal;
		}
	}

	retVal = sl_Connect(n->my_socket, (SlSockAddr_t *)&sAddr, addrSize);
	if (retVal < 0) {
		if (server_verify || retVal != -453) {
			sl_Close(n->my_socket);
			return retVal;
		}
	}

	SysTickIntRegister(SysTickIntHandler);
	SysTickPeriodSet(80000);
	SysTickEnable();

	return retVal;
}
#endif

/**
 * @file main.c
 * @brief 智能家居应用程序入口（F407 + 诊断任务）
 */

#include "main.h"
#include "dev_io.h"
#include "driver_net.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

void SystemClock_Config(void);

/* Keep the vehicle control path ahead of cloud protocol processing.
 * Lower-rate UI and diagnostic work must not delay CAN handling. */
#define APP_TASK_PRIO_DIAG    1U
#define APP_TASK_PRIO_LED     2U
#define APP_TASK_PRIO_KEY     4U
#define APP_TASK_PRIO_MQTT    6U
#define APP_TASK_PRIO_CAN     8U

#if !(APP_TASK_PRIO_CAN > APP_TASK_PRIO_MQTT && \
	APP_TASK_PRIO_MQTT > APP_TASK_PRIO_KEY && \
	APP_TASK_PRIO_KEY > APP_TASK_PRIO_LED && \
	APP_TASK_PRIO_LED > APP_TASK_PRIO_DIAG)
#error "Application task priorities must preserve the control-path ordering."
#endif

#if APP_TASK_PRIO_CAN >= configMAX_PRIORITIES
#error "Application task priority exceeds configMAX_PRIORITIES."
#endif

volatile const char *g_pcFreeRTOSFaultReason = NULL;
volatile const char *g_pcFreeRTOSFaultTaskName = NULL;
volatile const char *g_pcFreeRTOSAssertFile = NULL;
volatile unsigned long g_ulFreeRTOSAssertLine = 0UL;

extern void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartLEDTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartKeyTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartCANTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

extern volatile uint32_t g_mqtt_reconn_count;
extern volatile int g_mqtt_state;
extern volatile uint32_t g_can_tx_count;
extern volatile uint32_t g_can_tx_fail_count;
extern volatile uint32_t g_can_last_tx_fail_id;
extern volatile uint32_t g_can_last_error;
extern volatile uint32_t g_can_last_esr;
extern volatile uint32_t g_can_rx_count;
extern volatile uint32_t g_can_last_rx_id;
extern volatile uint8_t g_can_last_cmd_seq;
extern volatile uint8_t g_can_last_status_seq;
extern volatile uint8_t g_can_pending_cmd_seq;
extern volatile uint8_t g_can_pending_cmd_active;
extern volatile uint8_t g_can_seq_consistent;
extern volatile uint32_t g_can_seq_match_count;
extern volatile uint32_t g_can_seq_mismatch_count;
extern volatile uint8_t g_can_last_seq_expected;
extern volatile uint8_t g_can_last_seq_observed;
extern volatile uint8_t g_can_gateway_mode;
extern volatile uint8_t g_can_slave_mode;
extern volatile uint8_t g_can_body_status;
extern volatile uint8_t g_can_slave_online;
extern volatile uint32_t g_can_slave_timeout_count;
extern volatile uint32_t g_can_last_slave_rx_age_ms;
extern volatile uint32_t g_can_dtc_mask;
extern volatile uint8_t g_can_last_dtc;
extern volatile uint32_t g_can_cmd_drop_count;
extern volatile uint32_t g_can_isr_drop_count;
extern volatile uint32_t g_can_uplink_drop_count;
extern TaskHandle_t ledTaskHandle;
extern TaskHandle_t keyTaskHandle;
TaskHandle_t xMqttTaskHandle = NULL;

static void vDiagnosticTask(void *pvParameters)
{
	(void)pvParameters;
	vTaskDelay(pdMS_TO_TICKS(5000));

	while(1)
	{
		printf("\n========== DIAG ==========\n");
		printf("Uptime:       %lu ticks\n", (unsigned long)xTaskGetTickCount());
		printf("MQTT state:   %d\n", (int)g_mqtt_state);
		printf("MQTT reconns: %lu\n", (unsigned long)g_mqtt_reconn_count);
		printf("CAN mode:     gateway=%u slave=%u\n",
			(unsigned)g_can_gateway_mode,
			(unsigned)g_can_slave_mode);
		printf("CAN body:     0x%02X\n", (unsigned)g_can_body_status);
		printf("CAN tx/rx:    %lu / %lu\n",
			(unsigned long)g_can_tx_count,
			(unsigned long)g_can_rx_count);
		printf("CAN tx fail:  %lu\n", (unsigned long)g_can_tx_fail_count);
		printf("CAN fail id:  0x%03lX\n", (unsigned long)g_can_last_tx_fail_id);
		printf("CAN error:    0x%08lX\n", (unsigned long)g_can_last_error);
		printf("CAN ESR:      0x%08lX\n", (unsigned long)g_can_last_esr);
		printf("CAN seq:      cmd=%u status=%u\n",
			(unsigned)g_can_last_cmd_seq,
			(unsigned)g_can_last_status_seq);
		printf("CAN seq chk:  ok=%u pending=%u active=%u match=%lu mismatch=%lu\n",
			(unsigned)g_can_seq_consistent,
			(unsigned)g_can_pending_cmd_seq,
			(unsigned)g_can_pending_cmd_active,
			(unsigned long)g_can_seq_match_count,
			(unsigned long)g_can_seq_mismatch_count);
		printf("CAN seq exp:  expected=%u observed=%u\n",
			(unsigned)g_can_last_seq_expected,
			(unsigned)g_can_last_seq_observed);
		printf("CAN dtc:      mask=0x%08lX last=0x%02X\n",
			(unsigned long)g_can_dtc_mask,
			(unsigned)g_can_last_dtc);
		printf("CAN drops:    cmd=%lu isr=%lu uplink=%lu\n",
			(unsigned long)g_can_cmd_drop_count,
			(unsigned long)g_can_isr_drop_count,
			(unsigned long)g_can_uplink_drop_count);
		printf("NET RX drops: %lu\n",
			(unsigned long)Driver_Net_GetRxDropCount());
		printf("CAN slave:    online=%u timeout=%lu age=%lu ms\n",
			(unsigned)g_can_slave_online,
			(unsigned long)g_can_slave_timeout_count,
			(unsigned long)g_can_last_slave_rx_age_ms);
		printf("CAN last id:  0x%03lX\n", (unsigned long)g_can_last_rx_id);
		printf("Free heap:    %lu\n", (unsigned long)xPortGetFreeHeapSize());

		if(xMqttTaskHandle != NULL)
			printf("MQTT stack:   %lu\n",
				(unsigned long)uxTaskGetStackHighWaterMark(xMqttTaskHandle));
		if(keyTaskHandle != NULL)
			printf("Key  stack:   %lu\n",
				(unsigned long)uxTaskGetStackHighWaterMark(keyTaskHandle));
		if(ledTaskHandle != NULL)
			printf("LED  stack:   %lu\n",
				(unsigned long)uxTaskGetStackHighWaterMark(ledTaskHandle));

		printf("==========================\n\n");
		vTaskDelay(pdMS_TO_TICKS(60000));
	}
}

int main(void)
{
	ptIODev dbgoutDev = NULL;

	HAL_Init();
	SystemClock_Config();

	dbgoutDev = IODev_GetDev(DBGOUT);
	if(dbgoutDev != NULL)
	{
		dbgoutDev->Init(dbgoutDev);
	}

	vStartMQTTTasks(512, APP_TASK_PRIO_MQTT);
	vStartLEDTasks(128, APP_TASK_PRIO_LED);
	vStartKeyTasks(128, APP_TASK_PRIO_KEY);
	vStartCANTasks(256, APP_TASK_PRIO_CAN);

	if(xTaskCreate(vDiagnosticTask, "Diag", 256, NULL,
		APP_TASK_PRIO_DIAG, NULL) != pdPASS)
	{
		printf("Create Diag Task failed.\r\n");
	}
	else
	{
		printf("Create Diag Task success.\r\n");
	}

	printf("[MAIN] About to start scheduler, free heap=%lu\r\n",
		(unsigned long)xPortGetFreeHeapSize());

	vTaskStartScheduler();

	printf("[MAIN] Scheduler returned unexpectedly, free heap=%lu\r\n",
		(unsigned long)xPortGetFreeHeapSize());

	while(1)
	{
	}
}

void vApplicationMallocFailedHook(void)
{
	g_pcFreeRTOSFaultReason = "malloc failed";
	taskDISABLE_INTERRUPTS();
	for(;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	g_pcFreeRTOSFaultReason = "stack overflow";
	g_pcFreeRTOSFaultTaskName = pcTaskName;
	taskDISABLE_INTERRUPTS();
	for(;;) {}
}

void vAssertCalled(const char *file, unsigned long line)
{
	g_pcFreeRTOSFaultReason = "configASSERT";
	g_pcFreeRTOSAssertFile = file;
	g_ulFreeRTOSAssertLine = line;
	taskDISABLE_INTERRUPTS();
	for(;;) {}
}

void SystemClock_Config(void)
{
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};
	RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLM = 8;
	RCC_OscInitStruct.PLL.PLLN = 336;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 4;
	HAL_RCC_OscConfig(&RCC_OscInitStruct);

	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
		RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

	HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

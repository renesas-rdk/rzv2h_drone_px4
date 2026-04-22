#include "main_task.h"
/* Main Task entry function */
/* pvParameters contains TaskHandle_t */
void main_task_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/* TODO: add your own code here */
	while (1) {
		vTaskDelay(1);
	}
}

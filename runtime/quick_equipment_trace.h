// Read-only developer trace; enable only after exact main NSO validation.
#pragma once
#include "control.h"

void quick_equipment_trace_enable(void);
void quick_equipment_trace_command(int argc, char **argv, ControlReply *reply);

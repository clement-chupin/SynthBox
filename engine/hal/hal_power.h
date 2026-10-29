#pragma once
// HAL: the power supply. One implementation per platform; a platform without a battery (the simulator) reports none.
#ifdef __cplusplus
extern "C" {
#endif

// The battery's charge, 0..100, from its voltage; -1 = no battery is measured (none on this platform, none connected, or the
// first readings are not in yet). Cheap: the platform measures by itself and this returns the last smoothed value.
int power_battery_percent(void);
int power_battery_mv(void);                 // its voltage in millivolts (0 with no battery)

#ifdef __cplusplus
}
#endif

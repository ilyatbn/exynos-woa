#ifndef __DEBUGDEF_H__
#include <evntrace.h>   // TRACE_LEVEL_* (WPP used to pull this in)
#define __DEBUGDEF_H__

//
// SerialBusWdk's control Guid
// {a2bf23a4-bc00-4903-81f2-6fbd82ad2d13}
//
#define WPP_CONTROL_GUIDS_UNUSED \
    WPP_DEFINE_CONTROL_GUID(serialbuswdk,(a2bf23a4,bc00,4903,81f2,6fbd82ad2d13),  \
        WPP_DEFINE_BIT(TFLAG_PNP)           \
        WPP_DEFINE_BIT(TFLAG_POWER)         \
        WPP_DEFINE_BIT(TFLAG_UART)          \
        WPP_DEFINE_BIT(TFLAG_IOCTL)         \
        WPP_DEFINE_BIT(TFLAG_IO)            \
        WPP_DEFINE_BIT(TFLAG_DATA)          \
        WPP_DEFINE_BIT(TFLAG_HCI))          

//
// Define shorter versions of the ETW trace levels
//
#define LEVEL_CRITICAL  TRACE_LEVEL_CRITICAL
#define LEVEL_ERROR     TRACE_LEVEL_ERROR
#define LEVEL_WARNING   TRACE_LEVEL_WARNING
#define LEVEL_INFO      TRACE_LEVEL_INFORMATION
#define LEVEL_VERBOSE   TRACE_LEVEL_VERBOSE

#define WPP_LEVEL_FLAG_ENABLED(lvl, component) \
    (WPP_LEVEL_ENABLED(component) && WPP_CONTROL(WPP_BIT_ ## component).Level >=lvl)
    
#define WPP_LEVEL_FLAG_LOGGER(lvl, component) \
    WPP_LEVEL_LOGGER(component)

//
// IFR enable macros
//
#define WPP_RECORDER_LEVEL_FLAG_ARGS(lvl, component) \
    WPP_CONTROL(WPP_BIT_ ## component).AutoLogContext, 0, WPP_BIT_ ## component
#define WPP_RECORDER_LEVEL_FLAG_FILTER(lvl, component) \
    (lvl < TRACE_LEVEL_VERBOSE || WPP_CONTROL(WPP_BIT_ ## component).AutoLogVerboseEnabled)

//
// Use for WPP trace
//
#define WithinRange(min, value, max) (min <= value && value <= max)
#define MinToPrint(val1, val2) (val1 < val2 ? val1 : val2)
#define MAX_EVENT_PARAMS_TO_DISPLAY     8   // maximun number of event parameter to be printed (WPP)
#define MAX_COMMAND_PARAMS_TO_DISPLAY   8   // maximun number of vommand parameter to be printed (WPP)

//
// exynos-woa: WPP replaced by DbgPrintEx so the messages show up in the KD log (DoTrace keeps its WPP call shape:
// DoTrace(LEVEL, FLAG, ("fmt", args)); %!STATUS! was rewritten to 0x%08X).
//
#ifndef BT_TRACE_MAX_LEVEL
#define BT_TRACE_MAX_LEVEL  LEVEL_INFO
#endif
#define DoTrace(lvl, flg, args)                                                        \
    do {                                                                               \
        if ((lvl) <= BT_TRACE_MAX_LEVEL) {                                             \
            DbgPrintEx (DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Exynos9820Bt: ");    \
            DbgPrintEx (DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, _BT_TRACE_ARGS args); \
            DbgPrintEx (DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "\n");                \
        }                                                                              \
    } while (0)
#define _BT_TRACE_ARGS(...)  __VA_ARGS__
#define WPP_INIT_TRACING(drv, reg)
#define WPP_CLEANUP(drv)

#endif // __DEBUGDEF_H__

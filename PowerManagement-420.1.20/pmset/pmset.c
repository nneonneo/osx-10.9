/*
 * Copyright (c) 2010 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFDateFormatter.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/pwr_mgt/IOPMUPSPrivate.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>
#include <IOKit/IOCFSerialize.h>

#if TARGET_OS_EMBEDDED
    #define PLATFORM_HAS_DISPLAYSERVICES    0
#else
    #define PLATFORM_HAS_DISPLAYSERVICES    1
    // ResentAmbientLightAll is defined in <DisplayServices/DisplayServices.h> 
    // and implemented by DisplayServices.framework
    IOReturn DisplayServicesResetAmbientLightAll( void );
#endif

#include "../pmconfigd/PrivateLib.h"

// dynamically mig generated
#include "powermanagement.h"

#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/IOReportTypes.h>
#include <IOReport.h>

#include <servers/bootstrap.h>
#include <bootstrap_priv.h>
#include <mach/mach_port.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <notify.h>
#include <asl.h>
#include <asl_private.h>
#include <asl_core.h>
#include <asl_file.h>
#include <asl_store.h>
#include <dirent.h>
#include <sysexits.h>

/*
 * This is the command line interface to Energy Saver Preferences in
 * /Library/Preferences/SystemConfiguration/com.apple.PowerManagement.plist
 *
 * pmset does many things, but here are a few of them:
 *
 Usage: pmset [-b | -c | -a] <action> <minutes> [[<opts>] <action> <minutes> ...]
       -c adjust settings used while connected to a charger
       -b adjust settings used when running off a battery
       -a (default) adjust settings for both
       <action> is one of: dim, sleep, spindown, slower, womp* (* flag = 1/0)
       eg. pmset womp 1 -c dim 5 sleep 15 -b dim 3 spindown 5 sleep 8
 */

// Settings options
#define ARG_DIM             "dim"
#define ARG_DISPLAYSLEEP    "displaysleep"
#define ARG_SLEEP           "sleep"
#define ARG_SPINDOWN        "spindown"
#define ARG_DISKSLEEP       "disksleep"
#define ARG_WOMP            "womp"
#define ARG_LIDWAKE         "lidwake"

#define ARG_HIBERNATEMODE      "hibernatemode"
#define ARG_HIBERNATEFILE      "hibernatefile"
#define ARG_HIBERNATEFREERATIO "hibernatefreeratio"
#define ARG_HIBERNATEFREETIME  "hibernatefreetime"
#define ARG_AUTOPOWEROFF       "autopoweroff"
#define ARG_AUTOPOWEROFFDELAY  "autopoweroffdelay"

#define ARG_RING            "ring"
#define ARG_AUTORESTART     "autorestart"
#define ARG_WAKEONACCHANGE  "acwake"
#define ARG_REDUCEBRIGHT    "lessbright"
#define ARG_SLEEPUSESDIM    "halfdim"
#define ARG_MOTIONSENSOR    "sms"
#define ARG_MOTIONSENSOR2   "ams"
#define ARG_TTYKEEPAWAKE    "ttyskeepawake"
#define ARG_GPU             "gpuswitch"
#define ARG_NETAVAILABLE    "networkoversleep"
#define ARG_DEEPSLEEP       "standby"
#define ARG_DEEPSLEEPDELAY  "standbydelay"
#define ARG_DARKWAKES       "darkwakes"

// Scheduling options
#define ARG_SCHEDULE        "schedule"
#define ARG_SCHED           "sched"
#define ARG_REPEAT          "repeat"
#define ARG_CANCEL          "cancel"
#define ARG_RELATIVE        "relative"
//#define ARG_SLEEP         "sleep"
#define ARG_SHUTDOWN        "shutdown"
#define ARG_RESTART         "restart"
#define ARG_WAKE            "wake"
#define ARG_POWERON         "poweron"
#define ARG_WAKEORPOWERON   "wakeorpoweron"

// UPS options
#define ARG_HALTLEVEL       "haltlevel"
#define ARG_HALTAFTER       "haltafter"
#define ARG_HALTREMAIN      "haltremain"

// get options
#define ARG_CAP             "cap"
#define ARG_DISK            "disk"
#define ARG_CUSTOM          "custom"
#define ARG_LIVE            "live"
#define ARG_SCHED           "sched"
#define ARG_UPS             "ups"
#define ARG_SYS_PROFILES    "profiles"
#define ARG_ADAPTER_AC      "ac"
#define ARG_ADAPTER         "adapter"
#define ARG_BATT            "batt"
#define ARG_PS              "ps"
#define ARG_PSLOG           "pslog"
#define ARG_TRCOLUMNS       "trcolumns"
#define ARG_BATTRAW         "rawbatt"
#define ARG_PSRAW           "rawlog"
#define ARG_THERM           "therm"
#define ARG_THERMLOG        "thermlog"
#define ARG_ASSERTIONS      "assertions"
#define ARG_ASSERTIONSLOG   "assertionslog"
#define ARG_SYSLOAD         "sysload"
#define ARG_SYSLOADLOG      "sysloadlog"
#define ARG_USERACTIVITYLOG "useractivitylog"
#define ARG_USERACTIVITY    "useractivity"
#define ARG_LOG             "log"
#define ARG_LISTEN          "listen"
#define ARG_HISTORY         "history"
#define ARG_HISTORY_DETAILED "historydetailed"
#define ARG_HID_NULL        "hidnull"
#define ARG_BOOKMARK        "bookmark"
#define ARG_CLEAR_HISTORY   "clearpmhistory"
#define ARG_SEARCH          "searchforuuid"
#define ARG_USERCLIENTS     "userclients"
#define ARG_UUID            "uuid"
#define ARG_UUID_LOG        "uuidlog"
#define ARG_EVERYTHING      "everything"
#define ARG_PRINT_GETTERS   "getters"
#define ARG_POWERSTATE      "powerstate"
#define ARG_POWERSTATELOG   "powerstatelog"
#define ARG_RDSTATS         "stats"
#define ARG_SYSSTATE        "systemstate"

// special
#define ARG_BOOT            "boot"
#define ARG_UNBOOT          "unboot"
#define ARG_POLLBOOT        "readboot"
#define ARG_POLLALL         "readall"
#define ARG_POLLUSER        "readuser"
#define ARG_FORCE           "force"
#define ARG_TOUCH           "touch"
#define ARG_NOIDLE          "noidle"
#define ARG_SLEEPNOW        "sleepnow"
#define ARG_DISPLAYSLEEPNOW "displaysleepnow"
#define ARG_DEBUGTRIG       "debugTrig"
#define ARG_RESETDISPLAYAMBIENTPARAMS       "resetdisplayambientparams"
#define ARG_DISABLEASSERTION                "disableassertion"
#define ARG_ENABLEASSERTION                 "enableassertion"
#define ARG_RDAP            "rdap"
#define ARG_DEBUGFLAGS      "debugflags"
#define ARG_BTINTERVAL      "btinterval"
#define ARG_DWLINTERVAL     "dwlinterval"
#define ARG_MT2BOOK         "mt2book"
#define ARG_SETSAAFLAGS     "saaflags"
#define ARG_NOPOLL          "nopoll"

// special system
#define ARG_DISABLESLEEP    "disablesleep"
#define ARG_DISABLEFDEKEYSTORE  "destroyfvkeyonstandby"

// return values for parseArgs
#define kParseSuccess                   0       // success
#define kParseBadArgs                   -1      // error
#define kParseInternalError             -2      // error

// bitfield for tracking what's been modified in parseArgs()
#define kModSettings                    (1<<0)
#define kModProfiles                    (1<<1)
#define kModUPSThresholds               (1<<2)
#define kModSched                       (1<<3)
#define kModRepeat                      (1<<4)
#define kModSystemSettings              (1<<5)

// return values for idleSettingsNotConsistent
#define kInconsistentDisplaySetting     1
#define kInconsistentDiskSetting        2
#define kConsistentSleepSettings        0

// day-of-week constants for repeating power events
#define daily_mask      ( kIOPMMonday | kIOPMTuesday | kIOPMWednesday \
                        | kIOPMThursday | kIOPMFriday | kIOPMSaturday \
                        | kIOPMSunday)
#define weekday_mask    ( kIOPMMonday | kIOPMTuesday | kIOPMWednesday \
                        | kIOPMThursday | kIOPMFriday )
#define weekend_mask    ( kIOPMSaturday | kIOPMSunday )

#define kDateAndTimeFormat      "MM/dd/yy HH:mm:ss"
#define kTimeFormat             "HH:mm:ss"

#define kMaxLongStringLength        255

static const size_t kMaxArgStringLength = 49;

#define kUSecPerSec 1000000.0


#ifndef kPMPowerStatesChID
#define kPMPowerStatesChID  IOREPORT_MAKEID('P','M','S','t','H','i','s','t')
#endif

#ifndef kPMCurrStateChID
#define kPMCurrStateChID  IOREPORT_MAKEID('P','M','C','u','r','S','t','\0' )
#endif                                                                                                                          

/* RootDomain IOReporting channels */
#define kSleepCntChID IOREPORT_MAKEID('S','l','e','e','p','C','n','t')
#define kDarkWkCntChID IOREPORT_MAKEID('G','U','I','W','k','C','n','t')
#define kUserWkCntChID IOREPORT_MAKEID('D','r','k','W','k','C','n','t')

typedef struct {
    const char *name;
    const char *displayAs;
} PMFeature;

/* list of all features */
PMFeature all_features[] =
{ 
    { kIOPMDisplaySleepKey,         ARG_DISPLAYSLEEP },
    { kIOPMDiskSleepKey,            ARG_DISKSLEEP },
    { kIOPMSystemSleepKey,          ARG_SLEEP },
    { kIOPMWakeOnLANKey,            ARG_WOMP },
    { kIOPMWakeOnRingKey,           ARG_RING },
    { kIOPMWakeOnACChangeKey,       ARG_WAKEONACCHANGE },
    { kIOPMRestartOnPowerLossKey,   ARG_AUTORESTART },
    { kIOPMWakeOnClamshellKey,      ARG_LIDWAKE },
    { kIOPMReduceBrightnessKey,     ARG_REDUCEBRIGHT },
    { kIOPMDisplaySleepUsesDimKey,  ARG_SLEEPUSESDIM },
    { kIOPMMobileMotionModuleKey,   ARG_MOTIONSENSOR },
    { kIOPMGPUSwitchKey,            ARG_GPU },
    { kIOPMDeepSleepEnabledKey,     ARG_DEEPSLEEP },
    { kIOPMDeepSleepDelayKey,       ARG_DEEPSLEEPDELAY },
    { kIOPMDarkWakeBackgroundTaskKey, ARG_DARKWAKES },
    { kIOPMTTYSPreventSleepKey,     ARG_TTYKEEPAWAKE },
    { kIOHibernateModeKey,          ARG_HIBERNATEMODE },
    { kIOHibernateFileKey,          ARG_HIBERNATEFILE },
    { kIOPMAutoPowerOffEnabledKey,  ARG_AUTOPOWEROFF },
    { kIOPMAutoPowerOffDelayKey,    ARG_AUTOPOWEROFFDELAY }     
};

#define kNUM_PM_FEATURES    (sizeof(all_features)/sizeof(PMFeature))

enum ArgumentType {
    kApplyToBattery = 1,
    kApplyToCharger = 2,
    kApplyToUPS     = 4,
    kShowColumns    = 8
};

enum AssertionBitField {
    kAssertionCPU = 1,
    kAssertionInflow = 2,
    kAssertionCharge = 4,
    kAssertionIdle = 8
};

// ack port for sleep/wake callback
static io_connect_t gPMAckPort = MACH_PORT_NULL;


enum SleepCallbackBehavior {
    kLogSleepEvents = (1<<0),
    kCancelSleepEvents = (1<<1)
};

/* pmset commands */
enum PMCommandType {
    kPMCommandSleepNow = 1,
    kPMCommandTouch,
    kPMCommandNoIdle,
    kPMCommandDisplaySleepNow,
    kPMCommandDebugTrig
};

/* check and set int value multiplier */
enum {
    kNoMultiplier = 0,
    kMillisecondsMultiplier = 1000
};

typedef struct {
    CFStringRef         who;
    CFDateRef           when;
    CFStringRef         which;
} ScheduledEventReturnType;


#define RING_SIZE 100
typedef struct {
   aslmsg msgRing[RING_SIZE];
   uint32_t readIdx;
   uint32_t writeIdx;
} MsgCache;

MsgCache *msgCache = NULL;

// function declarations
static void usage(void);
static IOReturn setRootDomainProperty(CFStringRef key, CFTypeRef val);
static io_registry_entry_t copyRootDomainRef(void);
static IOReturn _pm_connect(mach_port_t *newConnection);
static IOReturn _pm_disconnect(mach_port_t connection);

static void show_pm_settings_dict(
        CFDictionaryRef d, 
        int indent, 
        bool log_overrides,
        bool prune_unsupported);
static void show_system_power_settings(void);
static void show_supported_pm_features(void);
static void show_custom_pm_settings(void);
static void show_live_pm_settings(void);
static void show_ups_settings(void);
static void show_active_profiles(void);
static void show_system_profiles(void);
static void show_scheduled_events(void);
static void show_active_assertions(uint32_t which);
static void show_power_sources(int which);
static bool prevent_idle_sleep(void);
static void show_assertions(const char *);
static void log_assertions(void);
static void show_systemload(void);
static void log_systemload(void);
static void log_useractivitylog(void);
static void log_useractivity(void);
static void show_log(void);
static void show_uuid(bool keep_running);
static void listen_for_everything(void);
static bool is_display_dim_captured(void);
static void show_power_adapter(void);
static void show_getters(void);
static void show_power_state(char **argv);
static void show_power_statelog(char **argv);
static void show_rdStats(char **argv);
static void show_sysstate(char **argv);
static void show_everything(char **);

static void show_power_event_history(void);
static void show_power_event_history_detailed(void);
static void set_new_power_bookmark(void);
static void set_debugFlags(char **argv);
static void set_btInterval(char **argv);
static void set_dwlInterval(char **argv);
static void set_saaFlags(char **argv);
static void show_details_for_UUID(char **UUID_string);
static void show_NULL_HID_events(void);
static void show_root_domain_user_clients(void);
static void mt2bookmark(void);
static bool isBatteryPollingStopped(void);
static void set_nopoll(void);

static void print_pretty_date(CFAbsoluteTime t, bool newline);
static void print_short_date(CFAbsoluteTime t, bool newline);
static void print_date_with_style(const char *, CFDateFormatterStyle dayStyle, CFDateFormatterStyle timeStyle, CFAbsoluteTime t, bool newline);

static void sleepWakeCallback(
        void *refcon, 
        io_service_t y __unused,
        natural_t messageType,
        void * messageArgument);

static void install_listen_for_notify_system_power(void);
static void install_listen_PM_connection(void);
static void install_listen_IORegisterForSystemPower(void);
static void install_listen_com_apple_powermanagement_sleepservices_notify(void);

static void log_ps_change_handler(void *);
static int install_listen_for_power_sources(uintptr_t which);
static int log_raw_power_source_changes(void);
static void log_raw_battery_interest(
        void *refcon, 
        io_service_t batt, 
        natural_t messageType, 
        void *messageArgument);
static void log_raw_battery_match(
        void *refcon, 
        io_iterator_t b_iter);

static void log_thermal_events(void);        
static void show_systempower_notify(void);
static void show_thermal_warning_level(void);
static void show_thermal_cpu_power_level(void);

static void print_raw_battery_state(io_registry_entry_t b_reg);
static void print_setting_value(CFTypeRef a, int divider);
static void print_override_pids(int assertion_type);
static void print_time_of_day_to_buf(int m, char *buf, int buflen);
static void print_days_to_buf(int d, char *buf, int buflen);
static void print_repeating_report(CFDictionaryRef repeat);
static void print_scheduled_report(CFArrayRef events);

static CFDictionaryRef getPowerEvent(int type, CFDictionaryRef events);
static int getRepeatingDictionaryMinutes(CFDictionaryRef event);
static int getRepeatingDictionaryDayMask(CFDictionaryRef event);
static CFStringRef getRepeatingDictionaryType(CFDictionaryRef event);
static int arePowerSourceSettingsInconsistent(CFDictionaryRef set);
static void checkSettingConsistency(CFDictionaryRef profiles);
static ScheduledEventReturnType *scheduled_event_struct_create(void);
static void scheduled_event_struct_destroy(ScheduledEventReturnType *);
static void string_tolower(char *lower_me, char *dst);
static void string_toupper(char *upper_me, char *dst);
static int checkAndSetIntValue(
        char *valstr, 
        CFStringRef settingKey, 
        int apply,
        int isOnOffSetting, 
        int multiplier,
        CFMutableDictionaryRef ac, 
        CFMutableDictionaryRef batt, 
        CFMutableDictionaryRef ups);
static int setUPSValue(
        char *valstr, 
        CFStringRef    whichUPS, 
        CFStringRef settingKey, 
        int apply, 
        CFMutableDictionaryRef thresholds);
static int parseScheduledEvent(
        char                        **argv,
        int                         *num_args_parsed,
        ScheduledEventReturnType    *local_scheduled_event, 
        bool                        *cancel_scheduled_event,
        bool                        is_relative_event);
static int parseRepeatingEvent(
        char                        **argv,
        int                         *num_args_parsed,
        CFMutableDictionaryRef      local_repeating_event, 
        bool                        *local_cancel_repeating);
static int parseArgs(
        int argc, 
        char* argv[], 
        CFDictionaryRef             *settings,
        int                         *modified_power_sources,
        bool                        *force_activate_settings,
        CFDictionaryRef             *active_profiles,
        CFDictionaryRef             *system_power_settings,
        CFDictionaryRef             *ups_thresholds,
        ScheduledEventReturnType    **scheduled_event,
        bool                        *cancel_scheduled_event,
        CFDictionaryRef             *repeating_event,
        bool                        *cancel_repeating_event,
        uint32_t                    *pmCmd);
static void displaySleepNow();
static void swd_debugTrig();

static const char *getCanonicalArgForSynonym(char *pass)
{
    if (!pass || 0 == strlen(pass)) 
        return ARG_LIVE;
    if (!strncmp(ARG_DISK, pass, kMaxArgStringLength))
        return ARG_CUSTOM;
    if (!strncmp(ARG_ADAPTER_AC, pass, kMaxArgStringLength))
        return ARG_ADAPTER;
    if (!strncmp(ARG_BATT, pass, kMaxArgStringLength))
        return ARG_PS;
    return pass;
}

typedef enum {
    kActionGetOnceNoArgs,
    kActionGetLog,
    kActionRecursiveBeCareful
} CommandActionType;

typedef struct {
    CommandActionType       actionType;
	const char              *arg;
	void                    (^action)(char **);
} CommandAndAction;

static CommandAndAction the_getters[] =
	{ 
		{kActionGetOnceNoArgs,  ARG_LIVE,           ^(char **arg){ show_system_power_settings(); show_active_profiles(); show_live_pm_settings();}},	
		{kActionGetOnceNoArgs,  ARG_CUSTOM,         ^(char **arg){ show_custom_pm_settings(); }},
        {kActionGetOnceNoArgs,  ARG_CAP,            ^(char **arg){ show_supported_pm_features(); }},
        {kActionGetOnceNoArgs,  ARG_SCHED,          ^(char **arg){ show_scheduled_events(); }},
        {kActionGetOnceNoArgs,  ARG_UPS,            ^(char **arg){ show_ups_settings(); }},
        {kActionGetOnceNoArgs,  ARG_SYS_PROFILES,   ^(char **arg){ show_active_profiles(); show_system_profiles(); }},
    	{kActionGetOnceNoArgs,  ARG_ADAPTER,        ^(char **arg){ show_power_adapter(); }},
    	{kActionGetOnceNoArgs,  ARG_PS,             ^(char **arg){ show_power_sources(kApplyToBattery | kApplyToUPS); }},
    	{kActionGetLog,         ARG_PSLOG,          ^(char **arg){ install_listen_IORegisterForSystemPower();
                                                        install_listen_for_power_sources(kApplyToBattery | kApplyToUPS);
                                                        CFRunLoopRun(); }},
    	{kActionGetLog,         ARG_TRCOLUMNS,      ^(char **arg){ install_listen_IORegisterForSystemPower();
                                                        install_listen_for_power_sources(kShowColumns);
                                                        CFRunLoopRun(); }},
    	{kActionGetLog,         ARG_PSRAW,          ^(char **arg){ log_raw_power_source_changes(); }},
        {kActionGetOnceNoArgs,  ARG_BATTRAW,        ^(char **arg){ print_raw_battery_state(IO_OBJECT_NULL); }},
        {kActionGetOnceNoArgs,  ARG_THERM,          ^(char **arg){ show_thermal_warning_level(); show_thermal_cpu_power_level(); }},
    	{kActionGetLog,         ARG_THERMLOG,       ^(char **arg){ log_thermal_events(); }},
    	{kActionGetOnceNoArgs,  ARG_ASSERTIONS,     ^(char **arg){ show_assertions(NULL); }},
    	{kActionGetLog,         ARG_ASSERTIONSLOG,  ^(char **arg){ log_assertions(); }},
    	{kActionGetOnceNoArgs,  ARG_SYSLOAD,        ^(char **arg){ show_systemload(); }},
    	{kActionGetLog,         ARG_SYSLOADLOG,     ^(char **arg){ log_systemload(); }},
    	{kActionGetLog,         ARG_USERACTIVITYLOG,^(char **arg){ log_useractivitylog(); }},
    	{kActionGetOnceNoArgs,  ARG_USERACTIVITY   ,^(char **arg){ log_useractivity(); }},
    	{kActionGetOnceNoArgs,  ARG_LOG,            ^(char **arg){ show_log(); }},
    	{kActionGetLog,         ARG_LISTEN,         ^(char **arg){ listen_for_everything(); }},
    	{kActionGetOnceNoArgs,  ARG_HISTORY,        ^(char **arg){ show_power_event_history(); }},
    	{kActionGetOnceNoArgs,  ARG_HISTORY_DETAILED, ^(char **arg){ show_power_event_history_detailed(); }},
    	{kActionGetOnceNoArgs,  ARG_HID_NULL,       ^(char **arg){ show_NULL_HID_events(); }},
    	{kActionGetOnceNoArgs,  ARG_USERCLIENTS,    ^(char **arg){ show_root_domain_user_clients(); }},
        {kActionGetOnceNoArgs,  ARG_UUID,           ^(char **arg){ show_uuid(kActionGetOnceNoArgs); }},
    	{kActionGetLog,         ARG_UUID_LOG,       ^(char **arg){ show_uuid(kActionGetLog); }},
        {kActionGetOnceNoArgs,  ARG_PRINT_GETTERS,  ^(char **arg){ show_getters(); }},
        {kActionGetLog,         ARG_SEARCH,         ^(char **arg){show_details_for_UUID(arg); }},
        {kActionGetOnceNoArgs,  ARG_POWERSTATE,     ^(char **arg){show_power_state(arg); }},
        {kActionGetLog,         ARG_POWERSTATELOG,  ^(char **arg){show_power_statelog(arg); }},
        {kActionGetOnceNoArgs,  ARG_RDSTATS,        ^(char **arg){show_rdStats(arg); }},
        {kActionGetOnceNoArgs,  ARG_SYSSTATE,       ^(char **arg){show_sysstate(arg); }},
        {kActionRecursiveBeCareful, ARG_EVERYTHING, ^(char **arg){ show_everything(arg); }}
	};


static const int the_getters_count = (sizeof(the_getters) / sizeof(CommandAndAction));


//****************************
//****************************
//****************************

static void usage(void)
{
    printf("Usage: pmset <options>\n");
    printf("See pmset(1) for details: \'man pmset\'\n");
}



int main(int argc, char *argv[]) {
    IOReturn                        ret, ret1, err;
    io_connect_t                    fb;
    CFDictionaryRef                 es_custom_settings = 0;
    int                             modified_power_sources = 0;
    bool                            force_it = 0;
    CFDictionaryRef                 ups_thresholds = 0;
    CFDictionaryRef                 system_power_settings = 0;
    CFDictionaryRef                 active_profiles = 0;
    ScheduledEventReturnType        *scheduled_event_return = 0;
    bool                            cancel_scheduled_event = 0;
    CFDictionaryRef                 repeating_event_return = 0;
    bool                            cancel_repeating_event = 0;
    uint32_t                        pmCommand = 0;
        
    ret = parseArgs(argc, argv, 
        &es_custom_settings, &modified_power_sources, &force_it,
        &active_profiles,
        &system_power_settings,
        &ups_thresholds,
        &scheduled_event_return, &cancel_scheduled_event,
        &repeating_event_return, &cancel_repeating_event,
        &pmCommand);

    if(ret == kParseBadArgs)
    {
        //printf("pmset: error in parseArgs!\n");
        usage();
        exit(1);
    }
    
    if(ret == kParseInternalError)
    {
        fprintf(stderr, "%s: internal error!\n", argv[0]); fflush(stdout);    
        exit(1);
    }

    switch ( pmCommand )    
    {
        /*************** Sleep now *************************************/
        case kPMCommandSleepNow:            
            fb = IOPMFindPowerManagement(MACH_PORT_NULL);
            if ( MACH_PORT_NULL != fb ) {
                err = IOPMSleepSystem ( fb );
            
                if( kIOReturnNotPrivileged == err )
                {
                    printf("Sleep error 0x%08x; You must run this as root.\n", err);
                    exit(EX_NOPERM);
                } else if ( (MACH_PORT_NULL == fb) || (kIOReturnSuccess != err) )
                {
                    printf("Unable to sleep system: error 0x%08x\n", err);
                    exit(EX_OSERR);
                } else {
                    printf("Sleeping now...\n");
                }
            }
            
            return 0;

        /*************** Display Sleep now *************************************/
        case kPMCommandDisplaySleepNow:
            displaySleepNow();
            break;

        case kPMCommandDebugTrig:
            swd_debugTrig();
            break;

        /*************** Touch settings *************************************/
        case kPMCommandTouch:
            printf("touching prefs file on disk...\n");

            ret = IOPMSetPMPreferences(NULL);
            if(kIOReturnSuccess != ret)
            {
                printf("\'%s\' must be run as root...\n", argv[0]);
            }

            return 0;

        /*************** Prevent idle sleep **********************************/
        case kPMCommandNoIdle:
            if(!prevent_idle_sleep())
            {
                printf("Error preventing idle sleep\n");
            }
            return 1;

        default:
            // If no command is specified, execution continues with processing
            // other command-line arguments
            break;
    }
    
    if(force_it && es_custom_settings)
    {

        mach_port_t             pm_server = MACH_PORT_NULL;
        CFDataRef               settings_data;
        int32_t                 return_code;
        kern_return_t           kern_result;
        
        /* 
         * Step 1 - send these forced settings over to powerd.
         * 
         */
        err = _pm_connect(&pm_server);
        if (kIOReturnSuccess == err) {
            settings_data = IOCFSerialize(es_custom_settings, 0);
            
            if (settings_data)
            {
                kern_result = io_pm_force_active_settings(
                        pm_server, 
                        (vm_offset_t)CFDataGetBytePtr(settings_data),
                        CFDataGetLength(settings_data),
                        &return_code);
            
                if( KERN_SUCCESS != kern_result ) {
                    printf("exit kern_result = 0x%08x\n", kern_result);
                }
                if( kIOReturnSuccess != return_code ) {
                    printf("exit return_code = 0x%08x\n", return_code);
                }

                CFRelease(settings_data);

                if (KERN_SUCCESS != kern_result || kIOReturnSuccess != return_code) {
                    exit(1);
                }
            }
            _pm_disconnect(pm_server);
        }

        /* 
         * Step 2 - this code might be running in a partilially uninitialized OS,
         * and powerd might not be running. e.g. at Installer context.
         * Since powerd may not be running, we also activate these settings directly 
         * to the controlling drivers in the kernel.
         *
         */
        CFTypeRef       powersources = IOPSCopyPowerSourcesInfo();
        CFStringRef     activePowerSource = NULL;
        CFDictionaryRef useSettings = NULL;
        if (powersources) {
            activePowerSource = IOPSGetProvidingPowerSourceType(powersources);
            if (!activePowerSource) {
                activePowerSource = CFSTR(kIOPMACPowerKey);
            }
            useSettings = CFDictionaryGetValue(es_custom_settings, activePowerSource);
            if (useSettings) {
                ActivatePMSettings(useSettings, true);
            }
            CFRelease(powersources);
        }

        // Return here. If this is a 'force' call we do _not_ want
        // to attempt to write any settings to the disk, or to try anything
        // else at all, as our environment may be unwritable / diskless
        // (booted from install CD)
        return 0;
    }

    if(es_custom_settings)
    {
        CFNumberRef             neg1 = NULL;
        int                     tmp_int = -1;
        CFDictionaryRef         tmp_dict = NULL;
        CFMutableDictionaryRef  customize_active_profiles = NULL;
    
        // Send pmset changes out to disk
        if(kIOReturnSuccess != (ret1 = IOPMSetPMPreferences(es_custom_settings)))
        {
            if(ret1 == kIOReturnNotPrivileged) 
            {
                printf("\'%s\' must be run as root...\n", argv[0]);
            } else {
                printf("Error 0x%08x writing Energy Saver preferences to disk\n", ret1);
            }
            exit(1);
        }
        
        // Also need to change the active profile to -1 (Custom)
        // We assume the user intended to activate these new custom settings.
        neg1 = CFNumberCreate(0, kCFNumberIntType, &tmp_int);
        tmp_dict = IOPMCopyActivePowerProfiles();
        if(!tmp_dict) {
            printf("Custom profile set; unable to update active profile to -1.\n");
            exit(1);
        }
        customize_active_profiles = CFDictionaryCreateMutableCopy(0, 0, tmp_dict);        
        if(!customize_active_profiles) {
            printf("Internal error\n");
            exit(1);
        }
        // For each power source we modified settings for, flip that 
        // source's profile to -1
        if(modified_power_sources & kApplyToCharger) {
            CFDictionarySetValue( customize_active_profiles, 
                                    CFSTR(kIOPMACPowerKey), neg1);
        }
        if(modified_power_sources & kApplyToBattery) {
            CFDictionarySetValue( customize_active_profiles, 
                                    CFSTR(kIOPMBatteryPowerKey), neg1);
        }
        if(modified_power_sources & kApplyToUPS) {
            CFDictionarySetValue( customize_active_profiles, 
                                    CFSTR(kIOPMUPSPowerKey), neg1);
        }
            
        ret = IOPMSetActivePowerProfiles(customize_active_profiles);
        if(kIOReturnSuccess != ret) {
            printf("Error 0x%08x writing customized power profiles to disk\n", ret);
            exit(1);
        }
        CFRelease(neg1);
        CFRelease(tmp_dict);
        CFRelease(customize_active_profiles);
        
                
        // Print a warning to stderr if idle sleep settings won't 
        // produce expected result; i.e. sleep < (display | dim)
        checkSettingConsistency(es_custom_settings);
        CFRelease(es_custom_settings);
    }
    
    if(active_profiles)
    {
        ret = IOPMSetActivePowerProfiles(active_profiles);
        if(kIOReturnSuccess != ret) {
            printf("Error 0x%08x writing active power profiles to disk\n", ret);
            exit(1);
        }
    
        CFRelease(active_profiles);    
    }
    
    if(system_power_settings)
    {
        int             iii;
        int             sys_setting_count = 0;
        
        if( isA_CFDictionary(system_power_settings)) {
            sys_setting_count = CFDictionaryGetCount(system_power_settings);
        }
 
        if (0 != sys_setting_count)
        {
            CFStringRef     *keys = malloc(sizeof(CFStringRef) * sys_setting_count);
            CFTypeRef       *vals = malloc(sizeof(CFTypeRef) * sys_setting_count);
        
            if(keys && vals)
            {
                CFDictionaryGetKeysAndValues( system_power_settings, 
                                          (const void **)keys, (const void **)vals);
            
                for(iii=0; iii<sys_setting_count; iii++)
                {
                    // We write the settings to disk here; PM configd picks them up and
                    // sends them to xnu from configd space.
                    ret = IOPMSetSystemPowerSetting(keys[iii], vals[iii]);            
                    if(kIOReturnNotPrivileged == ret)
                    {
                        printf("\'%s\' must be run as root...\n", argv[0]);
                    }
                    else if (kIOReturnSuccess != ret)
                    {
                        printf("\'%s\' failed to set the value.\n", argv[0]);
                    }
                }        
            }
        
            if (keys) free (keys);
            if (vals) free (vals);
        }
        
        CFRelease(system_power_settings);
    }

    // Did the user change UPS settings too?
    if(ups_thresholds)
    {
        // Only write out UPS settings if thresholds were changed. 
        //      * UPS sleep timers & energy settings have already been 
        //        written with IOPMSetPMPreferences() regardless.
        ret1 = IOPMSetUPSShutdownLevels(
                            CFSTR(kIOPMDefaultUPSThresholds), 
                            ups_thresholds);
        if(kIOReturnSuccess != ret1)
        {
            if(ret1 == kIOReturnNotPrivileged)
                printf("\'%s\' must be run as root...\n", argv[0]);
            if(ret1 == kIOReturnError
                || ret1 == kIOReturnBadArgument)
                printf("Error writing UPS preferences to disk\n");
            exit(1);
        }
        CFRelease(ups_thresholds);
    }
    
    
    if(scheduled_event_return)
    {
        if(cancel_scheduled_event)
        {
            // cancel the event described by scheduled_event_return
            ret = IOPMCancelScheduledPowerEvent(
                    scheduled_event_return->when,
                    scheduled_event_return->who,
                    scheduled_event_return->which);
        } else {
            ret = IOPMSchedulePowerEvent(
                    scheduled_event_return->when,
                    scheduled_event_return->who,
                    scheduled_event_return->which);
        }

        if(kIOReturnNotPrivileged == ret) {
            fprintf(stderr, "%s: This operation must be run as root\n", argv[0]);
            fflush(stderr);
            exit(1);
        }
        if(kIOReturnSuccess != ret) {
            fprintf(stderr, "%s: Error in scheduling operation\n", argv[0]);
            fflush(stderr);
            exit(1);
        }
        
        // free individual members
        scheduled_event_struct_destroy(scheduled_event_return);
    }
    
    if(cancel_repeating_event) 
    {
        ret = IOPMCancelAllRepeatingPowerEvents();
        if(kIOReturnSuccess != ret) {
            if(kIOReturnNotPrivileged == ret) {
                fprintf(stderr, "pmset: Must be run as root to modify settings\n");
            } else {
                fprintf(stderr, "pmset: Error 0x%08x cancelling repeating events\n", ret);
            }
            fflush(stderr);
            exit(1);
        }
    }
    
    if(repeating_event_return)
    {
        ret = IOPMScheduleRepeatingPowerEvent(repeating_event_return);
        if(kIOReturnSuccess != ret) {
            if(kIOReturnNotPrivileged == ret) {
                fprintf(stderr, "pmset: Must be run as root to modify settings\n");
            } else {
                fprintf(stderr, "pmset: Error 0x%08x scheduling repeating events\n", ret);
            }
            fflush(stderr);
            exit(1);
        }
        CFRelease(repeating_event_return);
    }
    
    
    return 0;
}

//****************************
//****************************
//****************************


io_registry_entry_t _getRootDomain(void)
{
    static io_registry_entry_t gRoot = MACH_PORT_NULL;

    if (MACH_PORT_NULL == gRoot)
        gRoot = IORegistryEntryFromPath( kIOMasterPortDefault, 
                kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain");

    return gRoot;
}


static void swd_debugTrig()
{
    io_service_t                rootDomainService = IO_OBJECT_NULL;
    io_connect_t                gRootDomainConnect = IO_OBJECT_NULL;
    kern_return_t               kr = 0;
    IOReturn                    ret;

    // Find it
    rootDomainService = _getRootDomain();
    if (IO_OBJECT_NULL == rootDomainService) {
        goto exit;
    }

    // Open it
    kr = IOServiceOpen(rootDomainService, mach_task_self(), 0, &gRootDomainConnect);    
    if (KERN_SUCCESS != kr) {
        printf("Failed to connect to rootDomain. rc=0x%x\n", kr);
        goto exit;    
    }
    
    ret = IOConnectCallMethod(gRootDomainConnect, kPMSleepWakeDebugTrig, 
                    NULL, 0, 
                    NULL, 0, NULL, 
                    NULL, NULL, NULL);

    if (kIOReturnSuccess != ret)
    {
        printf("Failed to trigger a sleep wake kernel log collection. rc=0x%x\n", ret);
        goto exit;
    }

exit:
    if (IO_OBJECT_NULL != gRootDomainConnect)
        IOServiceClose(gRootDomainConnect);
    return;

}


static void displaySleepNow()
{
    io_registry_entry_t disp_wrangler = IO_OBJECT_NULL;
    kern_return_t kr;

    disp_wrangler = IORegistryEntryFromPath(
                            kIOMasterPortDefault,
                            kIOServicePlane ":/IOResources/IODisplayWrangler");
    if(disp_wrangler == IO_OBJECT_NULL)
        return ;

    kr = IORegistryEntrySetCFProperty( disp_wrangler, CFSTR("IORequestIdle"),kCFBooleanTrue);

    if(kr)
        fprintf(stderr, "pmset: Failed to set the display to sleep(err:0x%x)\n", kr);

    IOObjectRelease(disp_wrangler);

    return ;

}

static CFTypeRef copyRootDomainProperty(CFStringRef key)
{
    static io_object_t      rd = IO_OBJECT_NULL;
    
    if (IO_OBJECT_NULL == rd) {
        rd = copyRootDomainRef();
    }
    
    return IORegistryEntryCreateCFProperty(rd, key, 0, 0);
}

static IOReturn setRootDomainProperty(CFStringRef key, CFTypeRef val) 
{
    io_registry_entry_t         root_domain = copyRootDomainRef();
    IOReturn                    ret;
    
    if(!root_domain) return kIOReturnError;
    
    ret = IORegistryEntrySetCFProperty(root_domain, key, val);
    
    IOObjectRelease(root_domain);
    return ret;
}

static io_registry_entry_t copyRootDomainRef(void)
{
    return (io_registry_entry_t)IOServiceGetMatchingService(
                    MACH_PORT_NULL, IOServiceNameMatching("IOPMrootDomain"));
 }

static void print_setting_value(CFTypeRef a, int divider)
{
    int n;
    
    if(isA_CFNumber(a))
    {
        CFNumberGetValue(a, kCFNumberIntType, (void *)&n);

        if( 0 != divider ) n/=divider;

        printf("%d", n);    
    } else if(isA_CFBoolean(a))
    {
        printf("%d", CFBooleanGetValue(a));
    } else if(isA_CFString(a))
    {
        char buf[100];
        if(CFStringGetCString(a, buf, 100, kCFStringEncodingUTF8))
        {
            printf("%s", buf);
        }    
    } else printf("oops - print_setting_value unknown data type\n");
}

// Arguments to print_override_pids
enum {
    _kIOPMAssertionDisplayOn = 5,
    _kIOPMAssertionSystemOn = 6
};

static void show_pm_settings_dict(
    CFDictionaryRef d, 
    int indent, 
    bool show_overrides,
    bool prune_unsupported)
{
    int                     count;
    int                     i;
    int                     j;
    int                     divider = 0;
    char                    ps[kMaxArgStringLength];
    CFStringRef             *keys;
    CFTypeRef               *vals;
    CFTypeRef               ps_blob;
    CFStringRef             activeps = NULL;
    int                     show_override_type = 0;
    bool                    show_display_dim = false;
    
    ps_blob = IOPSCopyPowerSourcesInfo();
    if(ps_blob) {
        activeps = IOPSGetProvidingPowerSourceType(ps_blob);
    }
    if(!activeps) activeps = CFSTR(kIOPMACPowerKey);
    if(activeps) CFRetain(activeps);

    count = CFDictionaryGetCount(d);
    keys = (CFStringRef *)malloc(count * sizeof(CFStringRef));
    vals = (CFTypeRef *)malloc(count * sizeof(CFTypeRef));
    if(!keys || !vals) 
        goto exit;
    CFDictionaryGetKeysAndValues(d, (const void **)keys, (const void **)vals);

    for(i=0; i<count; i++)
    {
        show_override_type = 0;

        if (!CFStringGetCString(keys[i], ps, sizeof(ps), kCFStringEncodingMacRoman)) 
            continue;
                
        if( prune_unsupported 
            && !IOPMFeatureIsAvailable(keys[i], CFSTR(kIOPMACPowerKey)) )
        {
            // unsupported setting for the current power source!
            // do not show it!
            continue;
        }
        
        for(j=0; j<indent;j++) printf(" ");

        divider = 0;
        
		if (strcmp(ps, kIOPMPrioritizeNetworkReachabilityOverSleepKey) == 0)
		{
            printf(" %-20s ", ARG_NETAVAILABLE);                    
		} else if (strcmp(ps, kIOPMDisplaySleepKey) == 0) {
	        printf(" %-20s ", "displaysleep");  
	        if(show_overrides) {
	            show_override_type = _kIOPMAssertionDisplayOn;
		}
	        show_display_dim = true;
		} else if (strcmp(ps, kIOPMSystemSleepKey) == 0) {
			printf(" %-20s ", "sleep");
            if(show_overrides) {
                show_override_type = _kIOPMAssertionSystemOn;
			}
  		} else {
	        for(j=0; j<kNUM_PM_FEATURES; j++) {
	            if (!strncmp(ps, all_features[j].name, kMaxArgStringLength)) {
	                printf(" %-20s ", all_features[j].displayAs);
                    break;
	            }
	        }
            if (j==kNUM_PM_FEATURES)
                printf(" %-20s ",ps);
		}
		  
        print_setting_value(vals[i], divider);  

        if(show_override_type) 
            print_override_pids(show_override_type);
        if(show_display_dim) {
            if( is_display_dim_captured() )
                printf(" (Graphics dim captured)");
            show_display_dim = false;
        }
        printf("\n");
    }

exit:
    if (ps_blob) CFRelease(ps_blob);
    if (activeps) CFRelease(activeps);
    free(keys);
    free(vals);
}

static void show_system_power_settings(void)
{
    CFDictionaryRef     system_power_settings = NULL;
    CFBooleanRef        b;
    
    system_power_settings = IOPMCopySystemPowerSettings();
    if(!isA_CFDictionary(system_power_settings)) {
        goto exit;
    }

    printf("System-wide power settings:\n");

    if((b = CFDictionaryGetValue(system_power_settings, kIOPMSleepDisabledKey)))
    {
        printf(" SleepDisabled\t\t%d\n", (b==kCFBooleanTrue) ? 1:0);
    }

    if((b = CFDictionaryGetValue(system_power_settings, CFSTR(kIOPMDestroyFVKeyOnStandbyKey))))
    {
        printf(" DestroyFVKeyOnStandby\t\t%d\n", (b==kCFBooleanTrue) ? 1:0);
    }
exit:    
    if (system_power_settings)
        CFRelease(system_power_settings);
}



static void print_override_pids(int assertionType)
{
    CFDictionaryRef         assertions_pids = NULL;
    CFDictionaryRef         assertions_state = NULL;
    CFNumberRef             assertion_value;
    IOReturn                ret;
    char                    display_string[kMaxLongStringLength] = "\0";
    int                     length = 0;
    int                     bgTaskLevel = 0, pushTaskLevel = 0, preventSleepLevel = 0;
    int                     proxyLevel = 0;
    
    /*
     * Early pre-screening - don't dive into the assertions data
     * unless we know that there's an active assertion somewhere in there.
     */
    
    ret = IOPMCopyAssertionsStatus(&assertions_state);
    if( (kIOReturnSuccess != ret) || !isA_CFDictionary(assertions_state)) {
        goto bail;
    }
    
    if (_kIOPMAssertionDisplayOn == assertionType)
    {
        int userIdleLevel = 0;
        int noDisplayLevel = 0;
        int displayWake = 0;
        
        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypePreventUserIdleDisplaySleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &userIdleLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypeNoDisplaySleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &noDisplayLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertDisplayWake);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &displayWake);

        if ((kIOPMAssertionLevelOff == userIdleLevel)
            && (kIOPMAssertionLevelOff == noDisplayLevel)
            && (kIOPMAssertionLevelOff == displayWake))
        {
            goto bail;
        }
       snprintf(display_string, kMaxLongStringLength, " (display sleep prevented by ");
       length = strlen(display_string);
    }


    if (_kIOPMAssertionSystemOn == assertionType)
    {
        int noIdleLevel = 0;
        int userIdleLevel = 0;
        
        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypeNoIdleSleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &noIdleLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypePreventSystemSleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &preventSleepLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypePreventUserIdleSystemSleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &userIdleLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypeBackgroundTask);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &bgTaskLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertInternalPreventSleep);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &proxyLevel);

        assertion_value = CFDictionaryGetValue(assertions_state, kIOPMAssertionTypeApplePushServiceTask);
        if(assertion_value) 
            CFNumberGetValue(assertion_value, kCFNumberIntType, &pushTaskLevel);
        if ((kIOPMAssertionLevelOff == noIdleLevel)
            && (kIOPMAssertionLevelOff == preventSleepLevel)
            && (kIOPMAssertionLevelOff == bgTaskLevel)
            && (kIOPMAssertionLevelOff == pushTaskLevel)
            && (kIOPMAssertionLevelOff == userIdleLevel))
        {
            goto bail;
        }
        
       snprintf(display_string, kMaxLongStringLength, " (sleep prevented by ");
       length = strlen(display_string);
    }


    /*
     * Find out which pids have asserted this and print 'em out
     * We conclude that at least one pid is forcing a relevant assertion.
     */
    CFNumberRef             *pids = NULL;
    CFArrayRef              *assertions = NULL;
    int                     process_count;
    int                     this_is_the_first = 1;
    int                     i;

    
    ret = IOPMCopyAssertionsByProcess(&assertions_pids);
    if( (kIOReturnSuccess != ret) || !isA_CFDictionary(assertions_pids) ) {
        goto bail;    
    }
    

    process_count = CFDictionaryGetCount(assertions_pids);
    pids = malloc(sizeof(CFNumberRef)*process_count);
    assertions = malloc(sizeof(CFArrayRef)*process_count);
    CFDictionaryGetKeysAndValues(assertions_pids, 
                        (const void **)pids, 
                        (const void **)assertions);
    for(i=0; i<process_count; i++)
    {
        int         j;
        
        for(j=0; j<CFArrayGetCount(assertions[i]); j++)
        {
            CFDictionaryRef     tmp_assertion = NULL;
            CFNumberRef         tmp_level = NULL;
            CFStringRef         tmp_type = NULL;
            CFStringRef         process_name = NULL;
            char                pnameBuf[kProcNameBufLen];
            int                 level = kIOPMAssertionLevelOff;
            bool                print_this_pid = false;

            tmp_assertion = CFArrayGetValueAtIndex(assertions[i], j);
            
            if (!tmp_assertion || (kCFBooleanFalse == (CFBooleanRef)tmp_assertion)) 
            {
                continue;
            }
            
            if ( (tmp_type = CFDictionaryGetValue(tmp_assertion, kIOPMAssertionTrueTypeKey)) == NULL)
                tmp_type = CFDictionaryGetValue(tmp_assertion, kIOPMAssertionTypeKey);
            tmp_level = CFDictionaryGetValue(tmp_assertion, kIOPMAssertionLevelKey);
            process_name = CFDictionaryGetValue(tmp_assertion, CFSTR("Process Name"));
            
            level = kIOPMAssertionLevelOff;
            if (tmp_level) {
                CFNumberGetValue(tmp_level, kCFNumberIntType, &level);
            }

            if (level != kIOPMAssertionLevelOn) continue;

            if (_kIOPMAssertionSystemOn == assertionType) {
                if (CFEqual(tmp_type, kIOPMAssertionTypePreventUserIdleSystemSleep)) {
                    /* Found an assertion that keeps the system on */
                    print_this_pid = true;
                }
                else if ( (preventSleepLevel) &&
                          (CFEqual(tmp_type, kIOPMAssertionTypePreventSystemSleep)) ) {
                    print_this_pid = true;
                }
                else if ( (bgTaskLevel) &&
                          (CFEqual(tmp_type, kIOPMAssertionTypeBackgroundTask)) )
                {
                    print_this_pid = true;
                }
                else if ( (pushTaskLevel) &&
                          (CFEqual(tmp_type, kIOPMAssertionTypeApplePushServiceTask)) )
                {
                    print_this_pid = true;
                }
                else if ( (proxyLevel) &&
                          (CFEqual(tmp_type, kIOPMAssertInternalPreventSleep )) )
                {
                    print_this_pid = true;
                }            
            }
            else if ((_kIOPMAssertionDisplayOn == assertionType) 
                    && CFEqual(tmp_type, kIOPMAssertionTypePreventUserIdleDisplaySleep) )
            {
                /* Found an assertion that keeps the display on */
                print_this_pid = true;
            }
            else if ((_kIOPMAssertionDisplayOn == assertionType) 
                    && CFEqual(tmp_type, kIOPMAssertDisplayWake) )
            {
                /* Found an assertion that keeps the display on */
                print_this_pid = true;
            }            
            if (print_this_pid) 
            {
                if (this_is_the_first) {
                    this_is_the_first = 0;
                } else {
                    strncat(display_string, ", ", kMaxLongStringLength-length-1);
                    length = strlen(display_string);
                }            
                
                if (process_name && CFStringGetCString(process_name, pnameBuf, sizeof(pnameBuf), kCFStringEncodingUTF8))
                {
                    strncat(display_string, pnameBuf, kMaxLongStringLength-length-1);
                    length = strlen(display_string);
                }
            }            
        }        
    }

    strncat(display_string, ")", sizeof(display_string)-strlen(display_string)-1);
    
    printf("%s", display_string);
    
    free(pids);
    free(assertions);

bail:
    if(assertions_state) CFRelease(assertions_state);
    if(assertions_pids) CFRelease(assertions_pids);
    return;
}

static void show_supported_pm_features(void) 
{
    CFStringRef 		feature;
    CFTypeRef           ps_info = IOPSCopyPowerSourcesInfo();    
    CFStringRef 		source;
    char         	    ps_buf[40];
    int 				i;
    
    if(!ps_info) {
        source = CFSTR(kIOPMACPowerKey);
    } else {
        source = IOPSGetProvidingPowerSourceType(ps_info);
    }
    if(!isA_CFString(source) ||
       !CFStringGetCString(source, ps_buf, 40, kCFStringEncodingMacRoman)) {
        printf("internal supported features string error!\n");
    }
    
    printf("Capabilities for %s:\n", ps_buf);
    // iterate the list of all features
    for(i=0; i<kNUM_PM_FEATURES; i++)
    {
        feature = CFStringCreateWithCStringNoCopy(NULL, all_features[i].name, 
                                                  kCFStringEncodingMacRoman, kCFAllocatorNull);
        if (feature)
 		{
	        if (IOPMFeatureIsAvailable(feature, source)) 
			{
                printf(" %s\n", all_features[i].displayAs);  
	        }
	        CFRelease(feature);
		}
    }
    if(ps_info) {
		CFRelease(ps_info);
	}
}

static void show_power_profile(
    CFDictionaryRef     es,
    int                 indent)
{
    int                 num_profiles;
    int                 i, j;
    char                ps[kMaxArgStringLength];
    CFStringRef         *keys;
    CFDictionaryRef     *values;

    if(indent<0 || indent>30) indent=0;

    num_profiles = CFDictionaryGetCount(es);
    keys = (CFStringRef *)malloc(num_profiles * sizeof(CFStringRef));
    values = (CFDictionaryRef *)malloc(num_profiles * sizeof(CFDictionaryRef));
    if(keys && values)
    {
        CFDictionaryGetKeysAndValues(es, (const void **)keys, (const void **)values);
        
        for(i=0; i<num_profiles; i++)
        {
            if(!isA_CFDictionary(values[i])) 
                continue;
            
            if(!CFStringGetCString(keys[i], ps, sizeof(ps), kCFStringEncodingMacRoman)) 
                continue; // with for loop

            for(j=0; j<indent; j++) {
                printf(" ");
            }
            printf("%s:\n", ps);
            show_pm_settings_dict(values[i], indent, 0, false);
        }
    }
    if (keys) free(keys);
    if (values) free(values);
}

static void show_custom_pm_settings(void)
{
    CFDictionaryRef     es = NULL;

    // read settings file from /Library/Preferences/SystemConfiguration/com.apple.PowerManagement.plist
    es = IOPMCopyPMPreferences();
    if(!es) 
        return;
    show_power_profile(es, 0);
    CFRelease(es);
}

static void show_live_pm_settings(void)
{
    SCDynamicStoreRef        ds;
    CFDictionaryRef        live;

    ds = SCDynamicStoreCreate(NULL, CFSTR("pmset"), NULL, NULL);

    // read current settings from SCDynamicStore key
    live = SCDynamicStoreCopyValue(ds, CFSTR(kIOPMDynamicStoreSettingsKey));
    if(!live) 
        return;
    printf("Currently in use:\n");
    show_pm_settings_dict(live, 0, true, true);

    CFRelease(live);
    CFRelease(ds);
}

static void show_ups_settings(void)
{
    CFDictionaryRef     thresholds;
    CFDictionaryRef     d;
    CFNumberRef         n_val;
    int                 val;
    CFBooleanRef        b;

    thresholds = IOPMCopyUPSShutdownLevels(CFSTR(kIOPMDefaultUPSThresholds));    
    if(!thresholds) 
        return;

    printf("UPS settings:\n");
    
    if((d = CFDictionaryGetValue(thresholds, CFSTR(kIOUPSShutdownAtLevelKey))))
    {
        b = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelEnabledKey));
        n_val = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelValueKey));
        CFNumberGetValue(n_val, kCFNumberIntType, &val);
        printf("  %s\t%s\t%d\n", ARG_HALTLEVEL, (kCFBooleanTrue==b)?"on":"off", val);        
    }
    if((d = CFDictionaryGetValue(thresholds, CFSTR(kIOUPSShutdownAfterMinutesOn))))
    {
        b = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelEnabledKey));
        n_val = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelValueKey));
        CFNumberGetValue(n_val, kCFNumberIntType, &val);
        printf("  %s\t%s\t%d\n", ARG_HALTAFTER, (kCFBooleanTrue==b)?"on":"off", val);        
    }
    if((d = CFDictionaryGetValue(thresholds, CFSTR(kIOUPSShutdownAtMinutesLeft))))
    {
        b = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelEnabledKey));
        n_val = CFDictionaryGetValue(d, CFSTR(kIOUPSShutdownLevelValueKey));
        CFNumberGetValue(n_val, kCFNumberIntType, &val);
        printf("  %s\t%s\t%d\n", ARG_HALTREMAIN, (kCFBooleanTrue==b)?"on":"off", val);        
    }
    CFRelease(thresholds);
}

static void
show_active_profiles(void)
{
    CFDictionaryRef             active_prof = 0;
    int                         i;
    int                         count;
    int                         val;
    CFNumberRef                 *prof_val = 0;
    CFStringRef                 *ps = NULL;
    char                        ps_str[40];
    
    CFTypeRef                   ps_info = 0;
    CFStringRef                 current_ps = 0;

    ps_info = IOPSCopyPowerSourcesInfo();
    if(ps_info) current_ps = IOPSGetProvidingPowerSourceType(ps_info);
    if(!ps_info || !current_ps) current_ps = CFSTR(kIOPMACPowerKey);

    active_prof = IOPMCopyActivePowerProfiles();
    if(!active_prof) {
        printf("PM system error - no active profiles found\n");
        goto exit;
    }
    
    count = CFDictionaryGetCount(active_prof);
    prof_val = (CFNumberRef *)malloc(sizeof(CFNumberRef)*count);
    ps = (CFStringRef *)malloc(sizeof(CFStringRef)*count);
    if(!prof_val || !ps) goto exit;
    
    printf("Active Profiles:\n");

    CFDictionaryGetKeysAndValues(active_prof, (const void **)ps, (const void **)prof_val);
    for(i=0; i<count; i++)
    {
        if( CFStringGetCString(ps[i], ps_str, 40, kCFStringEncodingMacRoman)
          && CFNumberGetValue(prof_val[i], kCFNumberIntType, &val)) {
            printf("%s\t\t%d", ps_str, val);

            // Put a * next to the currently active power supply
            if( current_ps && (kCFCompareEqualTo == CFStringCompare(ps[i], current_ps, 0))) {
                printf("*");
            }

            printf("\n");
        }
    }

exit:
    if(active_prof) CFRelease(active_prof);
    if(ps_info) CFRelease(ps_info);
    if(prof_val) free(prof_val);
    if(ps) free(ps);
}

static void
show_system_profiles(void)
{
    CFArrayRef                  sys_prof;
    int                         prof_count;
    int                         i;

    sys_prof = IOPMCopyPowerProfiles();
    if(!sys_prof) {
        printf("No system profiles found\n");
        return;
    }

    prof_count = CFArrayGetCount(sys_prof);
    for(i=0; i<prof_count;i++)
    {
        printf("=== Profile %d ===\n", i);
        show_power_profile( CFArrayGetValueAtIndex(sys_prof, i), 0 );
        if(i!=(prof_count-1)) printf("\n");
    }

    CFRelease(sys_prof);
}

static CFDictionaryRef
getPowerEvent(int type, CFDictionaryRef     events)
{
    if(type)
        return (CFDictionaryRef)isA_CFDictionary(CFDictionaryGetValue(events, CFSTR(kIOPMRepeatingPowerOnKey)));
    else
        return (CFDictionaryRef)isA_CFDictionary(CFDictionaryGetValue(events, CFSTR(kIOPMRepeatingPowerOffKey)));
}
static int
getRepeatingDictionaryMinutes(CFDictionaryRef event)
{
    int val;
    CFNumberRef tmp_num;    
    tmp_num = (CFNumberRef)CFDictionaryGetValue(event, CFSTR(kIOPMPowerEventTimeKey));
    CFNumberGetValue(tmp_num, kCFNumberIntType, &val);
    return val;    
}
static int
getRepeatingDictionaryDayMask(CFDictionaryRef event)
{
    int val;
    CFNumberRef tmp_num;
    tmp_num = (CFNumberRef)CFDictionaryGetValue(event, CFSTR(kIOPMDaysOfWeekKey));
    CFNumberGetValue(tmp_num, kCFNumberIntType, &val);
    return val;
}
static CFStringRef
getRepeatingDictionaryType(CFDictionaryRef event)
{
    return (CFStringRef)CFDictionaryGetValue(event, CFSTR(kIOPMPowerEventTypeKey));
}

static void
print_time_of_day_to_buf(int m, char *buf, int buflen)
{
    int hours, minutes, afternoon;
    
    hours = m/60;
    minutes = m%60;
    afternoon = 0;
    if(hours >= 12) afternoon = 1; 
    if(hours > 12) hours-=12;

    snprintf(buf, buflen, "%d:%d%d%cM", hours,
            minutes/10, minutes%10,
            (afternoon? 'P':'A'));    
}

static void
print_days_to_buf(int d, char *buf, int buflen)
{
    switch(d) {
        case daily_mask:
            snprintf(buf, buflen, "every day");
            break;

        case weekday_mask:                        
            snprintf(buf, buflen, "weekdays only");
            break;
            
        case weekend_mask:
            snprintf(buf, buflen, "weekends only");
            break;

        case  0x01 :                        
            snprintf(buf, buflen, "Monday");
            break;
 
        case  0x02 :                        
            snprintf(buf, buflen, "Tuesday");
            break;

        case 0x04 :                       
            snprintf(buf, buflen, "Wednesday");
            break;

        case  0x08 :                       
            snprintf(buf, buflen, "Thursday");
            break;
 
        case 0x10 :
            snprintf(buf, buflen, "Friday");
            break;

        case 0x20 :                      
            snprintf(buf, buflen, "Saturday");
            break;
       
        case  0x40 :
            snprintf(buf, buflen, "Sunday");
            break;

        default:
            snprintf(buf, buflen, "Some days");
            break;       
    }
}

#define kMaxDaysOfWeekLength     20
static void print_repeating_report(CFDictionaryRef repeat)
{
    CFDictionaryRef     on, off;
    char                time_buf[kMaxDaysOfWeekLength];
    char                day_buf[kMaxDaysOfWeekLength];
    CFStringRef         type_str = NULL;
    char                type_buf[kMaxArgStringLength];

    // assumes validly formatted dictionary - doesn't do any error checking
    on = getPowerEvent(1, repeat);
    off = getPowerEvent(0, repeat);

    if(on || off)
    {
        printf("Repeating power events:\n");
        if(on)
        {
            print_time_of_day_to_buf(getRepeatingDictionaryMinutes(on), time_buf, kMaxDaysOfWeekLength);
            print_days_to_buf(getRepeatingDictionaryDayMask(on), day_buf, kMaxDaysOfWeekLength);
        
            type_str = getRepeatingDictionaryType(on);
            if (type_str) {
                CFStringGetCString(type_str, type_buf, sizeof(type_buf),  kCFStringEncodingMacRoman);
            } else {
                snprintf(type_buf, sizeof(type_buf), "?type?");
            }
            
            printf("  %s at %s %s\n", type_buf, time_buf, day_buf);
        }
        
        if(off)
        {
            print_time_of_day_to_buf(getRepeatingDictionaryMinutes(off), time_buf, kMaxDaysOfWeekLength);
            print_days_to_buf(getRepeatingDictionaryDayMask(off), day_buf, kMaxDaysOfWeekLength);

            type_str = getRepeatingDictionaryType(off);
            if (type_str) {
                CFStringGetCString(type_str, type_buf, sizeof(type_buf),  kCFStringEncodingMacRoman);
            } else {
                snprintf(type_buf, sizeof(type_buf), "?type?");
            }
            
            printf("  %s at %s %s\n", type_buf, time_buf, day_buf);
        }
        fflush(stdout);
    }
}

static void
print_scheduled_report(CFArrayRef events)
{
    CFDictionaryRef     ev;
    int                 count, i;
    char                date_buf[40];
    char                name_buf[255];
    char                type_buf[40];
    char                *type_ptr = NULL;
    CFStringRef         type = NULL;
    CFStringRef         author = NULL;
    CFDateFormatterRef  formatter = NULL;
    CFStringRef         cf_str_date = NULL;
    
    if(!events || !(count = CFArrayGetCount(events))) return;
    
    formatter = CFDateFormatterCreate(kCFAllocatorDefault, CFLocaleGetSystem(),
                                      kCFDateFormatterShortStyle, kCFDateFormatterMediumStyle);
    CFDateFormatterSetFormat(formatter, CFSTR(kDateAndTimeFormat));
    
    printf("Scheduled power events:\n");
    for(i=0; i<count; i++)
    {
        ev = (CFDictionaryRef)CFArrayGetValueAtIndex(events, i);
        
        cf_str_date = CFDateFormatterCreateStringWithDate(kCFAllocatorDefault,
                formatter, CFDictionaryGetValue(ev, CFSTR(kIOPMPowerEventTimeKey)));
        date_buf[0] = 0;
        if(cf_str_date) {
            CFStringGetCString(cf_str_date, date_buf, 40, kCFStringEncodingMacRoman);
            CFRelease(cf_str_date);
        }
            
        author = CFDictionaryGetValue(ev, CFSTR(kIOPMPowerEventAppNameKey));
        name_buf[0] = 0;
        if(isA_CFString(author)) 
            CFStringGetCString(author, name_buf, 255, kCFStringEncodingMacRoman);

        type = CFDictionaryGetValue(ev, CFSTR(kIOPMPowerEventTypeKey));
        type_buf[0] = 0;
        if(isA_CFString(type)) 
            CFStringGetCString(type, type_buf, 40, kCFStringEncodingMacRoman);

        // rename "wakepoweron" to "wakeorpoweron" to make things consistent between
        // "pmset -g" and "pmset sched"
        if(!strcmp(type_buf, kIOPMAutoWakeOrPowerOn)) 
            type_ptr = ARG_WAKEORPOWERON;
        else 
            type_ptr = type_buf;
        
        printf(" [%d]  %s at %s", i, type_ptr, date_buf);
        if(name_buf[0]) printf(" by %s", name_buf);
        printf("\n");
    }
    
    if (formatter)
        CFRelease(formatter);
}

static void show_scheduled_events(void)
{
    CFDictionaryRef     repeatingEvents;
    CFArrayRef          scheduledEvents;
 
    repeatingEvents = IOPMCopyRepeatingPowerEvents();
    scheduledEvents = IOPMCopyScheduledPowerEvents();

    if(!repeatingEvents && !scheduledEvents) {
        printf("No scheduled events.\n"); fflush(stdout);
        return;
    }

    if(repeatingEvents) {
        print_repeating_report(repeatingEvents);
        CFRelease(repeatingEvents);
    }
     
    if(scheduledEvents) {
        print_scheduled_report(scheduledEvents);
        CFRelease(scheduledEvents);
    }    
}

static bool matchingAssertion(CFDictionaryRef asst_dict, CFStringRef asst)
{
    if(!asst_dict || (kCFBooleanFalse == (CFTypeRef)asst_dict)) return false;

    return CFEqual(asst, 
                   CFDictionaryGetValue(asst_dict, kIOPMAssertionTypeKey));
}


static void show_active_assertions(uint32_t which)
{
    // Print active DisableInflow or ChargeInhibit assertions on the 
    // following line
    CFDictionaryRef         assertions_status = NULL;
    CFDictionaryRef         assertions_by_pid = NULL;
    CFStringRef             *assertionNames = NULL;
    CFNumberRef             *assertionValues = NULL;
    CFNumberRef             *pids = NULL;
    CFArrayRef              *pidAssertions = NULL;
    char                    name[50];
    int                     val;
    int                     total_assertion_count;
    int                     pid_count;
    IOReturn                ret;
    int                     i, j, k;
    
    if(0 == which) return;

    ret = IOPMCopyAssertionsStatus(&assertions_status);
    if(kIOReturnSuccess != ret || !assertions_status)
        return;
    
    ret = IOPMCopyAssertionsByProcess(&assertions_by_pid);
    if(kIOReturnSuccess != ret || !assertions_by_pid)
        return;

    // Grab out the total/aggregate sate of the assertions
    total_assertion_count = CFDictionaryGetCount(assertions_status);
    if(0 == total_assertion_count)
        return;

    assertionNames = (CFStringRef *)malloc(
                                sizeof(CFStringRef) * total_assertion_count);
    assertionValues = (CFNumberRef *)malloc(
                                sizeof(CFNumberRef) * total_assertion_count);
    CFDictionaryGetKeysAndValues(assertions_status, 
                                (const void **)assertionNames, 
                                (const void **)assertionValues);

    // Grab the list of activated assertions per-process
    pid_count = CFDictionaryGetCount(assertions_by_pid);
    if(0 == pid_count)
        return;
        
    pids = malloc(sizeof(CFNumberRef) * pid_count);
    pidAssertions = malloc(sizeof(CFArrayRef) * pid_count);
 
    CFDictionaryGetKeysAndValues(assertions_by_pid, 
                                (const void **)pids, 
                                (const void **)pidAssertions);

    if ( assertionNames && assertionValues && pidAssertions && pids)
    {        

        for(i=0; i<total_assertion_count; i++)
        {
            CFStringGetCString(assertionNames[i], name, 50, kCFStringEncodingMacRoman);
            CFNumberGetValue(assertionValues[i], kCFNumberIntType, &val);    
        
            // Determine if we want to display this assertion 
            if( !( ( (which & kAssertionCPU) &&
                    CFEqual(assertionNames[i], kIOPMCPUBoundAssertion))
                || ( (which & kAssertionInflow) &&
                    CFEqual(assertionNames[i], kIOPMInflowDisableAssertion))
                || ( (which & kAssertionCharge) &&
                    CFEqual(assertionNames[i], kIOPMChargeInhibitAssertion))
                || ( (which & kAssertionIdle) &&
                    CFEqual(assertionNames[i], kIOPMAssertionTypeNoIdleSleep)) ) )
            {
                // If the caller wasn't interested is this assertion, we pass
                continue;
            }
        
            if(val)
            {
                printf("\t'%s':\t", name);
                for(j=0; j<pid_count; j++)
                {                
                    for(k=0; k<CFArrayGetCount(pidAssertions[j]); k++)
                    {
                        CFDictionaryRef     obj;
                        if( (obj = (CFDictionaryRef)CFArrayGetValueAtIndex(
                                            pidAssertions[j], k)))
                        {
                            if(matchingAssertion(obj, assertionNames[i]))
                            {
                                int pid_num = 0;
                                CFNumberGetValue(pids[j], kCFNumberIntType, &pid_num);
                                printf("%d ", pid_num);
                            }
                       }
                    }
                }
                printf("\n"); fflush(stdout);
            }
        }
    }
    if (assertionNames) free(assertionNames);
    if (assertionValues) free(assertionValues);
    if (pidAssertions) free(pidAssertions);
    if (pids) free(pids);
    if (assertions_status) CFRelease(assertions_status);
    if (assertions_by_pid) CFRelease(assertions_by_pid);

    return;
}

/******************************************************************************/
/*                                                                            */
/*      BLOCK IDLE SLEEP                                                      */
/*                                                                            */
/******************************************************************************/

static bool prevent_idle_sleep(void)
{
    IOPMAssertionID         neverSleep;
    
    if (kIOReturnSuccess != 
        IOPMAssertionCreateWithName(kIOPMAssertionTypeNoIdleSleep,
                            kIOPMAssertionLevelOn,
                            CFSTR("pmset prevent sleep"),
                            &neverSleep))
    {
        // Failure
        return false;
    }
    
    printf("Preventing idle sleep (^C to exit)...\n");

    // ctrl-c at command-line exits
    while(1) {
        sleep(100);
    }
    
    return true;
}
enum {
    kPrintLotsOfThings  = 0,
    kJustPrintSleep     = 1,
    kJustPrintWake      = 2
};

static void printSleepAndWakeReasons(int just_do_it)
{
    CFStringRef lastSleepReason     = copyRootDomainProperty(CFSTR("Last Sleep Reason"));
    CFStringRef wakeReason          = copyRootDomainProperty(CFSTR("Wake Reason"));
    CFStringRef wakeType            = copyRootDomainProperty(CFSTR("Wake Type"));
    char buf[100];
    
    if (just_do_it != kJustPrintWake)
    {
        if (lastSleepReason) {
            CFStringGetCString(lastSleepReason, buf, sizeof(buf), kCFStringEncodingUTF8);
            printf("  Last Sleep Reason = %s\n", buf);
        }
    }
    
    if (just_do_it != kJustPrintSleep)
    {
        if (wakeReason) {
            CFStringGetCString(wakeReason, buf, sizeof(buf), kCFStringEncodingUTF8);
            printf("  Wake Reason = %s\n", buf);
        }
        if (wakeType) {
            CFStringGetCString(wakeType, buf, sizeof(buf), kCFStringEncodingUTF8);
            printf("  wakeType = %s\n", buf);
        }
    }
    
    if (lastSleepReason)        { CFRelease(lastSleepReason); }
    if (wakeReason)             { CFRelease(wakeReason); }
    if (wakeType)               { CFRelease(wakeType); }

    return;
}


/* sleepWakeCallback
 * 
 * Receives notifications on system sleep and system wake.
 */
static void
sleepWakeCallback(
    void *refcon, 
    io_service_t y __unused,
    natural_t messageType,
    void * messageArgument)
{
    uint32_t behavior = (uintptr_t)refcon;

    switch ( messageType ) {
    case kIOMessageSystemWillSleep:
        if ( behavior & kLogSleepEvents )
        {
            printf("\n");
            print_pretty_date(CFAbsoluteTimeGetCurrent(), false); 
            printf("IORegisterForSystemPower: ...Sleeping...\n");
//            printSleepAndWakeReasons(kJustPrintSleep);
            fflush(stdout);
        }

        IOAllowPowerChange(gPMAckPort, (long)messageArgument);
        break;
        
    case kIOMessageCanSystemSleep:
        if( behavior & kCancelSleepEvents ) 
        {
            IOCancelPowerChange(gPMAckPort, (long)messageArgument);
            printf("\n");
            print_pretty_date(CFAbsoluteTimeGetCurrent(), false); 
            printf("IORegisterForSystemPower: ...Cancelling...\n");
//            printSleepAndWakeReasons(kJustPrintSleep);
        } else {
            IOAllowPowerChange(gPMAckPort, (long)messageArgument);
        }
        break;
        
    case kIOMessageSystemHasPoweredOn:
        if ( behavior & kLogSleepEvents )
        {
            printf("\n");
            print_pretty_date(CFAbsoluteTimeGetCurrent(), false);   // false == no newline        
            printf("IORegisterForSystemPower: ...HasPoweredOn...\n");
            printSleepAndWakeReasons(kJustPrintWake);
            fflush(stdout);
        }
        break;
    }

    return;
}



/******************************************************************************/
/*                                                                            */
/*     PS LOGGING                                                             */
/*                                                                            */
/******************************************************************************/

#define kMaxHealthLength            10
#define kMaxNameLength              60

static void show_power_sources(int which)
{
    CFTypeRef           ps_info = IOPSCopyPowerSourcesInfo();
    CFArrayRef          list = NULL;
    CFStringRef         ps_name = NULL;
    static CFStringRef  last_ps = NULL;
    static CFAbsoluteTime   invocationTime = 0.0;        
    CFDictionaryRef     one_ps = NULL;
    char                strbuf[kMaxLongStringLength];
    int                 count;
    int                 i;
    int                 show_time_estimate;
    CFNumberRef         remaining, charge, capacity;
    CFBooleanRef        charging;
    CFBooleanRef        charged;
    CFBooleanRef        finishingCharge;
    CFBooleanRef        present;
    CFStringRef         name;
    CFStringRef         state;
    CFStringRef         transport;
    CFStringRef         health;
    CFStringRef         confidence;
    CFStringRef         failure;
    char                _health[kMaxHealthLength];
    char                _confidence[kMaxHealthLength];
    char                _name[kMaxNameLength];
    char                _failure[kMaxLongStringLength];
    int                 _hours = 0;
    int                 _minutes = 0;
    int                 _charge = 0;
    int                 _FCCap = 0;
    bool                _charging = false;
    bool                _charged = false;
    bool                _finishingCharge = false;
    int                 _warningLevel = 0;
    CFArrayRef          permFailuresArray = NULL;
    CFStringRef         pfString = NULL;
    
    if(!ps_info) {
        printf("No power source info available\n");
        return;
    }
    
    if (isBatteryPollingStopped()) {
        printf("* Battery Polling is Stopped by \"pmset nopoll\" *\n");
    }
    
    /* Output path for Time Remaining Columns
     *  - Only displays battery
     */
    if (kShowColumns & which) 
    {    
        CFTypeRef               one_ps_descriptor = NULL;
        CFAbsoluteTime          nowTime = CFAbsoluteTimeGetCurrent();
        uint32_t                minutesSinceInvocation = 0;
        int32_t                 estimatedMinutesRemaining = 0;
    
        if (invocationTime == 0.0) {
            invocationTime = nowTime;
        }
    
        // Note: We assume a one-battery system.
        one_ps_descriptor = IOPSGetActiveBattery(ps_info);
        if (one_ps_descriptor) {
            one_ps = IOPSGetPowerSourceDescription(ps_info, one_ps_descriptor);
        }
        if(!one_ps) {
            printf("Logging power sources: unable to locate battery.\n");
            goto exit;
        };

        charging = CFDictionaryGetValue(one_ps, CFSTR(kIOPSIsChargingKey));
        state = CFDictionaryGetValue(one_ps, CFSTR(kIOPSPowerSourceStateKey));
        if (CFEqual(state, CFSTR(kIOPSBatteryPowerValue)))
        {
            remaining = CFDictionaryGetValue(one_ps, CFSTR(kIOPSTimeToEmptyKey));
        } else {
            remaining = CFDictionaryGetValue(one_ps, CFSTR(kIOPSTimeToFullChargeKey));
        }       
        
        if (remaining) {
            CFNumberGetValue(remaining, kCFNumberIntType, &estimatedMinutesRemaining);
        } else {
            estimatedMinutesRemaining = -1;
        }

        minutesSinceInvocation = (nowTime - invocationTime)/60;

        charge = CFDictionaryGetValue(one_ps, CFSTR(kIOPSCurrentCapacityKey));
        if(charge) CFNumberGetValue(charge, kCFNumberIntType, &_charge);
        
        //  "Elapsed", "TimeRemaining", "Percent""Charge", "Timestamp");
        printf("%10d\t%15d\t%10d%%\t%10s\t", minutesSinceInvocation, estimatedMinutesRemaining,
                    _charge, (kCFBooleanTrue == charging) ? "charge" : "discharge");
        print_pretty_date(CFAbsoluteTimeGetCurrent(), true);
        
        goto exit;
    }
    /* Completed output path
     */
    
    ps_name = IOPSGetProvidingPowerSourceType(ps_info);
    if(!ps_name || !CFStringGetCString(ps_name, strbuf, kMaxLongStringLength, kCFStringEncodingUTF8))
    {
        goto exit;
    }
    if(!last_ps || kCFCompareEqualTo != CFStringCompare(last_ps, ps_name, 0))
    {
        printf("Now drawing from '%s'\n", strbuf);
    }
    if(last_ps) CFRelease(last_ps);
    last_ps = CFStringCreateCopy(kCFAllocatorDefault, ps_name);
    
    list = IOPSCopyPowerSourcesList(ps_info);
    if(!list) goto exit;
    count = CFArrayGetCount(list);
    for(i=0; i<count; i++)
    {
        bzero(_health, sizeof(_health));    
        bzero(_confidence, sizeof(_confidence));    
        bzero(_name, sizeof(_name));    
        bzero(_failure, sizeof(_failure));
        _hours = _minutes = _charge = _FCCap = 0;
        _charging = _charged = _finishingCharge = false;

        one_ps = IOPSGetPowerSourceDescription(ps_info, CFArrayGetValueAtIndex(list, i));
        if(!one_ps) break;
       
        // Only display settings for power sources we want to show
        transport = CFDictionaryGetValue(one_ps, CFSTR(kIOPSTransportTypeKey));
        if(!transport) continue;
        if(kCFCompareEqualTo != CFStringCompare(transport, CFSTR(kIOPSInternalType), 0))
        {
            // Internal transport means internal battery
            if(!(which & kApplyToBattery)) continue;
        } else {
            // Any specified non-Internal transport is a UPS
            if(!(which & kApplyToUPS)) continue;
        }
        
        charging = CFDictionaryGetValue(one_ps, CFSTR(kIOPSIsChargingKey));
        state = CFDictionaryGetValue(one_ps, CFSTR(kIOPSPowerSourceStateKey));
        if(kCFCompareEqualTo == CFStringCompare(state, CFSTR(kIOPSBatteryPowerValue), 0))
        {
            remaining = CFDictionaryGetValue(one_ps, CFSTR(kIOPSTimeToEmptyKey));
        } else {
            remaining = CFDictionaryGetValue(one_ps, CFSTR(kIOPSTimeToFullChargeKey));
        }            
        name = CFDictionaryGetValue(one_ps, CFSTR(kIOPSNameKey));
        charge = CFDictionaryGetValue(one_ps, CFSTR(kIOPSCurrentCapacityKey));
        capacity = CFDictionaryGetValue(one_ps, CFSTR(kIOPSMaxCapacityKey));
        present = CFDictionaryGetValue(one_ps, CFSTR(kIOPSIsPresentKey));
        health = CFDictionaryGetValue(one_ps, CFSTR(kIOPSBatteryHealthKey));
        confidence = CFDictionaryGetValue(one_ps, CFSTR(kIOPSHealthConfidenceKey));
        failure = CFDictionaryGetValue(one_ps, CFSTR("Failure"));
        charged = CFDictionaryGetValue(one_ps, CFSTR(kIOPSIsChargedKey));
        finishingCharge = CFDictionaryGetValue(one_ps, CFSTR(kIOPSIsFinishingChargeKey));
        
        permFailuresArray = CFDictionaryGetValue(one_ps, CFSTR(kIOPSBatteryFailureModesKey));
        
        if(name) CFStringGetCString(name, _name, 
                                kMaxNameLength, kCFStringEncodingMacRoman);
        if(health) CFStringGetCString(health, _health, 
                                kMaxHealthLength, kCFStringEncodingMacRoman);
        if(confidence) CFStringGetCString(confidence, _confidence, 
                                kMaxHealthLength, kCFStringEncodingMacRoman);
        if(failure) CFStringGetCString(failure, _failure,
                                kMaxLongStringLength, kCFStringEncodingMacRoman);
        if(charge) CFNumberGetValue(charge, kCFNumberIntType, &_charge);
        if(capacity) CFNumberGetValue(capacity, kCFNumberIntType, &_FCCap);
        if(remaining) 
        {
            CFNumberGetValue(remaining, kCFNumberIntType, &_minutes);
            if(-1 != _minutes) {
                _hours = _minutes/60;
                _minutes = _minutes%60;
            }
        }
        if(charging) _charging = (kCFBooleanTrue == charging);
        if (charged) _charged = (kCFBooleanTrue == charged);
        if (finishingCharge) _finishingCharge = (kCFBooleanTrue == finishingCharge);

        _warningLevel = IOPSGetBatteryWarningLevel();
        
        show_time_estimate = 1;
        
        printf(" -");
        if(name) printf("%s\t", _name);
        if(present && (kCFBooleanTrue == present))
        {
            if(charge && _FCCap) printf("%d%%; ", _charge*100/_FCCap);
            if(charging) {
                if (_finishingCharge) {
                    printf("finishing charge");
                } else if (_charged) {
                    printf("charged");
                } else if(_charging) {
                    printf("charging");
                } else {
                    if(kCFCompareEqualTo == CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0)) {
                        printf("AC attached; not charging");
                        show_time_estimate = 0;
                    } else {
                        printf("discharging");
                    }
                }
            }
            if(show_time_estimate && remaining) {
                if(-1 != _minutes) {
                    printf("; %d:%d%d remaining", _hours, _minutes/10, _minutes%10);
                } else {
                    printf("; (no estimate)");
                }
            }
            if (health && confidence
                && !CFEqual(CFSTR("Good"), health)) {
                printf(" (%s/%s)", _health, _confidence);
            }
            if(failure) {
                printf("\n\tfailure: \"%s\"", _failure);
            }
            if (permFailuresArray) {
                int failure_count = CFArrayGetCount(permFailuresArray);
                int m = 0;
    
                printf("\n\tDetailed failures:");
                
                for(m=0; m<failure_count; m++) {
                    pfString = CFArrayGetValueAtIndex(permFailuresArray, m);
                    if (pfString) 
                    {
                        CFStringGetCString(pfString, strbuf, kMaxLongStringLength, kCFStringEncodingMacRoman);
                        printf(" \"%s\"", strbuf);
                    }
                    if (m != failure_count - 1)
                        printf(",");
                }
            }
            printf("\n"); fflush(stdout);

            // Show batery warnings on a new line:
            if (kIOPSLowBatteryWarningEarly == _warningLevel) {
                printf("\tBattery Warning: Early\n");
            } else if (kIOPSLowBatteryWarningFinal == _warningLevel) {
                printf("\tBattery Warning: Final\n");
            }       
        } else {
            printf(" (removed)\n");        
        }
        
        
    }
    
    // Display the battery-specific assertions that are affecting the system
    show_active_assertions( kAssertionInflow | kAssertionCharge );
    
exit:
    if(ps_info) CFRelease(ps_info);  
    if(list) CFRelease(list);
    return;
}

static void print_pretty_date(CFAbsoluteTime t, bool newline)
{
    print_date_with_style("%-24.24s ", kCFDateFormatterShortStyle, kCFDateFormatterLongStyle, t, newline);
}

static void print_short_date(CFAbsoluteTime t, bool newline)
{
    print_date_with_style("%s ", kCFDateFormatterShortStyle, kCFDateFormatterShortStyle, t, newline);
}


static void print_date_with_style(const char *dsf, CFDateFormatterStyle dayStyle, CFDateFormatterStyle timeStyle, CFAbsoluteTime t, bool newline)
{
    CFDateFormatterRef  date_format;
    CFTimeZoneRef       tz;
    CFStringRef         time_date;
    CFLocaleRef         loc;
    char                _date[60];
 
    loc = CFLocaleCopyCurrent();
    date_format = CFDateFormatterCreate(kCFAllocatorDefault, loc,
        dayStyle, timeStyle);
    CFRelease(loc);
    tz = CFTimeZoneCopySystem();
    CFDateFormatterSetProperty(date_format, kCFDateFormatterTimeZone, tz);
    CFRelease(tz);
    time_date = CFDateFormatterCreateStringWithAbsoluteTime(kCFAllocatorDefault,
        date_format, t);
    CFRelease(date_format);

    if(time_date)
    {
        CFStringGetCString(time_date, _date, 60, kCFStringEncodingMacRoman);
        printf(dsf, _date); fflush(stdout);
        if(newline) printf("\n");
        CFRelease(time_date); 
    }
}

/******************************************************************************/

static void show_assertions_system_aggregates(void)
{
    /*
     *   Copy aggregates
     */
    CFStringRef             *assertionNames = NULL;
    CFNumberRef             *assertionValues = NULL;
    char                    name[50];
    int                     val;
    int                     count;
    CFDictionaryRef         assertions_status;
    IOReturn                ret;
    int                     i;
    
    ret = IOPMCopyAssertionsStatus(&assertions_status);
    if ((kIOReturnSuccess != ret) || (NULL == assertions_status))
    {
        printf("No assertions.\n");
        return;
    }
    
    count = CFDictionaryGetCount(assertions_status);
    if (0 == count)
    {
        return;
    }
    
    assertionNames = (CFStringRef *)malloc(sizeof(void *)*count);
    assertionValues = (CFNumberRef *)malloc(sizeof(void *)*count);
    CFDictionaryGetKeysAndValues(assertions_status, 
                                 (const void **)assertionNames, (const void **)assertionValues);
    
    printf("Assertion status system-wide:\n");
    for (i=0; i<count; i++)
    {
        CFNumberGetValue(assertionValues[i], kCFNumberIntType, &val);    
        if ((kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeNeedsCPU, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeDisableInflow, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeInhibitCharging, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeDisableLowBatteryWarnings, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertInternalPreventSleep, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertInternalPreventDisplaySleep, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertDisplayWake, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeDisableRealPowerSources_Debug, 0)))
        {
            /* These are rarely used. So, print only if they are set */
            if (val == 0)
                continue;
        }
        
#if !TARGET_OS_EMBEDDED
        if ((kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeEnableIdleSleep, 0)) ||
            (kCFCompareEqualTo == CFStringCompare(assertionNames[i], kIOPMAssertionTypeSystemIsActive, 0)))
            continue;
#endif
        
        CFStringGetCString(assertionNames[i], name, 50, kCFStringEncodingMacRoman);
        printf("   %-30s %d\n", name, val);
    }
    
    free(assertionNames);
    free(assertionValues);
    CFRelease(assertions_status);
}

static void show_assertions_individually(void)
{
    CFDictionaryRef         assertions_info = NULL;
    IOReturn                ret;
    
    ret = IOPMCopyAssertionsByProcess(&assertions_info);
    if ((kIOReturnSuccess != ret) || !assertions_info) {
        return;
    }
    
//    printf("\nListed by owning process:\n");
    printf("Listed by owning process:\n");
    if (!assertions_info) {
        printf("   None\n");
    } else {
        
        CFNumberRef             *pids = NULL;
        CFArrayRef              *assertions = NULL;
        int                     process_count;
        int                     i;
        
        process_count = CFDictionaryGetCount(assertions_info);
        pids = malloc(sizeof(CFNumberRef)*process_count);
        assertions = malloc(sizeof(CFArrayRef *)*process_count);
        CFDictionaryGetKeysAndValues(assertions_info, 
                                     (const void **)pids, 
                                     (const void **)assertions);
        
        for(i=0; i<process_count; i++)
        {
            int the_pid;
            int j;
            
            CFNumberGetValue(pids[i], kCFNumberIntType, &the_pid);
            
            for(j=0; j<CFArrayGetCount(assertions[i]); j++)
            {
                CFDictionaryRef         tmp_dict = NULL;
                CFStringRef             val_string = NULL;
                CFStringRef             raw_localizable_string = NULL;
                CFStringRef             localized_string = NULL;
                CFStringRef             bundlePath = NULL;
                CFBundleRef             bundle = NULL;
                CFURLRef                _url = NULL;
                CFNumberRef             uniqueID = NULL;
                CFBooleanRef            power_limits = NULL;
                uint64_t                uniqueID_int = 0;
                CFDateRef               createdDate = NULL;
                CFAbsoluteTime          createdCFTime = 0.0;
                char                    ageString[40];
                CFStringRef             pidName = NULL;
                char                    pid_name_buf[100];
                
                char                    all_assertions_buf[400];
                char                    val_buf[300];
                bool                    timed_out = false;
                
                tmp_dict = CFArrayGetValueAtIndex(assertions[i], j);
                if(!tmp_dict) {
                    goto exit;
                }
                
                bzero(all_assertions_buf, sizeof(all_assertions_buf));
                
                val_string = CFDictionaryGetValue(tmp_dict, kIOPMAssertionTypeKey);
                if (val_string) {
                    CFStringGetCString(val_string, all_assertions_buf, sizeof(all_assertions_buf), kCFStringEncodingMacRoman);
                } else {
                    snprintf(all_assertions_buf, sizeof(all_assertions_buf), "Missing AssertType property");
                }
                
                val_string = CFDictionaryGetValue(tmp_dict, kIOPMAssertionNameKey);
                if (val_string) {
                    CFStringGetCString(val_string, val_buf, sizeof(val_buf), kCFStringEncodingMacRoman);
                }
                
                timed_out = CFDictionaryGetValue(tmp_dict, kIOPMAssertionTimedOutDateKey);
                
                uniqueID = CFDictionaryGetValue(tmp_dict, kIOPMAssertionGlobalUniqueIDKey);
                if (uniqueID) {
                    CFNumberGetValue(uniqueID, kCFNumberSInt64Type, &uniqueID_int);
                }
                
                if ((createdDate = CFDictionaryGetValue(tmp_dict, kIOPMAssertionCreateDateKey))) 
                {
                    createdCFTime                   = CFDateGetAbsoluteTime(createdDate);
                    int createdSince                = (int)(CFAbsoluteTimeGetCurrent() - createdCFTime);
                    int hours                       = createdSince / 3600;
                    int minutes                     = (createdSince / 60) % 60;
                    int seconds                     = createdSince % 60;
                    snprintf(ageString, sizeof(ageString), "%02d:%02d:%02d ", hours, minutes, seconds);
                }
                
                if ((pidName = CFDictionaryGetValue(tmp_dict, kIOPMAssertionProcessNameKey)))
                {
                    CFStringGetCString(pidName, pid_name_buf, sizeof(pid_name_buf), kCFStringEncodingUTF8);
                }
                
                printf("   pid %d(%s): [0x%016llx] %s%s named: \"%s\" %s\n",
                       the_pid, pidName?pid_name_buf:"?",
                       uniqueID_int,
                       createdDate ? ageString:"",
                       all_assertions_buf,
                       val_string ? val_buf : "(error - no name)",
                       timed_out ? "(timed out)" : "");                
                
                val_string = CFDictionaryGetValue(tmp_dict, kIOPMAssertionDetailsKey);
                if (val_string) {
                    CFStringGetCString(val_string, val_buf, sizeof(val_buf), kCFStringEncodingMacRoman);
                    printf("\tDetails: %s\n", val_buf);
                }
                
                raw_localizable_string = CFDictionaryGetValue(tmp_dict, kIOPMAssertionHumanReadableReasonKey);                
                bundlePath = CFDictionaryGetValue(tmp_dict, kIOPMAssertionLocalizationBundlePathKey);
                
                if (raw_localizable_string && bundlePath) {
                    _url = CFURLCreateWithFileSystemPath(0, bundlePath, kCFURLPOSIXPathStyle, TRUE);
                    if (_url) {
                        bundle = CFBundleCreate(0, _url);
                        CFRelease(_url);
                    }
                    if (bundle) {
                        
                        localized_string = CFBundleCopyLocalizedString(bundle, raw_localizable_string, NULL, NULL);
                        
                        if (localized_string) {
                            if (CFStringGetCString(localized_string, val_buf, sizeof(val_buf), kCFStringEncodingUTF8)) {
                                printf("\tLocalized=%s\n", val_buf);
                            }
                            CFRelease(localized_string);
                        }
                    }
                } // Localize & display human readable string
                
                if ( (power_limits = CFDictionaryGetValue(tmp_dict, 
                                                          kIOPMAssertionAppliesToLimitedPowerKey)) ) {
                    if ((CFBooleanGetValue(power_limits)))
                        printf("\tAssertion applied on Battery power also\n");
                    else
                        printf("\tAssertion applied on AC  power only\n");
                }
                
                CFNumberRef         timeoutNumCF    = NULL;
                uint64_t            timeout         = 0;
                CFStringRef         actionStr       = NULL;
                char                actionBuf[55];
                CFDateRef           updateDate      = NULL;
                CFAbsoluteTime      updateTime      = 0;
                CFTimeInterval      timeLeft      = 0;
                CFAbsoluteTime      now             = CFAbsoluteTimeGetCurrent();
                
                if ( (timeoutNumCF = CFDictionaryGetValue(tmp_dict, kIOPMAssertionTimeoutTimeLeftKey)))
                {
                    CFNumberGetValue(timeoutNumCF, kCFNumberIntType, &timeout);

                    updateDate = CFDictionaryGetValue(tmp_dict, kIOPMAssertionTimeoutUpdateTimeKey);
                    if (updateDate) {
                        updateTime = CFDateGetAbsoluteTime(updateDate);
                    }

                    if (timeout && updateDate && ((timeLeft = updateTime+timeout-now) > 0)) {
                        
                        actionStr = CFDictionaryGetValue(tmp_dict, kIOPMAssertionTimeoutActionKey);
                        if (actionStr) {
                            CFStringGetCString(actionStr, actionBuf, sizeof(actionBuf), kCFStringEncodingUTF8);
                        }
                        
                        printf("\tTimeout will fire in %.0f secs Action=%s\n", 
                               timeLeft, actionStr ? actionBuf : "<unknown action>");
                    }
                }
            }
        }
    } 

exit:
    if(assertions_info) 
        CFRelease(assertions_info);
}

static void print_descriptive_kernel_assertions(uint32_t val32)
{
    bool first = false;
    if (0!= val32)
        printf("=");
    if (val32&kIOPMDriverAssertionCPUBit) {
        printf("CPU");
        first = true;
    }
    if (val32&kIOPMDriverAssertionUSBExternalDeviceBit) {
        if (first) printf(",");
        first=true;
        printf("USB");
    }
    if (val32&kIOPMDriverAssertionBluetoothHIDDevicePairedBit) {
        if (first) printf(",");
        first=true;
        printf("BT-HID");
    }
    if (val32&kIOPMDriverAssertionExternalMediaMountedBit) {
        if (first) printf(",");
        first=true;
        printf("MEDIA");
    }
    if (val32&kIOPMDriverAssertionReservedBit5) {
        if (first) printf(",");
        first=true;
        printf("THNDR");
    }
    if (val32&kIOPMDriverAssertionPreventDisplaySleepBit) {
        if (first) printf(",");
        first=true;
        printf("DSPLY");
    }
    if (val32&kIOPMDriverAssertionReservedBit7) {
        if (first) printf(",");
        first=true;
        printf("STORAGE");
    }
    if (val32&kIOPMDriverAssertionMagicPacketWakeEnabledBit) {
        if (first) printf(",");
        first=true;
        printf("MAGICWAKE");
    }
    return;
}

static void show_assertions_in_kernel(void)
{
    CFMutableDictionaryRef  rootDomainProperties = NULL;
   
    CFArrayRef      kernelAssertionsArray = NULL;
    CFNumberRef     kernelAssertions = NULL;
    int             kernelAssertionsSum = 0;
    int             count;
    int             i;
    io_registry_entry_t rootDomain = copyRootDomainRef();
    
    IORegistryEntryCreateCFProperties(rootDomain, &rootDomainProperties, 0, 0);
    
    if (rootDomainProperties)
    {
        kernelAssertions = CFDictionaryGetValue(rootDomainProperties, CFSTR(kIOPMAssertionsDriverKey));
        if (kernelAssertions) {
            CFNumberGetValue(kernelAssertions, kCFNumberIntType, &kernelAssertionsSum);
        }
        
        kernelAssertionsArray = CFDictionaryGetValue(rootDomainProperties, CFSTR(kIOPMAssertionsDriverDetailedKey));
    }
    
    if (0 == kernelAssertionsSum) {
        printf("No kernel assertions.\n");
//        printf("\nNo kernel assertions.\n");
        return;
    }
   
    printf("Kernel Assertions: 0x%x", kernelAssertionsSum);
//    printf("\nKernel Assertions: 0x%x", kernelAssertionsSum);
    print_descriptive_kernel_assertions(kernelAssertionsSum);
    printf("\n");
    
    if (!kernelAssertionsArray
        || !(count = CFArrayGetCount(kernelAssertionsArray)))
    {
        printf("   None");
    } else {
        CFDictionaryRef whichAssertion = NULL;
        CFStringRef     ownerString = NULL;
        char            ownerBuf[100];
        io_name_t       serviceNameBuf;
        CFNumberRef     registryEntryID = NULL;
        CFNumberRef     n_id = NULL;
        CFNumberRef     n_modified = NULL;
        CFNumberRef     n_owner = NULL;
        CFNumberRef     n_level = NULL;
        CFNumberRef     n_asserted = NULL;
        uint64_t        val64=0;
        uint32_t        val32=0;
        
        for (i=0; i<count; i++) 
        {
            whichAssertion = isA_CFDictionary(CFArrayGetValueAtIndex(kernelAssertionsArray, i));
            if (!whichAssertion)
                continue;
#ifndef kIOPMDriverRegistryEntryIDKey
#define kIOPMDriverRegistryEntryIDKey "RegistryEntryID"
#endif
            ownerString         = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionOwnerStringKey));
            registryEntryID     = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverRegistryEntryIDKey));
            n_id                = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionIDKey));
            n_modified          = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionModifiedTimeKey));
            n_owner             = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionOwnerServiceKey));
            n_level             = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionLevelKey));
            n_asserted          = CFDictionaryGetValue(whichAssertion, CFSTR(kIOPMDriverAssertionAssertedKey));

            
            CFAbsoluteTime      modifiedTime = 1.0;
            uint32_t            level = 0;

            if (n_modified && CFNumberGetValue(n_modified, kCFNumberSInt64Type, &val64)) {
                modifiedTime = _CFAbsoluteTimeFromPMEventTimeStamp(val64);
            }
            if (n_level) {
                CFNumberGetValue(n_level, kCFNumberSInt32Type, &level);
            }
                
            if (level != kIOPMAssertionLevelOff)
            {
                if (n_id && CFNumberGetValue(n_id, kCFNumberSInt64Type, &val64)) {
                    printf("   id=%ld ", (long)val64);
                }
                if (n_owner && CFNumberGetValue(n_owner, kCFNumberSInt64Type, &val64)) {
                    printf("by [0x%016lx]", (unsigned long)val64);
                }
                
                if (n_asserted) {
                    if (CFNumberGetValue(n_asserted, kCFNumberSInt32Type, &val32)) {
                        printf(" level=%d 0x%x", level, val32);
                        print_descriptive_kernel_assertions(val32);
                    }
                }
                printf(" mod=");
                print_short_date(modifiedTime, false);
                if (ownerString &&
                    CFStringGetCString(ownerString, ownerBuf, sizeof(ownerBuf), kCFStringEncodingUTF8))
                {
                    printf("description=%s ", ownerBuf);
                }
                
                if (registryEntryID && CFNumberGetValue(registryEntryID, kCFNumberSInt64Type, &val64))
                {
                    io_service_t match = IO_OBJECT_NULL;
                    match = IOServiceGetMatchingService(kIOMasterPortDefault, IORegistryEntryIDMatching(val64));
                    if (match) {
                        IORegistryEntryGetName(match, serviceNameBuf);
                        printf("owner=%s", serviceNameBuf);
                        IOObjectRelease(match);
                    }
                }
                printf("\n");
            }
        }
    }
    CFRelease(rootDomainProperties);
}
static void show_assertions(const char *decorate)
{
    print_pretty_date(CFAbsoluteTimeGetCurrent(), decorate?false:true);
    if (decorate) {
        printf("%s\n", decorate);
    }
    
    show_assertions_system_aggregates();
    show_assertions_individually();
    show_assertions_in_kernel();
    
    return;
}

static void log_assertions(void)
{
    int                 token;
    int                 notify_status;

    IOPMAssertionNotify(kIOPMAssertionsAnyChangedNotifyString, kIOPMNotifyRegister);
    notify_status = notify_register_dispatch(
            kIOPMAssertionsAnyChangedNotifyString, 
            &token, 
            dispatch_get_main_queue(),
            ^(int t) {
               show_assertions(NULL);
             });

    if (NOTIFY_STATUS_OK != notify_status) {
        printf("Could not get notification for %s. Exiting.\n",
                kIOPMAssertionsAnyChangedNotifyString);
        return;
    }

    printf("Logging all assertion changes.\n");
    show_assertions(NULL);
    
    dispatch_main();
}

/******************************************************************************/

static const char *stringForGTLevel(int gtl)
{
    if (kIOSystemLoadAdvisoryLevelGreat == gtl) {
        return "Great";
    } else if (kIOSystemLoadAdvisoryLevelOK == gtl) {
        return "OK";
    } else if (kIOSystemLoadAdvisoryLevelBad == gtl) {
        return "Bad";
    }
    return "(Unknown system load level)";
}

static void show_systemload(void)
{
    CFDictionaryRef     detailed = NULL;
    CFNumberRef         n = NULL;
    int                 userLevel       = kIOSystemLoadAdvisoryLevelOK;
    int                 batteryLevel    = kIOSystemLoadAdvisoryLevelOK;
    int                 thermalLevel    = kIOSystemLoadAdvisoryLevelOK;
    int                 combinedLevel   = kIOSystemLoadAdvisoryLevelOK;

    print_pretty_date(CFAbsoluteTimeGetCurrent(), true);

    combinedLevel = IOGetSystemLoadAdvisory();
    if (0 == combinedLevel) {
        printf("- Internal error: IOGetSystemLoadAdvisory returns error value %d\n", combinedLevel);
        return;
    }

    detailed = IOCopySystemLoadAdvisoryDetailed();
    if (!detailed) {
        printf("- Internal error: Invalid dictionary %p returned from IOCopySystemLoadAdvisoryDetailed.\n", detailed);
        return;
    }

    n = CFDictionaryGetValue(detailed, kIOSystemLoadAdvisoryUserLevelKey);
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &userLevel); 
    }
    n = CFDictionaryGetValue(detailed, kIOSystemLoadAdvisoryBatteryLevelKey);
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &batteryLevel); 
    }
    n = CFDictionaryGetValue(detailed, kIOSystemLoadAdvisoryThermalLevelKey);
    if (n) {
        CFNumberGetValue(n, kCFNumberIntType, &thermalLevel); 
    }    
    CFRelease(detailed);
    
    printf("  combined level = %s\n",  stringForGTLevel(combinedLevel));
    printf("  - user level = %s\n",    stringForGTLevel(userLevel));
    printf("  - battery level = %s\n", stringForGTLevel(batteryLevel));
    printf("  - thermal level = %s\n", stringForGTLevel(thermalLevel));
    fflush(stdout);

    return;
}

static void log_systemload(void)
{
    int                 token = 0;
    uint32_t            notify_status = 0;
    
    show_systemload();

    notify_status = notify_register_dispatch(
            kIOSystemLoadAdvisoryNotifyName, 
            &token, 
            dispatch_get_main_queue(), 
            ^(int t) {
               show_systemload();
             });

    if (NOTIFY_STATUS_OK != notify_status)
    {
        printf("LogSystemLoad: notify_register_dispatch returns error %d; Exiting.\n", notify_status);
        return;
    }

    dispatch_main();
}

static void log_useractivity()
{
    int                 notifyToken = 0;
    int                 status;
    uint64_t            newVal=0xabcd;

    status = notify_register_check(kIOUserActivityNotifyName, &notifyToken);
    if (NOTIFY_STATUS_OK == status)
    {
        notify_get_state(notifyToken, &newVal);
        notify_cancel(notifyToken);

        print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
        if (newVal == kIOUserIsIdle)
           printf("User is idle on the system\n");
        else
           printf("User is active on the system\n");
    }

}

static void log_useractivitylog()
{
    int                 token = 0;
    uint32_t            notify_status = 0;
    int                 notifyToken = 0;
    int                 status;
    uint64_t            newVal=0xabcd;

    status = notify_register_check(kIOUserActivityNotifyName, &notifyToken);
    if (NOTIFY_STATUS_OK == status)
    {
        notify_get_state(notifyToken, &newVal);
        notify_cancel(notifyToken);

        print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
        if (newVal == kIOUserIsIdle)
           printf("User is idle on the system\n");
        else
           printf("User is active on the system\n");
    }

    notify_status = notify_register_dispatch(
            kIOUserActivityNotifyName, 
            &token, 
            dispatch_get_main_queue(), 
            ^(int t) {
               uint64_t newVal;
               notify_get_state(t, &newVal);

               print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
               if (newVal == kIOUserIsIdle)
                    printf("User is idle on the system\n");
               else
                    printf("User is active on the system\n");
             });

    if (NOTIFY_STATUS_OK != notify_status)
    {
        printf("LogSystemLoad: notify_register_dispatch returns error %d; Exiting.\n", notify_status);
        return;
    }



    dispatch_main();

}

/******************************************************************************/

static void log_ps_change_handler(void *info)
{
    int which = (uintptr_t)info;
    
    if (!(which & kShowColumns)) {
        print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
        printf("IOPSNotificationCreateRunLoopSource\n");
    }
    show_power_sources(which);
}

static int install_listen_for_power_sources(uintptr_t which)
{    
    CFRunLoopSourceRef          rls = NULL;

    /* Log changes to all attached power sources */   
    rls = IOPSNotificationCreateRunLoopSource(log_ps_change_handler, (void *)which);
    if(!rls) {
        printf("Error - IOPSNotificationCreateRunLoopSource failure.\n");
        return kParseInternalError;
    } else {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
        CFRelease(rls);

        printf("pmset is in logging mode now. Hit ctrl-c to exit.\n");

        if (kShowColumns & which)
        {
            printf("%10s\t%15s\t%10s\t%10s\t%20s\n", 
                "Elapsed", "TimeRemaining", "Charge", "Charging", "Timestamp");
        }

        // and show initial power source state:
        log_ps_change_handler((void *)which);
    }
    
    if (!(which & kShowColumns)) {
        int tokenA, tokenB, tokenC;
        
        notify_register_dispatch(kIOPSNotifyLowBattery,
                                &tokenA, dispatch_get_main_queue(),
                                ^(int t) {
                                    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
                                    printf("%s\n", kIOPSNotifyLowBattery);
                                });

        notify_register_dispatch(kIOPSNotifyTimeRemaining,
                                 &tokenB, dispatch_get_main_queue(),
                                 ^(int t) {
                                     print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
                                     printf("%s\n", kIOPSNotifyTimeRemaining);
                                 });
        notify_register_dispatch(kIOPSNotifyPowerSource,
                                 &tokenC, dispatch_get_main_queue(),
                                 ^(int t) {
                                     print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
                                     printf("%s\n", kIOPSNotifyPowerSource);
                                 });
        notify_register_dispatch(kIOPSNotifyAttach,
                                 &tokenC, dispatch_get_main_queue(),
                                 ^(int t) {
                                     print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
                                     printf("%s\n", kIOPSNotifyAttach);
                                 });
        notify_register_dispatch(kIOPSNotifyAnyPowerSource,
                                 &tokenC, dispatch_get_main_queue(),
                                 ^(int t) {
                                     print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
                                     printf("%s\n", kIOPSNotifyAnyPowerSource);
                                 });
    }
    
    return 0;
}




/******************************************************************************/
/*                                                                            */
/*     RAW PS LOGGING                                                         */
/*                                                                            */
/******************************************************************************/

static CFAbsoluteTime getAbsoluteTimeForProperty(CFDictionaryRef d, CFStringRef key)
{
    CFNumberRef     secSince1970 = NULL;
    uint32_t        secs = 0;
    CFAbsoluteTime  return_val = 0.0;
    
    if (d && key)
    {
        secSince1970 = CFDictionaryGetValue(d, key);
        if (secSince1970) {
            CFNumberGetValue(secSince1970, kCFNumberIntType, &secs);
            return_val = (CFAbsoluteTime)secs - kCFAbsoluteTimeIntervalSince1970;
            CFRelease(secSince1970);
        }
    }
    
    return return_val;
}

static void print_raw_battery_state(io_registry_entry_t b_reg)
{
    CFDateFormatterRef      date_format;
    CFTimeZoneRef           tz;
    CFStringRef             time_date;
    CFLocaleRef             loc;
    char                    _date[60];

    CFStringRef             failure;
    char                    _failure[200];
    CFBooleanRef            boo;
    CFNumberRef             n;
    int                     tmp;
    int                     cur_cap = -1;
    int                     max_cap = -1;
    int                     design_cap = -1;
    int                     cur_cycles = -1;
    CFMutableDictionaryRef  prop = NULL;
    IOReturn                ret;
    
    loc = CFLocaleCopyCurrent();
    date_format = CFDateFormatterCreate(kCFAllocatorDefault, loc,
                                        kCFDateFormatterShortStyle, kCFDateFormatterLongStyle);
    CFRelease(loc);
    tz = CFTimeZoneCopySystem();
    CFDateFormatterSetProperty(date_format, kCFDateFormatterTimeZone, tz);
    CFRelease(tz);
    CFDateFormatterSetFormat(date_format, CFSTR(kDateAndTimeFormat));
    time_date = CFDateFormatterCreateStringWithAbsoluteTime(kCFAllocatorDefault,
                                                            date_format, CFAbsoluteTimeGetCurrent());

    if(time_date)
    {
        CFStringGetCString(time_date, _date, 60, kCFStringEncodingMacRoman);
        printf("%s\n", _date); fflush(stdout);
        CFRelease(time_date);
    }

    if (IO_OBJECT_NULL == b_reg) {
        b_reg = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("AppleSmartBattery"));
    }
    
    ret = IORegistryEntryCreateCFProperties(b_reg, &prop, 0, 0);
    if( (kIOReturnSuccess != ret) || (NULL == prop) )
    {
        printf("Couldn't read battery status; error = 0%08x\n", ret);
        return;
    }
    
    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalConnectedKey));
    printf(" %s; ", (kCFBooleanTrue == boo) ? "AC" : "No AC");

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryInstalledKey));
    printf("%s", (kCFBooleanTrue == boo) ? "" : "No battery; ");

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSIsChargingKey));
    printf("%s; ", (kCFBooleanTrue == boo) ? "Charging" : "Not Charging");

    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCurrentCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &cur_cap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &max_cap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSDesignCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &design_cap);
    }
    
    if( (-1 != cur_cap) && (-1 != max_cap) )
    {
        if (0 == max_cap) {
            printf("NaN%%; Cap=%d: FCC=%d; Design=%d; ", cur_cap, max_cap, design_cap);
        } else {
            printf("%d%%; Cap=%d: FCC=%d; Design=%d; ", (cur_cap*100)/max_cap, cur_cap, max_cap, design_cap);
        }
    }
    
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSTimeRemainingKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &tmp);
        printf("Time=%d:%02d; ", tmp/60, tmp%60);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSAmperageKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &tmp);
        printf("%dmA; ", tmp);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCycleCountKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &cur_cycles);
    }

    printf("Cycles=%d", cur_cycles);
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSLocationKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &tmp);
        printf("; Location=%d; ", tmp);
    }
    
    failure = CFDictionaryGetValue(prop, CFSTR("ErrorCondition"));
    if(failure) {
        CFStringGetCString(failure, _failure, 200, kCFStringEncodingMacRoman);
        printf("\n Failure=\"%s\"", _failure);
    }

#ifndef kIOBatteryBootPathKey
#define kIOBatteryBootPathKey             "BootPathUpdated"
#define kIOBatteryFullPathKey             "FullPathUpdated"
#define kIOBatterykUserVisPathKey          "UserVisiblePathUpdated"
#endif
    
    printf("\n");
    CFAbsoluteTime  since = 0.0;
    CFStringRef     since_string = NULL;
    char            since_str[65];
    since = getAbsoluteTimeForProperty(prop, CFSTR(kIOBatteryBootPathKey));
    if (0.0 != since) {
        since_string = CFDateFormatterCreateStringWithAbsoluteTime(0, date_format, since);
        CFStringGetCString(since_string, since_str, sizeof(since_str), kCFStringEncodingUTF8);
        printf(" Polled boot=%s", since_str);
        CFRelease(since_string);
    }
    since = getAbsoluteTimeForProperty(prop, CFSTR(kIOBatteryFullPathKey));
    if (0.0 != since) {
        since_string = CFDateFormatterCreateStringWithAbsoluteTime(0, date_format, since);
        CFStringGetCString(since_string, since_str, sizeof(since_str), kCFStringEncodingUTF8);
        printf("; Full=%s", since_str);
        CFRelease(since_string);
    }
    since = getAbsoluteTimeForProperty(prop, CFSTR(kIOBatterykUserVisPathKey));
    if (0.0 != since) {
        since_string = CFDateFormatterCreateStringWithAbsoluteTime(0, date_format, since);
        CFStringGetCString(since_string, since_str, sizeof(since_str), kCFStringEncodingUTF8);
        printf("; User visible=%s", since_str);
        CFRelease(since_string);
    }
    printf("\n"); fflush(stdout);

    CFRelease(date_format);
    CFRelease(prop);
    return;
}


static void log_raw_battery_match(
    void *refcon, 
    io_iterator_t b_iter) 
{
    IONotificationPortRef       notify = *((IONotificationPortRef *)refcon);
    io_registry_entry_t         battery;
    io_object_t                 notification_ref;
    int                         found = false;
    
    while ((battery = (io_registry_entry_t)IOIteratorNext(b_iter)))
    {
        found = true;        
        printf(" * Battery matched at registry = %d\n", (int32_t)battery);

        print_raw_battery_state(battery);
        
        // And install an interest notification on it
        IOServiceAddInterestNotification(notify, battery, 
                            kIOGeneralInterest, log_raw_battery_interest,
                            NULL, &notification_ref);

        IOObjectRelease(battery);
    }
    
    if(!found) {
        printf("  (no batteries found; waiting)\n");
    }    
}


static void log_raw_battery_interest(
    void *refcon, 
    io_service_t batt, 
    natural_t messageType, 
    void *messageArgument)
{
    if(kIOPMMessageBatteryStatusHasChanged == messageType)
    {
        print_raw_battery_state((io_registry_entry_t)batt);

    }
    return;
}

static int log_raw_power_source_changes(void)
{
    IONotificationPortRef       notify_port = 0;
    io_iterator_t               battery_iter = 0;
    CFRunLoopSourceRef          rlser = 0;
    IOReturn                    ret;

    printf("pmset is in RAW logging mode now. Hit ctrl-c to exit.\n");

    notify_port = IONotificationPortCreate(0);
    rlser = IONotificationPortGetRunLoopSource(notify_port);
    if(!rlser) return 0;
    CFRunLoopAddSource(CFRunLoopGetCurrent(), rlser, kCFRunLoopDefaultMode);


    ret = IOServiceAddMatchingNotification(
                              notify_port,
                              kIOFirstMatchNotification,  
                              IOServiceMatching("IOPMPowerSource"), 
                              log_raw_battery_match, 
                              (void *)&notify_port,  
                              &battery_iter);
    if(KERN_SUCCESS != ret){
         printf("!!Error prevented matching notifications; err = 0x%08x\n", ret);
    }

    // Install notifications on existing instances.
    log_raw_battery_match((void *)&notify_port, battery_iter);
        
    CFRunLoopRun();
    
    // should never return from CFRunLoopRun
    return 0;
}


static void show_systempower_notify(void)
{
    IOPMCapabilityBits b;
    char stateDescriptionStr[100];

    b = IOPMConnectionGetSystemCapabilities();
    
    IOPMGetCapabilitiesDescription(stateDescriptionStr, sizeof(stateDescriptionStr), b);

    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
    printf("com.apple.powermanagement.systempowerstate=%s\n", stateDescriptionStr);

    return;
}

static void install_listen_for_notify_system_power(void)
{
    uint32_t        status;
    int             token;
    
    printf("Logging: com.apple.powermanagement.systempowerstate\n");

    status = notify_register_dispatch(
            kIOSystemLoadAdvisoryNotifyName, 
            &token, 
            dispatch_get_main_queue(), 
            ^(int t) {
               show_systempower_notify();
             });

    if (NOTIFY_STATUS_OK != status) {
        fprintf(stderr, "Registration failed for \"%s\" with (%u)\n",
                        kIOPMSystemPowerStateNotify, status);
    }                        

}

/*************************************************************************/

void myPMConnectionHandler(
    void *param, 
    IOPMConnection                      connection,
    IOPMConnectionMessageToken          token, 
    IOPMSystemPowerStateCapabilities    capabilities)
{
#if TARGET_OS_EMBEDDED
    return;
#else
    char                        stateDescriptionStr[100];
    IOReturn                    ret;

    printf("\n");
    print_pretty_date(CFAbsoluteTimeGetCurrent(), true);
    IOPMGetCapabilitiesDescription(stateDescriptionStr, sizeof(stateDescriptionStr), (uint64_t)capabilities);

    printf("PMConnection: %s caps:0x%x\n", stateDescriptionStr, capabilities);

    IOPMSystemPowerStateCapabilities    fromAPI = IOPMConnectionGetSystemCapabilities();
    if (capabilities != fromAPI)
    {
        printf("PMConnection: API IOPMConnectionGetSystemCapabilities() returns 0x%04x, should have returned 0x%04x\n", (uint32_t)fromAPI, (uint32_t)capabilities);
    }
    
    if (!(kIOPMCapabilityCPU & capabilities))
    {
        printSleepAndWakeReasons(kJustPrintSleep);
    } else {
        printSleepAndWakeReasons(kJustPrintWake);
    }
    
    ret = IOPMConnectionAcknowledgeEvent(connection, token);    
    if (kIOReturnSuccess != ret)
    {
        printf("\t-> PM Connection acknowledgement error 0x%08x\n", ret);    
    }
#endif /* TARGET_OS_EMBEDDED */
}

static void install_listen_PM_connection(void)
{
#if TARGET_OS_EMBEDDED
    return;
#else
    IOPMConnection      myConnection;
    IOReturn            ret;

    printf("Logging IOPMConnection\n");
    
    ret = IOPMConnectionCreate(
                        CFSTR("SleepWakeLogTool"),
                        kIOPMEarlyWakeNotification
                            | kIOPMCapabilityCPU
                            | kIOPMCapabilityDisk 
                            | kIOPMCapabilityNetwork
                            | kIOPMCapabilityAudio 
                            | kIOPMCapabilityVideo
                            | kIOPMCapabilityPushServiceTask
                            | kIOPMCapabilityBackgroundTask
                            | kIOPMCapabilitySilentRunning,
                        &myConnection);

    if (kIOReturnSuccess != ret) {
        printf("IOPMConnectionCreate Create: Error 0x%08x\n", ret);
        return;
    }

    ret = IOPMConnectionSetNotification(
                        myConnection, NULL,
                        (IOPMEventHandlerType)myPMConnectionHandler);

    if (kIOReturnSuccess != ret) {
        printf("IOPMConnectionCreate SetNotification: Error 0x%08x\n", ret);
        return;
    }

    ret = IOPMConnectionScheduleWithRunLoop(
                        myConnection, CFRunLoopGetCurrent(),
                        kCFRunLoopDefaultMode);

    if (kIOReturnSuccess != ret) {
        printf("IOPMConnection ScheduleWithRunloop: Error 0x%08x\n", ret);
        return;
    }
#endif /* TARGET_OS_EMBEDDED */
}

static void install_listen_com_apple_powermanagement_sleepservices_notify(void)
{
    int     token;
    int     status;
    
    status = notify_register_dispatch(kIOPMSleepServiceActiveNotifyName, &token, dispatch_get_main_queue(), 
                                      ^(int t) {
                                          if (IOPMGetSleepServicesActive()) {
                                              printf("SleepServices are: ON\n");
                                          } else {
                                              printf("SleepServices are: OFF\n");
                                          }
                                      });

    
    if (NOTIFY_STATUS_OK != status) {
        fprintf(stderr, "Registration failed for \"%s\" with (%u)\n",
                kIOPMSleepServiceActiveNotifyName, status);
    }                        

    if (IOPMGetSleepServicesActive()) {
        printf("SleepServices are: ON\n");
    } else {
        printf("SleepServices are: OFF\n");
    }
}

static void install_listen_IORegisterForSystemPower(void)
{
    io_object_t                 root_notifier = MACH_PORT_NULL;
    IONotificationPortRef       notify = NULL;
    
    printf("Logging IORegisterForSystemPower sleep/wake messages\n");

    /* Log sleep/wake messages */
    gPMAckPort = IORegisterForSystemPower (
                        (void *)kLogSleepEvents, &notify, 
                        sleepWakeCallback, &root_notifier);

   if( notify && (MACH_PORT_NULL != gPMAckPort) )
   {
        CFRunLoopAddSource(CFRunLoopGetCurrent(),
                    IONotificationPortGetRunLoopSource(notify),
                    kCFRunLoopDefaultMode);
    }    

    return;
}

static void listen_for_everything(void)
{
    install_listen_for_power_sources(kApplyToBattery | kApplyToUPS);
    install_listen_for_notify_system_power();
    install_listen_PM_connection();
    install_listen_IORegisterForSystemPower();
    install_listen_com_apple_powermanagement_sleepservices_notify();
    
    CFRunLoopRun();    
    // should never return from CFRunLoopRun
}

static void log_thermal_events(void)
{
    int             powerConstraintNotifyToken = 0;
    int             cpuPowerNotifyToken = 0;

    uint32_t        status;
    

    status = notify_register_dispatch(
            kIOPMCPUPowerNotificationKey, 
            &cpuPowerNotifyToken, 
            dispatch_get_main_queue(), 
            ^(int t) {
                show_thermal_cpu_power_level();
             });


    if (NOTIFY_STATUS_OK != status) {
        fprintf(stderr, "Registration failed for \"%s\" with (%u)\n",
                        kIOPMCPUPowerNotificationKey, status);
    }                        


    status = notify_register_dispatch(
            kIOPMThermalWarningNotificationKey, 
            &powerConstraintNotifyToken, 
            dispatch_get_main_queue(), 
            ^(int t) {
                show_thermal_warning_level();
             });


    if (NOTIFY_STATUS_OK != status)
    {
        fprintf(stderr, "Registration failed for \"%s\" with (%u)\n",
                        kIOPMThermalWarningNotificationKey, status);
    }

    show_thermal_warning_level();
    show_thermal_cpu_power_level();
    
    dispatch_main();
}


static void show_thermal_warning_level(void)
{
    uint32_t                warn = -1;
    IOReturn                ret;
    
    ret = IOPMGetThermalWarningLevel(&warn);

    if (kIOReturnNotFound == ret) {
        printf("Note: No thermal warning level has been recorded\n");
        return;
    }

    if (kIOReturnSuccess != ret) 
    {
        printf("Error: No thermal warning level with error code 0x%08x\n", ret);
        return;
    }

    // successfully found warning level
    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
    printf("Thermal Warning Level = %d\n", warn);
    return;
}


static void show_thermal_cpu_power_level(void)
{
    CFDictionaryRef         cpuStatus;
    CFStringRef             *keys = NULL;
    CFNumberRef             *vals = NULL;
    int                     count = 0;
    int                     i;
    IOReturn                ret;

    ret = IOPMCopyCPUPowerStatus(&cpuStatus);

    if (kIOReturnNotFound == ret) {
        printf("Note: No CPU power status has been recorded\n");
        return;
    }

    if (!cpuStatus || (kIOReturnSuccess != ret)) 
    {
        printf("Error: No CPU power status with error code 0x%08x\n", ret);
        return;
    }

    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
    fprintf(stderr, "CPU Power notify\n"), fflush(stderr);        
    
    count = CFDictionaryGetCount(cpuStatus);
    keys = (CFStringRef *)malloc(count*sizeof(CFStringRef));
    vals = (CFNumberRef *)malloc(count*sizeof(CFNumberRef));
    if (!keys||!vals) 
        goto exit;

    CFDictionaryGetKeysAndValues(cpuStatus, 
                    (const void **)keys, (const void **)vals);

    for(i=0; i<count; i++) {
        char strbuf[125];
        int  valint;
        
        CFStringGetCString(keys[i], strbuf, 125, kCFStringEncodingUTF8);
        CFNumberGetValue(vals[i], kCFNumberIntType, &valint);
        printf("\t%s \t= %d\n", strbuf, valint);
    }


exit:    
    if (keys)
        free(keys);
    if (vals)
        free(vals);
    if (cpuStatus)
        CFRelease(cpuStatus);

}

static void show_power_adapter(void)
{
    CFDictionaryRef     acInfo = NULL;
    CFNumberRef         valNum = NULL;
    int                 val;
    
    acInfo = IOPSCopyExternalPowerAdapterDetails();
    if (!acInfo) {
        printf("No adapter attached.\n");
        return;
    }
       
    valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterWattsKey));
    if (valNum) {
        CFNumberGetValue(valNum, kCFNumberIntType, &val);
        printf(" Wattage = %dW\n", val); 
    }

    valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterSourceKey));
    if (valNum) {
        // New format
        CFNumberGetValue(valNum, kCFNumberIntType, &val);
        printf(" SourceID = 0x%04x\n", val); 

        valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterCurrentKey));
        if (valNum) {
            CFNumberGetValue(valNum, kCFNumberIntType, &val);
            printf(" Current = %dmA\n", val); 
        }

        if ((valNum = CFDictionaryGetValue(acInfo, CFSTR("Voltage")))) {
            CFNumberGetValue(valNum, kCFNumberIntType, &val);
            printf(" Voltage = %dmW\n", val);
        }
    }
    else {
        valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterRevisionKey));
        if (valNum) {
            CFNumberGetValue(valNum, kCFNumberIntType, &val);
            printf(" Revision = 0x%04x\n", val); 
        }
    }

    // Legacy format
    valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterIDKey));
    if (valNum) {
        CFNumberGetValue(valNum, kCFNumberIntType, &val);
        printf(" AdapterID = 0x%04x\n", val); 
    }

    valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterFamilyKey));
    if (valNum) {
        CFNumberGetValue(valNum, kCFNumberIntType, &val);
        printf(" Family Code = 0x%04x\n", val); 
    }

    valNum = CFDictionaryGetValue(acInfo, CFSTR(kIOPSPowerAdapterSerialNumberKey));
    if (valNum) {
        CFNumberGetValue(valNum, kCFNumberIntType, &val);
        printf(" Serial Number = 0x%08x\n", val); 
    }
    CFRelease(acInfo);
}


/******************************************************************************/
/*                                                                            */
/*     BORING SETTINGS & PARSING                                              */
/*                                                                            */
/******************************************************************************/


static int checkAndSetIntValue(
    char *valstr, 
    CFStringRef settingKey, 
    int apply,
    int isOnOffSetting, 
    int multiplier,
    CFMutableDictionaryRef ac, 
    CFMutableDictionaryRef batt, 
    CFMutableDictionaryRef ups)
{
    CFNumberRef     cfnum;
    char            *endptr = NULL;
    long            val;
    int32_t         val32;

    if(!valstr) return -1;

    val = strtol(valstr, &endptr, 0);

    if(0 != *endptr)
    {
        // the string contained some non-numerical characters - bail
        return -1;
    }
    
    // for on/off settings, turn any non-zero number into a 1
    if(isOnOffSetting) {
        val = (val?1:0);
    } else {
        // Numerical values may have multipliers (i.e. x1000 for sec -> msec)
        if(0 != multiplier) val *= multiplier;
    }
    // negative number? reject it
    if(val < 0) return -1;

    val32 = (int32_t)val;
    cfnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &val32);
    if(!cfnum) return -1;
    if(apply & kApplyToBattery)
        CFDictionarySetValue(batt, settingKey, cfnum);
    if(apply & kApplyToCharger)
        CFDictionarySetValue(ac, settingKey, cfnum);
    if(apply & kApplyToUPS)
        CFDictionarySetValue(ups, settingKey, cfnum);
    CFRelease(cfnum);
    return 0;
}

static int checkAndSetStrValue(char *valstr, CFStringRef settingKey, int apply,
                CFMutableDictionaryRef ac, CFMutableDictionaryRef batt, CFMutableDictionaryRef ups)
{
    CFStringRef     cfstr;

    if(!valstr) return -1;

    cfstr = CFStringCreateWithCString(kCFAllocatorDefault, 
                        valstr, kCFStringEncodingMacRoman);
    if(!cfstr) return -1;
    if(apply & kApplyToBattery)
        CFDictionarySetValue(batt, settingKey, cfstr);
    if(apply & kApplyToCharger)
        CFDictionarySetValue(ac, settingKey, cfstr);
    if(apply & kApplyToUPS)
        CFDictionarySetValue(ups, settingKey, cfstr);
    CFRelease(cfstr);
    return 0;
}


static int setUPSValue(char *valstr, 
    CFStringRef    whichUPS, 
    CFStringRef settingKey, 
    int apply, 
    CFMutableDictionaryRef thresholds)
{
    CFMutableDictionaryRef ups_setting = NULL;
    CFDictionaryRef     tmp_ups_setting = NULL;
    CFNumberRef     cfnum = NULL;
    CFBooleanRef    on_off = kCFBooleanTrue;
    char            *endptr = NULL;
    long            val;
    int32_t         val32;

    if(!valstr) return -1;

    val = strtol(valstr, &endptr, 10);

    if(0 != *endptr)
    {
        // the string contained some non-numerical characters - bail
        return -1;
    }

    if(-1 == val)
    {
        on_off = kCFBooleanFalse;
    }

    // negative number? reject it
    if(val < 0) val = 0;
    
    // if this should be a percentage, cap the value at 100%
    if(kCFCompareEqualTo == CFStringCompare(settingKey, CFSTR(kIOUPSShutdownAtLevelKey), 0))
    {
        if(val > 100) val = 100;
    };

    // bail if -u or -a hasn't been specified:
    if(!(apply & kApplyToUPS)) return -1;
    
    // Create the nested dictionaries of UPS settings
    tmp_ups_setting = CFDictionaryGetValue(thresholds, settingKey);
    ups_setting = CFDictionaryCreateMutableCopy(0, 0, tmp_ups_setting); 
    if(!ups_setting)
    {
        ups_setting = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    val32 = (int32_t)val;
    cfnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &val32);
    
    if(kCFBooleanFalse == on_off) {
        // If user is turning this setting off, then preserve the existing value in there.
        // via CFDictionaryAddValue
        CFDictionaryAddValue(ups_setting, CFSTR(kIOUPSShutdownLevelValueKey), cfnum);
    } else {
        // If user is providing a new value for this setting, overwrite the existing value.
        CFDictionarySetValue(ups_setting, CFSTR(kIOUPSShutdownLevelValueKey), cfnum);
    }
    CFRelease(cfnum);    
    CFDictionarySetValue(ups_setting, CFSTR(kIOUPSShutdownLevelEnabledKey), on_off);

    CFDictionarySetValue(thresholds, settingKey, ups_setting);
    CFRelease(ups_setting);
    return 0;
}


//  pmset repeat cancel
//  pmset repeat <type> <days of week> <time> [<type> <days of week> <time>]\n");
static int parseRepeatingEvent(
    char                        **argv,
    int                         *num_args_parsed,
    CFMutableDictionaryRef      local_repeating_event, 
    bool                        *local_cancel_repeating)
{
    CFDateFormatterRef          formatter = 0;
    CFTimeZoneRef               tz = 0;
    CFStringRef                 cf_str_date = 0;
    int                         i = 0;
    int                         j = 0;
    int                         str_len = 0;
    int                         days_mask = 0;
    int                         on_off = 0;
    IOReturn                    ret = kParseInternalError;

    CFStringRef                 the_type = 0;
    CFNumberRef                 the_days = 0;
    CFDateRef                   cf_date = 0;
    CFGregorianDate             cf_greg_date;
    int                         event_time = 0;
    CFNumberRef                 the_time = 0;       // in minutes from midnight
    CFMutableDictionaryRef      one_repeating_event = 0;

    formatter = CFDateFormatterCreate(kCFAllocatorDefault, CFLocaleGetSystem(),
        kCFDateFormatterShortStyle, kCFDateFormatterMediumStyle);
    if(!formatter) {
        ret = kParseInternalError;
        goto exit;
    }
    tz = CFTimeZoneCopySystem();
    if(!tz) { 
        ret = kParseInternalError;
        goto exit;
    }
    CFDateFormatterSetFormat(formatter, CFSTR(kTimeFormat));
    if(!argv[i]) {
        ret = kParseBadArgs;
        goto exit;
    }
    // cancel ALL repeating events
    if(0 == strcmp(argv[i], ARG_CANCEL) ) {

        *local_cancel_repeating = true;
        i++;
        ret = kParseSuccess;
        goto exit;
    }
    
    while(argv[i])
    {    
        string_tolower(argv[i], argv[i]);
        
        // type
        if(0 == strcmp(argv[i], ARG_SLEEP))
        {
            on_off = 0;
            the_type = CFSTR(kIOPMAutoSleep);
        } else if(0 == strcmp(argv[i], ARG_SHUTDOWN))
        {
            on_off = 0;
            the_type =  CFSTR(kIOPMAutoShutdown);
        } else if(0 == strcmp(argv[i], ARG_RESTART))
        {
            on_off = 0;
            the_type =  CFSTR(kIOPMAutoRestart);
        } else if(0 == strcmp(argv[i], ARG_WAKE))
        {
            on_off = 1;
            the_type =  CFSTR(kIOPMAutoWake);
        } else if(0 == strcmp(argv[i], ARG_POWERON))
        {
            on_off = 1;
            the_type =  CFSTR(kIOPMAutoPowerOn);
        } else if(0 == strcmp(argv[i], ARG_WAKEORPOWERON))
        {
            on_off = 1;
            the_type =  CFSTR(kIOPMAutoWakeOrPowerOn);
        } else {
            printf("Error: Unspecified scheduled event type\n");
            ret = kParseBadArgs;
            goto bail;
        }
    
        i++;
        
        // days of week
        // Expect argv[i] to be a NULL terminated string with a subset of: MTWRFSU
        // indicating the days of the week to schedule repeating wakeup
        // TODO: accept M-F style ranges
        if (!argv[i] || !argv[i+1]) {
            ret = kParseBadArgs;
            goto bail;
        }

        string_tolower(argv[i], argv[i]);
        
		days_mask = 0;
        str_len = strlen(argv[i]);
        for(j=0; j<str_len; j++)
        {
            if('m' == argv[i][j]) {
                days_mask |= kIOPMMonday;
            } else if('t' == argv[i][j]) {
                days_mask |= kIOPMTuesday;
            } else if('w' == argv[i][j]) {
                days_mask |= kIOPMWednesday;
            } else if('r' == argv[i][j]) {
                days_mask |= kIOPMThursday;
            } else if('f' == argv[i][j]) {
                days_mask |= kIOPMFriday;
            } else if('s' == argv[i][j]) {
                days_mask |= kIOPMSaturday;
            } else if('u' == argv[i][j]) {
                days_mask |= kIOPMSunday;
            }
        }
        if(0 == days_mask) {
            // something went awry; we expect a non-zero days mask.
            ret = kParseBadArgs;
            goto bail;
        }

        i++;
        string_tolower(argv[i], argv[i]);
    
        cf_str_date = CFStringCreateWithCString(kCFAllocatorDefault, 
                        argv[i], kCFStringEncodingMacRoman);
        if (!cf_str_date) {
            ret = kParseInternalError;
            goto bail;
        }
        cf_date = CFDateFormatterCreateDateFromString(0, formatter, cf_str_date, 0);
        CFRelease(cf_str_date);
        if (!cf_date) {
            ret = kParseBadArgs;
            goto bail;
        }
        cf_greg_date = CFAbsoluteTimeGetGregorianDate(CFDateGetAbsoluteTime(cf_date), tz);
        event_time = cf_greg_date.hour*60 + cf_greg_date.minute;
        the_time = CFNumberCreate(0, kCFNumberIntType, &event_time);
        
        i++;

        the_days = CFNumberCreate(0, kCFNumberIntType, &days_mask);

        // check for existence of the_days, the_time, the_type
        // if this was a validly formatted dictionary, pack the repeating dict appropriately.
        if( isA_CFNumber(the_days) && isA_CFString(the_type) && isA_CFNumber(the_time) )
        {
            one_repeating_event = CFDictionaryCreateMutable(0, 0, 
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if(one_repeating_event) 
            {
                CFDictionarySetValue(one_repeating_event, CFSTR(kIOPMPowerEventTypeKey), the_type);
                CFDictionarySetValue(one_repeating_event, CFSTR(kIOPMDaysOfWeekKey), the_days);
                CFDictionarySetValue(one_repeating_event, CFSTR(kIOPMPowerEventTimeKey), the_time);
                
                CFDictionarySetValue(local_repeating_event,
                     (on_off ? CFSTR(kIOPMRepeatingPowerOnKey):CFSTR(kIOPMRepeatingPowerOffKey)),
                                    one_repeating_event);
                CFRelease(one_repeating_event);            
            }
        }
        if (the_days)
            CFRelease(the_days);
        if (the_time)
            CFRelease(the_time);
        if (cf_date)
            CFRelease(cf_date);
        
    } // while loop
    
    ret = kParseSuccess;
    goto exit;

bail:
    fprintf(stderr, "Error: badly formatted repeating power event\n");
    fflush(stderr);

exit:
    if (the_type)
        CFRelease(the_type);
    if(num_args_parsed) 
        *num_args_parsed = i;
    if(tz) 
        CFRelease(tz);
    if(formatter) 
        CFRelease(formatter);
    return ret;
}


// pmset sched wake "4/27/04 1:00:00 PM" "Ethan Bold"
// pmset sched cancel sleep "4/27/04 1:00:00 PM" "MyAlarmClock"
// pmset sched cancel shutdown "4/27/04 1:00:00 PM"
static int parseScheduledEvent(
    char                        **argv,
    int                         *num_args_parsed,
    ScheduledEventReturnType    *local_scheduled_event, 
    bool                        *cancel_scheduled_event,
    bool                        is_relative_event)
{
    CFDateFormatterRef          formatter = 0;
    CFStringRef                 cf_str_date = 0;
    int                         i = 0;
    IOReturn                    ret = kParseInternalError;

    formatter = CFDateFormatterCreate(kCFAllocatorDefault, CFLocaleGetSystem(),
        kCFDateFormatterShortStyle, kCFDateFormatterMediumStyle);
    if(!formatter) 
        return kParseInternalError;

    *num_args_parsed = 0;

    // We manually set the format (as recommended by header comments)
    // to ensure it doesn't vary from release to release or from locale
    // to locale.
    CFDateFormatterSetFormat(formatter, CFSTR(kDateAndTimeFormat));
    if(!argv[i]) {
        ret = kParseInternalError;
        goto exit;
    }
    
    string_tolower(argv[i], argv[i]);
    
    // cancel
    if(!is_relative_event && (0 == strcmp(argv[i], ARG_CANCEL))) {
        char            *endptr = NULL;
        long            val;
        CFArrayRef      all_events = 0;
        CFDictionaryRef the_event = 0;

        *cancel_scheduled_event = true;
        i++;
        
        // See if the next field is an integer. If so, we cancel the event
        // indicated by the indices printed in "pmset -g sched"
        // If not, parse out the rest of the entry for a full-description
        // of the event to cancel.
        if(!argv[i]) {
            goto exit;
        }
        
        val = strtol(argv[i], &endptr, 10);
    
        if(0 == *endptr)
        {
            all_events = IOPMCopyScheduledPowerEvents();
            if(!all_events) {
                ret = kParseInternalError;
                goto exit;
            }
            if(val >= 0 && val < CFArrayGetCount(all_events)) {
                
                // the string was indeed a number
                the_event = isA_CFDictionary(CFArrayGetValueAtIndex(all_events, val));
                if(!the_event) {
                        ret = kParseInternalError;
                } else {
                    local_scheduled_event->when = CFRetain(
                        CFDictionaryGetValue(the_event, CFSTR(kIOPMPowerEventTimeKey)));
                    local_scheduled_event->who = CFRetain(
                        CFDictionaryGetValue(the_event, CFSTR(kIOPMPowerEventAppNameKey)));
                    local_scheduled_event->which = CFRetain(
                        CFDictionaryGetValue(the_event, CFSTR(kIOPMPowerEventTypeKey)));
                    ret = kParseSuccess;
                }                

                i++;
            } else {
                ret = kParseBadArgs;
            }
            
            CFRelease(all_events);
            goto exit;
        }
    }

    string_tolower(argv[i], argv[i]);

    // type
    if(0 == strcmp(argv[i], ARG_SLEEP))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoSleep, kCFStringEncodingMacRoman) : 0;
        i++;
    } else if(0 == strcmp(argv[i], ARG_SHUTDOWN))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoShutdown, kCFStringEncodingMacRoman) : 0;
        i++;
    } else if(0 == strcmp(argv[i], ARG_RESTART))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoRestart, kCFStringEncodingMacRoman) : 0;
        i++;
    } else if(0 == strcmp(argv[i], ARG_WAKE))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoWake, kCFStringEncodingMacRoman) :
            CFStringCreateWithCString(0, kIOPMAutoWakeRelativeSeconds, kCFStringEncodingMacRoman);
        i++;
    } else if(0 == strcmp(argv[i], ARG_POWERON))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoPowerOn, kCFStringEncodingMacRoman) : 0;
        i++;
    } else if(0 == strcmp(argv[i], ARG_WAKEORPOWERON))
    {
        local_scheduled_event->which = (!is_relative_event) ?
            CFStringCreateWithCString(0, kIOPMAutoWakeOrPowerOn, kCFStringEncodingMacRoman) : 0;
        i++;
    } else {
        printf("Error: Unspecified scheduled event type\n");
        ret = kParseBadArgs;
        goto exit;
    }
    
    if(0 == local_scheduled_event->which) {
        printf("Error: Unspecified scheduled event type (2)\n");
        ret = kParseBadArgs;
        goto exit;
    }

    // date & time
    if(argv[i]) {
        if (is_relative_event) {
            char    *endptr = NULL;
            long    secs;

            secs = strtol(argv[i], &endptr, 10);
            if ((0 != *endptr) || !secs) {
                ret = kParseBadArgs;
                goto exit;
            }

            local_scheduled_event->when = CFDateCreate(0, CFAbsoluteTimeGetCurrent() + secs);
            i++;
        } else {
            string_tolower(argv[i], argv[i]);    

            cf_str_date = CFStringCreateWithCString(kCFAllocatorDefault, 
                            argv[i], kCFStringEncodingMacRoman);
            if(!cf_str_date) {
                local_scheduled_event->when = NULL;
                ret = kParseInternalError;
                goto exit;
            }
            local_scheduled_event->when = 
                CFDateFormatterCreateDateFromString(
                    0,
                    formatter, 
                    cf_str_date, 
                    NULL);
            CFRelease(cf_str_date);
            i++;
        }
    } else {
        printf("Error: Badly formatted date\n");
        ret = kParseBadArgs;
        goto exit;
    }

    if(0 == local_scheduled_event->when) {
        printf("Error: Badly formatted date (2)\n");
        ret = kParseBadArgs;
        goto exit;
    }

    // Author. Please preserve case - do not lowercase - this argument.
    if(argv[i]) {
        local_scheduled_event->who = CFStringCreateWithCString(0, argv[i], kCFStringEncodingMacRoman);
        i++;
    } else {
        local_scheduled_event->who = 0;
    }
    
    ret = kParseSuccess;

exit:
    if(num_args_parsed) *num_args_parsed = i;

    if(formatter) CFRelease(formatter);
    
    if (kParseSuccess != ret) {
        exit(EX_SOFTWARE);
    }
    
    return ret;
}

static void string_tolower(char *lower_me, char *dst)
{
	int length = strlen(lower_me);
	int j = 0;

	for (j=0; j<length; j++)
	{
	    dst[j] = tolower(lower_me[j]);
	}
}

static void string_toupper(char *upper_me, char *dst)
{
	int length = strlen(upper_me);
	int j = 0;

	for (j=0; j<length; j++)
	{
	    dst[j] = toupper(upper_me[j]);
	}
}

/*
 * parseArgs - parse argv input stream into executable commands
 *      and returns executable commands.
 * INPUTS:
 *  int argc, 
 *  char* argv[], 
 * OUTPUTS:
 * If these pointers are specified on exit, that means parseArgs modified these settings
 * and they should be written out to persistent store by the caller.
 *  settings: Energy Saver settings
 *  modified_power_sources: which power sources the modified Energy Saver settings apply to
 *                          (only valid if settings is defined)
 *  active_profiles: Changes the active profile
 *  system_power_settings: system-wide settings, not tied to power source. like "disablesleep"
 *  ups_thresholds: UPS shutdown thresholds
 *  scheduled_event: Description of a one-time power event
 *  cancel_scheduled_event: true = cancel the scheduled event/false = schedule the event
 *                          (only valid if scheduled_event is defined)
 *  repeating_event: Description of a repeating power event
 *  cancel_repeating_event: true = cancel the repeating event/false = schedule the repeating event
 *                          (only valid if repeating_event is defined)
*/

static int parseArgs(int argc, 
    char* argv[], 
    CFDictionaryRef             *settings,
    int                         *modified_power_sources,
    bool                        *force_activate_settings,
    CFDictionaryRef             *active_profiles,
    CFDictionaryRef             *system_power_settings,
    CFDictionaryRef             *ups_thresholds,
    ScheduledEventReturnType    **scheduled_event,
    bool                        *cancel_scheduled_event,
    CFDictionaryRef             *repeating_event,
    bool                        *cancel_repeating_event,
    uint32_t                    *pmCmd)
{
    int                         i = 1;
    int                         apply = 0;
    int                         ret = kParseSuccess;
    int                         modified = 0;
    IOReturn                    kr;
    ScheduledEventReturnType    *local_scheduled_event = 0;
    bool                        local_cancel_event = false;
    CFMutableDictionaryRef      local_repeating_event = 0;
    bool                        local_cancel_repeating = false;
    CFDictionaryRef             tmp_profiles = 0;
    CFMutableDictionaryRef      local_profiles = 0;
    CFDictionaryRef             tmp_ups_settings = 0;
    CFMutableDictionaryRef      local_ups_settings = 0;
    CFDictionaryRef             tmp_settings = 0;
    CFMutableDictionaryRef      local_settings = 0;
    CFMutableDictionaryRef      local_system_power_settings = 0;
    CFDictionaryRef             tmp_battery = 0;
    CFMutableDictionaryRef      battery = 0;
    CFDictionaryRef             tmp_ac = 0;
    CFMutableDictionaryRef      ac = 0;
    CFDictionaryRef             tmp_ups = 0;
    CFMutableDictionaryRef      ups = 0;

    if(argc == 1) {
        return kParseBadArgs;
    }


/*
 * Check for any commands
 * Commands may not be combined with any other flags
 *
 */
    if(0 == strcmp(argv[1], ARG_TOUCH)) 
    {
        *pmCmd = kPMCommandTouch;
        return kIOReturnSuccess;
    } else if(0 == strcmp(argv[1], ARG_NOIDLE)) 
    {
        *pmCmd = kPMCommandNoIdle;
        return kIOReturnSuccess;
    } else if(0 == strcmp(argv[1], ARG_SLEEPNOW)) 
    {
        *pmCmd = kPMCommandSleepNow;
        return kIOReturnSuccess;
    } else if(0 == strcmp(argv[1], ARG_DISPLAYSLEEPNOW)) 
    {
        *pmCmd = kPMCommandDisplaySleepNow;
        return kIOReturnSuccess;
    } else if(0 == strcmp(argv[1], ARG_DEBUGTRIG)) 
    {
        *pmCmd = kPMCommandDebugTrig;
        return kIOReturnSuccess;
    } else if ((0 == strcmp(argv[1], ARG_RESETDISPLAYAMBIENTPARAMS))
              || (0 == strcmp(argv[1], ARG_RDAP)) )
    {
        #if PLATFORM_HAS_DISPLAYSERVICES
        {
            IOReturn ret = DisplayServicesResetAmbientLightAll();
            
            if (kIOReturnSuccess == ret) {
                printf("Success.\n");
            } else if (kIOReturnNoDevice == ret) {
                printf("Error: No supported displays found for pmset argument \"%s\"\n", argv[1]);
            } else {
                printf("Error: Failure 0%08x setting display ambient parameters.\n", ret);
            }
        }
        #else
        {
            printf("Error: this command isn't supported on this platform (no DisplayServices).\n");
            exit (EX_UNAVAILABLE);
        }
        #endif
    
        return kIOReturnSuccess;
    }


/***********
 * Setup mutable PM preferences
 ***********/
    tmp_settings = IOPMCopyActivePMPreferences();    
    if(!tmp_settings) {
        ret = kParseInternalError;
        goto exit;
    }
    local_settings = CFDictionaryCreateMutableCopy(0, 0, tmp_settings);
    CFRelease(tmp_settings);
    if(!local_settings) {
        ret = kParseInternalError;
        goto exit;
    }
    
    // Either battery or AC settings may not exist if the system doesn't support it.
    tmp_battery = isA_CFDictionary(CFDictionaryGetValue(local_settings, CFSTR(kIOPMBatteryPowerKey)));
    if(tmp_battery) {
        battery = CFDictionaryCreateMutableCopy(0, 0, tmp_battery);
        if(battery) {
            CFDictionarySetValue(local_settings, CFSTR(kIOPMBatteryPowerKey), battery);
            CFRelease(battery);
        }
    }
    tmp_ac = isA_CFDictionary(CFDictionaryGetValue(local_settings, CFSTR(kIOPMACPowerKey)));
    if(tmp_ac) {
        ac = CFDictionaryCreateMutableCopy(0, 0, tmp_ac);
        if(ac) {
            CFDictionarySetValue(local_settings, CFSTR(kIOPMACPowerKey), ac);
            CFRelease(ac);
        }
    }
    tmp_ups = isA_CFDictionary(CFDictionaryGetValue(local_settings, CFSTR(kIOPMUPSPowerKey)));
    if(tmp_ups) {
        ups = CFDictionaryCreateMutableCopy(0, 0, tmp_ups);
        if(ups) {
            CFDictionarySetValue(local_settings, CFSTR(kIOPMUPSPowerKey), ups);
            CFRelease(ups);
        }
    }
/***********
 * Setup mutable UPS thersholds
 ***********/
    tmp_ups_settings = IOPMCopyUPSShutdownLevels(CFSTR(kIOPMDefaultUPSThresholds));
    if(tmp_ups_settings) {
        local_ups_settings = CFDictionaryCreateMutableCopy(0, 0, tmp_ups_settings);
        CFRelease(tmp_ups_settings);
    }

/***********
 * Setup mutable Active profiles
 ***********/
    tmp_profiles = IOPMCopyActivePowerProfiles();
    if(tmp_profiles) {
        local_profiles = CFDictionaryCreateMutableCopy(0, 0, tmp_profiles);
        CFRelease(tmp_profiles);
    }
    
/************
 * Setup system power settings holder dictionary
 ************/
    local_system_power_settings = CFDictionaryCreateMutable(0, 0, 
                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    // Unless specified, apply changes to both battery and AC
    if(battery) apply |= kApplyToBattery;
    if(ac) apply |= kApplyToCharger;
    if(ups) apply |= kApplyToUPS;

    i=1;
    while(i < argc)
    {    
		string_tolower(argv[i], argv[i]);	// in place
	
        if( (argv[i][0] == '-')
            && ('1' != argv[i][1]) ) // don't try to process it as a abcg argument if it's a -1
                                     // the profiles parsing code below is expecting the -1
        {
        // Process -a/-b/-c/-g arguments
            apply = 0;
            switch (argv[i][1])
            {
                case 'a':
                    if(battery) apply |= kApplyToBattery;
                    if(ac) apply |= kApplyToCharger;
                    if(ups) apply |= kApplyToUPS;
                    break;
                case 'b':
                    if(battery) apply = kApplyToBattery;
                    break;
                case 'c':
                    if(ac) apply = kApplyToCharger;
                    break;
                case 'u':
                    if(ups) apply = kApplyToUPS;
                    break;
                case 'g':
                    // One of the "getters"
                    if('\0' != argv[i][2]) {
                        ret = kParseBadArgs;
                        goto exit;
                    }
                    i++;
                    
                    const char *canonical_arg = getCanonicalArgForSynonym(argv[i]);
                    if (!canonical_arg) {
                        ret = kParseBadArgs;
                        goto exit;
                    }

                    int getter_iterator;
                    bool handled_getter_arg = false;
                    for (getter_iterator=0; getter_iterator<the_getters_count; getter_iterator++) {
                        if (!strncmp(the_getters[getter_iterator].arg, canonical_arg, kMaxArgStringLength)) {
                            the_getters[getter_iterator].action(&argv[i+1]);
                            handled_getter_arg = true;
                            break;
                        }                        
                    }
                    if (!handled_getter_arg)
                    {
#if 0
                        // TODO: ARG_SEARCH is the only getter that's not described in the
                        // "the_getters" array. If possible, we should fit it in there too.
                        if (!strncmp(canonical_arg, ARG_SEARCH, kMaxArgStringLength) && argv[i+1] )
                        {                        
                            string_toupper(argv[i+1], argv[i+1]);
                            show_details_for_UUID(argv[i+1]);
                            goto exit;
                        }
#endif
                    }

                    // return immediately - don't handle any more setting arguments
                    ret = kParseSuccess;
                    goto exit;
                    break;
                default:
                    // bad!
                    ret = kParseBadArgs;
                    goto exit;
                    break;
            }
            
            i++;
        } else if( (0 == strncmp(argv[i], ARG_SCHEDULE, kMaxArgStringLength))
                || (0 == strncmp(argv[i], ARG_SCHED, kMaxArgStringLength)) )
        {
            // Process rest of input as a cancel/schedule power event
            int args_parsed;
        
            local_scheduled_event = scheduled_event_struct_create();
            if(!local_scheduled_event) {
                ret = kParseInternalError;
                goto exit;
            }
            i += 1;            
            ret = parseScheduledEvent(
                            &(argv[i]),
                            &args_parsed,
                            local_scheduled_event, 
                            &local_cancel_event,
                            false);
            if(kParseSuccess != ret)
            {
                goto exit;
            }

            i += args_parsed;
            modified |= kModSched;
        } else if(0 == strncmp(argv[i], ARG_RELATIVE, kMaxArgStringLength))
        {
            // Process rest of input as a relative power event
            int args_parsed;

            local_scheduled_event = scheduled_event_struct_create();
            if(!local_scheduled_event) {
                ret = kParseInternalError;
                goto exit;
            }
            i += 1;
            ret = parseScheduledEvent(
                            &(argv[i]),
                            &args_parsed,
                            local_scheduled_event, 
                            &local_cancel_event,
                            true);
            if(kParseSuccess != ret)
            {
                goto exit;
            }

            i += args_parsed;
            modified |= kModSched;
        } else if(0 == strncmp(argv[i], ARG_REPEAT, kMaxArgStringLength))
        {
            int args_parsed;
            
            local_repeating_event = CFDictionaryCreateMutable(0, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if(!local_repeating_event) {
                ret = kParseInternalError;
                goto exit;
            }
            i+=1;
            ret = parseRepeatingEvent(
                            &(argv[i]),
                            &args_parsed,
                            local_repeating_event, 
                            &local_cancel_repeating);
            
            if(kParseSuccess == ret)
            {
                modified |= kModRepeat;
            } else {
                ret = kParseBadArgs;
            }
            goto exit;
        } else 
        {
        // Process the settings
          if(!strncmp(argv[i], ARG_BOOKMARK, kMaxArgStringLength)) 
          {
            set_new_power_bookmark();
            goto exit;
          } else if(0 == strncmp(argv[i], ARG_DEBUGFLAGS, kMaxArgStringLength))
          {
             if (argv[i+1])
                set_debugFlags(&argv[i+1]);
              else
                  printf("Error: You need to specify debug flags value\n");
              goto exit;
          } else if(0 == strncmp(argv[i], ARG_BTINTERVAL, kMaxArgStringLength))
          {
              if(argv[i+1])
                set_btInterval(&argv[i+1]);
              else
                  printf("Error: You need to specify an interval in seconds\n");
              goto exit;
          } else if(0 == strncmp(argv[i], ARG_DWLINTERVAL, kMaxArgStringLength))
          {
              if(argv[i+1])
                  set_dwlInterval(&argv[i+1]);
              else
                  printf("Error: You need to specify an interval in seconds\n");
              goto exit;
          } else if(0 == strncmp(argv[i], ARG_DISABLEASSERTION, kMaxArgStringLength))
          {
              if(argc <= 2) {
                  printf("Error: Assertion type to disable is missing\n");
                  goto exit;
              }
              kr = IOPMCtlAssertionType(argv[i+1], kIOPMDisableAssertionType);
              if (kr == kIOReturnNotPrivileged)
                  printf("\'%s\' must be run as root to disable assertions\n", argv[0]);
              else if (kr != kIOReturnSuccess)
                  printf("Failed to disable assertions with err code 0x%x\n", kr);
              goto exit;
          } else if(0 == strncmp(argv[i], ARG_ENABLEASSERTION, kMaxArgStringLength))
          {
              if(argc <= 2) {
                  printf("Error: Assertion type to enable is missing\n");
                  goto exit;
              }
              kr = IOPMCtlAssertionType(argv[i+1], kIOPMEnableAssertionType);
              if (kr == kIOReturnNotPrivileged)
                  printf("\'%s\' must be run as root to enable assertions\n", argv[0]);
              else if (kr != kIOReturnSuccess)
                  printf("Failed to enable assertions with err code 0x%x\n", kr);
              goto exit;

          } else if (0 == strncmp(argv[i], ARG_MT2BOOK, kMaxArgStringLength))
          {
              mt2bookmark();
              goto exit;
          } else if (0 == strncmp(argv[i], ARG_SETSAAFLAGS, kMaxArgStringLength))
          {
              if(argv[i+1])
                set_saaFlags(&argv[i+1]);
              else
                  printf("Error: You need to specify an integer flag value\n");
              goto exit;
          } else if (0 == strncmp(argv[i], ARG_NOPOLL, kMaxArgStringLength))
          {
              set_nopoll();
              goto exit;
         } else if(0 == strncmp(argv[i], ARG_BOOT, kMaxArgStringLength))
          {
              // Tell kernel power management that bootup is complete
              kr = setRootDomainProperty(CFSTR("System Boot Complete"), kCFBooleanTrue);
              if(kr == kIOReturnSuccess) {
                 printf("Setting boot completed.\n");
              } else {
                  fprintf(stderr, "pmset: Error 0x%x setting boot property\n", kr);
                  fflush(stderr);
              }

                i++;
            } else if(0 == strncmp(argv[i], ARG_UNBOOT, kMaxArgStringLength))
            {
                // Tell kernel power management that bootup is complete
                kr = setRootDomainProperty(CFSTR("System Shutdown"), kCFBooleanTrue);
                if(kr == kIOReturnSuccess) {
                    printf("Setting shutdown true.\n");
                } else {
                    fprintf(stderr, "pmset: Error 0x%x setting boot property\n", kr);
                    fflush(stderr);
                }

                i++;
            } else if (0 == strncmp(argv[i], ARG_POLLBOOT, kMaxArgStringLength)) {
                ret = IOPSRequestBatteryUpdate(kIOPSReadSystemBoot);
                if (kIOReturnSuccess != ret) {
                    fprintf(stderr, "pmset: Must be run as root.\n");
                }
                goto exit;
            } else if (0 == strncmp(argv[i], ARG_POLLALL, kMaxArgStringLength)) {
                ret = IOPSRequestBatteryUpdate(kIOPSReadAll);
                if (kIOReturnSuccess != ret) {
                    fprintf(stderr, "pmset: Must be run as root.\n");
                }
                goto exit;
            } else if (0 == strncmp(argv[i], ARG_POLLUSER, kMaxArgStringLength)) {
                ret = IOPSRequestBatteryUpdate(kIOPSReadUserVisible);
                if (kIOReturnSuccess != ret) {
                    fprintf(stderr, "pmset: Must be run as root.\n");
                }
                goto exit;
            } else if(0 == strncmp(argv[i], ARG_FORCE, kMaxArgStringLength))
            {
                *force_activate_settings = true;
                i++;
            } else if( (0 == strncmp(argv[i], ARG_DIM, kMaxArgStringLength)) ||
                       (0 == strncmp(argv[i], ARG_DISPLAYSLEEP, kMaxArgStringLength)) )
            {
                // either 'dim' or 'displaysleep' argument sets minutes until display dims
                if(-1 == checkAndSetIntValue( argv[i+1], 
                                                CFSTR(kIOPMDisplaySleepKey), 
                                                apply, false, kNoMultiplier,
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if( (0 == strncmp(argv[i], ARG_SPINDOWN, kMaxArgStringLength)) ||
                       (0 == strncmp(argv[i], ARG_DISKSLEEP, kMaxArgStringLength)))
            {
                // either 'spindown' or 'disksleep' argument sets minutes until disk spindown
                if(-1 == checkAndSetIntValue( argv[i+1], 
                                                CFSTR(kIOPMDiskSleepKey), 
                                                apply, false, kNoMultiplier,
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_SLEEP, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue( argv[i+1], 
                                                CFSTR(kIOPMSystemSleepKey), 
                                                apply, false, kNoMultiplier,
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_WOMP, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnLANKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_RING, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnRingKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_AUTORESTART, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMRestartOnPowerLossKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_WAKEONACCHANGE, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnACChangeKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            }  else if(0 == strncmp(argv[i], ARG_LIDWAKE, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMWakeOnClamshellKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_REDUCEBRIGHT, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMReduceBrightnessKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_SLEEPUSESDIM, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDisplaySleepUsesDimKey),
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if((0 == strncmp(argv[i], ARG_MOTIONSENSOR, kMaxArgStringLength))
                   || (0 == strncmp(argv[i], ARG_MOTIONSENSOR2, kMaxArgStringLength)) )
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMMobileMotionModuleKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_TTYKEEPAWAKE, kMaxArgStringLength))
            {            
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMTTYSPreventSleepKey), 
                                                apply, true, kNoMultiplier, 
                                                ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_DISABLESLEEP, kMaxArgStringLength))
            {
                char            *endptr = NULL;
                long            val;

                if( argv[i+1] ) 
                {
                    val = strtol(argv[i+1], &endptr, 10);
                    
                    if(0 != *endptr)
                    {
                        // the string contained some non-numerical characters - bail
                        ret = kParseBadArgs;
                        goto exit;
                    }
                    
                    // Any non-zero value of val (preferably 1) means DISABLE sleep.
                    // A zero value means ENABLE sleep.
    
                    CFDictionarySetValue( local_system_power_settings, 
                                          kIOPMSleepDisabledKey, 
                                          val ? kCFBooleanTrue : kCFBooleanFalse);
    
                    modified |= kModSystemSettings;
                }
                i+=2;
            } else if(0 ==  strncmp(argv[i], ARG_DISABLEFDEKEYSTORE, kMaxArgStringLength))
            {
                char            *endptr = NULL;
                long            val;

                if( argv[i+1] ) 
                {
                    val = strtol(argv[i+1], &endptr, 10);
                    
                    if(0 != *endptr)
                    {
                        // the string contained some non-numerical characters - bail
                        ret = kParseBadArgs;
                        goto exit;
                    }
                    
                    // Any non-zero value of val (preferably 1) means Allow storing
                    // FDE Keys to hardware
                    // A zero value means Avoid storing keys to hardware
    
                    CFDictionarySetValue( local_system_power_settings, 
                                          CFSTR(kIOPMDestroyFVKeyOnStandbyKey), 
                                          val ? kCFBooleanTrue : kCFBooleanFalse );
    
                    modified |= kModSystemSettings;
                }
                i+=2;

            } else if(0 == strncmp(argv[i], ARG_HALTLEVEL, kMaxArgStringLength))
            {
                if(-1 == setUPSValue(argv[i+1], CFSTR(kIOPMDefaultUPSThresholds), 
                                                CFSTR(kIOUPSShutdownAtLevelKey), 
                                                apply, local_ups_settings))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModUPSThresholds;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HALTAFTER, kMaxArgStringLength))
            {
                if(-1 == setUPSValue(argv[i+1], CFSTR(kIOPMDefaultUPSThresholds), 
                                                CFSTR(kIOUPSShutdownAfterMinutesOn), 
                                                apply, local_ups_settings))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModUPSThresholds;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HALTREMAIN, kMaxArgStringLength))
            {
                if(-1 == setUPSValue(argv[i+1], CFSTR(kIOPMDefaultUPSThresholds), 
                                                CFSTR(kIOUPSShutdownAtMinutesLeft), 
                                                apply, local_ups_settings))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModUPSThresholds;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HIBERNATEMODE, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOHibernateModeKey), 
                                                        apply, false, kNoMultiplier, 
                                                        ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HIBERNATEFREERATIO, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOHibernateFreeRatioKey), 
                                                        apply, false, kNoMultiplier, 
                                                        ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HIBERNATEFREETIME, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOHibernateFreeTimeKey), 
                                                        apply, false, kNoMultiplier, 
                                                        ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_HIBERNATEFILE, kMaxArgStringLength))
            {
                if(-1 == checkAndSetStrValue(argv[i+1], CFSTR(kIOHibernateFileKey), 
                                                        apply, ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_GPU, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMGPUSwitchKey), 
                                                        apply, false, kNoMultiplier,
                                                        ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }                
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_NETAVAILABLE, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], 
                                     CFSTR(kIOPMPrioritizeNetworkReachabilityOverSleepKey), 
                                     apply, false, kNoMultiplier,
                                     ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_DEEPSLEEP, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDeepSleepEnabledKey), 
                                             apply, false, kNoMultiplier,
                                             ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_DEEPSLEEPDELAY, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMDeepSleepDelayKey), 
                                             apply, false, kNoMultiplier,
                                             ac, battery, ups))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_DARKWAKES, kMaxArgStringLength))
            {
                if((-1 == checkAndSetIntValue(argv[i+1],
                            CFSTR(kIOPMDarkWakeBackgroundTaskKey), apply,
                            false, kNoMultiplier, ac, battery, ups)) ||
                    (-1 == checkAndSetIntValue(argv[i+1],
                             CFSTR(kIOPMSleepServicesKey), apply, false,
                             kNoMultiplier, ac, battery, ups)))
                {
                    ret = kParseBadArgs;
                    goto exit;
                }
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_AUTOPOWEROFF, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMAutoPowerOffEnabledKey),
                                             apply, false, kNoMultiplier,
                                             ac, battery, ups))
                    return kParseBadArgs;
                modified |= kModSettings;
                i+=2;
            } else if(0 == strncmp(argv[i], ARG_AUTOPOWEROFFDELAY, kMaxArgStringLength))
            {
                if(-1 == checkAndSetIntValue(argv[i+1], CFSTR(kIOPMAutoPowerOffDelayKey),
                                             apply, false, kNoMultiplier,
                                             ac, battery, ups))
                    return kParseBadArgs;
                modified |= kModSettings;
                i+=2;        
            } else {
                // Determine if this is a number.
                // If so, the user is setting the active power profile
                // for the power sources specified in applyTo
                // If not, bail with bad input.
                char                    *endptr = 0;
                long                    val;
                CFNumberRef             prof_val = 0;
            
                if(!local_profiles) {
                    ret = kParseInternalError;
                    goto exit;
                }

                val = strtol(argv[i], &endptr, 10);
            
                if(0 != *endptr || (val < -1) || (val > 4) )
                {
                    // the string contained some non-numerical characters
                    // or the profile number was out of the expected range
                    ret = kParseBadArgs;
                    goto exit;
                }
                prof_val = CFNumberCreate(0, kCFNumberIntType, &val);
                if(!prof_val) {
                    ret = kParseInternalError;
                } else {
                    // setting a profile
                    if(kApplyToBattery & apply) {
                        CFDictionarySetValue(local_profiles, CFSTR(kIOPMBatteryPowerKey), prof_val);
                    }
                    if(kApplyToCharger & apply) {
                        CFDictionarySetValue(local_profiles, CFSTR(kIOPMACPowerKey), prof_val);                
                    }
                    if(kApplyToUPS & apply) {
                        CFDictionarySetValue(local_profiles, CFSTR(kIOPMUPSPowerKey), prof_val);
                    }
                    CFRelease(prof_val);
                
                    modified |= kModProfiles;
                }
                goto exit;
            }
        } // if
    } // while

exit:
    if(modified & kModSettings) {
        *settings = local_settings;    
        *modified_power_sources = apply;
    } else {
        if(local_settings) CFRelease(local_settings);
    }

    if(modified & kModSystemSettings) {
        *system_power_settings = local_system_power_settings;
    } else {
        if(local_system_power_settings) CFRelease(local_system_power_settings);
    }

    if(modified & kModProfiles) {
        *active_profiles = local_profiles;
    } else {
        if(local_profiles) CFRelease(local_profiles);
    }
    
    if(modified & kModUPSThresholds) {
        if(ups_thresholds) *ups_thresholds = local_ups_settings;    
    } else {
        if(local_ups_settings) CFRelease(local_ups_settings);
    }

    if(modified & kModSched) {
        *scheduled_event = local_scheduled_event;
        *cancel_scheduled_event = local_cancel_event;
    }
    
    if(modified & kModRepeat) {
        *repeating_event = local_repeating_event;
        *cancel_repeating_event = local_cancel_repeating;
    } else {
        if(local_repeating_event) CFRelease(local_repeating_event);
    }
    
    return ret;
}

// int arePowerSourceSettingsInconsistent(CFDictionaryRef)
// Function - determine if the settings will produce the "intended" idle sleep consequences
// Parameter - The CFDictionary contains energy saver settings of 
//             CFStringRef keys and CFNumber/CFBoolean values
// Return - non-zero bitfield, each bit indicating a setting inconsistency
//          0 if settings will produce expected result 
//         -1 in case of other error
static int arePowerSourceSettingsInconsistent(CFDictionaryRef set)
{
    int                 sleep_time, disk_time, dim_time;
    CFNumberRef         num;
    int                 ret = 0;

    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMSystemSleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &sleep_time)) return -1;
    
    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMDisplaySleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &dim_time)) return -1;

    num = isA_CFNumber(CFDictionaryGetValue(set, CFSTR(kIOPMDiskSleepKey)));
    if(!num || !CFNumberGetValue(num, kCFNumberIntType, &disk_time)) return -1;

    // For system sleep to occur around the time you set it to, the disk and display
    // sleep timers must conform to these rules:
    // 1. display sleep <= system sleep
    // 2. If system sleep != Never, then disk sleep can not be == Never
    //    2a. It is, however, OK for disk sleep > system sleep. A funky hack in
    //        the kernel IOPMrootDomain allows this.
    // and note: a time of zero means "never power down"
    
    if(sleep_time != 0)
    {
        if(dim_time > sleep_time || dim_time == 0) ret |= kInconsistentDisplaySetting;
    
        if(disk_time == 0) ret |= kInconsistentDiskSetting;
    }
    
    return ret;
}

// void checkSettingConsistency(CFDictionaryRef)
// Checks ES settings profile by profile
// Prints a user warning if any idle timer settings violate the kernel's assumptions
// about idle, disk, and display sleep timers.
// Parameter - a CFDictionary of energy settings "profiles", usually one per power supply
static void checkSettingConsistency(CFDictionaryRef profiles)
{
    int                 num_profiles;
    int                 i;
    int                 ret;
    char                buf[kMaxLongStringLength];
    CFStringRef         *keys;
    CFDictionaryRef     *values;
    
    num_profiles = CFDictionaryGetCount(profiles);
    keys = (CFStringRef *)malloc(num_profiles * sizeof(void *));
    values = (CFDictionaryRef *)malloc(num_profiles * sizeof(void *));
    if(!keys || !values) return;
    CFDictionaryGetKeysAndValues(profiles, (const void **)keys, (const void **)values);
    
// TODO: Warn user if 1) they have just changed the custom settings
//       2) their active profile is _NOT_ custom
    
    for(i=0; i<num_profiles; i++)
    {
        // Check settings profile by profile
        if( isA_CFDictionary(values[i])
            && (ret = arePowerSourceSettingsInconsistent(values[i])) )
        {
            // get profile name
            if(!CFStringGetCString(keys[i], buf, kMaxLongStringLength, kCFStringEncodingMacRoman)) break;
            
            fprintf(stderr, "Warning: Idle sleep timings for \"%s\" may not behave as expected.\n", buf);
            if(ret & kInconsistentDisplaySetting)
            {
                fprintf(stderr, "- Display sleep should have a lower timeout than system sleep.\n");
            }
            if(ret & kInconsistentDiskSetting)
            {
                fprintf(stderr, "- Disk sleep should be non-zero whenever system sleep is non-zero.\n");
            }
            fflush(stderr);        
        }
    }

    free(keys);
    free(values);
}

static ScheduledEventReturnType *scheduled_event_struct_create(void)
{
    ScheduledEventReturnType *ret = malloc(sizeof(ScheduledEventReturnType));
    if(!ret) return NULL;
    ret->who = 0;
    ret->when = 0;
    ret->which = 0;
    return ret;
}

static void scheduled_event_struct_destroy(
    ScheduledEventReturnType *free_me)
{
    if( (0 == free_me) ) return;
    if(free_me->who) {
        CFRelease(free_me->who);
        free_me->who = 0;
    }
    if(free_me->when) {
        CFRelease(free_me->when);
        free_me->when = 0;
    }
    if((free_me)->which) {
        CFRelease(free_me->which);
        free_me->which = 0;
    }
    free(free_me);
}


static IOReturn _pm_connect(mach_port_t *newConnection)
{
    kern_return_t       kern_result = KERN_SUCCESS;
    
    if(!newConnection) return kIOReturnBadArgument;

    // open reference to PM configd
    kern_result = bootstrap_look_up2(bootstrap_port, 
                                    kIOPMServerBootstrapName, 
                                    newConnection, 
                                    0, 
                                    BOOTSTRAP_PRIVILEGED_SERVER);    

    if(KERN_SUCCESS != kern_result) {
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

static IOReturn _pm_disconnect(mach_port_t connection)
{
    if(!connection) return kIOReturnBadArgument;
    mach_port_destroy(mach_task_self(), connection);
    return kIOReturnSuccess;
}

/******************************************************************************/
/*                                                                            */
/*     ASL & MESSAGETRACER & HISTORY                                          */
/*                                                                            */
/******************************************************************************/

/*
 * Cache the message in a temp ring buffer. 
 *
 * ASL doesn't allow re-positioning the current read pointer into 
 * aslresponse. There is no aslresponse_prev() API. So, we 
 * store the message retrieved from aslreponse in a ring
 * buffer.
 */
static aslmsg _cacheAndGetMsg(aslresponse pmresponse)
{
   aslmsg msg = NULL;

   if (msgCache == NULL) 
   {
      msgCache = calloc(1, sizeof(MsgCache));
      if (!msgCache) return NULL;
   }

   if (((msgCache->writeIdx+1) % RING_SIZE) == msgCache->readIdx)
      return NULL; /* overflow */

   msg = aslresponse_next(pmresponse);
   if (!msg) return NULL;

   msgCache->msgRing[msgCache->writeIdx] = msg;

   msgCache->writeIdx = (msgCache->writeIdx+1) % RING_SIZE;
   return  msg;
}

static aslmsg _my_next_response(aslresponse pmresponse)
{
    aslmsg   next_msg = 0;
    
    /*
     * _my_next_response returns messages from our cache 
     * until there aren't any more messages in it.
     * Then it returns messages from the PM ASL store, via pmresponse.
     */    
    if (msgCache && (msgCache->readIdx != msgCache->writeIdx))
    {
        next_msg = msgCache->msgRing[msgCache->readIdx];
        msgCache->readIdx = (msgCache->readIdx+1) % RING_SIZE;
    }
    
    /*
     * If the cache is empty, then we pull messages straight from the ASL store.
     */
    if (0 == next_msg) {
        next_msg = aslresponse_next(pmresponse);
    }
    
    return next_msg;
}

/*
 * Called when current event is 'Sleep', to find out
 * the time stamp of next wake event.
 *
 * Possible transitions:
 * Sleep -> Wake
 * Sleep -> DarkWake
 */

static int32_t _getNextWakeTime(aslresponse pmresponse)
{
   const char *domain = NULL;
   const char *timeStr = NULL;
   int32_t wakeTime = -1;
   aslmsg next;

   do {

      if ((next = _cacheAndGetMsg(pmresponse)) == NULL) break;

      domain = asl_get(next, kPMASLDomainKey);
      if (!domain) break;

      if ( (!strncmp(kPMASLDomainPMWake, domain, sizeof(kPMASLDomainPMWake) )) ||
            (!strncmp(kPMASLDomainPMDarkWake, domain, sizeof(kPMASLDomainPMDarkWake) )) )
      {
         timeStr = asl_get(next, ASL_KEY_TIME);
         if (timeStr)
            wakeTime = atol(timeStr);
         break;
      }
      else if (!strncmp(kPMASLDomainPMSleep, domain, sizeof(kPMASLDomainPMSleep) ))
      {
          /* Came across another sleep trace. Wake trace is missing */
          break;
      }
   } while(1);

   return wakeTime;
}

/*
 * Called when current event is 'wake/darkWake', to find out
 * the time stamp of next sleep/wake event.
 *
 * Possible transitions:
 * Wake -> Sleep
 * DarkWake -> Sleep
 * DarkWake -> Wake
 */
static int32_t _getNextSleepTime(aslresponse pmresponse, const char *curr_domain)
{
   const char *domain = NULL;
   const char *timeStr = NULL;
   int32_t sleepTime = -1;
   uint32_t sleepCnt = 0;
   aslmsg next;

   do {


      if ((next = _cacheAndGetMsg(pmresponse)) == NULL) break;
      domain = asl_get(next, kPMASLDomainKey);
      if (!domain) continue;

      /* Check for events with unexpected transitions */ 
      if (( !strncmp(kPMASLDomainPMWake, curr_domain, sizeof(kPMASLDomainPMWake) )) &&
          ( (!strncmp(kPMASLDomainPMWake, domain, sizeof(kPMASLDomainPMWake) )) ||
            (!strncmp(kPMASLDomainPMDarkWake, domain, sizeof(kPMASLDomainPMDarkWake)) )
          ))
      {
          /* Wake -> Wake or Wake -> DarkWake is unexpected */
          break;
      }

      else if ((!strncmp(kPMASLDomainPMDarkWake, curr_domain, sizeof(kPMASLDomainPMDarkWake))) &&
               (!strncmp(kPMASLDomainPMDarkWake, domain, sizeof(kPMASLDomainPMDarkWake)) ))
      {
          /* DarkWake ->  DarkWake is unexpected */
          break;
      }

      if ( (!strncmp(kPMASLDomainPMSleep, domain, sizeof(kPMASLDomainPMSleep) )) ||
               (!strncmp(kPMASLDomainPMWake, domain, sizeof(kPMASLDomainPMWake) )) )
      {
         const char *value1 = asl_get(next, kPMASLValueKey);
         if (value1) {
            sleepCnt = strtol(value1, NULL, 0);
            
            if (sleepCnt == 1)
               break;  /* System rebooted at this point */
         }

         timeStr = asl_get(next, ASL_KEY_TIME);
         if (timeStr)
            sleepTime = atol(timeStr);
         break;
      }
   } while(1);

   return sleepTime;
}

#define kPMASLStorePath                 "/var/log/powermanagement"

static aslresponse open_pm_asl_store(int *result)
{
    aslresponse         query = NULL;
    aslresponse         response = NULL;
    asl_store_t         *pmstore = NULL;
    uint64_t            endMessageID;
    
    query = (aslresponse)calloc(1, sizeof(asl_search_result_t));
    if (query != NULL)
    {
        query->msg = (asl_msg_t **)calloc(1, sizeof(asl_msg_t *));
        if (query->msg != NULL)
        {
            asl_msg_t *cq = (asl_msg_t *)asl_new(ASL_TYPE_QUERY);
            if (cq != NULL)
            {
                query->msg[0] = cq;
                query->count = 1;
                asl_set_query((aslmsg)cq, ASL_KEY_FACILITY, kPMFacility, ASL_QUERY_OP_EQUAL);
                
                *result = asl_store_open_read(kPMASLStorePath, &pmstore);
                if (0 == *result) {
                    *result = asl_store_match(pmstore, query, &response, &endMessageID, 0, 0, 1);
                }
                asl_free((aslmsg)cq);
            }
            free(query->msg);
        }
        free(query);
    }

    return response;
}

/* All PM messages in ASL log */
static void show_log(void)
{
    aslmsg              m = 0;
    char                uuid[100];
    long                sleep_cnt = 0;
    int                 dark_wake_cnt = 0;
    bool                first_iter = true;
    CFAbsoluteTime      boot_time = 0;

    int                 asl_result = -1;
    aslresponse         response = NULL;

    response = open_pm_asl_store(&asl_result);
    if (!response)
    {
        printf("Error(%d) - no messages found in PM ASL data store at: %s\n", asl_result, kPMASLStorePath);
        return;
    } else {
        printf("PM ASL data store: %s\n", kPMASLStorePath);
    }
    
    uuid[0] = 0;

    while ((m = _my_next_response(response)))
    {
        const char    *val = NULL;
        int32_t      print_duration_time = 0;
        uint32_t      time_read = 0;
        char          buf[40];
        bool          new_boot_cycle = false;

        if ((val = asl_get(m, kPMASLDomainKey)))
        {
            if (!strncmp(val, kPMASLDomainPMStart, sizeof(kPMASLDomainPMStart)-1)) {
                new_boot_cycle = true;
            }
        }
        
        if (((val = asl_get(m, kPMASLUUIDKey)) && (strncmp( val, uuid, sizeof(uuid)) != 0)) || new_boot_cycle ) 
        {
            // New Sleep cycle is about to begin
            // Print Sleep cnt and dark wake cnt of previous sleep/wake cycle
            if ( !first_iter )
            {
               printf("Sleep/Wakes since boot");
               if (boot_time) {
                  printf(" at ");
                  print_pretty_date(boot_time, false);

               }
               printf(":%ld   Dark Wake Count in this sleep cycle:%d\n", sleep_cnt, dark_wake_cnt);
            }
            first_iter = false;

            printf("\n");   // Extra line to seperate from previous sleep/wake cycles
            dark_wake_cnt = 0;

            // Print the header for each column
            printf("%-25s %-20s\t%-75s\t%-10s\t%-10s\n", "Time stamp", "Domain", "Message", "Duration", "Delay");
            printf("%-25s %-20s\t%-75s\t%-10s\t%-10s\n", "==========", "======", "=======", "========", "=====");

            // Print and save UUID of the new cycle
            snprintf(uuid, sizeof(uuid), "%s", val);
            printf("UUID: %s\n", val);
            sleep_cnt = 0;
        }

        // Time
        if ((val = asl_get(m, ASL_KEY_TIME))) 
        {
            time_read = atol(val);
            CFAbsoluteTime  abs_time = (CFAbsoluteTime)(time_read - kCFAbsoluteTimeIntervalSince1970);
            print_pretty_date(abs_time, false);
            if (new_boot_cycle)
               boot_time = abs_time;
        }


        // Domain
        if ((val = asl_get(m, kPMASLDomainKey)))
        {   
            const char *value1 = asl_get(m, kPMASLValueKey);

            if (strnstr(val, "Response.", strlen(val))) {
               printf("%-20s\t",  ((char *)val + (uintptr_t)strlen("Response.")));
            } else {
                printf("%-20s\t",  (char *)val);
            }
    
            if (!strncmp(kPMASLDomainPMSleep, val, sizeof(kPMASLDomainPMSleep) )) 
            {
               if (value1) {
                   sleep_cnt = strtol(value1, NULL, 0);
                
                }
                if ( (print_duration_time = _getNextWakeTime(response)) != -1) {
                    print_duration_time -= time_read;
                } else {
                    print_duration_time = 0;
                }
            }
            else if (!strncmp(kPMASLDomainPMDarkWake, val, sizeof(kPMASLDomainPMDarkWake) ))
            {
               if (value1) {
                  dark_wake_cnt = strtol(value1, NULL, 0);
               }
                
               if( (print_duration_time = _getNextSleepTime(response, val)) != -1) {
                    print_duration_time -= time_read;
                } else {
                    print_duration_time = 0;
                }
            }
            else if (!strncmp(kPMASLDomainPMWake, val, sizeof(kPMASLDomainPMWake) ))
            {
               if ( (print_duration_time = _getNextSleepTime(response, val)) != -1) {
                    print_duration_time -= time_read;
                } else {
                    print_duration_time = 0;
                }
            }
/*            else if (!strnstr(val, kPMASLDomainSummaryPrefix, strlen(kPMASLDomainSummaryPrefix)))
            {
                print_duration_time = value1;
            }
 */
        }
        else 
        {
            printf("%-20s\t",  " ");
        }
        
        // Message
        if ((val = asl_get(m, ASL_KEY_MSG))) {
            printf("%-75s\t", val);
        } else {
            printf("%-75s\t",  " ");
        }
        
        // Duration/Delay
        if (0 != print_duration_time) {
            snprintf(buf, sizeof(buf), "%d secs", print_duration_time);
        } else {
            buf[0] = 0;
        }
        printf("%-10s", buf);

        if ((val = asl_get(m, kPMASLDelayKey)))
        {   
            printf("%-10s\t", val);
        }
        
        printf("\n");
    }

    if (sleep_cnt || dark_wake_cnt) {
        printf("\nTotal Sleep/Wakes since boot");
        if (boot_time) {
            printf(" at ");
            print_pretty_date(boot_time, false);
        }
        printf(":%ld\n", sleep_cnt);
    }
    printf("\n");
    show_assertions("Showing all currently held IOKit power assertions");
}


static void show_power_event_history(void)
{
    IOReturn        ret;
    CFArrayRef      powpowHistory = NULL;
    CFStringRef     uuid = NULL;
    CFDictionaryRef uuid_details;
    char            uuid_cstr[255];
    int             uuid_index;
    int             uuid_count;
    //CFAbsoluteTime  set_time = 0.0;
    //CFAbsoluteTime  clear_time = 0.0;
    //CFNumberRef     timestamp;
    CFStringRef     timestamp;
    char            st[60];
    char            ct[60];

    ret = IOPMCopyPowerHistory(&powpowHistory);
    
    if (kIOReturnSuccess != ret || NULL == powpowHistory) 
    {
        printf("Error - no power history found. (IOPMCopyPowerHistory error = 0x%08x)\n", ret);
        goto exit;
    }
    
    uuid_count = CFArrayGetCount(powpowHistory);
    printf("Power History Summary (%d UUIDs)\n", uuid_count);
    
    int i = 0;
    for(; i < 9; i++)
      printf("----------");
    printf("\n");

    printf("%-40s|%-24s|%-24s|\n", "UUID", "Set Time", "Clear Time");

    i = 0;
    for(; i < 9; i++)
      printf("----------");
    printf("\n");

    for (uuid_index = 0; uuid_index < uuid_count; uuid_index++)
    {
        uuid_details = isA_CFDictionary(CFArrayGetValueAtIndex(powpowHistory, uuid_index));
        if (uuid_details) {
            uuid = CFDictionaryGetValue(
                                uuid_details, 
                                CFSTR(kIOPMPowerHistoryUUIDKey));
            CFStringGetCString(uuid, uuid_cstr, sizeof(uuid_cstr), kCFStringEncodingUTF8);
            
            printf("%-40s|", uuid_cstr);

            timestamp = CFDictionaryGetValue(uuid_details, 
                                       CFSTR(kIOPMPowerHistoryTimestampKey));
            CFStringGetCString(timestamp, st, sizeof(st), kCFStringEncodingUTF8);
            
            timestamp = CFDictionaryGetValue(uuid_details, 
                                       CFSTR(kIOPMPowerHistoryTimestampCompletedKey));
            CFStringGetCString(timestamp, ct, sizeof(ct), kCFStringEncodingUTF8);

            printf("%-24.24s|%-24.24s|\n", st, ct);
        }
    }
    i = 0;
    for(; i < 9; i++)
      printf("----------");
    printf("\n");
exit:
    return;
}


static void printHistoryDetailedEventDictionary(CFDictionaryRef event)
{
    CFStringRef     evTypeString = NULL;
    CFNumberRef     evReasonNum = NULL;
    CFNumberRef     evResultNum = NULL;
    CFStringRef     evDeviceNameString = NULL;
    CFStringRef     evUUIDString = NULL;
    CFStringRef     evInterestedDeviceNameString = NULL;
    CFNumberRef     evTimestampAbsTime = NULL;
    CFNumberRef     evElapsedTimeMicroSec = NULL;
    CFNumberRef     evOldState = NULL;
    CFNumberRef     evNewState = NULL;
    
    CFAbsoluteTime  ts = 0.0;
    int             foo = 0;
    int             bar = 0;
    char            buf_cstr[40];
    char            *display_cstr = NULL;

    evTypeString = isA_CFString(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryEventTypeKey)));
    evReasonNum = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryEventReasonKey)));
    evResultNum = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryEventResultKey)));
    evDeviceNameString = isA_CFString(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryDeviceNameKey)));
    evUUIDString = isA_CFString(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryUUIDKey)));
    evInterestedDeviceNameString = isA_CFString(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryInterestedDeviceNameKey)));
    evTimestampAbsTime = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryTimestampKey)));
    evOldState = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryOldStateKey)));
    evNewState = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryNewStateKey)));
    evElapsedTimeMicroSec = isA_CFNumber(CFDictionaryGetValue(event, CFSTR(kIOPMPowerHistoryElapsedTimeUSKey)));
    
    // Timestamp
    if (evTimestampAbsTime) {
        CFNumberGetValue(evTimestampAbsTime, kCFNumberDoubleType, &ts);
        print_pretty_date(ts, false);
        printf("|");
    }
    else
      printf("\t");
    
    bool systemEvent = false;

    //Event Type
    if (evTypeString) {
        if (CFStringGetCString(evTypeString, buf_cstr, sizeof(buf_cstr), kCFStringEncodingUTF8)) {
            display_cstr = buf_cstr;
        } else {
            display_cstr = "String encoding error";
        }
        
        if(   !strcmp(display_cstr, "UUIDSet")
           || !strcmp(display_cstr, "UUIDClear")
           || !strcmp(display_cstr, "Sleep")
           || !strcmp(display_cstr, "SleepDone")
           || !strcmp(display_cstr, "Wake")
           || !strcmp(display_cstr, "WakeDone") ) {

          systemEvent = true;

          // Prints bold lettering on supported systems
          printf("%c[1m", (char)27);
        }        
        printf("%-25.25s", display_cstr);
        if( systemEvent ) {
          printf("%c[0m", (char)27);
        }
        printf("|");
    } 
    else {
        printf("%-25s", " ");
    }
    
    // Power State Change 
    if (evOldState && evNewState) {
        CFNumberGetValue(evOldState, kCFNumberIntType, &foo);
        CFNumberGetValue(evNewState, kCFNumberIntType, &bar);
        printf("%d to %d |", foo, bar);
    }
    else
      printf("%-7s|", " ");
    
    // Reason number
    if (evReasonNum) {
        CFNumberGetValue(evReasonNum, kCFNumberIntType, &foo);
        printf("%-7d|", foo);
    }
    else
      printf("%-7s|", " ");
    
    // Result Number
    if (evResultNum) {
        CFNumberGetValue(evResultNum, kCFNumberIntType, &foo);
        printf("%-7d|", foo);
    }
    else
      printf("%-7s|", " ");

    // Device Name
    if (evDeviceNameString) {
        if (CFStringGetCString(evDeviceNameString, buf_cstr, sizeof(buf_cstr), kCFStringEncodingUTF8)) {
            display_cstr = buf_cstr;
        } else {
            display_cstr = "String encoding error";
        }

        printf("%-30.30s|", display_cstr);
    }
    else {
      if ( !systemEvent ) {
        printf("%-30s|", " ");
      }
    }
    
    // Event UUID, for system events
    if (evUUIDString) {
        if (CFStringGetCString(evUUIDString, buf_cstr, sizeof(buf_cstr), kCFStringEncodingUTF8)) {
            display_cstr = buf_cstr;
        } else {
            display_cstr = "String encoding error";
        }
        // Bold lettering, in supported systems
        printf("%c[1m", (char)27);
        printf("%-61s", display_cstr);
        printf("%c[0m", (char)27);

        printf("|");
    }

    // Interested Device
    if (evInterestedDeviceNameString) {
        if (CFStringGetCString(evInterestedDeviceNameString, buf_cstr, sizeof(buf_cstr), kCFStringEncodingUTF8)) {
            display_cstr = buf_cstr;
        } else {
            display_cstr = "String encoding error";
        }

        printf("%-30.30s|", display_cstr);
    }
    else {
      if( !systemEvent ) {
        printf("%-30s|", " ");
      }
    }

    // Time Elapsed
    if (evElapsedTimeMicroSec) {
        CFNumberGetValue(evElapsedTimeMicroSec, kCFNumberIntType, &foo);        
        printf("%-7d |", foo);
    }
    else
      printf("%-7s|", " ");

    printf("\n");
}

static void mt2bookmark(void)
{
    mach_port_t     connectIt = MACH_PORT_NULL;
    int disregard;
    
    if (kIOReturnSuccess == _pm_connect(&connectIt))
    {
        io_pm_get_value_int(connectIt, kIOPMMT2Bookmark, &disregard);
        _pm_disconnect(connectIt);
    }
    return;
}


static void show_power_event_history_detailed(void)
{
    IOReturn        ret;
    CFArrayRef      powpowHistory = NULL;
    CFStringRef     uuid = NULL;
    char            uuid_cstr[50];
    CFDictionaryRef uuid_details;
    int             uuid_index;
    int             uuid_count;
    
    ret = IOPMCopyPowerHistory(&powpowHistory);
    
    if (kIOReturnSuccess == kIOReturnNotFound) 
    {
        printf("No power management history to display. (See 'man pmset' to turn on history logging.)\n");
        goto exit;
    } else if (kIOReturnSuccess != ret)
    {
        printf("Error reading power management history (0x%08x)\n", ret);
        goto exit;
    }
    
    uuid_count = CFArrayGetCount(powpowHistory);
    
    //Bold lettering, on supported systems
    printf("%c[1m", (char)27);
    printf("Power History Detailed:\n");
    printf("%c[0m", (char)27);

    for (uuid_index = 0; uuid_index < uuid_count; uuid_index++)
    {
        uuid_details = isA_CFDictionary(CFArrayGetValueAtIndex(powpowHistory, uuid_index));
        assert(uuid_details);
        if (!uuid_details) {
            continue;
        }
        
        uuid = CFDictionaryGetValue(uuid_details, 
                                    CFSTR(kIOPMPowerHistoryUUIDKey));
        CFStringGetCString(uuid, uuid_cstr, 
                           sizeof(uuid_cstr), kCFStringEncodingUTF8);
        show_details_for_UUID( (char **)&uuid_cstr );    
    }
    
    CFRelease(powpowHistory);
exit:
    return;
}

static void set_debugFlags(char **argv)
{
    uint32_t  newFlags, oldFlags;
    IOReturn err;

    newFlags = strtol(argv[0], NULL, 0);
    if (errno == EINVAL ) {
        printf("Invalid argument\n");
        return;
    }
    err = IOPMSetDebugFlags(newFlags, &oldFlags);
    if (err == kIOReturnSuccess)
        printf("Debug flags changed from 0x%x to 0x%x\n",
                oldFlags, newFlags);
    else
        printf("Failed to change debugFlags. err=0x%x\n", err);

}

static void set_btInterval(char **argv)
{
    uint32_t  newInterval, oldInterval;
    IOReturn err;

    newInterval = strtol(argv[0], NULL, 0);
    if (errno == EINVAL) {
        printf("Invalid argument\n");
        return;
    }
    err = IOPMSetBTWakeInterval(newInterval, &oldInterval);
    if (err == kIOReturnSuccess)
        printf("Background task wake interval changed from %d secs to %d secs\n",
                oldInterval, newInterval);
    else
        printf("Failed to change Background task wake interval. err=0x%x\n", err);

}

static void set_dwlInterval(char **argv)
{
    uint32_t newInterval, oldInterval;
    IOReturn err;

    newInterval = strtol(argv[0], NULL, 0);
    if(errno == EINVAL) {
        printf("Invalid argument\n");
        return;
    }
    err = IOPMSetDWLingerInterval(newInterval, &oldInterval);
    if(err == kIOReturnSuccess)
        printf("DarkWake linger interval changed from %d secs to %d secs\n",
                oldInterval, newInterval);
    else
        printf("Failed to change DarkWake linger interval. err=0x%x\n", err);
}

static void set_saaFlags(char **argv)
{
    uint32_t newFlags, oldFlags;
    IOReturn err;

    newFlags = strtol(argv[0], NULL, 0);
    if(errno == EINVAL) {
        printf("Invalid argument\n");
        return;
    }

    err = IOPMChangeSystemActivityAssertionBehavior(newFlags, &oldFlags);
    if(err == kIOReturnSuccess)
        printf("System activity assertion behvior changed from %u to %u\n",
                oldFlags, newFlags);
    else
        printf("Failed to change system activity assertion behavior. err=0x%x\n", err);
}

static bool isBatteryPollingStopped(void)
{
    int                 myNotifyToken = 0;
    uint64_t            packedBatteryData = 0;
    int                 myNotifyStatus = 0;
    
    myNotifyStatus = notify_register_check(kIOPSTimeRemainingNotificationKey, &myNotifyToken);
    
    if (NOTIFY_STATUS_OK == myNotifyStatus) {
        notify_get_state(myNotifyToken, &packedBatteryData);
        notify_cancel(myNotifyToken);
    }
    
    return (packedBatteryData & kPSTimeRemainingNotifyNoPollBit) ? 1:0;
}

static void set_nopoll(void)
{
    mach_port_t     connectIt = MACH_PORT_NULL;
    bool            old_nopoll = isBatteryPollingStopped();
    
    if (kIOReturnSuccess == _pm_connect(&connectIt))
    {
        printf("Turning battery polling: %s\n", old_nopoll ? "On":"Off");
        io_pm_set_value_int(connectIt, kIOPMSetNoPoll, old_nopoll ? 0:1);
        _pm_disconnect(connectIt);
    } else {
        printf("Error: could not connect to powerd.\n");
    }
    
    if (!old_nopoll) {
        printf("Battery updates are stopped. Run \"pmset nopoll\" again, or reboot the system to resume.\n");
    }
}

static void set_new_power_bookmark(void) {
  printf("Bookmarked: Deprecated. Did not set a bookmark. \n");
}

static void show_details_for_UUID( char **argv ) {

  char *uuid_cstr = argv[0];
  
  if (!uuid_cstr) return;

  string_toupper(uuid_cstr, uuid_cstr);
  CFStringRef uuid = CFStringCreateWithCString( kCFAllocatorDefault, 
                                                uuid_cstr, 
                                                kCFStringEncodingUTF8);
  if( uuid == NULL )
    return;

  CFArrayRef event_array;
  CFDictionaryRef uuid_details;

  IOReturn ret = IOPMCopyPowerHistoryDetailed(uuid, &uuid_details);
  if (kIOReturnSuccess != ret)
  {
    printf("No power management history to display for this UUID! \n");
      CFRelease(uuid);
    return;
  }
  if (!uuid_details) 
  {
    printf("No power management history available for this UUID! \n");
    CFRelease(uuid);
    return;
  }
        
  CFNumberRef timestamp;
  CFAbsoluteTime set_time   = 0.0;
  CFAbsoluteTime clear_time = 0.0;

  timestamp = isA_CFNumber(
                  CFDictionaryGetValue(uuid_details, 
                                       CFSTR(kIOPMPowerHistoryTimestampKey)));

  CFNumberGetValue(timestamp, kCFNumberDoubleType, &set_time);

  timestamp = isA_CFNumber(
                  CFDictionaryGetValue(
                              uuid_details, 
                              CFSTR(kIOPMPowerHistoryTimestampCompletedKey)));

  CFNumberGetValue(timestamp, kCFNumberDoubleType, &clear_time);
  //Bold lettering, on supported systems

  printf("%c[1m", (char)27);
        
  // Hacky way of centering the UUID title
  printf("\t\t\t\t\t *UUID =  %s\n", uuid_cstr);
  // No more bold lettering
  printf("%c[0m", (char)27);
        
  // Print column headers, make nice table to display
  // power events under this UUID
  int i = 0;
  for(; i < 144; i+= 12)
    printf("------------");
  printf("--\n");

  printf("%-24s|", "Timestamp");
  printf("%-25s|", "Event Type");
  printf("%-7s|", "Change");
  printf("%-7s|", "Reason");
  printf("%-7s|", "Result");
  printf("%-30s|","Device Name");
  printf("%-30s|","Interested Device");
  printf("Time(uS)|");
  printf("\n");

  i = 0;
  for(; i < 144; i+= 12)
    printf("------------");
  printf("--\n");
  
  event_array = CFDictionaryGetValue(uuid_details, 
                                     CFSTR(kIOPMPowerHistoryEventArrayKey));
  int event_count = CFArrayGetCount(event_array);
  int event_index;
  CFDictionaryRef an_event;

  // Print out individual event details
  for (event_index = 0; event_index < event_count; event_index++)
  {
    an_event = isA_CFDictionary(CFArrayGetValueAtIndex(event_array, event_index));
    assert(an_event);
    if (!an_event)
      continue;
    printHistoryDetailedEventDictionary(an_event);
  }

  i = 0;
  for(; i < 144; i+= 12)
    printf("------------");
  printf("--\n");
        
  printf("\t Total of ");
        
  printf("%c[1m", (char)27); // Bold letters
  printf("%d ", event_count);
  printf("%c[0m", (char)27); // Un-bold letters

  printf("events under UUID "); 
        
  printf("%c[1m", (char)27); // Bold letters
  printf("%s ", uuid_cstr); 
  printf("%c[0m", (char)27); // Un-bold letters 
  printf("from ");
  printf("%c[1m", (char)27);
  print_pretty_date(set_time, false);
  printf("%c[0m", (char)27);
  printf("to ");
  printf("%c[1m", (char)27);
  print_pretty_date(clear_time, true); 
  printf("%c[0m", (char)27);

  // Done with our CFObjects. Don't want to own them anymore
  CFRelease(event_array);
  CFRelease(uuid);
}

static void _print_uuid_string(){
    CFStringRef     _uuid = IOPMSleepWakeCopyUUID();
    char            str[kMaxLongStringLength];

    if (!_uuid) {
        printf("(NULL)\n");
        return;
    }
    
    CFStringGetCString(_uuid, str, sizeof(str), kCFStringEncodingUTF8);
    printf("%s\n", str);
    CFRelease(_uuid);
}
static void _show_uuid_handler(
    void *refcon, 
    io_service_t batt, 
    natural_t messageType, 
    void *messageArgument)
{
    if (messageType != kIOPMMessageSleepWakeUUIDChange)
        return;

    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
    
    if (messageArgument == (void *)kIOPMMessageSleepWakeUUIDCleared) {
        printf("Cleared.\n");
    } else if (messageArgument == (void *)kIOPMMessageSleepWakeUUIDSet) {
        _print_uuid_string();        
    }
}

static void show_uuid(bool keep_running)
{
    if (!keep_running) {
        _print_uuid_string();
        return;
    }
    
    io_registry_entry_t     rd = copyRootDomainRef();
    IONotificationPortRef   notify = NULL;
    io_object_t             notification_ref = IO_OBJECT_NULL;
    
    notify = IONotificationPortCreate(kIOMasterPortDefault);
    IONotificationPortSetDispatchQueue(notify, dispatch_get_main_queue());
    
    IOServiceAddInterestNotification(notify, rd, 
                                     kIOGeneralInterest, _show_uuid_handler,
                                     NULL, &notification_ref);
     install_listen_IORegisterForSystemPower();
     printf("Logging UUID changes.\n");

    print_pretty_date(CFAbsoluteTimeGetCurrent(), false);
    _print_uuid_string();

    CFRunLoopRun();
}


static void show_NULL_HID_events(void)
{
    CFArrayRef          systemHIDEvents = NULL;
    IOReturn            ret;
    int                 topLevelCount = 0;
    
    ret = IOPMCopyHIDPostEventHistory(&systemHIDEvents);
    if (kIOReturnNotFound == ret) {
        // System is not collecting HID event history.
        // Assuming this is a setting that you can turn off with a hidden pmset flag,
        // then kIOReturnNotFound indicates it's turned off.
        printf("FAIL: ret=0x%08x kIOReturnNotFound - HID event capturing is OFF\n", ret);
        goto exit;
    } else if (kIOReturnSuccess != ret) {
        // Any other non-success return indicates a failure.
        printf("FAIL: ret=0x%08x - unexpected error\n", ret);
        return;
    }
    
    if (!systemHIDEvents || (0 == (topLevelCount = CFArrayGetCount(systemHIDEvents))))
    {
        printf("PASS: kIOReturnSuccess with %s\n", systemHIDEvents?"zero events":"no returned dictionary");
        goto exit;
    }
    
    int i;
    for (i=0; i<topLevelCount; i++)
    {
        CFDictionaryRef     d = CFArrayGetValueAtIndex(systemHIDEvents, i);
        char                app_path_string[1024];
        CFStringRef         appPathString = NULL;
        CFArrayRef          dataList = NULL;
        CFDataRef           dataChunk = NULL;
        IOPMHIDPostEventActivityWindow  *bucket = NULL;        
        CFNumberRef         pidNum = NULL;
        int                 _pid = 0;

        printf("\n");

        pidNum = CFDictionaryGetValue(d, kIOPMHIDAppPIDKey);
        CFNumberGetValue(pidNum, kCFNumberIntType, &_pid);       
        printf("* PID = %d\n", _pid);
    
        appPathString = CFDictionaryGetValue(d, kIOPMHIDAppPathKey);
        if (appPathString
            && CFStringGetCString(appPathString, app_path_string, sizeof(app_path_string), kCFStringEncodingMacRoman))
        {
            printf(" Name = %s\n", app_path_string);
        } else {
            printf(" Name = unknown\n");
        }
    
        dataList = CFDictionaryGetValue(d, kIOPMHIDHistoryArrayKey);
        int j;
        for (j=0; j<CFArrayGetCount(dataList); j++)
        {
            dataChunk = CFArrayGetValueAtIndex(dataList, j);
            bucket = (IOPMHIDPostEventActivityWindow *)CFDataGetBytePtr(dataChunk);
            printf(" Bucket (5 minute) starts: ");
            print_pretty_date(bucket->eventWindowStart, true);
            printf("   NULL events = %d\n", bucket->nullEventCount);
            printf("   Non-NULL events = %d\n", bucket->hidEventCount);
        }
    
    }

exit:
    if (systemHIDEvents)
        CFRelease(systemHIDEvents);
    return;
}
static bool is_display_dim_captured(void)
{
    io_registry_entry_t disp_wrangler = IO_OBJECT_NULL;
    CFBooleanRef dimCaptured          = NULL;
    bool ret                          = false;

    disp_wrangler = IORegistryEntryFromPath(
                            kIOMasterPortDefault,
                            kIOServicePlane ":/IOResources/IODisplayWrangler");
    if(!disp_wrangler)
        return false;

    dimCaptured = IORegistryEntryCreateCFProperty(
                        disp_wrangler,
                        CFSTR("DimCaptured"),
                        kCFAllocatorDefault,
                        kNilOptions);

    if(dimCaptured)
    {
        ret = (kCFBooleanTrue == dimCaptured);
        CFRelease(dimCaptured);
    }

    IOObjectRelease(disp_wrangler);

    return ret;
}
#define kRootDomainUserClientClass 	"RootDomainUserClient"
static void show_root_domain_user_clients(void)
{
	io_registry_entry_t		_rootdomain = copyRootDomainRef();
	io_iterator_t			_matchingUserClients = IO_OBJECT_NULL;
	io_registry_entry_t		_iter = IO_OBJECT_NULL;
	kern_return_t			kr;
	
	if (!_rootdomain) {
		printf("Internal Error - can't find root domain.");
		return;
	}
	
	kr = IORegistryEntryGetChildIterator(_rootdomain, kIOServicePlane, &_matchingUserClients);
	if (kr != KERN_SUCCESS || !_matchingUserClients) {
		printf("Internal Error - can't find user clients. (kern_return_t error = %d)", kr);
		return;
	}
	
	while ( ( _iter = IOIteratorNext(_matchingUserClients) ) ) 
	{
		io_name_t 			objectClass;
		
		IOObjectGetClass((io_object_t)_iter, objectClass);
		if (!strncmp(objectClass, kRootDomainUserClientClass, strlen(kRootDomainUserClientClass)))
		{
			CFStringRef 	creatorString = NULL;
			char			stringBuf[kMaxLongStringLength];

			creatorString = (CFStringRef)IORegistryEntryCreateCFProperty(_iter, CFSTR(kIOUserClientCreatorKey), 0, 0);			
			if (creatorString
			&& CFStringGetCString(creatorString, stringBuf, sizeof(stringBuf), kCFStringEncodingMacRoman))
			{
				printf(" - %s\n", stringBuf);
			}
            
            if(creatorString)
                CFRelease(creatorString);
		}
	}
	
	IOObjectRelease(_matchingUserClients);
	IOObjectRelease(_rootdomain);

	return;
}

static void show_getters(void)
{
    int i = 0;
    for (i=0; i<the_getters_count; i++) 
    {
        if (the_getters[i].actionType == kActionGetOnceNoArgs) {
            printf("%s\n", the_getters[i].arg);
        }
    }
}

void fetchChannelData( char     *object,
    uint64_t                channel_id,
    bool                    print_error,
    void                    (^processChannelData)(uint64_t, CFStringRef))
{
    CFMutableDictionaryRef  desiredChs = NULL, subbedChs = NULL;
    CFDictionaryRef 		mdict  = NULL;
    IOReportSubscriptionRef iorsub = NULL;
    CFDictionaryRef         samples = NULL;


    if (!(mdict = IOServiceMatching(object))) {
        printf("Failed to match an object with name %s in registry\n", object);
        goto exit;
    }
    desiredChs = IOReportCopyChannelsWithID(mdict, channel_id, NULL);
    if (!desiredChs) {
        if (print_error)
            printf("Failed to find channel reporting power state for the object %s\n", object);
        goto exit;
    }
    if (!(iorsub = IOReportCreateSubscription(nil, desiredChs, &subbedChs, 0, NULL))) {
            if (print_error)
                printf("Internal failure: Failed to get power state information\n");
        goto exit;
    }


    if ((samples = IOReportCreateSamples(iorsub, subbedChs, NULL))) {
        IOReportIterate(samples, ^(IOReportSampleRef ch) {
     
            uint64_t curstate = IOReportSimpleGetIntegerValue(ch, NULL);
            CFStringRef drv_name = IOReportChannelGetDriverName(ch);
            
            processChannelData(curstate, drv_name);
            return kIOReportIterOk;
        });
	}
    else {
        if (print_error)
            printf("Internal failure: Failed to get power state information\n");
        goto exit;
    }


exit:
    if (iorsub) CFRelease(iorsub);
    if (subbedChs)  CFRelease(subbedChs);
    if (desiredChs) CFRelease(desiredChs);
    if (mdict) CFRelease(mdict);


    return;

}


void display_powerstate( char *object, bool print_error)
{
    static  bool            row1 = true;


    if (row1) {
        printf("\n%15s  %13s  %9s  %s\n", "Driver ID", "Current State", "Max State", "Current State Description");
        row1= false;
    }

    fetchChannelData(object, kPMCurrStateChID, print_error,  ^(uint64_t state_id, CFStringRef drv_name) {
        bool is_on = false, is_usable = false, is_lowpower = false;
        uint32_t cur_st=0, max_st=0;
        CFNumberRef objRef = NULL;
        const char *dname_cstr;
        char dname_buf[25], *spcptr;

        // making this name useful is one reason this code might be better
        // in ioreg :P
        dname_cstr = CFStringGetCStringPtr(drv_name, kCFStringEncodingUTF8);
        if (!dname_cstr)    dname_cstr = "missing";
        snprintf(dname_buf, sizeof(dname_buf), "%s", dname_cstr);
        if ((spcptr = strchr(dname_buf, ' ')))        *spcptr = '\0';


        CFDictionaryRef dict = IOPMCopyPowerStateInfo(state_id);
        if (!dict) {
            printf("Internal error: Failed to obtain power state information for driver %s\n", dname_cstr);
            return ;
        }
        objRef = CFDictionaryGetValue(dict, kIOPMNodeCurrentState);
        if (objRef) 
            CFNumberGetValue(objRef, kCFNumberIntType, &cur_st);
        
        objRef = CFDictionaryGetValue(dict, kIOPMNodeMaxState);
        if (objRef)
            CFNumberGetValue(objRef, kCFNumberIntType, &max_st);

        is_on = (CFDictionaryGetValue(dict, kIOPMNodeIsPowerOn) == kCFBooleanTrue) ? true : false;
        is_usable = (CFDictionaryGetValue(dict, kIOPMNodeIsDeviceUsable) == kCFBooleanTrue) ? true : false;
        is_lowpower = (CFDictionaryGetValue(dict, kIOPMNodeIsLowPower) == kCFBooleanTrue) ? true : false;



        printf("%-25s %3d  %9d  ", dname_buf, cur_st, max_st);
        if ( (is_on || is_usable || is_lowpower) == false) printf("None\n");
        else {
            bool comma = false;
            if (is_on) {
                printf("ON"); 
                comma = true;
            }
            if (is_usable) {
                if (comma) printf(",");
                printf("USEABLE");
                comma = true;
            }
            if (is_lowpower) {
                if (comma) printf(",");
                printf("LOW_POWER");
            }
            printf("\n");
        }
        if (dict) CFRelease(dict);
        return ;
    });


    return;
}



void scan_powerplane(io_registry_entry_t service)
{
    io_registry_entry_t child       = 0; // (needs release)
    io_registry_entry_t childUpNext = 0; // (don't release)
    io_iterator_t       children    = 0; // (needs release)
    io_name_t           name;
    kern_return_t       status      = KERN_SUCCESS;


    status = IORegistryEntryGetChildIterator(service, "IOPower", &children);
    if (status != KERN_SUCCESS) {
        return;
    }

    childUpNext = IOIteratorNext(children);

    status = IORegistryEntryGetNameInPlane(service, "IOPower", name);

    if (strncmp(name, "IOPowerConnection", sizeof("IOPowerConnection"))) {

        display_powerstate(name, false);
    }


    while (childUpNext) {
        child       = childUpNext;
        childUpNext = IOIteratorNext(children);

        scan_powerplane(child);

        IOObjectRelease(child);
    }
    
    IOObjectRelease(children);

}
static void show_power_state(char **argv)
{
    char                    *object = NULL;
    int                     i = 0, cnt = 0;
    io_registry_entry_t     service  = 0;

    if (argv[i] != NULL) {
        object = argv[i++];

        do {
            display_powerstate(object, true);
            cnt++;
        } while ((object = argv[i++]) != NULL); 
        if (cnt == 0) goto exit;
    }
    else if ((service = IORegistryGetRootEntry(kIOMasterPortDefault)) != 0) {
        scan_powerplane(service);
        IOObjectRelease(service);
        goto exit;
    }
    else {
        printf("Internal failure: Failed to get the registry root entry\n");
        goto exit;
    }

exit:
    ;   // C doesn't allow labels at the end of functions?
}

static void display_statelog(IOReportChannelRef ch, int nobjects)
{
    uint32_t nstates, i;
    uint64_t state_id, transitions, ticks;
    uint64_t drv_name = IOReportChannelGetDriverName(ch);
    const char *dname_cstr;
    char dname_buf[22], *spcptr;
    uint32_t max_st=0;
    CFNumberRef objRef = NULL;
    CFDictionaryRef dict;
    static int max_states = 0;
    static int cnt = 0;
    int cur_state;


    nstates = IOReportStateGetCount(ch);
    if (cnt < nobjects) {
        for (i=0; i<nstates; i++) {
            state_id = IOReportStateGetIDForIndex(ch, i);

            dict = IOPMCopyPowerStateInfo(state_id);
            if (!dict) continue;

            objRef = CFDictionaryGetValue(dict, kIOPMNodeMaxState);
            if (objRef) {
                CFNumberGetValue(objRef, kCFNumberIntType, &max_st);
                if (max_st+1 > max_states) max_states=max_st+1;
            }

            CFRelease(dict);
        }

        cnt++;
        return;
    }

    cur_state = IOReportStateGetCurrent(ch);
    if ((cnt-nobjects) % (10*nobjects) == 0) {
        printf("\n");
        print_pretty_date(CFAbsoluteTimeGetCurrent(), true); 
        printf("%-18s ", "    Driver");
        for (i=0; i < max_states; i++) {
            printf("%8s[%d] ", "Time", i);
        }
        printf("       ");
        for (i=0; i < max_states; i++) {
            printf("%8s[%d] ", "Entries", i);
        }
        printf("\n");
    }

    // making this name useful is one reason this code might be better
    // in ioreg :P
    dname_cstr = CFStringGetCStringPtr(drv_name, kCFStringEncodingUTF8);
    if (!dname_cstr)    dname_cstr = "missing";
    snprintf(dname_buf, sizeof(dname_buf), "%s", dname_cstr);
    if ((spcptr = strchr(dname_buf, ' ')))        *spcptr = '\0';


    printf("%-22s ", dname_buf);
    for (i=0; i<nstates; i++) {
        ticks = IOReportStateGetResidency(ch, i);
        if (cur_state == i)
            printf("*%#-11llx", ticks);
        else
            printf("%#-12llx", ticks);
    }
    for (;i < max_states; i++) {
        printf("%-12s ", " ");
    }
    printf("    ");
    for (i=0; i<nstates; i++) {
        transitions = IOReportStateGetInTransitions(ch, i);
        printf("%-11lld ", transitions);
    }
    for (;i < max_states; i++) {
        printf("%-11s ", " ");
    }

    printf("\n");

    cnt++;
}


static void show_power_statelog(char **argv)
{
    CFDictionaryRef 		mdict = NULL;
    IOReportSubscriptionRef sub = NULL;
    CFMutableDictionaryRef  desiredChs = NULL, subbedChs = NULL;
    CFDictionaryRef         current = NULL, diff = NULL;
    CFDictionaryRef         prev = NULL;
    char                    *object = NULL;
    int                     i = 0, nobjects = 0;
    int                     interval = 0;

    if (argv[i] != NULL) {
        if (!strncmp(argv[i], "-i", sizeof("-i"))) {
            interval = strtol(argv[i+1], NULL, 0);
            i += 2;
        }
    }

    if (argv[i] != NULL) 
        object = argv[i++];
    else 
        object = "IOPMrootDomain";

    if (!interval) interval = 5;
    do {
        if (!(mdict = IOServiceMatching(object))) {
            printf("Failed to match an object with name %s in registry\n", object);
            continue;
        }

        desiredChs = IOReportCopyChannelsWithID(mdict, kPMPowerStatesChID, NULL);
        CFRelease(mdict); mdict = NULL;

        if (!desiredChs) {
            printf("Failed to find channel reporting power state for the object %s\n", object);
            continue;
        }
        nobjects++;
    } while ((object = argv[i++]) != NULL); 

    if (nobjects == 0) goto exit;

	if (!(sub = IOReportCreateSubscription(nil, desiredChs, &subbedChs, 0, NULL))) {
        printf("Internal failure: Failed to get power state information\n");
        goto exit;
	}

    printf("Polling at %d secs interval\n", interval);
    while ((current = IOReportCreateSamples(sub, subbedChs, NULL))) {
        if (prev) {
            diff = IOReportCreateSamplesDelta(prev, current, NULL);
            if (!diff) {
                printf("failed to compare power state to previous state");
                goto exit;
            }
            CFRelease(prev);
            prev = current;
            // samples = diff;
        }
        else {
            prev = current;
        }
        IOReportIterate(diff, ^(IOReportChannelRef ch) {
            display_statelog(ch, nobjects);

            return kIOReportIterOk;
        });

        sleep(interval);
	}

exit:
    if (sub) CFRelease(sub);
    if (desiredChs) CFRelease(desiredChs);
    if (subbedChs) CFRelease(subbedChs);
}

static void show_rdStats(char **argv)
{

    fetchChannelData("IOPMrootDomain", kSleepCntChID, true,
            ^(uint64_t state_id, CFStringRef drv_name){ printf("Sleep Count:%lld\n", state_id); } );

    fetchChannelData("IOPMrootDomain", kDarkWkCntChID, true,
            ^(uint64_t state_id, CFStringRef drv_name){ printf("Dark Wake Count:%lld\n", state_id); } );

    fetchChannelData("IOPMrootDomain", kUserWkCntChID, true,
            ^(uint64_t state_id, CFStringRef drv_name){ printf("User Wake Count:%lld\n", state_id); } );

}


#define kIOPMSystemCapabilitiesKey  "System Capabilities"
#define kPwrMgtKey                  "IOPowerManagement"
static void show_sysstate(char **argv)
{
    CFMutableDictionaryRef  rdProps = NULL;
    CFDictionaryRef  states = NULL;
    io_registry_entry_t rootDomain = copyRootDomainRef();
    CFNumberRef caps_cf = NULL, currSt_cf = NULL, desiredSt_cf = NULL;
    uint32_t caps;
    unsigned long currSt, desiredSt;
    
    IORegistryEntryCreateCFProperties(rootDomain, &rdProps, 0, 0);
    
    if (!rdProps) {
        printf("Internal error: Failed to get IOPMrootDomain properties\n");
        goto exit;
    }

    caps_cf = CFDictionaryGetValue(rdProps, CFSTR(kIOPMSystemCapabilitiesKey));
    if (caps_cf) {
        CFNumberGetValue(caps_cf, kCFNumberIntType, &caps);
        printf("Current System Capabilities are: ");
        if (caps) {
            if (caps & kIOPMSystemCapabilityCPU) printf("CPU ");
            if (caps & kIOPMSystemCapabilityGraphics) printf("Graphics ");
            if (caps & kIOPMSystemCapabilityAudio) printf("Audio ");
            if (caps & kIOPMSystemCapabilityNetwork) printf("Network ");
        }
        else {
            printf("None");
        }
        printf("\n");
    }
        
    states = CFDictionaryGetValue(rdProps, CFSTR(kPwrMgtKey));
    if (!states) goto exit;

    currSt_cf = CFDictionaryGetValue(states, CFSTR("CurrentPowerState"));
    if (currSt_cf) {
        CFNumberGetValue(currSt_cf, kCFNumberLongType, &currSt);
        printf("Current Power State: %lu\n", currSt);
    }
    desiredSt_cf = CFDictionaryGetValue(states, CFSTR("DesiredPowerState"));
    if (desiredSt_cf) {
        CFNumberGetValue(desiredSt_cf, kCFNumberLongType, &desiredSt);
        if (desiredSt != currSt) {
            printf("Desired State: %lu\n", desiredSt);
            if (desiredSt == 1)
                printf("System restart is in progress\n");
            else if(desiredSt == 2)
                printf("System State is changing to Sleep\n");
            else if (desiredSt == 3)
                printf("System is waking from sleep\n");
        }
    }

exit:
    if (rootDomain) IOObjectRelease(rootDomain);
    if (rdProps) CFRelease(rdProps);

}

static void show_everything(char **argv)
{
    printf("pmset is invoking all non-blocking -g arguments");
    int i=0;
	for (i=0; i<the_getters_count; i++) {
	    if (the_getters[i].actionType == kActionGetOnceNoArgs) {	    
            printf("\nINVOKE: pmset -g %s\n", the_getters[i].arg);
            the_getters[i].action(argv);
        }
    }    
}


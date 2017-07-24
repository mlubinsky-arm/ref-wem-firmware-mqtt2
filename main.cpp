// ****************************************************************************
//  Firmware Over The Air (FOTA) demo
//
//  This application demonstrates how to perform fota using mbed cloud 1.2.
//
//  By the ARM Reference Design (Red) Team
// ****************************************************************************
#include "m2mclient.h"

#include "commander.h"
#include "DHT.h"
#include "displayman.h"
#include "GL5528.h"
#include "keystore.h"
#include "lcdprogress.h"

#include <SDBlockDevice.h>
#include <errno.h>
#include <factory_configurator_client.h>
#include <mbed-trace-helper.h>
#include <mbed-trace/mbed_trace.h>

#if MBED_CONF_APP_WIFI
#include <ESP8266Interface.h>
#else
#include <EthernetInterface.h>
#endif

#define TRACE_GROUP "main"

// Convert the value of a C macro to a string that can be printed.  This trick
// is straight out of the GNU C documentation.
// (https://gcc.gnu.org/onlinedocs/gcc-4.9.0/cpp/Stringification.html)
#define xstr(s) str(s)
#define str(s) #s

#ifndef DEVTAG
#error "No dev tag created"
#endif

// ****************************************************************************
// DEFINEs and type definitions
// ****************************************************************************
#define MACADDR_STRLEN 18

enum FOTA_THREADS {
    FOTA_THREAD_DISPLAY = 0,
    FOTA_THREAD_SENSOR_LIGHT,
    FOTA_THREAD_DHT,
    FOTA_THREAD_COUNT
};

// ****************************************************************************
// Globals
// ****************************************************************************
/* declared in pal_plat_fileSystem.cpp, which is included because COMMON_PAL
 * is defined in mbed_app.json */
extern SDBlockDevice sd;
DisplayMan display;
M2MClient *gmbed_client;
NetworkInterface *gnet;

Thread tman[FOTA_THREAD_COUNT];

// ****************************************************************************
// Threads
// ****************************************************************************
static void thread_light_sensor(M2MClient *mbed_client)
{
    M2MObject *light_obj;
    M2MObjectInstance *light_inst;
    M2MResource *light_res;

    using namespace fota::sensor;
    uint8_t res_buffer[33] = {0};
    int size = 0;
    light::LightSensor<light::BOARD_GROVE_GL5528> light(A0);
    int light_id = display.register_sensor("Light");

    /* register the m2m object */
    light_obj = M2MInterfaceFactory::create_object("3301");
    light_inst = light_obj->create_object_instance();

    light_res = light_inst->create_dynamic_resource("1", "light_resource",
                                                    M2MResourceInstance::FLOAT,
                                                    true /* observable */);
    light_res->set_operation(M2MBase::GET_ALLOWED);
    light_res->set_value((uint8_t *)"0", 1);

    mbed_client->add_resource(light_obj);

    while (true) {
        light.update();
        float flux = light.getFlux();

        size = sprintf((char *)res_buffer, "%2.2f", flux);

        display.set_sensor_status(light_id, (char *)res_buffer);
        light_res->set_value(res_buffer, size);
        Thread::wait(5000);
    }
}

static void thread_dht(M2MClient *mbed_client)
{
    M2MObject *dht_h_obj, *dht_t_obj;
    M2MObjectInstance *dht_h_inst, *dht_t_inst;
    M2MResource *dht_h_res, *dht_t_res;

    uint8_t res_buffer[33] = {0};
    int size = 0;

    DHT dht(D4, AM2302);
    eError readError;
    float temperature, humidity;

    int thermo_id = display.register_sensor("Temp");
    int humidity_id = display.register_sensor("Humidity");

    /* register the m2m temperature object */
    dht_t_obj = M2MInterfaceFactory::create_object("3303");
    dht_t_inst = dht_t_obj->create_object_instance();

    dht_t_res = dht_t_inst->create_dynamic_resource("1", "temperature_resource",
                                                    M2MResourceInstance::FLOAT,
                                                    true /* observable */);
    dht_t_res->set_operation(M2MBase::GET_ALLOWED);
    dht_t_res->set_value((uint8_t *)"0", 1);

    mbed_client->add_resource(dht_t_obj);

    /* register the m2m humidity object */
    dht_h_obj = M2MInterfaceFactory::create_object("3304");
    dht_h_inst = dht_h_obj->create_object_instance();

    dht_h_res = dht_h_inst->create_dynamic_resource("1", "humidity_resource",
                                                    M2MResourceInstance::FLOAT,
                                                    true /* observable */);
    dht_h_res->set_operation(M2MBase::GET_ALLOWED);
    dht_h_res->set_value((uint8_t *)"0", 1);

    mbed_client->add_resource(dht_h_obj);

    while (true) {
        readError = dht.readData();
        if (readError == ERROR_NONE) {
            temperature = dht.ReadTemperature(CELCIUS);
            humidity = dht.ReadHumidity();
            tr_debug("DHT: temp = %fC, humi = %f%%\n", temperature, humidity);

            size = sprintf((char *)res_buffer, "%.1f", temperature);
            dht_t_res->set_value(res_buffer, size);
            display.set_sensor_status(thermo_id, (char *)res_buffer);

            size = sprintf((char *)res_buffer, "%.0f", humidity);
            dht_h_res->set_value(res_buffer, size);
            display.set_sensor_status(humidity_id, (char *)res_buffer);
        } else {
            tr_error("DHT: readData() failed with %d\n", readError);
        }
        Thread::wait(5000);
    }
}

static void start_sensors(M2MClient *mbed_client)
{
    printf("starting all sensors\n");
    tman[FOTA_THREAD_SENSOR_LIGHT].start(
        callback(thread_light_sensor, mbed_client));
    tman[FOTA_THREAD_DHT].start(callback(thread_dht, mbed_client));
}

static void stop_sensors()
{
    printf("stopping all sensors\n");
    tman[FOTA_THREAD_SENSOR_LIGHT].terminate();
    tman[FOTA_THREAD_DHT].terminate();
}

// ****************************************************************************
// Network
// ****************************************************************************
static void network_disconnect(NetworkInterface *net) { net->disconnect(); }

static char *network_get_macaddr(NetworkInterface *net, char *macstr)
{
    memcpy(macstr, net->get_mac_address(), MACADDR_STRLEN);
    return macstr;
}

#if MBED_CONF_APP_WIFI
static nsapi_security_t wifi_security_str2sec(const char *security)
{
    if (0 == strcmp("WPA/WPA2", security)) {
        return NSAPI_SECURITY_WPA_WPA2;

    } else if (0 == strcmp("WPA2", security)) {
        return NSAPI_SECURITY_WPA2;

    } else if (0 == strcmp("WPA", security)) {
        return NSAPI_SECURITY_WPA;

    } else if (0 == strcmp("WEP", security)) {
        return NSAPI_SECURITY_WEP;

    } else if (0 == strcmp("NONE", security)) {
        return NSAPI_SECURITY_NONE;

    } else if (0 == strcmp("OPEN", security)) {
        return NSAPI_SECURITY_NONE;
    }

    printf("warning: unknown wifi security type (%s), assuming NONE\n",
           security);
    return NSAPI_SECURITY_NONE;
}

/**
 * brings up wifi
 * */
static NetworkInterface *network_create(void)
{
    display.init_network("WiFi");
    return new ESP8266Interface(MBED_CONF_APP_WIFI_TX, MBED_CONF_APP_WIFI_RX,
                                MBED_CONF_APP_WIFI_DEBUG);
}

static int network_connect(NetworkInterface *net)
{
    int ret;
    char macaddr[MACADDR_STRLEN];
    ESP8266Interface *wifi;

    /* code is compiled -fno-rtti so we have to use C cast */
    wifi = (ESP8266Interface *)net;

    //wifi login info set to default values
    string ssid     = MBED_CONF_APP_WIFI_SSID;
    string pass     = MBED_CONF_APP_WIFI_PASSWORD;
    string security = MBED_CONF_APP_WIFI_SECURITY;

    //keystore db access
    Keystore k;

    //read the current state
    k.open();

    //use the keystore for ssid?
    if (k.exists("ssid")) {
        printf("Using SSID from keystore.\r\n");

        ssid = k.get("ssid");
    } else {
        printf("Using default SSID.\r\n");
    }

    //use the keystore for pass?
    if (k.exists("pass")) {
        printf("Using pass from keystore.\r\n");

        pass = k.get("pass");
    } else {
        printf("Using default pass.\r\n");
    }

    //use the keystor for security?
    if (k.exists("security")) {
        printf("Using security from keystore.\r\n");

        security = k.get("security");
    } else {
        printf("Using default security.\r\n");
    }

    printf("[WIFI] connecting: ssid=%s, mac=%s\n",
           ssid.c_str(), network_get_macaddr(wifi, macaddr));

    ret = wifi->connect(ssid.c_str(),
                        pass.c_str(),
                        wifi_security_str2sec(security.c_str()));
    if (0 != ret) {
        printf("[WIFI] Failed to connect to: %s (%d)\n",
               ssid.c_str(), ret);
        return ret;
    }

    printf("[WIFI] connected: ssid=%s, mac=%s, ip=%s, netmask=%s, gateway=%s\n",
           ssid.c_str(),
           network_get_macaddr(net, macaddr),
           net->get_ip_address(),
           net->get_netmask(),
           net->get_gateway());

    return 0;
}
#else
/**
 * brings up Ethernet
 * */
static NetworkInterface *network_create(void)
{
    display.init_network("Eth");
    return new EthernetInterface();
}

static int network_connect(NetworkInterface *net)
{
    int ret;
    char macaddr[MACADDR_STRLEN];

    /* note: Ethernet MAC isn't available until *after* a call to
     * EthernetInterface::connect(), so the first time we attempt to
     * connect this will print a NULL mac, but will work after a retry */
    printf("[ETH] obtaining IP address: mac=%s\n",
           network_get_macaddr(net, macaddr));
    ret = net->connect();
    if (0 != ret) {
        printf("ERROR: [ETH] Failed to connect! %d\n", ret);
        return ret;
    }
    printf("[ETH] connected: mac%s, ip=%s, netmask=%s, gateway=%s\n",
           network_get_macaddr(net, macaddr), net->get_ip_address(),
           net->get_netmask(), net->get_gateway());

    return ret;
}
#endif

// ****************************************************************************
// Cloud
// ****************************************************************************
void mbed_client_on_update_authorize(int32_t request)
{
    M2MClient *mbed_client = gmbed_client;

    switch (request) {
        /* Cloud Client wishes to download new firmware. This can have a
         * negative impact on the performance of the rest of the system.
         *
         * The user application is supposed to pause performance sensitive tasks
         * before authorizing the download.
         *
         * Note: the authorization call can be postponed and called later.
         * This doesn't affect the performance of the Cloud Client.
         * */
        case MbedCloudClient::UpdateRequestDownload:
            printf("Firmware download requested\r\n");
            printf("Authorization granted\r\n");
            stop_sensors();
            tman[FOTA_THREAD_DISPLAY].terminate();
            // From now on, display gets refreshed manually as the refresh
            // thread is gone.
            display.set_downloading();
            display.refresh();
            mbed_client->update_authorize(request);
            break;

        /* Cloud Client wishes to reboot and apply the new firmware.
         *
         * The user application is supposed to save all current work before
         * rebooting.
         *
         * Note: the authorization call can be postponed and called later.
         * This doesn't affect the performance of the Cloud Client.
         * */
        case MbedCloudClient::UpdateRequestInstall:
            printf("Firmware install requested\r\n");
            printf("Disconnecting network...\n");
            network_disconnect(gnet);
            display.set_installing();
            display.refresh();
            printf("Authorization granted\r\n");
            mbed_client->update_authorize(request);
            break;

        default:
            printf("ERROR: unknown request\r\n");
            led_set_color(IND_FWUP, IND_COLOR_FAILED);
            led_post();
            break;
    }
}

void mbed_client_on_update_progress(uint32_t progress, uint32_t total)
{
    uint32_t percent = progress * 100 / total;
    static uint32_t last_percent = 0;
    const char dl_message[] = "Downloading...";
    const char done_message[] = "Saving...";

    /* Drive the LCD in the main thread to prevent network corruption */
    display.set_progress(dl_message, progress, total);
    /* This lets the LED blink */
    display.refresh();

    if (last_percent < percent) {
        printf("Downloading: %lu\n", percent);
    }

    if (progress == total) {
        printf("\r\nDownload completed\r\n");
        display.set_progress(done_message, 0, 100);
        display.set_download_complete();
        display.refresh();
    }

    last_percent = percent;
}

static void mbed_client_on_registered(void *context)
{
    printf("mbed client registered\n");
    display.set_cloud_registered();
}

static void mbed_client_on_unregistered(void *context)
{
    printf("mbed client unregistered\n");
    display.set_cloud_unregistered();
}

static void mbed_client_on_error(void *context, int err_code,
                                 const char *err_name, const char *err_desc)
{
    printf("ERROR: mbed client (%d) %s\n", err_code, err_name);
    printf("    Error details : %s\n", err_desc);
    display.set_cloud_error();
}

static int register_mbed_client(NetworkInterface *iface, M2MClient *mbed_client)
{
    mbed_client->on_registered(NULL, mbed_client_on_registered);
    mbed_client->on_unregistered(NULL, mbed_client_on_unregistered);
    mbed_client->on_error(mbed_client, mbed_client_on_error);
    mbed_client->on_update_authorize(mbed_client_on_update_authorize);
    mbed_client->on_update_progress(mbed_client_on_update_progress);

    display.set_cloud_in_progress();
    mbed_client->call_register(iface);

    return 0;
}

static int init_fcc(void)
{
    fcc_status_e ret;

#if MBED_CONF_APP_FCC_WIPE
    ret = fcc_storage_delete();
    if (ret != FCC_STATUS_SUCCESS) {
        printf("ERROR: fcc delete failed: %d\n", ret);
    }
#endif

    ret = fcc_init();
    if (ret != FCC_STATUS_SUCCESS) {
        printf("ERROR: fcc init failed: %d\n", ret);
        return ret;
    }

    ret = fcc_developer_flow();
    if (ret == FCC_STATUS_KCM_FILE_EXIST_ERROR) {
        printf("fcc: developer credentials already exists\n");
    } else if (ret != FCC_STATUS_SUCCESS) {
        printf("ERROR: fcc failed to load developer credentials\n");
        return ret;
    }

    ret = fcc_verify_device_configured_4mbed_cloud();
    if (ret != FCC_STATUS_SUCCESS) {
        printf("ERROR: fcc device not configured for mbed cloud\n");
        return ret;
    }

    return 0;
}

// ****************************************************************************
// Generic Helpers
// ****************************************************************************
static int platform_init(void)
{
    int ret;

    /* setup the display */
    display.init(MBED_CONF_APP_VERSION);
    tman[FOTA_THREAD_DISPLAY].start(callback(thread_display_update, &display));

#if MBED_CONF_MBED_TRACE_ENABLE
    /* Create mutex for tracing to avoid broken lines in logs */
    if (!mbed_trace_helper_create_mutex()) {
        printf("ERROR: Mutex creation for mbed_trace failed!\n");
        return -EACCES;
    }

    /* Initialize mbed trace */
    mbed_trace_init();
    mbed_trace_mutex_wait_function_set(mbed_trace_helper_mutex_wait);
    mbed_trace_mutex_release_function_set(mbed_trace_helper_mutex_release);
#endif

    /* init the sd card */
    ret = sd.init();
    if (ret != BD_ERROR_OK) {
        printf("ERROR: sd init failed: %d\n", ret);
        return ret;
    }
    printf("sd init OK\n");

    return 0;
}

static void platform_shutdown()
{
    rtos::Thread::State state;

    state = tman[FOTA_THREAD_DISPLAY].get_state();
    if (rtos::Thread::Running == state) {
        tman[FOTA_THREAD_DISPLAY].join();
    }

    state = tman[FOTA_THREAD_SENSOR_LIGHT].get_state();
    if (rtos::Thread::Running == state) {
        tman[FOTA_THREAD_SENSOR_LIGHT].join();
    }

    state = tman[FOTA_THREAD_DHT].get_state();
    if (rtos::Thread::Running == state) {
        tman[FOTA_THREAD_DHT].join();
    }
}

// ****************************************************************************
// call back handlers for commandline interface
// ****************************************************************************
static void cmd_cb_del(vector<string>& params)
{
    //check params
    if (params.size() >= 2) {
        //the db
        Keystore k;

        //read the file
        k.open();

        //delete the given key
        k.del(params[1]);

        //write the changes back out
        k.close();

        //let user know
        cmd.printf("Deleted key %s\r\n",
                   params[1].c_str());
    } else {
        cmd.printf("Not enough arguments!\r\n");
    }
}

static void cmd_cb_get(vector<string>& params)
{
    //check params
    if (params.size() >= 1) {
        //database
        Keystore k;

        //don't show all keys by default
        bool ball = false;

        //read current values
        k.open();

        //if no param set to *
        if (params.size() == 1) {
            ball = true;
        } else if (params[1] == "*") {
            ball = true;
        }

        //show all keys?
        if (ball) {
            //get all keys
            vector<string> keys = k.keys();

            //walk the keys
            for (unsigned int n = 0; n < keys.size(); n++) {
                //get value
                string val = k.get(keys[n]);

                //format for display
                cmd.printf("%s=%s\r\n",
                           keys[n].c_str(),
                           val.c_str());
            }
        } else {

            // if not get one key
            string val = k.get(params[1]);

            //return just the value
            cmd.printf("%s\r\n",
                       val.c_str());
        }
    } else {
        cmd.printf("Not enough arguments!\r\n");
    }
}

static void cmd_cb_set(vector<string>& params)
{
    //check params
    if (params.size() >= 3) {

        //db
        Keystore k;

        //read the file into db
        k.open();

        //make the change
        k.set(params[1],params[2]);

        //write the file back out
        k.close();

        //return just the value
        cmd.printf("%s=%s\r\n",
                   params[1].c_str(),
                   params[2].c_str());

    } else {
        cmd.printf("Not enough arguments!\r\n");
    }
}

static void cmd_cb_reboot(vector<string>& params)
{
    cmd.printf("\r\nRebooting...");
    NVIC_SystemReset();
}

static void cmd_cb_flashything(vector<string>& params)
{
    Keystore k;

    k.kill_all();
}

/**
 * Wraps the prompt_interface with a loop for threading.
 */
void run_prompt()
{
    // add our callbacks
    cmd.add("get",
            "Get the value for the given key. Usage: get <key> defaults to *=all",
            cmd_cb_get);

    cmd.add("set",
            "Set a key to a the given value. Usage: set <key> <value>",
            cmd_cb_set);

    cmd.add("del",
            "Delete a key from the store. Usage: del <key> <value>",
            cmd_cb_del);

    cmd.add("reboot",
            "Reboot the device. Usage: reboot",
            cmd_cb_reboot);

    cmd.add("flashything",
            "Delete all user data. Usage: flashything",
            cmd_cb_flashything);

    //display the banner
    cmd.banner();

    //prime the serial
    cmd.init();

    //infinity and beyond
    while (true) {

        //did the user press a key?
        if (cmd.pump() == false) {

            // only sleep on zero buffer
            //slow down this tight loop please...
            wait(0.033f);
        }
    }
}

// ****************************************************************************
// Main
// main() runs in its own thread in the OS
// ****************************************************************************
int main()
{
    int ret;
    M2MClient *mbed_client;

    printf("FOTA demo version: %s\n", MBED_CONF_APP_VERSION);
    printf("     code version: " xstr(DEVTAG) "\n");

    /* minimal init sequence */
    printf("init platform\n");
    ret = platform_init();
    if (0 != ret) {
        return ret;
    }
    printf("init platform: OK\n");

    gmbed_client = new M2MClient();
    mbed_client = gmbed_client;

    /* create the network */
    printf("init network\n");
    gnet = network_create();
    if (NULL == gnet) {
        printf("ERROR: failed to create network stack\n");
        display.set_network_fail();
        return -ENODEV;
    }

    /* workaround: go ahead and connect the network.  it doesn't like being
     * polled for status before a connect() is attempted.
     * in addition, the fcc code requires a connected network when generating
     * creds the first time, so we need to spin here until we have an active
     * network. */
    do {
        display.set_network_in_progress();
        ret = network_connect(gnet);
        if (0 != ret) {
            display.set_network_fail();
            printf("WARN: failed to init network, retrying...\n");
            Thread::wait(2000);
        }
    } while (0 != ret);
    display.set_network_success();
    printf("init network: OK\n");

    /* initialize the factory configuration client
     * WARNING: the network must be connected first, otherwise this
     * will not return if creds haven't been provisioned for the first time.
     * */
    printf("init factory configuration client\n");
    ret = init_fcc();
    if (0 != ret) {
        printf("ERROR: failed to init factory configuration client: %d\n", ret);
        return ret;
    }
    printf("init factory configuration client: OK\n");

    /* start the sensors */
    /* WARNING: the sensor resources must be added to the mbed client
     * before the mbed client connects to the cloud, otherwise the
     * sensor resources will not exist in the portal. */
    printf("start sampling the sensors\n");
    start_sensors(mbed_client);

    /* connect to mbed cloud */
    printf("init mbed client\n");
    register_mbed_client(gnet, mbed_client);

    /* main run loop reads sensor samples and monitors connectivity */
    printf("main run loop\n");

    /*
        create thread and start our prompt
    */
    Thread thread_prompt(osPriorityNormal);
    thread_prompt.start(run_prompt);

    while (true) {
        /* TODO: move sensor sampling here instead of in separate threads */
        Thread::wait(1000);
    }

    /* stop sampling */
    stop_sensors();

    platform_shutdown();
    printf("exiting main\n");
    return 0;
}

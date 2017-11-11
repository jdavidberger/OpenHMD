#include <stdio.h>
#include <signal.h>
#include <string>
#include <map>

#include <json/value.h>
#include <json/reader.h>

#include "vl_driver.h"
#include "vl_config.h"
#include "vl_light.h"

vl_driver* driver;

bool quit = false;

static bool compare(const std::string& str1, const std::string& str2) {
    return str1.compare(str2) == 0;
}

static void dump_controller() {
    while(!quit)
        driver->log_watchman(driver->watchman_dongle_device);
}

static void dump_hmd_imu() {
    while(!quit)
        driver->log_hmd_imu(driver->hmd_imu_device);
}

static void dump_hmd_imu_pose() {
    while(!quit)
        driver->update_pose();
}

static void dump_hmd_light() {
    // hmd needs to be on to receive light reports.
    driver->send_hmd_on();
    driver->send_enable_lighthouse();
    while(!quit)
        driver->log_hmd_light(driver->hmd_light_sensor_device);
    printf("bye! closing display\n");
    driver->send_hmd_off();
    printf("closed display.\n");
    
}

static void dump_config_hmd() {
    char * config = vl_get_config(driver->hmd_imu_device);
    printf("hmd_imu_device config: %s\n", config);
}

static void dump_station_angle() {
    driver->send_hmd_on();

    vl_lighthouse_samples * raw_light_samples = new vl_lighthouse_samples();

    query_fun read_hmd_light = [raw_light_samples](unsigned char *buffer, int size) {
        if (buffer[0] == VL_MSG_HMD_LIGHT) {
            vive_headset_lighthouse_pulse_report2 pkt;
            vl_msg_decode_hmd_light(&pkt, buffer, size);
            vl_msg_print_hmd_light_csv(&pkt);

            for(int i = 0; i < 9; i++){
                raw_light_samples->push_back(pkt.samples[i]);
            }
        }
    };

    while(raw_light_samples->size() < 10000)
        hid_query(driver->hmd_light_sensor_device, read_hmd_light);

    vl_light_classify_samples(raw_light_samples);
}


void dump_positions() {
    driver->send_hmd_on();

    vl_lighthouse_samples * raw_light_samples = new vl_lighthouse_samples();

    query_fun read_hmd_light = [raw_light_samples](unsigned char *buffer, int size) {
        if (buffer[0] == VL_MSG_HMD_LIGHT) {
            vive_headset_lighthouse_pulse_report2 pkt;
            vl_msg_decode_hmd_light(&pkt, buffer, size);
            //vl_msg_print_hmd_light_csv(&pkt);

            for(int i = 0; i < 9; i++){
                raw_light_samples->push_back(pkt.samples[i]);
            }
        }
    };

    while(!quit) {
        while(raw_light_samples->size() < 1000)
            hid_query(driver->hmd_light_sensor_device, read_hmd_light);

        cv::Mat rvec, tvec;

        if (!raw_light_samples->empty())
            std::tie(tvec, rvec) = try_pnp('A', raw_light_samples, driver->config_sensor_positions);

        raw_light_samples->clear();
    }
}

static vl_lighthouse_samples parse_csv_file(const std::string& file_path) {

    printf("parsing csv %s\n", file_path.c_str());

    vl_lighthouse_samples samples;
    std::string line;
    std::ifstream csv_stream(file_path);

    if (!csv_stream.good()) {
        printf("CSV file not found: %s\n", file_path.c_str());
        return samples;
    }

    while(std::getline(csv_stream, line)) {
        std::istringstream s(line);

        std::string timestamp;
        std::string id;
        std::string length;

        getline(s, timestamp,',');
        getline(s, id,',');
        getline(s, length,',');

        vive_headset_lighthouse_pulse2 sample;

        sample.timestamp = std::stoul(timestamp);
        sample.sensor_id = std::stoul(id);
        sample.length = std::stoul(length);

        samples.push_back(sample);

        //printf("int ts %u id %u length %u\n", sample.timestamp, sample.sensor_id, sample.length);
    }

    if (samples.empty())
        printf("No samples found in %s\n", file_path.c_str());

    return samples;

}

static void dump_station_angle_from_csv(const std::string& file_path) {
    vl_lighthouse_samples samples = parse_csv_file(file_path);
    if (!samples.empty())
        vl_light_classify_samples(&samples);
}




static void pnp_from_csv(const std::string& file_path) {

    std::string config(vl_get_config(driver->hmd_imu_device));

    //printf("\n\nconfig:\n\n%s\n\n", config.c_str());

    std::stringstream foo;
    foo << config;

    Json::Value root;
    Json::CharReaderBuilder rbuilder;
    // Configure the Builder, then ...
    std::string errs;
    bool parsingSuccessful = Json::parseFromStream(rbuilder, foo, &root, &errs);
    if (!parsingSuccessful) {
        // report to the user the failure and their locations in the document.
        std::cout  << "Failed to parse configuration\n"
                   << errs;
        return;
    }

    std::string my_encoding = root.get("mb_serial_number", "UTF-32" ).asString();
    printf("mb_serial_number: %s\n", my_encoding.c_str());


    Json::Value modelPoints = root["lighthouse_config"]["modelPoints"];

    printf("model points size: %u\n", modelPoints.size());

    unsigned sensor_id = 0;

    std::map<unsigned, cv::Point3f> config_sensor_positions;

    for ( unsigned index = 0; index < modelPoints.size(); ++index ) {
        // Iterates over the sequence elements.

       Json::Value point = modelPoints[index];

       printf("%d: x %s y %s z %s\n", sensor_id, point[0].asString().c_str(), point[1].asString().c_str(), point[2].asString().c_str());

       cv::Point3f p = cv::Point3f(
               std::stod(point[0].asString()),
               std::stod(point[1].asString()),
               std::stod(point[2].asString()));

       config_sensor_positions.insert(std::pair<unsigned, cv::Point3f>(sensor_id, p));

       sensor_id++;
    }

    vl_lighthouse_samples samples = parse_csv_file(file_path);

    printf("Found %d samples\n", samples.size());

    if (!samples.empty())
        try_pnp('A', &samples, config_sensor_positions);
}

static void send_controller_off() {
    hid_send_feature_report(driver->watchman_dongle_device,
                            vive_controller_power_off,
                            sizeof(vive_controller_power_off));
}

static void signal_interrupt_handler(int sig) {
    quit = true;
}

typedef std::function<void(void)> taskfun;

void run(taskfun task) {
    if (!task)
        return;
    driver = new vl_driver();
    if (!driver->init_devices(0))
        return;
    signal(SIGINT, signal_interrupt_handler);
    task();
    delete(driver);
}

static std::map<std::string, taskfun> dump_commands {
    { "hmd-imu", dump_hmd_imu },
    { "hmd-light", dump_hmd_light },
    { "hmd-config", dump_config_hmd },
    { "controller", dump_controller },
    { "hmd-imu-pose", dump_hmd_imu_pose },
    { "lighthouse-angles", dump_station_angle }
};


static void send_hmd_on() {
  driver->send_hmd_on();
}

static void send_hmd_off() {
  driver->send_hmd_off();
}

static std::map<std::string, taskfun> send_commands {
    { "hmd-on", send_hmd_on },
    { "hmd-off", send_hmd_off },
    { "controller-off", send_controller_off }
};

static std::string commands_to_str(const std::map<std::string, taskfun>& commands) {
    std::string str;
    for (const auto& cm : commands)
        str += "  " + cm.first + "\n";
    return str;
}

static void print_usage() {
    std::string dmp_cmd_str = commands_to_str(dump_commands);
    std::string snd_cmd_str = commands_to_str(send_commands);

#define USAGE "\
Receive data from and send commands to Vive.\n\n\
usage: vivectl <command> <message>\n\n\
 dump\n\n\
%s\n\
 send\n\n\
%s\n\
Example: vivectl dump hmd-imu\n"

    printf(USAGE, dmp_cmd_str.c_str(), snd_cmd_str.c_str());
}

static void argument_error(const char * arg) {
    printf("Unknown argument %s\n", arg);
    print_usage();
}

taskfun _get_task_fun(char *argv[], const std::map<std::string, taskfun>& commands) {
    auto search = commands.find(std::string(argv[2]));
    if(search != commands.end()) {
        return search->second;
    } else {
        argument_error(argv[2]);
        return nullptr;
    }
}

int main(int argc, char *argv[]) {
    taskfun task = nullptr;

    if ( argc < 3 ) {
        print_usage();
    } else {
        if (compare(argv[1], "dump")) {
            task = _get_task_fun(argv, dump_commands);
            run(task);
        } else if (compare(argv[1], "send")) {
            task = _get_task_fun(argv, send_commands);
            run(task);
        } else if (compare(argv[1], "classify")) {
            std::string file_name = argv[2];
            dump_station_angle_from_csv(file_name);
        } else if (compare(argv[1], "pnp")) {
            std::string file_name = argv[2];
            task = [file_name]() {
                dump_positions();
                //pnp_from_csv(file_name);
            };
            run(task);
        } else {
            argument_error(argv[1]);
            return 0;
        }
    }

    return 0;
}

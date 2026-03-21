#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/pid/pid_climate.h"

namespace esphome {
namespace kiln_api {

class RequestHandler;

// https://github.com/esphome/esphome/blob/release/esphome/components/web_server_base/web_server_base.h
class KilnApi : public PollingComponent, public AsyncWebHandler {
 public:
  KilnApi(web_server_base::WebServerBase *base, pid::PIDClimate *kiln) : PollingComponent(1000), base_(base), kiln_(kiln) {}

  void setup() override;
  void update() override;
  void dump_config() override;

  void set_request_handler(RequestHandler *handler) { handler_ = handler; };
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override;

  // handle /schedule request
  void handle_schedule_request(AsyncWebServerRequest *request);

  // handle /state request (replaces SSE - clients poll this)
  void handle_state_request(AsyncWebServerRequest *request);

  // return json to sent as event
  std::string get_state();

  // reset kiln to ready state
  void reset_progress();

  // set PID climate mode
  void set_kiln_mode(climate::ClimateMode mode);

 protected:
  web_server_base::WebServerBase *base_;
  pid::PIDClimate *kiln_;
  RequestHandler *handler_;

  // runtime in seconds
  unsigned int runtime = 0;
  // starting temperature
  int schedule_start_temperature = 0;
  // schedule name
  std::string schedule_name = "";
  // array by three (ramp in celsius p/h, target temp in celsius, hold in minutes)
  std::vector<std::array<int, 3>> schedule;
  // index of the current_step in the schedule
  int current_step = 0;
  // remaining hold in seconds, negative means no hold
  int remaining_hold = -1;
  // monotonic counter incremented on every state change, used for ETag generation
  uint32_t state_etag_ = 0;
  // NVS preference for detecting restart during active schedule
  ESPPreferenceObject pref_;
  bool schedule_interrupted_ = false;
};

class RequestHandler : public Trigger<AsyncWebServerRequest &, AsyncResponseStream &> {
 public:
  explicit RequestHandler(KilnApi *parent);
};

}  // namespace kiln_api
}  // namespace esphome

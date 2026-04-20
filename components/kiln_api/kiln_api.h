#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/pid/pid_climate.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace kiln_api {

class RequestHandler;

// https://github.com/esphome/esphome/blob/release/esphome/components/web_server_base/web_server_base.h
class KilnApi : public PollingComponent, public AsyncWebHandler {
 public:
  KilnApi(web_server_base::WebServerBase *base, pid::PIDClimate *kiln, time::RealTimeClock *time) : PollingComponent(1000), base_(base), kiln_(kiln), time_(time) {}

  void setup() override;
  void update() override;
  void dump_config() override;

  void set_request_handler(RequestHandler *handler) { handler_ = handler; };
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override;

  // handle /schedule request
  void handle_schedule_request(AsyncWebServerRequest *request);

  // handle /schedule/cancel request
  void handle_cancel_request(AsyncWebServerRequest *request);

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
  time::RealTimeClock *time_;
  RequestHandler *handler_;

  // unix epoch timestamp of schedule start (0 = not running or NTP not synced)
  time_t started_at_ = 0;
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
  // pending mode change to be applied on the main loop task
  bool pending_mode_change_ = false;
  climate::ClimateMode pending_mode_ = climate::CLIMATE_MODE_OFF;
  // countdown in update() cycles before applying pending mode change — gives HTTP response time to flush
  int pending_mode_countdown_ = 0;
};

class RequestHandler : public Trigger<AsyncWebServerRequest &, AsyncResponseStream &> {
 public:
  explicit RequestHandler(KilnApi *parent);
};

}  // namespace kiln_api
}  // namespace esphome

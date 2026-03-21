#include "kiln_api.h"
#include "esphome.h"
#include <esp_http_server.h>

namespace esphome {
namespace kiln_api {

static const char *TAG = "kiln_api_0.2.1";

void KilnApi::setup() {
  pref_ = global_preferences->make_preference<bool>(fnv1_hash("kiln_api_active"));
  bool was_active = false;
  pref_.load(&was_active);
  if (was_active) {
    ESP_LOGW(TAG, "Schedule was interrupted by an unexpected restart");
    this->schedule_interrupted_ = true;
    bool cleared = false;
    pref_.save(&cleared);
  }
  base_->add_handler(this);
}

void KilnApi::dump_config() {
  ESP_LOGCONFIG(TAG, "Listening on path '/kiln'");
}

bool KilnApi::canHandle(AsyncWebServerRequest *request) const {
  std::string url = request->url();
  if (url.starts_with("/kiln/")) {
    return true;
  }
  return false;
}

void KilnApi::handle_state_request(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_GET) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  char etag[16];
  snprintf(etag, sizeof(etag), "\"%u\"", this->state_etag_);
  if (request->hasHeader("If-None-Match") && request->getHeader("If-None-Match")->value() == etag) {
    AsyncWebServerResponse *not_modified = request->beginResponse(304);
    not_modified->addHeader("Access-Control-Allow-Origin", "*");
    not_modified->addHeader("ETag", etag);
    request->send(not_modified);
    return;
  }
  std::string state_json = this->get_state();
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", state_json);
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("ETag", etag);
  request->send(response);
}

void KilnApi::handle_schedule_request(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_OPTIONS) {
    // basic CORS response
    AsyncWebServerResponse *response = request->beginResponse(204, "text/plain");
    response->addHeader("Access-Control-Allow-Methods", "OPTIONS, DELETE, POST");
    response->addHeader("Access-Control-Allow-Headers", "*");
    response->addHeader("Access-Control-Max-Age", "86400");
    request->send(response);

  } else if (request->method() == HTTP_DELETE) {
    // cancel current run and shutdown kiln
    ESP_LOGI(TAG, "Cancelled schedule %s, shutdown kiln", this->schedule_name.c_str());
    this->reset_progress();
    this->set_kiln_mode(climate::CLIMATE_MODE_OFF);
    request->send(200, "application/json", "{\"status\": \"ok\"}");

  } else if (request->method() == HTTP_POST) {
    // reset current progress
    this->reset_progress();

    // set the target temperature to the current temperature for the update loop to start at the right temp
    kiln_->target_temperature = kiln_->current_temperature;
    // set the schedule start temperature to the current
    this->schedule_start_temperature = kiln_->target_temperature;

    // Read body directly from the ESP-IDF httpd request
    // The IDF web server doesn't call handleBody for application/json content types
    httpd_req_t *req = *request;  // AsyncWebServerRequest has operator httpd_req_t*
    size_t content_len = request->contentLength();

    if (content_len > 0 && content_len < 4096) {
      std::string body_str;
      body_str.resize(content_len);
      int ret = httpd_req_recv(req, &body_str[0], content_len);

      if (ret > 0) {
        body_str.resize(ret);
        ESP_LOGD(TAG, "Received body: %s", body_str.c_str());

        json::parse_json(body_str, [&](JsonObject x) -> bool {
          if (x["name"].is<JsonVariant>() && x["schedule"].is<JsonVariant>()) {
            this->schedule_name.assign(x["name"].as<std::string>());
            JsonArrayConst parsed = x["schedule"].as<JsonArrayConst>();
            for(JsonVariantConst step : parsed) {
              int s[3];
              copyArray(step, s);
              this->schedule.push_back(std::to_array(s));
            }
          } else {
            request->send(500, "application/json", "{\"status\": \"error\", \"reason\": \"missing name or schedule key in JSON\"}");
          }
          return true;
        });
        ESP_LOGI(TAG, "Starting schedule %s, heating kiln", this->schedule_name.c_str());
        bool active = true;
        pref_.save(&active);
        this->schedule_interrupted_ = false;
        this->set_kiln_mode(climate::CLIMATE_MODE_HEAT);
        request->send(200, "application/json", "{\"status\": \"ok\"}");
      } else {
        ESP_LOGW(TAG, "Failed to read request body, ret=%d", ret);
        request->send(500, "application/json", "{\"status\": \"error\", \"reason\": \"failed to read body\"}");
      }
    } else {
      request->send(500, "application/json", "{\"status\": \"error\", \"reason\": \"invalid or missing JSON body\"}");
    }
  } else {  // unsupported method
    request->send(405, "text/plain", "Method Not Allowed");
  }
}

void KilnApi::handleRequest(AsyncWebServerRequest *request) {
  std::string url = request->url();
  std::string path = url.substr(5);  // strip /kiln
  if (path == "/schedule") {
    this->handle_schedule_request(request);
    return;
  }
  if (path == "/state") {
    this->handle_state_request(request);
    return;
  }
  request->send(404, "text/plain", "Not Found");
}

bool KilnApi::isRequestHandlerTrivial() const { return false; }

RequestHandler::RequestHandler(KilnApi *parent) {
  parent->set_request_handler(this);
}

void KilnApi::set_kiln_mode(climate::ClimateMode mode) {
  auto call = kiln_->make_call();
  call.set_mode(mode);
  call.perform();
}

void KilnApi::reset_progress() {
  this->schedule.clear();
  this->schedule_name = "";
  this->current_step = 0;
  this->remaining_hold = -1;
  this->runtime = 0;
  this->schedule_interrupted_ = false;
  bool inactive = false;
  pref_.save(&inactive);
  this->state_etag_++;
}

std::string KilnApi::get_state() {
  return json::build_json([this](JsonObject root) {
    root["step"] = this->current_step;
    root["runtime"] = this->runtime;
    root["start_temperature"] = this->schedule_start_temperature;
    root["temperature"] = this->kiln_->current_temperature;
    root["schedule"]["name"] = this->schedule_name;
    root["schedule"]["steps"] = this->schedule;
    root["interrupted"] = this->schedule_interrupted_;
  });
}

// controlloop, called every second
void KilnApi::update() {
  // id(kiln).mode = climate::CLIMATE_MODE_OFF;
  // id(kiln).action = climate::CLIMATE_ACTION_OFF;
  if (this->schedule.empty()) {
    return;
  }

  // shutdown kiln when last step done
  if(this->current_step >= this->schedule.size()) {
    ESP_LOGI(TAG, "Shutdown kiln");
    this->reset_progress();
    this->set_kiln_mode(climate::CLIMATE_MODE_OFF);
    return;
  }

  // update runtime seconds and invalidate ETag
  this->runtime++;
  this->state_etag_++;

  // extract data
  int ramp = this->schedule[current_step][0];
  int target = this->schedule[current_step][1];
  int hold = this->schedule[current_step][2];
  // divide 3600 to make ramp per second 1s
  float ramp_calculated = (float)ramp / (float)3600;
  // assume the first step is always heating since it is an kiln after all
  bool heating_step = true;

  // if target is lower then previous it is a cooling cycle, negate ramp_calculated
  if(current_step != 0 && target < this->schedule[current_step-1][1]) {
    ESP_LOGD(TAG, "Cooling step, negating ramp");
    ramp_calculated = -ramp_calculated;
    heating_step = false;
  }

  // calculate new target temperature
  float new_target = kiln_->target_temperature + ramp_calculated;
  // when overshoot, just set target
  if((heating_step && new_target >= target) || (!heating_step && new_target <= target)) {
    new_target = target;
  }

  // check if target from step reached
  // TODO: check this with real temp or on PID state?
  // id(kiln).action == climate::CLIMATE_ACTION_IDLE
  if (
    this->remaining_hold < 0
    && (
      // heating step, target must equal new_target to make sure the time is long enough
      // otherwise might exit early when target target temperature is reached prematurely
      (heating_step && kiln_->current_temperature >= target  && target == new_target)
      ||
      // cooling step
      (!heating_step && kiln_->current_temperature <= target && target == new_target)
    )
  ) {
    ESP_LOGI(TAG, "Target reached");
    if (hold > 0) {
      // set seconds to hold
      this->remaining_hold = hold * 60;
      ESP_LOGI(TAG, "Hold started, duration: %ih %im %is", this->remaining_hold/3600, (this->remaining_hold % 3600) /60, this->remaining_hold % 60);
    } else {
      // move to next step, no hold
      current_step++;
      ESP_LOGI(TAG, "Moving to step %i in schedule", this->current_step);
      return;
    }
  }
  // prepare when hold is at last iteration, next iteration will use new schedule
  if (this->remaining_hold == 1) {
    ESP_LOGI(TAG, "Hold done");
    // reset hold
    this->remaining_hold = -1;
    // move to next step
    this->current_step++;
    return;
  // just keep the hold going
  } else if (this->remaining_hold > 1) {
    this->remaining_hold--;
    ESP_LOGD(TAG, "Hold time remaining: %ih %im %is", this->remaining_hold/3600, (this->remaining_hold % 3600) /60, this->remaining_hold % 60);
    return;
  }

  if (new_target != kiln_->target_temperature) {
    ESP_LOGD(TAG, "Set target to %.1f", new_target);
    kiln_->target_temperature = new_target;
  }
}

}  // namespace kiln_api
}  // namespace esphome

// https://arduinojson.org/v6/how-to/create-converters-for-stl-containers/
namespace ArduinoJson {
template <typename T>
struct Converter<std::vector<T> > {
  static void toJson(const std::vector<T>& src, JsonVariant dst) {
    JsonArray array = dst.to<JsonArray>();
    for (T item : src)
      array.add(item);
  }

  static std::vector<T> fromJson(JsonVariantConst src) {
    std::vector<T> dst;
    for (T item : src.as<JsonArrayConst>())
      dst.push_back(item);
    return dst;
  }

  static bool checkJson(JsonVariantConst src) {
    JsonArrayConst array = src;
    bool result = array;
    for (JsonVariantConst item : array)
      result &= item.is<T>();
    return result;
  }
};
}  // namespace ArduinoJson

namespace ArduinoJson {
template <typename T, size_t N>
struct Converter<std::array<T, N> > {
  static void toJson(const std::array<T, N>& src, JsonVariant dst) {
    JsonArray array = dst.to<JsonArray>();
    for (T item : src)
      array.add(item);
  }

  static std::array<T, N> fromJson(JsonVariantConst src) {
    std::array<T, N> dst;
    size_t idx = 0;
    for (T item : src.as<JsonArrayConst>())
      dst[idx++] = item;
    return dst;
  }

  static bool checkJson(JsonVariantConst src) {
    JsonArrayConst array = src;
    bool result = array;
    size_t size = 0;
    for (JsonVariantConst item : array) {
      result &= item.is<T>();
      size++;
    }
    return result && size == N;
  }
};
}  // namespace ArduinoJson

#include "kiln_api.h"
#include "esphome.h"
#include <esp_http_server.h>
#include <cmath>

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
  char url_buf[513] = {};
  request->url_to(url_buf);
  std::string_view url(url_buf);
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
  if (request->hasHeader("If-None-Match") && request->get_header("If-None-Match") == etag) {
    AsyncWebServerResponse *not_modified = request->beginResponse(304, "text/plain");
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

void KilnApi::handle_cancel_request(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_OPTIONS) {
    AsyncWebServerResponse *response = request->beginResponse(204, "text/plain");
    response->addHeader("Access-Control-Allow-Methods", "OPTIONS, POST");
    response->addHeader("Access-Control-Allow-Headers", "*");
    response->addHeader("Access-Control-Max-Age", "86400");
    request->send(response);
  } else if (request->method() == HTTP_POST) {
    ESP_LOGI(TAG, "Cancelled schedule %s, shutdown kiln", this->schedule_name.c_str());
    this->reset_progress();
    this->pending_mode_ = climate::CLIMATE_MODE_OFF;
    this->pending_mode_change_ = true;
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"status\": \"ok\"}");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  } else {
    request->send(405, "text/plain", "Method Not Allowed");
  }
}

void KilnApi::handle_schedule_request(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_OPTIONS) {
    // basic CORS response
    AsyncWebServerResponse *response = request->beginResponse(204, "text/plain");
    response->addHeader("Access-Control-Allow-Methods", "OPTIONS, POST");
    response->addHeader("Access-Control-Allow-Headers", "*");
    response->addHeader("Access-Control-Max-Age", "86400");
    request->send(response);

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

        bool parse_ok = false;
        std::string parse_error;
        json::parse_json(body_str, [&](JsonObject x) -> bool {
          if (x["name"].is<JsonVariant>() && x["schedule"].is<JsonVariant>()) {
            JsonArrayConst parsed = x["schedule"].as<JsonArrayConst>();
            if (parsed.size() == 0) {
              parse_error = "schedule must not be empty";
              return true;
            }
            std::vector<std::array<int, 3>> steps;
            for (JsonVariantConst step : parsed) {
              JsonArrayConst arr = step.as<JsonArrayConst>();
              if (arr.size() != 3) {
                parse_error = "each step must have exactly 3 values [ramp, target, hold]";
                return true;
              }
              int ramp_val = arr[0].as<int>();
              int target_val = arr[1].as<int>();
              int hold_val = arr[2].as<int>();
              if (ramp_val <= 0 || ramp_val > 9999) {
                parse_error = "ramp must be between 1 and 9999 (deg/h)";
                return true;
              }
              if (target_val <= 0 || target_val > 1400) {
                parse_error = "target temperature must be between 1 and 1400";
                return true;
              }
              if (hold_val < 0 || hold_val > 43200) {
                parse_error = "hold must be between 0 and 43200 minutes";
                return true;
              }
              steps.push_back({ramp_val, target_val, hold_val});
            }
            if (!parse_error.empty()) {
              return true;
            }
            this->schedule_name.assign(x["name"].as<std::string>());
            this->schedule = std::move(steps);
            parse_ok = true;
          }
          return true;
        });
        if (!parse_ok) {
          std::string reason = parse_error.empty() ? "missing name or schedule key in JSON" : parse_error;
          std::string body = "{\"status\": \"error\", \"reason\": \"" + reason + "\"}";
          AsyncWebServerResponse *err = request->beginResponse(500, "application/json", body);
          err->addHeader("Access-Control-Allow-Origin", "*");
          request->send(err);
          return;
        }
        ESP_LOGI(TAG, "Starting schedule %s, heating kiln", this->schedule_name.c_str());
        bool active = true;
        pref_.save(&active);
        this->schedule_interrupted_ = false;
        this->pending_mode_ = climate::CLIMATE_MODE_HEAT;
        this->pending_mode_countdown_ = 2;
        this->pending_mode_change_ = true;
        {
          AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"status\": \"ok\"}");
          response->addHeader("Access-Control-Allow-Origin", "*");
          request->send(response);
        }
      } else {
        ESP_LOGW(TAG, "Failed to read request body, ret=%d", ret);
        AsyncWebServerResponse *err = request->beginResponse(500, "application/json", "{\"status\": \"error\", \"reason\": \"failed to read body\"}");
        err->addHeader("Access-Control-Allow-Origin", "*");
        request->send(err);
      }
    } else {
      AsyncWebServerResponse *err = request->beginResponse(500, "application/json", "{\"status\": \"error\", \"reason\": \"invalid or missing JSON body\"}");
      err->addHeader("Access-Control-Allow-Origin", "*");
      request->send(err);
    }
  } else {  // unsupported method
    request->send(405, "text/plain", "Method Not Allowed");
  }
}

void KilnApi::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[513] = {};
  request->url_to(url_buf);
  std::string_view url(url_buf);
  std::string_view path = url.substr(5);  // strip /kiln
  if (path == "/schedule") {
    this->handle_schedule_request(request);
    return;
  }
  if (path == "/schedule/cancel") {
    this->handle_cancel_request(request);
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
  this->pending_mode_change_ = false;
  this->pending_mode_countdown_ = 0;
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
  if (this->pending_mode_change_) {
    if (this->pending_mode_countdown_ > 0) {
      this->pending_mode_countdown_--;
      return;
    }
    ESP_LOGI(TAG, "Applying pending mode change: mode=%d", (int)this->pending_mode_);
    this->pending_mode_change_ = false;
    this->set_kiln_mode(this->pending_mode_);
    return;
  }
  if (this->schedule.empty()) {
    return;
  }

  // shutdown kiln when last step done
  if(this->current_step >= (int)this->schedule.size()) {
    ESP_LOGI(TAG, "Shutdown kiln: current_step=%d schedule_size=%d", this->current_step, (int)this->schedule.size());
    this->reset_progress();
    this->set_kiln_mode(climate::CLIMATE_MODE_OFF);
    return;
  }

  // enforce heat mode while schedule is running — re-applies if externally changed (e.g. HA state restore on reconnect)
  // uses the pending mechanism to avoid triggering successive API disconnects on rapid re-application
  if (kiln_->mode != climate::CLIMATE_MODE_HEAT) {
    ESP_LOGW(TAG, "Schedule active but climate not in heat mode (mode=%d), re-applying heat via pending", (int)kiln_->mode);
    if (!this->pending_mode_change_) {
      this->pending_mode_ = climate::CLIMATE_MODE_HEAT;
      this->pending_mode_countdown_ = 2;
      this->pending_mode_change_ = true;
    }
    return;
  }

  // guard against externally corrupted target temperature that would propagate NaN/Inf into schedule logic
  if (!std::isfinite(kiln_->target_temperature)) {
    ESP_LOGW(TAG, "Invalid target temperature (NaN/Inf), skipping schedule update");
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
    ESP_LOGI(TAG, "Target reached: step=%d current_temp=%.2f target=%d new_target=%.2f",
             this->current_step, kiln_->current_temperature, target, new_target);
    if (hold > 0) {
      // set seconds to hold (hold is validated <= 43200 minutes, so hold * 60 <= 2592000, safe for int)
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
    ESP_LOGI(TAG, "Hold done, moving to step %d", this->current_step + 1);
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
    ESP_LOGI(TAG, "Ramping: set target_temperature=%.2f (was %.2f)", new_target, kiln_->target_temperature);
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

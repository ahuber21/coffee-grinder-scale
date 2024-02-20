#pragma once

#include <ESPAsyncWebServer.h>

class API {
public:
  void begin(AsyncWebServer &server);

  // Handler for "/api/getDosage" endpoint
  void handleGetDosageRequest(AsyncWebServerRequest *request);

  // Check if a new value was received since the last call
  bool isNewValueReceived();

  // Get the new value
  float getNewValue();

private:
  float newDosageValue = 0;
  bool newValueReceived = false;
};
;

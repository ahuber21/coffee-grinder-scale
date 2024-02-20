#include "API.h"

void API::begin(AsyncWebServer &server) {
  // Set up your API endpoints here
  server.on("/api/example", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Hello from API!");
  });

  // Handler for "/api/getDosage" endpoint
  server.on(
      "/api/getDosage", HTTP_GET,
      std::bind(&API::handleGetDosageRequest, this, std::placeholders::_1));
}

void API::handleGetDosageRequest(AsyncWebServerRequest *request) {
  // Handle the GET request for "/api/getDosage" endpoint here
  // Extract the 'grams' parameter from the URL query
  if (request->hasParam("grams")) {
    String gramsParam = request->getParam("grams")->value();

    // Convert the gramsParam to integer
    newDosageValue = gramsParam.toFloat();
    newValueReceived = true;

    // Replace this example response with your actual implementation
    String jsonResponse = "{\"dosage\": \"" + gramsParam + " grams\"}";

    request->send(200, "application/json", jsonResponse);
  } else {
    // If 'grams' parameter is missing, send a 400 Bad Request response
    request->send(400, "text", "missing parameter");
  }
}

bool API::isNewValueReceived() { return newValueReceived; }

float API::getNewValue() {
  // Reset the flag after getting the new value
  newValueReceived = false;
  return newDosageValue;
}
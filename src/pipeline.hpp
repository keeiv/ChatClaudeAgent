// Configuration and review-gate helpers are independent of model transport.
#pragma once

#include "json.hpp"

bool validatePipeline(const Json& pipeline, std::string* error);
bool pipelineSimpleRoute(const Json& pipeline, const std::string& request);
std::string pipelineVerdict(const Json& message);
bool pipelineTestsPassed(const Json& handoff);
